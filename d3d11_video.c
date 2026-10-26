/* D3D11 video DDI: decoder decode + processor blit.
 *
 * What this file does:
 *
 *   - decoder side reports four profiles -- H.264 VLD without FMO/ASO
 *     (DXVA_ModeH264_VLD_NoFGT), VP9 VLD Profile 0 (DXVA_ModeVP9_VLD_Profile0),
 *     HEVC VLD Main (DXVA_ModeHEVC_VLD_Main) and AV1 VLD Profile 0
 *     (DXVA_ModeAV1_VLD_Profile0) -- plus HEVC VLD Main10
 *     (DXVA_ModeHEVC_VLD_Main10, HEVC-MAIN10) when this host can hold a P010
 *     surface, with one decoder config each.
 *   - P-BATCH P010: NV12 and P010 are both accepted as the decoder output
 *     format (CheckVideoDecoderFormat / GetVideoDecoderConfigCount / the output
 *     view), and EndFrame writes whichever surface the application allocated.
 *     A 10-bit (P010) surface receives the decoded picture MSB-aligned; when the
 *     host capture side really is P010 the picture is bit-exact, and when the
 *     driver had to fall back to NV12 the 8-bit picture is MSB-aligned
 *     (b << 8), which is lossless in the container. See the "P010" notes at the
 *     format-support helpers and the conversion in DecoderEndFrame. P010 is only
 *     advertised when the internal DXVK device can create such a texture
 *     (Vulkan G10X6_B10X6R10X6_2PLANE_420), so a host without it keeps NV12.
 *   - HEVC-MAIN10 (2026-10-25): Main10 is a *10-bit decode* profile, not just a
 *     10-bit container on an 8-bit picture. It is paired with P010 only
 *     (decoder_profile_format_supported), it opens the host session with
 *     VM_OPEN_CODEC_HEVC_MAIN10 so media-wdm asks S_FMT CAPTURE for
 *     V4L2_PIX_FMT_P010, and the HEVC SPS synthesis accepts
 *     bit_depth_luma_minus8 = 2 (hevc_params.h writes profile_idc 2). A driver
 *     that predates the selector answers OPEN with an error, which
 *     CreateVideoDecoder reports by name instead of timing out per frame.
 *     CreateVideoDecoder builds a real object and opens the
 *     virtio-media decoder session through vmedia_client's vm_open_codec(); the
 *     matching DestroyVideoDecoder flushes the old sequence (P3-3) and closes
 *     it with vm_close().
 *   - the decode path is real since P2-2b: CreateVideoDecoderOutputView records
 *     the NV12 texture the runtime will decode into, DecoderBeginFrame selects
 *     it, SubmitDecoderBuffers copies the Annex-B bytes out of the application's
 *     buffer(s) and appends them into one access unit, and DecoderEndFrame
 *     vm_send()s that unit, vm_get_frame()s the decoded NV12 and writes it into
 *     the output texture. See the decoder section below for the buffer/texture
 *     strategy and the error paths.
 *   - P2-2i: clients such as Chromium do not put SPS/PPS in the access unit --
 *     they pass them as a parsed DXVA_PicParams_H264 in the PICTURE_PARAMETERS
 *     decoder buffer. EndFrame therefore synthesises Annex-B SPS+PPS from that
 *     structure (h264_params.h) and prepends them to the access unit, and a
 *     failed decode is reported to the application through the next
 *     DecoderBeginFrame instead of pfnSetErrorCb (which removed the device).
 *   - P-BATCH VP9: the same decode path carries VP9.  VP9 has no out-of-band
 *     parameter sets, so nothing has to be rebuilt the way the H.264 SPS/PPS
 *     are; what VP9 does need is the access unit assembled from the
 *     SLICE_CONTROL records (DXVA_Slice_VPx_Short), a split of the VP9
 *     superframe into its members (one picture per EndFrame), and a header
 *     check before the bytes go to the host.  All of that is in vp9_params.h.
 *   - P-BATCH HEVC: the same decode path carries HEVC Main.  A D3D11 HEVC client
 *     sends slice NALs in BITSTREAM and the parsed SPS/PPS values in
 *     DXVA_PicParams_HEVC, exactly like the H.264 case, so EndFrame rebuilds
 *     VPS+SPS+PPS from those values (hevc_params.h) and prepends them.  A client
 *     that does carry VPS/SPS/PPS in the access unit (ffmpeg/PotPlayer style) is
 *     forwarded verbatim, which is also the fallback for streams whose
 *     SPS-level reference picture sets make synthesis impossible.
 *   - P-BATCH AV1: AV1 VLD Profile 0 (DXVA_ModeAV1_VLD_Profile0) is advertised
 *     and decoded through the same path.  A D3D11 AV1 client (Chromium's
 *     libgav1-backed accelerator) sends the PARSED headers in
 *     DXVA_PicParams_AV1 and only the raw tile payloads in BITSTREAM, so
 *     EndFrame rebuilds the temporal unit -- temporal delimiter, sequence
 *     header, frame header, tile group -- from those values (av1_params.h),
 *     exactly like the H.264/HEVC parameter-set synthesis.  The one field DXVA
 *     does not carry is refresh_frame_flags; it is derived from the NEXT
 *     frame's RefFrameMapTextureIndex and the picture is re-sent once as a
 *     hidden "DPB catch-up" frame (see the AV1 section below).
 *   - processor side reports the one conversion it can promise at the caps
 *     level (NV12 / YUY2 in -> BGRA8 out, linear space + YCbCr->RGB matrix
 *     conversion) and creates the enumerator / processor objects so
 *     ID3D11VideoDevice exists and MF's Video Processor MFT sees a processor.
 *   - VideoProcessorBlt is implemented. It does not contain a CSC or a
 *     scaler: it forwards the DDI objects to DXVK's video processor, which the
 *     UMD already links into this DLL (thirdparty/dxvk, d3d11_video.cpp).
 *     DXVK runs the YUV->RGB matrix in d3d11_video_blit_frag.frag, samples the
 *     NV12 luma/chroma planes (or the packed YUY2 texture), scales through the
 *     stream source/dest rectangles and draws into the output view. All of it
 *     is queued on the internal DXVK immediate context that device.c created
 *     (device->base.pDev1 / pCtx1), i.e. the same Vulkan queue as every other
 *     D3D11 command this UMD issues.
 *   - Synchronisation is not re-invented: the decode upload, the blit and the
 *     present all join DXVK's command stream, and the existing present path
 *     (dxgi.c) signals device->base.pPresentFence on pCtx4, flushes the context
 *     and waits for the fence. No busy-wait.
 *
 * Every entry of the table is populated (no NULL pointers), because the
 * runtime dispatches through it without checking individual slots.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h> /* snprintf: P3-3's flush reason strings */
/* P2-2i: DXVA_PicParams_H264 is the PICTURE_PARAMETERS payload Chromium fills
 * (it never puts SPS/PPS into the BITSTREAM buffer); h264_params.h turns those
 * derived values back into Annex-B SPS/PPS for the host decoder. */
#include <dxva.h>
#include "h264_params.h"
/* P-BATCH VP9: uncompressed-header + superframe parsing.  VP9 needs no
 * parameter-set synthesis (its headers are in band); what it needs is the
 * access unit rebuilt from the SLICE_CONTROL records and split at the superframe
 * index, plus a header check before the bytes are handed to the host. */
#include "vp9_params.h"
/* P-BATCH HEVC: VPS/SPS/PPS synthesis from DXVA_PicParams_HEVC (plus the
 * bitstream side: NAL scan, slice header pps id, IRAP detection).  HEVC is the
 * other codec whose parameter sets a D3D11 client does not send in band. */
#include "hevc_params.h"
/* P-BATCH AV1: sequence/frame header + tile group synthesis from
 * DXVA_PicParams_AV1 and DXVA_Tile_AV1.  A D3D11 AV1 client sends the parsed
 * headers and only the tile payloads, so the host (stateful) decoder gets the
 * OBU stream rebuilt here. */
#include "av1_params.h"

#include <winddk_compat.h>
#include <virtio_wddm_uapi.h>

#undef ERROR
#include "adapter.h"
#include "d3d11_video.h"
/* device.h / resource.h give the D3D11 resource and the internal DXVK device
 * that the video path has to reach; the DXVK header keeps the C++ D3D11
 * runtime headers (and COM) out of this translation unit. */
#include "device.h"
#include "resource.h"
#include "d3d11_video_dxvk.h"
/* The virtio-media user-mode client is the only code that speaks the driver's
 * IOCTL protocol (deliverables/p2-1a/client). P2-2a compiles it into this DLL
 * with VMEDIA_CLIENT_STATIC instead of re-implementing CreateFileW /
 * DeviceIoControl here, so the ABI stays single-sourced in vmedia_abi.h. */
#include "vmedia_client.h"

/* WDK spells this E_NOT_SUPPORTED in the DDI docs; winerror.h does not carry
 * the name, only the Win32 error it maps to. */
#ifndef E_NOT_SUPPORTED
#define E_NOT_SUPPORTED HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
#endif

/* P2-2c diagnostics: one unconditional line at each video DDI entry, emitted
 * before the entry looks at any argument.  The log then tells apart "the
 * runtime never called this DDI" (no line) from "it called us and we failed"
 * (line followed by the existing ERROR/INFO lines).  It uses the same
 * VIRTIO_WDDM_LOG_FILE mechanism as the rest of the UMD; nothing here changes
 * what the entry does. */
#define VIRTIO_WDDM_DDI_ENTRY(fmt, ...) \
    INFO("DDI-ENTRY[P2-2C] %s: " fmt, __FUNCTION__, ##__VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Reported caps                                                             */
/* ------------------------------------------------------------------------- */

/* One input stream is all a plain video processor needs for playback, and it is
 * the smallest value the runtime accepts. MaxStreamStates must be >= the number
 * of streams the driver accepts states for. */
#define VIRTIO_WDDM_VIDEO_MAX_INPUT_STREAMS 1
#define VIRTIO_WDDM_VIDEO_MAX_STREAM_STATES 1

/* A processor must expose at least one rate-conversion entry even when it does
 * no frame-rate conversion at all: ID3D11VideoDevice::CreateVideoProcessor takes
 * a RateConversionCapsIndex and 0 is only valid if the count is non-zero. */
#define VIRTIO_WDDM_VIDEO_RATE_CONVERSION_CAPS 1

/* DeviceCaps for GetVideoProcessorCaps. LINEAR_SPACE covers the RGB output
 * range handling; YCbCr_MATRIX_CONVERSION is the NV12/YUY2 -> RGB matrix. We do
 * not report xvYCC, RGB range conversion, nominal range or any feature caps
 * (alpha fill, constriction, luma key, stereo, rotation, mirror, HDR10): this
 * driver does not implement them. */
#define VIRTIO_WDDM_VIDEO_DEVICE_CAPS \
    (D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_LINEAR_SPACE | \
     D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_YCbCr_MATRIX_CONVERSION)

/* ------------------------------------------------------------------------- */
/* Private objects                                                           */
/* ------------------------------------------------------------------------- */

/* The runtime allocates the private block from the matching CalcPrivate*Size
 * and hands it back as hDrv…  (…) .pDrvPrivate; nothing here allocates. The magic
 * is a cheap way to notice a handle that was never created before it is
 * dereferenced.
 *
 * The `dxvk_*` members are DXVK COM objects (ID3D11VideoProcessor*, …) created
 * through d3d11_video_dxvk.cpp. Each object owns exactly one reference, taken
 * when it is created and dropped in the matching Destroy* entry. They are
 * stored as void * so this file never has to see a C++ D3D11 header. */
#define VIRTIO_WDDM_VIDEO_MAGIC_ENUM 0x56454e55u /* 'VENU' */
#define VIRTIO_WDDM_VIDEO_MAGIC_PROC 0x56505243u /* 'VPRC' */
#define VIRTIO_WDDM_VIDEO_MAGIC_VIEW 0x56564957u /* 'VVIW' */

typedef struct {
    UINT magic;
    D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC content;
    /* ID3D11VideoDevice * / ID3D11VideoContext * / ID3D11VideoProcessorEnumerator*.
     * The first two are held here so destroying the enumerator cannot pull the
     * interfaces out from under a processor that is still alive. */
    void *dxvk_device;
    void *dxvk_context;
    void *dxvk_enum;
} VIRTIO_WDDM_VideoProcessorEnum;

typedef struct {
    UINT magic;
    /* Enumeration this processor was created from. Kept for logging only; the
     * DXVK objects below carry their own references, so a processor outliving
     * the enumerator still works. */
    VIRTIO_WDDM_VideoProcessorEnum *enumeration;
    UINT rate_conversion_caps_index;
    /* ID3D11VideoProcessor * plus its own device/context references. */
    void *dxvk_device;
    void *dxvk_context;
    void *dxvk_processor;
} VIRTIO_WDDM_VideoProcessor;

typedef struct {
    UINT magic;
    D3D10DDI_HRESOURCE resource;
    UINT four_cc;
    UINT mip_slice;
    UINT first_array_slice;
    UINT array_size;
    void *dxvk_view; /* ID3D11VideoProcessorInputView * */
} VIRTIO_WDDM_VideoProcessorInputView;

typedef struct {
    UINT magic;
    D3D10DDI_HRESOURCE resource;
    UINT mip_slice;
    UINT first_array_slice;
    UINT array_size;
    void *dxvk_view; /* ID3D11VideoProcessorOutputView * */
} VIRTIO_WDDM_VideoProcessorOutputView;

/* ------------------------------------------------------------------------- */
/* Format support                                                            */
/* ------------------------------------------------------------------------- */

/* Only the formats the processor path really negotiates: the two multi-plane /
 * packed 4:2:2 YCbCr formats MF's Video Processor MFT feeds us, and the BGRA8
 * render target it converts to. Reporting more (P010, YV12, RGB input, ...)
 * would advertise conversions that were never validated.
 *
 * P-BATCH P010: P010 stays out of *this* list on purpose. It is a decoder
 * output format (see decoder_output_format_supported below), but DXVK's video
 * processor blit has no P010 input path (d3d11_video.cpp:IsYCbCrFormat only
 * knows NV12/YUY2/AYUV), so a P010 surface has to be sampled by the
 * application's own shader rather than by our VideoProcessorBlt. Claiming it
 * here would turn a correct "unsupported" answer into wrong colours. */
static BOOL video_input_format_supported(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_YUY2:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL video_output_format_supported(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return TRUE;
    default:
        return FALSE;
    }
}

/* P-BATCH P010: the formats a *decoder* may write into.
 *
 *   NV12  8-bit 4:2:0, 1 byte per sample, two planes (the P2-2b layout).
 *   P010  10-bit 4:2:0 in 16-bit words, MSB-aligned (bits 15:6 of each word,
 *         i.e. the Vulkan G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 that DXVK
 *         maps DXGI_FORMAT_P010 to). Luma rows first, then the interleaved UV
 *         rows starting at byte offset stride * height; both planes have the
 *         same row pitch in bytes, and the frame is stride * height * 3/2 bytes
 *         in both formats -- only the row width differs (width vs width * 2).
 *
 * The two are interchangeable as far as the DDI is concerned: whichever one the
 * application's decoder descriptor names is the one EndFrame writes, converting
 * the host frame when it does not already match (decoder_convert_420).
 * HEVC-MAIN10 adds one asymmetry: a 10-bit profile may only be paired with
 * P010 (see decoder_profile_format_supported), because NV12 would drop two bits
 * of every sample -- decoder_output_format_supported itself still answers for
 * the *format*, not for the pairing. */
static BOOL decoder_output_format_supported(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
        return TRUE;
    default:
        return FALSE;
    }
}

/* ------------------------------------------------------------------------- */
/* Decoder: H.264/NV12 caps, lifecycle and the actual decode (P2-2b)         */
/* ------------------------------------------------------------------------- */

/* One decoder object per ID3D11VideoDecoder. The runtime allocates the block
 * from CalcPrivateVideoDecoderSize; the session is the vmedia_client handle
 * (one CreateFileW + one OPEN ioctl) and is owned by this object alone.
 *
 * P2-2b adds the per-frame state: the output view of the open
 * BeginFrame/EndFrame pair, the Annex-B access unit assembled from the
 * application's BITSTREAM buffers, and one grow-only NV12 intermediate buffer
 * that vm_get_frame() fills before it is written into the output texture. Once
 * these two buffers reach their high-water mark a frame costs no allocation. */
#define VIRTIO_WDDM_VIDEO_MAGIC_DECODE 0x56444543u /* 'VDEC' */
#define VIRTIO_WDDM_VIDEO_MAGIC_DECOUT 0x56444F56u /* 'VDOV' */

typedef struct {
    UINT magic;
    D3D11_1DDI_VIDEO_DECODER_DESC desc;
    D3D11_1DDI_VIDEO_DECODER_CONFIG config;
    UINT codec; /* VIRTIO_WDDM_CODEC_H264 / _VP9: which decode path runs */
    /* HEVC-MAIN10: the sample depth of the decoder profile (8 or 10, from
     * VIRTIO_WDDM_DecoderProfile::bit_depth).  It travels no further than this
     * object: the output surface's DXGI_FORMAT and VM_FRAME_INFO::FourCC decide
     * the byte layout of every frame, while this field decides what the HEVC
     * path accepts (a 10-bit PicParams on the 8-bit Main profile is refused
     * instead of being decoded as a truncated picture). */
    UINT bit_depth;
    VM_SESSION *session;
    UINT session_id;

    /* Borrowed. The runtime destroys a video decoder before the device it was
     * created on, and every DDI call arrives with the same hDevice, so this
     * stays valid for the object's lifetime. Used for pDev1/pCtx1 and for the
     * runtime error callback in EndFrame (which is VOID-returning). */
    VIRTIO_WDDM_Device *device;

    D3D11_1DDI_HVIDEODECODEROUTPUTVIEW output_view;
    BOOL frame_open; /* BeginFrame seen, EndFrame not yet */
    BOOL frame_sent; /* EndFrame already vm_send()ed this frame's access unit */

    UINT8 *bitstream; /* Annex-B access unit of the open frame */
    UINT bitstream_size;
    UINT bitstream_cap;

    /* Last SPS+PPS pair seen in an access unit, Annex-B with 4-byte start
     * codes. The one-shot 1.0.0.7 driver means every frame after the first is
     * decoded by a fresh host session (decoder_reopen_session), which has no
     * parameter-set state, so later access units get these prepended when they
     * do not carry a set themselves. */
    UINT8 *params;
    UINT params_size;
    UINT params_cap;

    UINT8 *send; /* params + access unit: exactly what vm_send() is given */
    UINT send_cap;

    /* P2-2i: the last PICTURE_PARAMETERS buffer (DXVA_PicParams_H264) and the
     * Annex-B SPS+PPS synthesised from it. Chromium puts the parameter sets
     * here, never in the access unit, so EndFrame rebuilds them per frame. */
    DXVA_PicParams_H264 pic;
    BOOL pic_valid;
    H264ParamSetFields pic_fields;
    UINT8 *synth;
    UINT synth_cap;
    UINT synth_size;
    UINT synth_logs; /* first few synthesis lines are INFO, then VERBOSE */
    UINT64 pic_buffers; /* PICTURE_PARAMETERS buffers read (should be 1/frame) */
    BOOL qmatrix_seen;  /* logged once: scaling matrices are not forwarded */

    /* P2-2i: EndFrame is VOID; a dropped frame is remembered here and returned
     * by the next BeginFrame. Nothing is reported to pfnSetErrorCb, so the
     * D3D11 device is not removed (P2-2h: 0x887A0020 -> GPU process exit 34). */
    HRESULT pending_error;
    UINT64 decode_failures;
    UINT64 synth_injections; /* frames whose SPS/PPS came from PICTURE_PARAMETERS */

    UINT8 *nv12; /* decoded frame as vm_get_frame returns it (stride layout) */
    UINT nv12_cap;

    /* P-BATCH ZEROCOPY (driver 1.0.0.18): the frame currently mapped out of the
     * media pool instead of copied into `nv12`.  `direct_map.UserVa` points at
     * the decoded picture inside the pool's own pages (mapped MmNonCached into
     * this process); the capture buffer is checked out of the device until
     * vm_release_frame() and `direct_active` says so.  `direct_ok` is cleared
     * for good the first time the route fails, so a plain 1.0.0.17 driver costs
     * one failed ioctl per session, not one per frame. */
    VM_FRAME_MAP direct_map;
    BOOL direct_active;
    BOOL direct_ok;
    UINT64 direct_frames; /* frames uploaded straight out of the mapped pool */
    /* VP9FIX (2026-10-25): MAP_FRAME/RELEASE_FRAME pairing audit.  The driver
     * holds at most ONE mapped frame per session and refuses the next
     * MAP_FRAME until it is released ("previous frame was never released"), so
     * every successful map must be paired on every path -- success, failure and
     * a superframe member that is not the picture.  direct_maps - direct_releases
     * is the number of buffers currently checked out of the device; at
     * DestroyVideoDecoder it must be 0, and the summary says so. */
    UINT64 direct_maps;
    UINT64 direct_releases;
    UINT64 direct_release_failures;
    UINT64 direct_pairing_fixes;   /* stale mapping released before a remap */
    UINT64 direct_member_releases; /* superframe members released unconsumed */
    /* AV1FIX (2026-10-25): "a timeout is not a refusal", and "a failed frame
     * must still give the capture buffer back".
     *
     * The first is a defect this round found in the AV1 path: E173's ring
     * (p-batch-poolfix/guest-evidence/ring-av1.txt) shows the FIRST MAP_FRAME of
     * the session answering err=121 because the host had not produced a frame
     * yet, and the UMD then ran the *copy* path (direct=staging) for every
     * following frame.  One slow frame permanently cost the zero-copy route
     * because a timeout and a refusal were the same branch.  They are not: a
     * refusal means "this driver cannot map" and stays disabled, a timeout means
     * "no frame arrived" and must be retried on the next picture.
     *
     * The second is the queue rule this round was asked to make airtight: a
     * capture buffer that was mapped out of the device can only come back
     * through RELEASE_FRAME, so every failed frame acquisition now runs
     * decoder_release_stray_frame() (Index 0xFFFFFFFF = "whatever is mapped"). */
    UINT64 direct_timeouts;     /* MAP_FRAME answered ERROR_SEM_TIMEOUT (121) */
    UINT64 direct_refusals;     /* route disabled for good (pre-1.0.0.18 driver) */
    UINT64 direct_other_failures;
    UINT direct_fail_streak;    /* consecutive non-timeout MAP_FRAME failures */
    UINT64 stray_releases;      /* RELEASE_FRAME(0xFFFFFFFF) that DID requeue */
    UINT64 stray_release_empty; /* ... that found nothing mapped (normal case) */
    /* AV1FIX: where a frame's time went.  vm_get_frame() does the host wait and
     * the NV12 copy inside one ioctl, so the split only exists when the frame is
     * fetched with TimeoutMs = 0 polls (VM_GET_FRAME_IN documents 0 = poll once).
     * `frame_get_segmented` is armed for the rest of the session by the first
     * timeout -- exactly the state AV1 gets stuck in -- so a healthy session pays
     * nothing for this and a failing one reports wait/copy per frame. */
    BOOL frame_get_segmented;
    UINT64 frame_get_timeouts;
    double last_wait_ms;  /* host decode wait: time until a frame was available */
    double last_copy_ms;  /* NV12 copy out of the pool inside the last GET_FRAME */
    UINT last_get_polls;  /* poll-once calls the last frame needed */

    /* P-BATCH P010: the frame converted into the output surface's layout,
     * used when the host frame and the output texture disagree (NV12 host ->
     * P010 surface today; the reverse once media-wdm negotiates
     * V4L2_PIX_FMT_P010 and the application still asked for NV12).  Grow-only,
     * like nv12, so a steady-state frame costs no allocation. */
    UINT8 *conv;
    UINT conv_cap;

    UINT64 submits; /* SubmitDecoderBuffers calls */
    UINT64 frames;  /* frames decoded and written into an output texture */
    UINT64 bytes_sent;
    UINT64 reopens;          /* fresh vm sessions opened because the driver is one-shot */
    UINT64 param_set_injections; /* frames that needed the cached SPS/PPS prepended */

    /* P3-2 (driver 1.0.0.11): flush/EOS.  A seek, a loop restart or a
     * pause-then-resume hands us an IDR after a gap; a resolution change hands
     * us a different SPS/PPS or geometry.  The old host session still holds
     * reference frames and queued access units, so it is flushed
     * (VM_IOCTL_FLUSH) and the next SEND reconfigures a fresh session that
     * starts from that IDR. */
    UINT64 flushes;
    UINT64 aus_dropped;    /* sum of VM_FLUSH_OUT.DroppedAu */
    UINT64 frames_dropped; /* sum of VM_FLUSH_OUT.DroppedFrames */
    LONGLONG last_send_qpc; /* QPC at the previous EndFrame's successful send */
    UINT last_width;
    UINT last_height;

    /* P3-3: make the flush actually happen in real playback.
     * (a) DestroyVideoDecoder flushes before it closes: a player that seeks by
     *     destroying and recreating the decoder (PotPlayer does) would otherwise
     *     leave the old host session holding reference frames, queued access
     *     units and CAPTURE buffers.
     * (b) The frame_num carried in DXVA_PicParams_H264 is followed across access
     *     units, so a player that keeps the SAME decoder across a seek is caught
     *     too, without waiting for it to re-send an IDR.  POC is deliberately not
     *     used: B frames / reference reordering make it non-monotone in decode
     *     order, while frame_num obeys a simple rule (H.264 7.4.3). */
    UINT driver_version;       /* VM_OPEN_OUT.DriverVersion; flush needs >= 0x1000B */
    BOOL pic_this_frame;       /* PICTURE_PARAMETERS arrived for the open frame */
    BOOL prev_frame_num_valid; /* prev_frame_num describes the previous AU */
    UINT prev_frame_num;
    UINT prev_frame_num_max;   /* MaxFrameNum = 1 << (log2_max_frame_num_minus4 + 4) */
    UINT64 frame_num_advances; /* deltas that followed the H.264 rule (0 or 1) */
    UINT64 fn_flushes;         /* flushes triggered by the frame_num rule */
    BOOL fn_live_logged;       /* one INFO line when the field is proven live */

    /* P-BATCH VP9: the payload of a VP9 frame is the frame itself, but the
     * access unit still has to be rebuilt from the client's control buffers:
     *
     *  - vp9_pic is the last DXVA_PicParams_VP9 (PICTURE_PARAMETERS).  It is
     *    used for logging/validation and for the sequence-start decision; the
     *    geometry it carries is compared against the descriptor so a mismatch is
     *    visible in the log instead of as a GetFrame timeout.
     *  - slices holds the raw SLICE_CONTROL bytes (DXVA_Slice_VPx_Short
     *    records).  Chromium 155 does fill this buffer for VP9; the records are
     *    resolved against bitstream, which is why vp9_au exists at all.  When the
     *    client sends no records the whole BITSTREAM buffer is one frame.
     *  - vp9_au is the assembled access unit (slice records concatenated in
     *    record order), before the superframe split. */
    DXVA_PicParams_VP9 vp9_pic;
    BOOL vp9_pic_valid;
    BOOL vp9_pic_this_frame;
    UINT64 vp9_pic_buffers;
    UINT8 *slices;
    UINT slices_size;
    UINT slices_cap;
    UINT8 *vp9_au;
    UINT vp9_au_size;
    UINT vp9_au_cap;
    Vp9AccessUnit vp9_unit;
    UINT64 vp9_frames;      /* VP9 pictures sent+decoded (superframe members too) */
    UINT64 vp9_superframes; /* access units that carried more than one frame */
    UINT64 vp9_keyframes;   /* sequence starts seen */
    UINT64 vp9_bad_headers; /* frames whose header did not parse (sent anyway) */
    UINT vp9_header_logs;   /* first few headers are INFO, then VERBOSE */
    /* VP9FIX (2026-10-25): frames refused before a single byte was handed to the
     * host because a length/index/field was out of bounds (E_INVALIDARG), and
     * non-fatal oddities seen in DXVA_PicParams_VP9 (logged once each). */
    UINT64 vp9_refusals;
    UINT vp9_pic_warnings;

    /* P-BATCH HEVC: HEVC is the *other* codec whose parameter sets a D3D11
     * client keeps out of the bitstream, so it needs both halves of the VP9 and
     * H.264 machinery:
     *
     *  - hevc_pic is the last DXVA_PicParams_HEVC (PICTURE_PARAMETERS).  It is
     *    the only transcription of the stream's SPS/PPS the client gives us, so
     *    the synthesised VPS+SPS+PPS are built from it.
     *  - hevc_params caches the VPS+SPS+PPS of an access unit that carries them
     *    (ffmpeg/PotPlayer style).  Those bytes are exact, so they are preferred
     *    over synthesis and are also the fallback for streams whose SPS-level
     *    reference picture sets cannot be rebuilt.
     *  - hevc_fields is the plain-C field set hevc_params.h consumes, filled
     *    from hevc_pic once per frame.
     *  - hevc_au is the access unit with the parameter sets prepended, i.e. the
     *    bytes vm_send() gets (separate from object->send, which the H.264 path
     *    owns, because both codecs can be live in one process and the buffers
     *    have different high-water marks). */
    DXVA_PicParams_HEVC hevc_pic;
    BOOL hevc_pic_valid;
    BOOL hevc_pic_this_frame;
    UINT64 hevc_pic_buffers;
    HevcParamSetFields hevc_fields;
    UINT8 *hevc_params;
    UINT hevc_params_size;
    UINT hevc_params_cap;
    UINT8 *hevc_au;
    UINT hevc_au_size;
    UINT hevc_au_cap;
    UINT hevc_param_logs;      /* first few parameter-set decisions are INFO */
    UINT64 hevc_frames;        /* pictures sent to the host decoder */
    UINT64 hevc_irap;         /* IRAP pictures seen (sequence starts) */
    UINT64 hevc_synth_injections; /* frames whose VPS/SPS/PPS were synthesised */
    UINT64 hevc_inband_injections;/* frames whose parameter sets came from the AU */
    UINT64 hevc_synth_refusals;   /* frames synthesis declined (RPS in the SPS) */
    UINT64 hevc_slice_controls;   /* SLICE_CONTROL buffers the client submitted */

    /* P-BATCH AV1: AV1 is the one codec whose input is NOT a bitstream the
     * client copied out: it is the parsed DXVA_PicParams_AV1 plus the tile
     * payloads, and the temporal unit has to be rebuilt.  The state is
     *
     *  - av1_pic / av1_tile_data / av1_tiles: what SubmitDecoderBuffers copied
     *    out of the three buffers (PICTURE_PARAMETERS, BITSTREAM,
     *    SLICE_CONTROL), valid for the open frame only;
     *  - av1_seq: the sequence parameters (the sequence header is re-emitted
     *    on the first frame, on a key frame, and whenever these change);
     *  - av1_dpb: a mirror of the DECODER's DPB (slot -> order hint).  Its
     *    update is deferred one frame because the refresh mask is;
     *  - av1_frame / av1_au: the current picture's field set and assembled
     *    temporal unit;
     *  - av1_prev_*: the one-frame lookahead.  An inter frame is sent with
     *    refresh_frame_flags = 0 (the only non-destructive mask) because DXVA
     *    does not carry the real one; the next picture's
     *    RefFrameMapTextureIndex reveals it, and the picture is then re-sent
     *    as a HIDDEN frame (same tile payload, no output) spliced into the
     *    next temporal unit.  Key frames need no lookahead: a shown key frame
     *    refreshes all eight slots by definition. */
    DXVA_PicParams_AV1 av1_pic;
    BOOL av1_pic_valid;
    BOOL av1_pic_this_frame;
    UINT64 av1_pic_buffers;
    UINT64 av1_slice_controls;
    DXVA_Tile_AV1 *av1_tiles;
    UINT av1_tiles_size;
    UINT av1_tiles_cap;
    /* The tile payloads are the BITSTREAM buffer itself: object->bitstream /
     * bitstream_size already hold exactly what the client wrote, and BeginFrame
     * resets them per frame. */
    Av1SeqParams av1_seq;
    Av1SeqParams av1_seq_last; /* the sequence header last emitted */
    BOOL av1_seq_valid;
    BOOL av1_seq_emitted;
    Av1FrameParams av1_frame;
    Av1Dpb av1_dpb;
    UINT8 *av1_au;
    UINT av1_au_size;
    UINT av1_au_cap;
    Av1FrameParams av1_prev_frame;
    Av1SeqParams av1_prev_seq;
    UINT av1_prev_texture;
    BOOL av1_prev_pending;
    UINT8 *av1_prev_tile_group;
    UINT av1_prev_tile_group_size;
    UINT av1_prev_tile_group_cap;
    UINT64 av1_frames;
    UINT64 av1_key_frames;
    UINT64 av1_catchups;      /* hidden DPB catch-up frames re-sent */
    UINT64 av1_masks_derived; /* refresh masks recovered from a successor */
    UINT64 av1_bad_tile_records;

    /* PIPELINE (2026-10-25): the decode pipeline (send/fetch on a worker thread
     * while the DDI thread uploads the previous picture).  NULL means the serial
     * path -- no pipeline, or one refused by the environment, the codec (VP9) or
     * the driver version.  See the pipeline block for the whole story. */
    struct VIRTIO_WDDM_DecodePipeline *pipeline;

} VIRTIO_WDDM_VideoDecoder;

/* Output view: the runtime creates one per NV12/P010 surface the application
 * decodes into (ID3D11VideoDevice::CreateVideoDecoderOutputView). BeginFrame
 * selects one, EndFrame writes the frame into it.
 *
 * P2-2j: MipSlice / FirstArraySlice are NOT just log decoration. A player
 * allocates one NV12 texture with many array slices as its decode pool
 * (PotPlayer creates 22 slices and one output view per slice) and rotates
 * through them frame by frame, so the frame has to be written into the
 * subresource this view names. Writing subresource 0 instead left every slice
 * the renderer sampled at zero, and a zeroed NV12 surface converts to a flat
 * green picture (Y=0, U=V=0 -> RGB 0,135,0) -- the "画面绿" bug.
 *
 * P-BATCH P010: `format` records the texture's format when the view is created
 * (NV12 or P010). EndFrame re-reads it from the resource and converts the host
 * frame whenever the two disagree, so both formats go through the same
 * array-slice/subresource path. */
typedef struct {
    UINT magic;
    D3D10DDI_HRESOURCE resource;
    DXGI_FORMAT format;
    UINT mip_slice;
    UINT first_array_slice;
    UINT array_size;
} VIRTIO_WDDM_VideoDecoderOutputView;

/* P-BATCH ZEROCOPY: the helpers live next to the decode path (~line 2980) but
 * CreateVideoDecoder decides the mode and DestroyVideoDecoder has to release a
 * frame that was mapped and never given back, so both are declared here (after
 * the decoder typedef, which is what they operate on). */
static BOOL decoder_zerocopy_enabled(VIRTIO_WDDM_VideoDecoder *object);
static void decoder_drop_direct(VIRTIO_WDDM_VideoDecoder *object);
/* AV1FIX (2026-10-25): the "give the capture buffer back" safety net, used by
 * every failed frame acquisition (see its definition). */
static void decoder_release_stray_frame(VIRTIO_WDDM_VideoDecoder *object);

/* PIPELINE (2026-10-25): the decode pipeline lives in its own struct next to
 * the helpers it needs (~line 3300); the decoder object only carries a pointer,
 * NULL meaning "the serial path".  These four are used by code that is defined
 * before the pipeline block: CreateVideoDecoder arms it, DestroyVideoDecoder
 * stops it, decoder_flush_session quiesces it, and decoder_drop_direct tells it
 * that a capture buffer came back. */
static BOOL pipeline_create(VIRTIO_WDDM_VideoDecoder *object);
static void pipeline_destroy(VIRTIO_WDDM_VideoDecoder *object);
static void pipeline_quiesce(VIRTIO_WDDM_VideoDecoder *object, const char *reason);
static void pipeline_on_mapping_freed(VIRTIO_WDDM_VideoDecoder *object);
/* Every session call the DDI thread makes in pipeline mode goes through these
 * two, so the worker's vm_send/vm_map_frame and the DDI's vm_release_frame can
 * never overlap (the driver serialises IOCTLs, but the session state and the
 * VM_* error slot expect one caller at a time).  They are no-ops when the
 * pipeline is not armed. */
static void pipeline_session_lock(VIRTIO_WDDM_VideoDecoder *object);
static void pipeline_session_unlock(VIRTIO_WDDM_VideoDecoder *object);

/* The decode profiles this UMD advertises.
 *
 * H.264: the bytes are identical to the runtime-facing
 * D3D11_DECODER_PROFILE_H264_VLD_NOFGT (d3d11.h:10039) and to
 * D3D11_1DDI_DECODER_PROFILE_H264_VLD_NOFGT (d3d10umddi.h:3010) -- i.e.
 * DXVA_ModeH264_VLD_NoFGT.
 *
 * VP9: DXVA_ModeVP9_VLD_Profile0 (dxva.h:88) == D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0
 * (d3d11.h:2242) == D3D11_1DDI_DECODER_PROFILE_VP9_VLD_PROFILE0.  Profile 0 is
 * 8-bit 4:2:0, which is what the host c2.qti.vp9.decoder is fed by the Android
 * MediaCodec path (p0-probe 10-roundtrip-vp9.txt) and what an NV12 D3D11 surface
 * can hold.
 *
 * HEVC: DXVA_ModeHEVC_VLD_Main (dxva.h:75, WDK 10.0.28000 dxva.h:98) ==
 * D3D11_DECODER_PROFILE_HEVC_VLD_MAIN (d3d11.h:2240 and WDK d3d11.h:10058) ==
 * D3D11_1DDI_DECODER_PROFILE_HEVC_VLD_MAIN.  Main is 8-bit 4:2:0 -- the same
 * NV12 surface and the same c2.qti.hevc.decoder (P0 probe: hevc decode
 * 60/60 frames, PSNR 59.45 dB).
 *
 * HEVC-MAIN10 (2026-10-25): DXVA_ModeHEVC_VLD_Main10 (WDK dxva.h:99) ==
 * D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10 (WDK d3d11.h:10059) ==
 * {107af0e0-ef1a-4d19-ab-a8-67a163073d13}.  Main10 is 10-bit 4:2:0, so a decoder
 * created with it writes a P010 surface (8 bits per sample would truncate the
 * two LSBs of every sample). It is the profile a Main10 asset (t1.mp4, 4K HDR
 * PQ) needs to stop falling back to the software decoder; the host capture
 * format it asks media-wdm for is V4L2_PIX_FMT_P010 (see the P010 note above
 * and media-wdm's configure: P010, NV12 fallback with a log line).
 * Main 12 / 4:2:2 / 4:4:4 / 16-bit are deliberately not advertised: the SPS
 * synthesis only reproduces 8- and 10-bit 4:2:0, and neither the D3D11 surface
 * the runtime can hand us nor the host capture path carries them.
 *
 * All are kept as local constants because this translation unit does not
 * define INITGUID, so DEFINE_GUID() would only declare them. */
static const GUID virtio_wddm_decoder_profile_h264_vld_nofgt = {
    0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}
};

static const GUID virtio_wddm_decoder_profile_vp9_vld_profile0 = {
    0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}
};

static const GUID virtio_wddm_decoder_profile_hevc_vld_main = {
    0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}
};

/* HEVC-MAIN10: DXVA_ModeHEVC_VLD_Main10 {
 * 0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}
 * }. Same bytes in WDK 10.0.28000 dxva.h:99 and d3d11.h:10059; the UMD reports
 * it only when a P010 surface can really be allocated (decoder_p010_supported),
 * because Main10 must decode into one. */
static const GUID virtio_wddm_decoder_profile_hevc_vld_main10 = {
    0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}
};

/* P-BATCH AV1: DXVA_ModeAV1_VLD_Profile0 (dxva.h) ==
 * D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0 (d3d11.h).  Profile 0 is 8-bit 4:2:0
 * with no film-grain requirement on the output format, which is exactly what
 * the decode path writes.  Profile 1 (4:4:4) and 2 (10/12-bit) would need a
 * 10-bit *decode* (the host capture format is NV12, see the HEVC note above)
 * and are deliberately not advertised; the P010 output surface of P-BATCH P010
 * is a container for the same 8-bit picture. */
static const GUID virtio_wddm_decoder_profile_av1_vld_profile0 = {
    0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}
};

/* DXVA_NoEncrypt (dxva.h:129): the config's three encryption GUIDs are set to
 * this, not to an encryption protocol. This driver has no crypto session. */
static const GUID virtio_wddm_decoder_no_encrypt = {
    0x1b81bed0, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}
};

/* Codec ids, used for every per-codec decision in this file (descriptor check,
 * ConfigBitstreamRaw, decoder buffer types, the decode path). */
#define VIRTIO_WDDM_CODEC_H264 0u
#define VIRTIO_WDDM_CODEC_VP9 1u
#define VIRTIO_WDDM_CODEC_HEVC 2u
#define VIRTIO_WDDM_CODEC_AV1 3u

/* VP9FIX (2026-10-25): hard bounds for the VP9 decode path.
 *
 * The D3D11 client owns the BITSTREAM/SLICE_CONTROL buffers and the
 * DXVA_PicParams_VP9 it sends, so every number that reaches a pointer, a
 * memcpy() or vm_send() has to be checked against the buffer it came from
 * before it is used.  These are the caps this file enforces; a value outside
 * them fails the frame with E_INVALIDARG (recorded by decoder_fail_frame and
 * returned by the next DecoderBeginFrame) instead of being clamped or
 * truncated, because a truncated access unit decodes to garbage and a size
 * taken from garbage becomes an out-of-bounds read.
 *
 *  - one access unit: 64 MiB.  A 4K 8-bit 4:2:0 intra frame is ~12 MB, so
 *    only a corrupt buffer chain can exceed this.
 *  - SLICE_CONTROL: exactly the 16 KiB GetVideoDecoderBufferInfo reports for
 *    VP9 (d3d11_video.c:virtio_wddm_decoder_buffer_types_vp9), i.e. 1638
 *    DXVA_Slice_VPx_Short records.  A larger DataSize is a client bug, and the
 *    old code silently truncated it (which split records).
 *  - tile columns/rows: VP9 6.2.10 codes log2_tile_cols in 6 bits (max 6, 64
 *    columns) and log2_tile_rows in 2 bits (max 2, 4 rows).  DXVA_PicParams_VP9
 *    carries both fields; the UMD does not parse the compressed header, so this
 *    is the only place the tile geometry can be bounded. */
#define VIRTIO_WDDM_MAX_ACCESS_UNIT_BYTES (64u * 1024u * 1024u)
#define VIRTIO_WDDM_VP9_SLICE_CONTROL_BYTES (16u * 1024u)
#define VIRTIO_WDDM_VP9_MAX_SLICE_RECORDS \
    (VIRTIO_WDDM_VP9_SLICE_CONTROL_BYTES / (UINT)sizeof(DXVA_Slice_VPx_Short))
#define VIRTIO_WDDM_VP9_MAX_TILE_COLS_LOG2 6u
#define VIRTIO_WDDM_VP9_MAX_TILE_ROWS_LOG2 2u

/* P-BATCH P010 / HEVC-MAIN10: VM_FRAME_INFO::FourCC values, i.e. V4L2 fourccs
 * (v4l2_fourcc('N','V','1','2') etc.).  NV12 is what media-wdm returns whenever
 * the CAPTURE queue could not be negotiated as P010 (its S_FMT asks
 * V4L2_PIX_FMT_P010 for a Main10 session since 1.0.0.21 and falls back to NV12
 * with a log line); P010 is what the same driver reports when the host took the
 * 10-bit format, and the decode path accepts either one, so the UMD side needs
 * no further change when the driver does. Both are defined here rather than in
 * vmedia_abi.h because that header is generated from the driver ABI
 * (tools/gen_abi.py) and these are V4L2 constants. */
#define VIRTIO_WDDM_FOURCC_NV12 0x3231564Eu /* 'NV12' */
#define VIRTIO_WDDM_FOURCC_P010 0x30313050u /* 'P010' */

/* One row per supported profile.
 *
 * bitstream_raw is D3D11_1DDI_VIDEO_DECODER_CONFIG::ConfigBitstreamRaw and is a
 * *client contract*, not a decode knob: the client decides from it whether it
 * fills DXVA_Slice_H264_Short (2) or the short VPx slice records (1).  The
 * runtime-facing rule (Chromium 155 media/gpu/windows/d3d11_video_decoder.cc,
 * see P2-2G) is 2 for H.264 and 1 for VP9/AV1/HEVC.
 *
 * open_codec is the VM_OPEN_IN.Codec codec selector of the virtio-media ABI
 * (vmedia_abi.h).  The host decoder is chosen by the V4L2 OUTPUT pixel format
 * the driver sets at configure time, so the guest has to say which codec the
 * session will carry once, at OPEN.
 *
 * bit_depth is the sample depth the profile decodes to: 8 for everything up to
 * and including AV1 Profile 0, 10 for HEVC Main10 (HEVC-MAIN10).  It decides
 * two things: which output formats the profile may be paired with (a 10-bit
 * profile needs a P010 surface -- decoder_profile_format_supported) and which
 * host capture format the driver is asked for (open_codec = HEVC_MAIN10 makes
 * media-wdm try V4L2_PIX_FMT_P010).
 *
 * HEVC Main10 is the only row that is not always advertised: it is appended
 * last and reported only when this host can allocate a P010 texture
 * (decoder_p010_supported), so a host without one keeps exactly the four
 * profiles it had before HEVC-MAIN10. */
typedef struct {
    const GUID *profile;
    const char *name;
    UINT codec;
    UINT bitstream_raw;
    UINT open_codec;
    UINT bit_depth;
} VIRTIO_WDDM_DecoderProfile;

static const VIRTIO_WDDM_DecoderProfile virtio_wddm_decoder_profiles[] = {
    {&virtio_wddm_decoder_profile_h264_vld_nofgt, "H.264 VLD NoFGT",
     VIRTIO_WDDM_CODEC_H264, 2u, VM_OPEN_CODEC_H264, 8u},
    {&virtio_wddm_decoder_profile_vp9_vld_profile0, "VP9 VLD Profile0",
     VIRTIO_WDDM_CODEC_VP9, 1u, VM_OPEN_CODEC_VP9, 8u},
    {&virtio_wddm_decoder_profile_hevc_vld_main, "HEVC VLD Main",
     VIRTIO_WDDM_CODEC_HEVC, 1u, VM_OPEN_CODEC_HEVC, 8u},
    /* P-BATCH AV1: ConfigBitstreamRaw = 1, like VP9/HEVC (the Chromium rule
     * quoted above); the client then submits DXVA_Tile_AV1 records in
     * SLICE_CONTROL and the raw tile payloads in BITSTREAM. */
    {&virtio_wddm_decoder_profile_av1_vld_profile0, "AV1 VLD Profile0",
     VIRTIO_WDDM_CODEC_AV1, 1u, VM_OPEN_CODEC_AV1, 8u},
    /* HEVC-MAIN10: last on purpose.  Index 4 exists only while the device can
     * hold a P010 surface, so indices 0..3 keep the pre-HEVC-MAIN10 meaning and
     * GetVideoDecoderProfile(index) stays stable for every existing caller. */
    {&virtio_wddm_decoder_profile_hevc_vld_main10, "HEVC VLD Main10",
     VIRTIO_WDDM_CODEC_HEVC, 1u, VM_OPEN_CODEC_HEVC_MAIN10, 10u},
};

#define VIRTIO_WDDM_DECODER_PROFILE_COUNT \
    (sizeof(virtio_wddm_decoder_profiles) / sizeof(virtio_wddm_decoder_profiles[0]))

/* The rows 0..3 that every device reports, and the index of the one row that is
 * conditional (HEVC Main10). */
#define VIRTIO_WDDM_DECODER_PROFILE_COUNT_BASE 4u
#define VIRTIO_WDDM_DECODER_PROFILE_MAIN10_INDEX 4u

static const VIRTIO_WDDM_DecoderProfile *decoder_profile_from_guid(const GUID *guid) {
    UINT i;
    if (!guid)
        return NULL;
    for (i = 0; i < VIRTIO_WDDM_DECODER_PROFILE_COUNT; i++) {
        if (IsEqualGUID(guid, virtio_wddm_decoder_profiles[i].profile))
            return &virtio_wddm_decoder_profiles[i];
    }
    return NULL;
}

/* The profile for a decoder descriptor, or NULL when this UMD cannot decode it
 * (unknown profile, or an output format the decode path cannot write). Both
 * NV12 and P010 descriptors are accepted here; whether the *pair* is legal
 * (Main10 requires P010) and available on this device is decided by
 * decoder_profile_format_supported(), which every caps entry point calls. */
static const VIRTIO_WDDM_DecoderProfile *decoder_profile_from_desc(
    CONST D3D11_1DDI_VIDEO_DECODER_DESC *desc) {
    if (!desc || !decoder_output_format_supported(desc->OutputFormat))
        return NULL;
    return decoder_profile_from_guid(&desc->Guid);
}

static const char *decoder_codec_name(UINT codec) {
    switch (codec) {
    case VIRTIO_WDDM_CODEC_VP9:
        return "VP9";
    case VIRTIO_WDDM_CODEC_HEVC:
        return "HEVC";
    case VIRTIO_WDDM_CODEC_AV1:
        return "AV1";
    default:
        return "H.264";
    }
}

/* HEVC-MAIN10: can this device report `profile` at all?  Every profile except
 * Main10 always can; Main10 needs a P010 surface to decode into.  Defined after
 * decoder_p010_supported() below and forward declared here because the
 * device-level profile count/profile callbacks come first in this file. */
static BOOL decoder_profile_advertised(VIRTIO_WDDM_Device *device,
                                       const VIRTIO_WDDM_DecoderProfile *profile);

static VOID APIENTRY virtio_wddm_video_get_video_decoder_profile_count(
    D3D10DDI_HDEVICE hDevice, UINT *pCount) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    UINT count;

    VIRTIO_WDDM_DDI_ENTRY("hDevice=%p pCount=%p", (void *)hDevice.pDrvPrivate,
                          (void *)pCount);
    if (!pCount)
        return;
    count = VIRTIO_WDDM_DECODER_PROFILE_COUNT_BASE;
    if (decoder_profile_advertised(device,
                                   &virtio_wddm_decoder_profiles
                                        [VIRTIO_WDDM_DECODER_PROFILE_MAIN10_INDEX]))
        count = VIRTIO_WDDM_DECODER_PROFILE_MAIN10_INDEX + 1u;
    *pCount = count;
    VERBOSE("%s: count=%u (H.264 VLD NoFGT, VP9 VLD Profile0, HEVC VLD Main, "
            "AV1 VLD Profile0%s)", __FUNCTION__, *pCount,
            count > VIRTIO_WDDM_DECODER_PROFILE_COUNT_BASE ? ", HEVC VLD Main10" : "");
}

static VOID APIENTRY virtio_wddm_video_get_video_decoder_profile(
    D3D10DDI_HDEVICE hDevice, UINT index, GUID *pProfile) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    UINT count = VIRTIO_WDDM_DECODER_PROFILE_COUNT_BASE;

    VIRTIO_WDDM_DDI_ENTRY("index=%u pProfile=%p", index, (void *)pProfile);
    if (!pProfile)
        return;
    if (decoder_profile_advertised(device,
                                   &virtio_wddm_decoder_profiles
                                        [VIRTIO_WDDM_DECODER_PROFILE_MAIN10_INDEX]))
        count = VIRTIO_WDDM_DECODER_PROFILE_MAIN10_INDEX + 1u;
    if (index >= count) {
        ERROR("%s: index=%u out of range (count=%u)", __FUNCTION__, index, count);
        memset(pProfile, 0, sizeof(*pProfile));
        return;
    }
    *pProfile = *virtio_wddm_decoder_profiles[index].profile;
}

/* P-BATCH P010: can the internal DXVK device create a P010 texture at all?
 *
 * Advertising a 10-bit decode output format is only useful if the application
 * can create the surface it decodes into, so the answer comes from the device
 * that will hold it: DXVK maps DXGI_FORMAT_P010 to
 * VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 and reports
 * D3D11_FORMAT_SUPPORT_TEXTURE2D only when the Vulkan device does
 * (dxvk_format.cpp:816 and D3D11Device::CheckFormatSupport). On a host without
 * that format the DDI answers "not supported" and every client keeps NV12 --
 * i.e. the pre-P010 behaviour -- instead of failing at CreateTexture2D.
 *
 * Called from CheckVideoDecoderFormat, which the runtime asks once per
 * (profile, format) while the application enumerates its decode configurations;
 * the check itself is a CPU-side format lookup, no GPU work and no allocation.
 *
 * HEVC-MAIN10 rides on exactly this answer: the Main10 profile is reported only
 * while this function returns TRUE, so a host without the format keeps the four
 * pre-Main10 profiles and no 10-bit decode is ever offered. */
static BOOL decoder_p010_supported(VIRTIO_WDDM_Device *device) {
    BOOL supported = FALSE;
    if (!device || !device->base.pDev1)
        return FALSE;
    if (FAILED(virtio_wddm_dxvk_video_format_supported(device->base.pDev1,
                                                      (UINT)DXGI_FORMAT_P010,
                                                      &supported)))
        return FALSE;
    return supported;
}

/* P-BATCH P010: can this device offer `format` as a decoder output? NV12 always;
 * P010 only when the surface can really be allocated (decoder_p010_supported).
 * Every entry that hands a format back to the runtime goes through this, so
 * CheckVideoDecoderFormat, GetVideoDecoderConfigCount/Config and
 * CreateVideoDecoder cannot disagree. */
static BOOL decoder_output_format_available(VIRTIO_WDDM_Device *device, DXGI_FORMAT format) {
    if (!decoder_output_format_supported(format))
        return FALSE;
    if (format == DXGI_FORMAT_P010)
        return decoder_p010_supported(device);
    return TRUE;
}

/* HEVC-MAIN10: can this device advertise `profile` at all?  Every profile with
 * an 8-bit picture always can; Main10 decodes 10-bit samples and therefore
 * needs the P010 texture this device can or cannot create. */
static BOOL decoder_profile_advertised(VIRTIO_WDDM_Device *device,
                                       const VIRTIO_WDDM_DecoderProfile *profile) {
    if (!profile)
        return FALSE;
    if (profile->bit_depth >= 10u)
        return decoder_p010_supported(device);
    return TRUE;
}

/* HEVC-MAIN10: may `profile` write into `format` on this device?
 *
 * This is the single pairing rule the four caps entry points share:
 *   - the format must be one the decode path can write and this device can
 *     allocate (decoder_output_format_available);
 *   - an 8-bit profile keeps the P-BATCH P010 behaviour, i.e. NV12 and (when
 *     available) P010 are both fine -- the 8-bit picture is MSB-aligned into the
 *     10-bit container;
 *   - a 10-bit profile (Main10) is P010 only.  An NV12 surface would drop the
 *     two LSBs of every sample, and answering "supported" for it would let an
 *     application configure a decode that silently loses precision. */
static BOOL decoder_profile_format_supported(VIRTIO_WDDM_Device *device,
                                             const VIRTIO_WDDM_DecoderProfile *profile,
                                             DXGI_FORMAT format) {
    if (!decoder_profile_advertised(device, profile))
        return FALSE;
    if (!decoder_output_format_available(device, format))
        return FALSE;
    if (profile->bit_depth >= 10u)
        return format == DXGI_FORMAT_P010;
    return TRUE;
}

static VOID APIENTRY virtio_wddm_video_check_video_decoder_format(
    D3D10DDI_HDEVICE hDevice, CONST GUID *pDecodeProfile, DXGI_FORMAT format,
    BOOL *pSupported) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    const VIRTIO_WDDM_DecoderProfile *profile;
    BOOL supported;

    VIRTIO_WDDM_DDI_ENTRY("profile=%p format=%u pSupported=%p",
                          (const void *)pDecodeProfile, (unsigned)format,
                          (void *)pSupported);
    if (!pSupported)
        return;
    profile = decoder_profile_from_guid(pDecodeProfile);
    supported = profile && decoder_profile_format_supported(device, profile, format);
    *pSupported = supported;
    VERBOSE("%s: %s format=%u supported=%d (bit_depth=%u p010=%d)", __FUNCTION__,
            profile ? profile->name : "unknown profile", (unsigned)format,
            (int)*pSupported, profile ? profile->bit_depth : 0u,
            (int)decoder_p010_supported(device));
}

static VOID APIENTRY virtio_wddm_video_get_video_decoder_config_count(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDI_VIDEO_DECODER_DESC *pDesc,
    UINT *pCount) {
    const VIRTIO_WDDM_DecoderProfile *profile;

    VIRTIO_WDDM_DDI_ENTRY("desc=%p (%ux%u fmt=%u) pCount=%p", (const void *)pDesc,
                          pDesc ? pDesc->SampleWidth : 0,
                          pDesc ? pDesc->SampleHeight : 0,
                          pDesc ? (unsigned)pDesc->OutputFormat : 0, (void *)pCount);
    if (!pCount)
        return;
    profile = decoder_profile_from_desc(pDesc);
    *pCount = profile && decoder_profile_format_supported(
                             hDevice.pDrvPrivate, profile,
                             (DXGI_FORMAT)pDesc->OutputFormat)
                  ? 1
                  : 0;
    VERBOSE("%s: %s fmt=%u -> %u config(s)", __FUNCTION__,
            profile ? profile->name : "unsupported profile",
            pDesc ? (unsigned)pDesc->OutputFormat : 0, *pCount);
}

/* The decoder config returned to the runtime.  ConfigBitstreamRaw is a
 * client-contract flag, not a decode knob:
 *
 *   H.264 -> 2: Chromium's D3D11 decoder wrapper accepts a config only when
 *   ConfigBitstreamRaw == 2 && codec == H.264 (media/gpu/windows/
 *   d3d11_video_decoder_wrapper.cc in tag 155.0.8059.85, :341), where 2 means
 *   "the decoder uses DXVA_Slice_H264_Short"; ==1 is accepted only for
 *   VP9/AV1/HEVC.  With ==1 the wrapper was never created and Chromium reported
 *   D3D11Status::10 (kDecoderCreationFailed) before CreateVideoDecoder was ever
 *   dispatched (E135 / P2-2g).
 *   VP9 -> 1: the same rule read the other way.  1 tells the client that
 *   SLICE_CONTROL carries the short VPx slice records (DXVA_Slice_VPx_Short)
 *   that DecoderEndFrame resolves against the BITSTREAM buffer.
 *   HEVC -> 1: also the "short" contract.  1 means the client submits the slice
 *   segments in BITSTREAM and the parsed picture values in DXVA_PicParams_HEVC
 *   (PICTURE_PARAMETERS); with 1 no parameter sets are expected in the
 *   BITSTREAM buffer, which is why the UMD rebuilds VPS+SPS+PPS itself.
 *
 * The client then appends each slice/frame to the BITSTREAM buffer, while
 * vm_send() still hands the assembled access unit -- the bytes the host decoder
 * consumes -- straight to the driver.  The other decoder buffers exist so the
 * runtime's CreateResource/GetDecoderBuffer calls succeed and are ignored on
 * submit.  No FMO/ASO, no encryption. */
static VOID APIENTRY virtio_wddm_video_get_video_decoder_config(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDI_VIDEO_DECODER_DESC *pDesc,
    UINT index, D3D11_1DDI_VIDEO_DECODER_CONFIG *pConfig) {
    const VIRTIO_WDDM_DecoderProfile *profile;

    VIRTIO_WDDM_DDI_ENTRY("desc=%p index=%u pConfig=%p", (const void *)pDesc, index,
                          (void *)pConfig);
    if (!pConfig)
        return;
    memset(pConfig, 0, sizeof(*pConfig));
    profile = decoder_profile_from_desc(pDesc);
    if (!profile || index != 0 ||
        !decoder_profile_format_supported(hDevice.pDrvPrivate, profile,
                                          (DXGI_FORMAT)pDesc->OutputFormat)) {
        ERROR("%s: no config for profile=%08lx-%04x-%04x format=%u index=%u",
              __FUNCTION__, pDesc ? (unsigned long)pDesc->Guid.Data1 : 0,
              pDesc ? pDesc->Guid.Data2 : 0, pDesc ? pDesc->Guid.Data3 : 0,
              pDesc ? (unsigned)pDesc->OutputFormat : 0, index);
        return;
    }
    pConfig->guidConfigBitstreamEncryption = virtio_wddm_decoder_no_encrypt;
    pConfig->guidConfigMBcontrolEncryption = virtio_wddm_decoder_no_encrypt;
    pConfig->guidConfigResidDiffEncryption = virtio_wddm_decoder_no_encrypt;
    pConfig->ConfigBitstreamRaw = profile->bitstream_raw;
    /* Every other *Config* member stays 0: host-side decode takes the raw
     * bitstream, so none of the DXVA control-buffer knobs apply. */
    INFO("%s: %s config: ConfigBitstreamRaw=%u (no encryption, no residual diff; "
         "%u-bit profile, output %s)", __FUNCTION__, profile->name,
         pConfig->ConfigBitstreamRaw, profile->bit_depth,
         (DXGI_FORMAT)pDesc->OutputFormat == DXGI_FORMAT_P010 ? "P010" : "NV12");
}

/* Driver-managed decoder buffers.
 *
 * D3D11 does not let the application point at its own compressed resource:
 * ID3D11VideoContext::GetDecoderBuffer(type) asks the *driver* for a buffer of
 * that type, the application writes the access unit into it and calls
 * ReleaseDecoderBuffer, and SubmitDecoderBuffers then names the buffer only by
 * BufferType. The runtime allocates one resource per type listed here, passes
 * it through CreateResource with DecoderBufferType set, and hands the resource
 * handle back to SubmitDecoderBuffers (d3d10umddi.h:
 * D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC::hResource).
 *
 * P2-2a reported zero types because Begin/Submit were stubs.  The set now
 * follows the codec: H.264 VLD NoFGT needs the four types an H.264 client asks
 * for (MF, Chrome, FFmpeg's dxva2 path all follow this sequence); VP9 VLD
 * Profile0 needs PICTURE_PARAMETERS (DXVA_PicParams_VP9), SLICE_CONTROL
 * (DXVA_Slice_VPx_Short records) and BITSTREAM -- VP9 has no scaling lists, so
 * the inverse-quantisation buffer is not reported for it.  Only BITSTREAM is
 * decoded; the others exist so the client's GetDecoderBuffer calls succeed, and
 * Submit ignores them.
 *
 * Sizes are upper bounds, allocated once per decoder:
 *   PICTURE_PARAMETERS 8 KiB (DXVA_PicParams_H264 ~600 B, _VP9 ~1.4 KiB)
 *   INVERSE_QUANTIZATION_MATRIX 4 KiB (DXVA_Qmatrix_H264 is 336 B)
 *   SLICE_CONTROL H.264 128 KiB (1024 x DXVA_Slice_H264_Long, 96 B each)
 *                 VP9 16 KiB (1600 x DXVA_Slice_VPx_Short, 10 B each)
 *                 HEVC 128 KiB (DXVA_Slice_HEVC_Short, 12 B each)
 *   BITSTREAM max(1 MiB, w*h): one whole access unit, sized off the coded size */
typedef struct {
    UINT type;
    UINT size;
} VIRTIO_WDDM_DecoderBufferType;

static const VIRTIO_WDDM_DecoderBufferType virtio_wddm_decoder_buffer_types_h264[] = {
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 8u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 4u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 128u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM, 0u },
};

static const VIRTIO_WDDM_DecoderBufferType virtio_wddm_decoder_buffer_types_vp9[] = {
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 8u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 16u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM, 0u },
};

/* HEVC: the same four types an H.264 client asks for, with the HEVC structures.
 *
 * sizeof(DXVA_PicParams_HEVC) is 232 B in both headers, which is the point: the
 * WDK 10.0.28000 dxva.h (what a D3D11 client compiles with) and the mingw-w64
 * one this build uses lay the struct out identically (no internal padding), so
 * copying the client's buffer into our struct is field-for-field correct.  The
 * assert below pins it; tools/recon/wdk-hevc-struct.sh measures the WDK header
 * verbatim and prints the same number.
 *
 * DXVA_Qmatrix_HEVC is 1000 B (fits the 4 KiB quantisation buffer).
 * DXVA_Slice_HEVC_Short is 12 B in the Windows SDK but *packed to 10* by
 * mingw-w64's dxva.h, so sizeof() must not be used for record arithmetic; the
 * client's records are 12 B and this UMD only counts them, never indexes with
 * that size (VIRTIO_WDDM_HEVC_SLICE_SHORT_SIZE). */
#define VIRTIO_WDDM_HEVC_SLICE_SHORT_SIZE 12u
typedef char hevc_picparams_size_check[(sizeof(DXVA_PicParams_HEVC) == 232) ? 1 : -1];
static const VIRTIO_WDDM_DecoderBufferType virtio_wddm_decoder_buffer_types_hevc[] = {
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 8u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 4u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 128u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM, 0u },
};

/* P-BATCH AV1: PICTURE_PARAMETERS carries DXVA_PicParams_AV1, SLICE_CONTROL the
 * DXVA_Tile_AV1 records, BITSTREAM the tile payloads.  No inverse-quantisation
 * buffer (AV1's qmatrix is part of the frame header) and no film-grain buffer
 * (DXVA_PicParams_AV1 carries the film grain parameters itself, and Chromium's
 * AV1 accelerator submits only the three buffers above).
 *
 * The two structure sizes are ABI-critical: the UMD memcpy()s the client's
 * buffer into its own struct, so a padding difference silently corrupts every
 * field.  tools/recon/dxva-av1-struct.sh measures the WDK 10.0.28000 header
 * (912 / 36 / 16 bytes, same field offsets) and these asserts pin the header
 * the UMD is compiled with to those numbers. */
typedef char av1_picparams_size_check[(sizeof(DXVA_PicParams_AV1) == 912) ? 1 : -1];
typedef char av1_picentry_size_check[(sizeof(DXVA_PicEntry_AV1) == 36) ? 1 : -1];
typedef char av1_tile_size_check[(sizeof(DXVA_Tile_AV1) == 16) ? 1 : -1];
static const VIRTIO_WDDM_DecoderBufferType virtio_wddm_decoder_buffer_types_av1[] = {
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 8u * 1024u },
    /* 512 tiles (the AV1 conformance limit) x 16 B records = 8 KiB; 16 KiB
     * leaves the same headroom the VP9 table has. */
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 16u * 1024u },
    { D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM, 0u },
};


#define VIRTIO_WDDM_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* The buffer-type table of a codec, with its length in *count. */
static const VIRTIO_WDDM_DecoderBufferType *decoder_buffer_types(UINT codec,
                                                               UINT *count) {
    if (codec == VIRTIO_WDDM_CODEC_VP9) {
        *count = (UINT)VIRTIO_WDDM_ARRAY_COUNT(virtio_wddm_decoder_buffer_types_vp9);
        return virtio_wddm_decoder_buffer_types_vp9;
    }
    if (codec == VIRTIO_WDDM_CODEC_HEVC) {
        *count = (UINT)VIRTIO_WDDM_ARRAY_COUNT(virtio_wddm_decoder_buffer_types_hevc);
        return virtio_wddm_decoder_buffer_types_hevc;
    }
    if (codec == VIRTIO_WDDM_CODEC_AV1) {
        *count = (UINT)VIRTIO_WDDM_ARRAY_COUNT(virtio_wddm_decoder_buffer_types_av1);
        return virtio_wddm_decoder_buffer_types_av1;
    }
    *count = (UINT)VIRTIO_WDDM_ARRAY_COUNT(virtio_wddm_decoder_buffer_types_h264);
    return virtio_wddm_decoder_buffer_types_h264;
}

/* One complete access unit of a 4K H.264 stream fits in w*h; below 1080p the
 * 1 MiB floor keeps small surfaces from getting a too-small bitstream buffer. */
static UINT decoder_bitstream_buffer_size(UINT width, UINT height) {
    UINT size = width * height;
    if (size < (1u << 20))
        size = 1u << 20;
    return size;
}

static VOID APIENTRY virtio_wddm_video_get_video_decoder_buffer_type_count(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDI_VIDEO_DECODER_DESC *pDesc,
    UINT *pCount) {
    const VIRTIO_WDDM_DecoderProfile *profile;
    UINT count = 0;
    if (!pCount)
        return;
    profile = decoder_profile_from_desc(pDesc);
    /* HEVC-MAIN10: the same profile/format pairing as CheckVideoDecoderFormat,
     * so a Main10 + NV12 descriptor cannot report buffer types it has no config
     * for. */
    if (profile &&
        decoder_profile_format_supported(hDevice.pDrvPrivate, profile,
                                         (DXGI_FORMAT)pDesc->OutputFormat)) {
        UINT n = 0;
        (void)decoder_buffer_types(profile->codec, &n);
        count = n;
    }
    *pCount = count;
    VERBOSE("%s: count=%u (driver-managed decode buffers, %s)", __FUNCTION__, *pCount,
            profile ? profile->name : "unsupported profile");
}

static VOID APIENTRY virtio_wddm_video_get_video_decoder_buffer_info(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDI_VIDEO_DECODER_DESC *pDesc,
    UINT index, D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO *pInfo) {
    const VIRTIO_WDDM_DecoderProfile *profile;
    const VIRTIO_WDDM_DecoderBufferType *types;
    UINT count = 0;
    if (!pInfo)
        return;
    memset(pInfo, 0, sizeof(*pInfo));
    profile = decoder_profile_from_desc(pDesc);
    if (!profile ||
        !decoder_profile_format_supported(hDevice.pDrvPrivate, profile,
                                          (DXGI_FORMAT)pDesc->OutputFormat))
        return;
    types = decoder_buffer_types(profile->codec, &count);
    if (index >= count)
        return;
    pInfo->Type = types[index].type;
    pInfo->Size = types[index].size ? types[index].size
                                    : decoder_bitstream_buffer_size(pDesc->SampleWidth,
                                                                    pDesc->SampleHeight);
    /* d3d11.dll's CDecodeContext::FinalConstruct validates this field before it
     * creates the decoder buffers and calls pfnCreateVideoDecoder: any Usage
     * outside {2,3} throws E_UNEXPECTED (0x8000FFFF) and the create DDI is
     * never dispatched.  WDK's D3D11_1DDI_VIDEO_USAGE_PLAYBACK_NORMAL is 0 and
     * is rejected there; 2 is the value the runtime's CDecodeBuffer::Init
     * treats as the normal case.  Verified in d3d11.dll 10.0.29671 (ARM64) and
     * 10.0.26100 (x64). */
    pInfo->Usage = 2;
    VERBOSE("%s: [%u] type=%u size=%u", __FUNCTION__, index, (unsigned)pInfo->Type,
            pInfo->Size);
}

static SIZE_T APIENTRY virtio_wddm_video_calc_private_video_decoder_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEODECODER *pArgs) {
    VIRTIO_WDDM_DDI_ENTRY("hDevice=%p pArgs=%p -> %zu bytes",
                          (void *)hDevice.pDrvPrivate, (const void *)pArgs,
                          sizeof(VIRTIO_WDDM_VideoDecoder));
    (void)hDevice;
    (void)pArgs;
    return sizeof(VIRTIO_WDDM_VideoDecoder);
}

static HRESULT APIENTRY virtio_wddm_video_create_video_decoder(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDIARG_CREATEVIDEODECODER *pArgs,
    D3D11_1DDI_HDECODE hDecode, D3D11_1DDI_HRTDECODE hRTDecode) {
    VIRTIO_WDDM_DDI_ENTRY("hDevice=%p pArgs=%p hDecode=%p hRTDecode=%p",
                          (void *)hDevice.pDrvPrivate, (const void *)pArgs,
                          (void *)hDecode.pDrvPrivate, (void *)hRTDecode.handle);
    if (pArgs) {
        VIRTIO_WDDM_DDI_ENTRY("desc=%08lx-%04x-%04x %ux%u fmt=%u bitstream_raw=%u",
                              (unsigned long)pArgs->Desc.Guid.Data1,
                              pArgs->Desc.Guid.Data2, pArgs->Desc.Guid.Data3,
                              pArgs->Desc.SampleWidth, pArgs->Desc.SampleHeight,
                              (unsigned)pArgs->Desc.OutputFormat,
                              pArgs->Config.ConfigBitstreamRaw);
    }
    (void)hRTDecode;

    VIRTIO_WDDM_VideoDecoder *object = hDecode.pDrvPrivate;
    const VIRTIO_WDDM_DecoderProfile *profile;
    if (!object || !pArgs)
        return E_INVALIDARG;
    memset(object, 0, sizeof(*object));

    profile = decoder_profile_from_desc(&pArgs->Desc);
    if (!profile) {
        ERROR("%s: unsupported decoder profile=%08lx-%04x-%04x format=%u -> E_INVALIDARG",
              __FUNCTION__, (unsigned long)pArgs->Desc.Guid.Data1,
              pArgs->Desc.Guid.Data2, pArgs->Desc.Guid.Data3,
              (unsigned)pArgs->Desc.OutputFormat);
        return E_INVALIDARG;
    }
    /* P-BATCH P010 / HEVC-MAIN10: the same profile/format pairing the caps
     * answered -- a 10-bit decoder is only created into a P010 surface this
     * host can hold, so an application that honoured CheckVideoDecoderFormat
     * never reaches this. */
    if (!decoder_profile_format_supported(hDevice.pDrvPrivate, profile,
                                          (DXGI_FORMAT)pArgs->Desc.OutputFormat)) {
        ERROR("%s: %s cannot decode into output format %u on this device "
              "(bit_depth=%u) -> E_INVALIDARG", __FUNCTION__, profile->name,
              (unsigned)pArgs->Desc.OutputFormat, profile->bit_depth);
        return E_INVALIDARG;
    }

    /* Open the virtio-media decoder session now, keep it in the object, close it
     * in Destroy. Nothing is decoded yet: the first access unit arrives with
     * SubmitDecoderBuffers between BeginFrame and EndFrame.
     *
     * The codec is part of the OPEN request: the host virtio-media decoder is a
     * V4L2 stateful decoder and its format is chosen once, by the S_FMT(OUTPUT)
     * pixel format the guest driver sends when it configures the session
     * (H264 vs VP90).  P-BATCH VP9 adds that selector to VM_OPEN_IN.Flags, so
     * the UMD has to open a VP9 session for a VP9 decoder; an older driver
     * ignores Flags and would configure H.264 for VP9 bytes, which can only
     * time out, so that combination is refused with a precise log line rather
     * than a one-second stall per frame. */
    VM_OPEN_OUT open_out;
    memset(&open_out, 0, sizeof(open_out));
    VM_SESSION *session = vm_open_codec(0 /* decoder */, profile->open_codec, &open_out);
    if (!session) {
        ERROR("%s: vm_open_codec(decoder, %s) failed err=%u -> E_FAIL", __FUNCTION__,
              profile->name, vm_last_error());
        return E_FAIL;
    }
    if (profile->codec == VIRTIO_WDDM_CODEC_VP9 &&
        open_out.DriverVersion < VMEDIA_VP9_MIN_DRIVER_VERSION) {
        ERROR("%s: driver 0x%08x predates the VP9 codec selector (needs 0x%08x): "
              "it would configure the host for H.264 and never return a frame "
              "-> refusing the VP9 decoder", __FUNCTION__, open_out.DriverVersion,
              (UINT)VMEDIA_VP9_MIN_DRIVER_VERSION);
        vm_close(session);
        return E_FAIL;
    }
    /* P-BATCH HEVC: same gate.  A driver older than 1.0.0.15 knows codec ids 0
     * and 1 only and would reject the OPEN outright; 1.0.0.15 added the HEVC
     * S_FMT selector, so anything below it has no way to decode HEVC bytes. */
    if (profile->codec == VIRTIO_WDDM_CODEC_HEVC &&
        open_out.DriverVersion < VMEDIA_HEVC_MIN_DRIVER_VERSION) {
        ERROR("%s: driver 0x%08x predates the HEVC codec selector (needs 0x%08x): "
              "it would configure the host for H.264 and never return a frame "
              "-> refusing the HEVC decoder", __FUNCTION__, open_out.DriverVersion,
              (UINT)VMEDIA_HEVC_MIN_DRIVER_VERSION);
        vm_close(session);
        return E_FAIL;
    }
    /* P-BATCH AV1: same gate.  The AV1 selector and the 'AV01' S_FMT arrive with
     * media-wdm 1.0.0.16; an older driver rejects the OPEN (codec id > 2), and
     * the UMD makes the reason explicit instead of leaving a per-frame timeout. */
    if (profile->codec == VIRTIO_WDDM_CODEC_AV1 &&
        open_out.DriverVersion < VMEDIA_AV1_MIN_DRIVER_VERSION) {
        ERROR("%s: driver 0x%08x predates the AV1 codec selector (needs 0x%08x): "
              "it would configure the host for H.264 and never return a frame "
              "-> refusing the AV1 decoder", __FUNCTION__, open_out.DriverVersion,
              (UINT)VMEDIA_AV1_MIN_DRIVER_VERSION);
        vm_close(session);
        return E_FAIL;
    }

    /* HEVC-MAIN10: the Main10 selector (codec id 4) and the P010 CAPTURE
     * negotiation arrive with media-wdm 1.0.0.21.  An older driver answers the
     * OPEN with ST_INVALID_PARAMETER (err=87), so the log names the version
     * instead of leaving a per-frame timeout -- and the application, which
     * honoured CheckVideoDecoderFormat, falls back to software decode once. */
    if (profile->bit_depth >= 10u &&
        open_out.DriverVersion < VMEDIA_HEVC_MAIN10_MIN_DRIVER_VERSION) {
        ERROR("%s: driver 0x%08x predates the HEVC Main10 selector + P010 CAPTURE "
              "negotiation (needs 0x%08x): the host would stay NV12 and the 10-bit "
              "picture would be truncated -> refusing the HEVC Main10 decoder",
              __FUNCTION__, open_out.DriverVersion,
              (UINT)VMEDIA_HEVC_MAIN10_MIN_DRIVER_VERSION);
        vm_close(session);
        return E_FAIL;
    }

    object->magic = VIRTIO_WDDM_VIDEO_MAGIC_DECODE;
    object->desc = pArgs->Desc;
    object->config = pArgs->Config;
    object->codec = profile->codec;
    object->bit_depth = profile->bit_depth;
    object->session = session;
    object->session_id = open_out.SessionId;
    object->driver_version = open_out.DriverVersion;
    object->device = hDevice.pDrvPrivate;
    /* P-BATCH ZEROCOPY: the direct-mapped frame path is opt-out by environment
     * variable (VIRTIO_WDDM_ZEROCOPY=0 forces the staged path everywhere) and
     * is only tried against a driver that implements MAP_FRAME, so an old guest
     * keeps its exact P3 behaviour.  `direct_ok` is cleared for the rest of the
     * session the first time the route proves unusable. */
    object->direct_ok = decoder_zerocopy_enabled(object) &&
                        open_out.DriverVersion >= VMEDIA_MAP_FRAME_MIN_DRIVER_VERSION;
    INFO("%s: %s/%s %ux%u decoder created (bit_depth=%u), vm session=%u caps=0x%08x "
         "drv=0x%08x direct=%s (ConfigBitstreamRaw=%u)",
         __FUNCTION__, profile->name,
         (DXGI_FORMAT)pArgs->Desc.OutputFormat == DXGI_FORMAT_P010 ? "P010" : "NV12",
         pArgs->Desc.SampleWidth, pArgs->Desc.SampleHeight, profile->bit_depth,
         open_out.SessionId, open_out.DeviceCaps, open_out.DriverVersion,
         object->direct_ok ? "map" : "staging",
         pArgs->Config.ConfigBitstreamRaw);
    /* PIPELINE (2026-10-25): arm the two-stage decode pipeline.  It refuses
     * itself (and logs why) for VP9, for a driver that predates VM_IOCTL_FLUSH,
     * and when VIRTIO_WDDM_PIPELINE=0, in which case every frame takes the exact
     * serial path this UMD shipped before. */
    (void)pipeline_create(object);
    return S_OK;
}

/* P3-3: flush the host session and account for it.  Used when a new sequence
 * starts inside one decoder (seek into another GOP, SPS/PPS change, geometry
 * change, frame_num discontinuity) and again from DestroyVideoDecoder, so the
 * host is never left holding reference frames or queued access units of a
 * picture the guest no longer wants.  The driver MUNMAPs + CLOSEs the old
 * session and opens a fresh one; the VM_SESSION handle stays the same, only its
 * session id changes (P3-2 driver 1.0.0.11's VM_IOCTL_FLUSH).
 *
 * DriverVersion is checked first: on a driver that predates the IOCTL the call
 * would only produce an error line per frame (the 1.0.0.7 one-shot driver is
 * still what some guests run).  Returns TRUE when the driver accepted it. */
static BOOL decoder_flush_session(VIRTIO_WDDM_VideoDecoder *object, const char *reason) {
    VM_FLUSH_OUT fout;
    UINT old_session;

    if (!object || !object->session)
        return FALSE;
    if (object->driver_version < VMEDIA_FLUSH_MIN_DRIVER_VERSION) {
        VERBOSE("%s: driver 0x%08x predates VM_IOCTL_FLUSH (needs 0x%08x) -> not flushing (%s)",
                __FUNCTION__, object->driver_version, (UINT)VMEDIA_FLUSH_MIN_DRIVER_VERSION,
                reason);
        return FALSE;
    }
    /* PIPELINE (2026-10-25): a flush is the "everything queued is from the
     * position the application just left" boundary.  Drop the in-flight jobs
     * (a picture from before a seek must never be written after it), give back
     * any capture buffer the worker is holding, and bump the generation so the
     * worker abandons what it is doing before the session is replaced. */
    pipeline_quiesce(object, reason);
    old_session = object->session_id;
    memset(&fout, 0, sizeof(fout));
    if (!vm_flush(object->session, &fout)) {
        ERROR("%s: vm_flush (%s) failed err=%u -> keeping the old session", __FUNCTION__,
              reason, vm_last_error());
        return FALSE;
    }
    object->flushes++;
    object->aus_dropped += fout.DroppedAu;
    object->frames_dropped += fout.DroppedFrames;
    object->session_id = fout.SessionId;
    INFO("%s: FLUSH (%s): dropped %u AU / %u frame(s), session %u -> %u "
         "(%llu flush(es))", __FUNCTION__, reason, fout.DroppedAu, fout.DroppedFrames,
         old_session, fout.SessionId, (unsigned long long)object->flushes);
    /* P-BATCH AV1: the flushed session owned the DPB and the sequence header.
     * The next temporal unit must start from a key frame; dropping the pending
     * catch-up (and the mirror) keeps a stale frame from being spliced in and
     * forces the sequence header to be re-emitted. */
    if (object->codec == VIRTIO_WDDM_CODEC_AV1) {
        av1_dpb_reset(&object->av1_dpb);
        object->av1_prev_pending = FALSE;
        object->av1_seq_emitted = FALSE;
    }
    return TRUE;
}

static VOID APIENTRY virtio_wddm_video_destroy_video_decoder(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode) {
    (void)hDevice;
    VIRTIO_WDDM_VideoDecoder *object = hDecode.pDrvPrivate;
    VIRTIO_WDDM_DDI_ENTRY("hDecode=%p priv=%p magic=0x%08x", (void *)hDecode.pDrvPrivate,
                          (void *)object, object ? object->magic : 0);
    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECODE)
        return;
    if (object->session) {
        /* PIPELINE (2026-10-25): stop the worker before anything below touches
         * the session or the decoder's scratch buffers.  A job still in flight
         * is dropped (its subresource is never written); the summary says how
         * many, so "the last frame of a clip never appeared" is explainable. */
        pipeline_destroy(object);
        /* P3-3: flush before closing.  A player that seeks by destroying and
         * recreating the decoder leaves its old host session (reference frames,
         * queued access units, CAPTURE buffers) behind unless the drop happens
         * here; it is also what makes the summary below show a real flush count
         * in playback instead of 0.  A decoder that never sent an AU has nothing
         * to drop, so it is only closed (no pointless close+open on the host). */
        BOOL destroy_flush = FALSE;
        /* P-BATCH ZEROCOPY: a frame that was mapped and never released would
         * keep a capture buffer out of the queue the flush is about to close. */
        decoder_drop_direct(object);
        if (object->submits > 0)
            destroy_flush = decoder_flush_session(object, "destroy");
        vm_close(object->session);
        INFO("%s: destroy: flush(es)=%llu, dropped %llu AU / %llu frame(s) "
             "(destroy flush=%d, frame_num-triggered=%llu, frame_num advances=%llu)",
             __FUNCTION__,
             (unsigned long long)object->flushes,
             (unsigned long long)object->aus_dropped,
             (unsigned long long)object->frames_dropped, (int)destroy_flush,
             (unsigned long long)object->fn_flushes,
             (unsigned long long)object->frame_num_advances);
        INFO("%s: closed vm session=%u after %llu submit(s) / %llu frame(s) / %llu byte(s) "
             "/ %llu reopen(s) / %llu SPS-PPS injection(s) (%llu synthesised from "
             "PICTURE_PARAMETERS) / %llu dropped frame(s) / %llu flush(es) "
             "(%llu AU / %llu frame(s) dropped by the flushes)",
             __FUNCTION__, object->session_id, (unsigned long long)object->submits,
             (unsigned long long)object->frames, (unsigned long long)object->bytes_sent,
             (unsigned long long)object->reopens,
             (unsigned long long)object->param_set_injections,
             (unsigned long long)object->synth_injections,
             (unsigned long long)object->decode_failures,
             (unsigned long long)object->flushes,
             (unsigned long long)object->aus_dropped,
             (unsigned long long)object->frames_dropped);
        if (object->codec == VIRTIO_WDDM_CODEC_VP9) {
            INFO("%s: VP9 summary: %llu picture(s) decoded / %llu key frame(s) / "
                 "%llu superframe(s) / %llu unparsable header(s) / %llu "
                 "PICTURE_PARAMETERS buffer(s) / %llu frame(s) refused by the "
                 "VP9FIX boundary checks / %u PicParams warning(s)",
                 __FUNCTION__, (unsigned long long)object->vp9_frames,
                 (unsigned long long)object->vp9_keyframes,
                 (unsigned long long)object->vp9_superframes,
                 (unsigned long long)object->vp9_bad_headers,
                 (unsigned long long)object->vp9_pic_buffers,
                 (unsigned long long)object->vp9_refusals, object->vp9_pic_warnings);
        }
        /* VP9FIX: the direct-mapped frame audit.  maps - releases is the number
         * of capture buffers still checked out of the device when the decoder
         * goes away; it must be 0 (the destroy path drops the last frame right
         * above).  A non-zero value, a release failure or a pairing fix names
         * the path that leaked, instead of the leak only showing up as the
         * driver's "previous frame was never released" on the next MAP_FRAME. */
        INFO("%s: zero-copy audit: %llu MAP_FRAME / %llu RELEASE_FRAME / "
             "%llu release failure(s) / %llu pairing fix(es) / %llu superframe "
             "member(s) released unconsumed / outstanding=%lld / %llu timeout(s) / "
             "%llu refusal(s) / %llu other failure(s) / %llu stray release(s) "
             "(%llu found nothing mapped)",
             __FUNCTION__, (unsigned long long)object->direct_maps,
             (unsigned long long)object->direct_releases,
             (unsigned long long)object->direct_release_failures,
             (unsigned long long)object->direct_pairing_fixes,
             (unsigned long long)object->direct_member_releases,
             (long long)((INT64)object->direct_maps - (INT64)object->direct_releases),
             (unsigned long long)object->direct_timeouts,
             (unsigned long long)object->direct_refusals,
             (unsigned long long)object->direct_other_failures,
             (unsigned long long)object->stray_releases,
             (unsigned long long)object->stray_release_empty);
        if (object->codec == VIRTIO_WDDM_CODEC_HEVC) {
            INFO("%s: HEVC summary: %llu picture(s) decoded / %llu IRAP(s) / "
                 "%llu VPS-SPS-PPS synthesised from PICTURE_PARAMETERS / %llu "
                 "forwarded from the access unit / %llu synthesis refusal(s) / "
                 "%llu PICTURE_PARAMETERS buffer(s) / %llu SLICE_CONTROL buffer(s)",
                 __FUNCTION__, (unsigned long long)object->hevc_frames,
                 (unsigned long long)object->hevc_irap,
                 (unsigned long long)object->hevc_synth_injections,
                 (unsigned long long)object->hevc_inband_injections,
                 (unsigned long long)object->hevc_synth_refusals,
                 (unsigned long long)object->hevc_pic_buffers,
                 (unsigned long long)object->hevc_slice_controls);
        }
        if (object->codec == VIRTIO_WDDM_CODEC_AV1) {
            INFO("%s: AV1 summary: %llu picture(s) decoded / %llu key frame(s) / "
                 "%llu refresh mask(s) derived from the next picture / %llu hidden "
                 "DPB catch-up frame(s) / %llu PICTURE_PARAMETERS buffer(s) / %llu "
                 "SLICE_CONTROL buffer(s) / %llu GET_FRAME timeout(s) / %llu tile "
                 "record refusal(s) / segmented timing %s",
                 __FUNCTION__, (unsigned long long)object->av1_frames,
                 (unsigned long long)object->av1_key_frames,
                 (unsigned long long)object->av1_masks_derived,
                 (unsigned long long)object->av1_catchups,
                 (unsigned long long)object->av1_pic_buffers,
                 (unsigned long long)object->av1_slice_controls,
                 (unsigned long long)object->frame_get_timeouts,
                 (unsigned long long)object->av1_bad_tile_records,
                 object->frame_get_segmented ? "armed" : "not needed");
        }
    }
    if (object->bitstream)
        HeapFree(GetProcessHeap(), 0, object->bitstream);
    if (object->params)
        HeapFree(GetProcessHeap(), 0, object->params);
    if (object->synth)
        HeapFree(GetProcessHeap(), 0, object->synth);
    if (object->send)
        HeapFree(GetProcessHeap(), 0, object->send);
    if (object->nv12)
        HeapFree(GetProcessHeap(), 0, object->nv12);
    if (object->conv)
        HeapFree(GetProcessHeap(), 0, object->conv);
    if (object->slices)
        HeapFree(GetProcessHeap(), 0, object->slices);
    if (object->vp9_au)
        HeapFree(GetProcessHeap(), 0, object->vp9_au);
    if (object->hevc_params)
        HeapFree(GetProcessHeap(), 0, object->hevc_params);
    if (object->hevc_au)
        HeapFree(GetProcessHeap(), 0, object->hevc_au);
    if (object->av1_tiles)
        HeapFree(GetProcessHeap(), 0, object->av1_tiles);
    if (object->av1_au)
        HeapFree(GetProcessHeap(), 0, object->av1_au);
    if (object->av1_prev_tile_group)
        HeapFree(GetProcessHeap(), 0, object->av1_prev_tile_group);
    memset(object, 0, sizeof(*object));
}

static HRESULT APIENTRY virtio_wddm_video_video_decoder_extension(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode,
    CONST D3D11_1DDIARG_VIDEODECODEREXTENSION *pArgs) {
    (void)hDevice;
    (void)hDecode;
    (void)pArgs;
    return E_NOTIMPL;
}

/* ------------------------------------------------------------------------- */
/* Decoder: BeginFrame / SubmitDecoderBuffers / EndFrame (P2-2b)             */
/* ------------------------------------------------------------------------- */

/* Grow one of the decoder's scratch buffers to at least `need` bytes. The
 * high-water mark is kept, so steady-state playback does not allocate. FALSE
 * means out of memory; the caller turns that into E_FAIL. */
static BOOL decoder_reserve(UINT8 **buffer, UINT *capacity, UINT need) {
    UINT8 *grown;
    UINT new_capacity;

    if (need <= *capacity)
        return TRUE;
    new_capacity = *capacity ? *capacity : 64u * 1024u;
    while (new_capacity < need) {
        if (new_capacity > 0x40000000u) {
            ERROR("%s: refusing to grow past 1 GiB (need=%u)", __FUNCTION__, need);
            return FALSE;
        }
        new_capacity *= 2u;
    }
    grown = *buffer ? HeapReAlloc(GetProcessHeap(), 0, *buffer, new_capacity)
                    : HeapAlloc(GetProcessHeap(), 0, new_capacity);
    if (!grown) {
        ERROR("%s: HeapAlloc(%u) failed", __FUNCTION__, new_capacity);
        return FALSE;
    }
    *buffer = grown;
    *capacity = new_capacity;
    return TRUE;
}

/* P-BATCH P010: move one 4:2:0 frame between the NV12 and the P010 layout.
 *
 * The two layouts are the same geometry with a different sample width:
 *
 *            NV12 (8-bit)                 P010 (10-bit in a 16-bit word)
 *   luma     1 byte  per sample           2 bytes per sample, value in bits 15:6
 *   chroma   U,V interleaved 1 byte each  U,V interleaved 2 bytes each
 *   rows     stride bytes                 stride bytes (>= 2 * width)
 *   size     stride * height * 3 / 2      stride * height * 3 / 2
 *
 * Because a P010 sample keeps its 10 bits in the top of the word, an 8-bit
 * value b maps to b << 8 and back with >> 8; the 10-bit sample is then
 * b << 2, i.e. the same code value scaled exactly into 10 bits. Converting
 * 8 -> 10 and back is therefore lossless (no rounding, no range change): the
 * picture a P010 surface holds is bit-exact with the NV12 one the host decoded.
 *
 * When the two layouts already match the frame is copied row by row, which is
 * also how a padded source stride becomes a tight destination stride.
 *
 * `dst` must hold dst_stride * height * 3 / 2 bytes: both planes packed, Y
 * first, then the interleaved chroma rows -- the layout vm_get_frame() returns
 * and the one UpdateSubresource expects. */
static BOOL decoder_convert_420(const UINT8 *src, UINT src_stride, BOOL src_p010,
                                UINT8 *dst, UINT dst_stride, BOOL dst_p010,
                                UINT width, UINT height) {
    UINT chroma_rows = height / 2u;
    UINT luma_row_bytes = dst_p010 ? width * 2u : width;
    UINT y;
    UINT x;

    if (!src || !dst || !width || !height || (width & 1u) || (height & 1u))
        return FALSE;
    if (src_stride < luma_row_bytes || dst_stride < luma_row_bytes)
        return FALSE;

    if (src_p010 == dst_p010) {
        UINT row_bytes = src_stride < dst_stride ? src_stride : dst_stride;
        for (y = 0; y < height; y++)
            memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
        for (y = 0; y < chroma_rows; y++)
            memcpy(dst + (size_t)(height + y) * dst_stride,
                   src + (size_t)(height + y) * src_stride, row_bytes);
        return TRUE;
    }

    if (dst_p010) {
        /* 8 -> 10: MSB-align each sample in its 16-bit word. Chroma is the
         * interleaved pair stream, so the same widening applies to it. */
        for (y = 0; y < height; y++) {
            const UINT8 *s = src + (size_t)y * src_stride;
            USHORT *d = (USHORT *)(dst + (size_t)y * dst_stride);
            for (x = 0; x < width; x++)
                d[x] = (USHORT)((USHORT)s[x] << 8);
        }
        for (y = 0; y < chroma_rows; y++) {
            const UINT8 *s = src + (size_t)(height + y) * src_stride;
            USHORT *d = (USHORT *)(dst + (size_t)(height + y) * dst_stride);
            for (x = 0; x < width; x++)
                d[x] = (USHORT)((USHORT)s[x] << 8);
        }
    } else {
        /* 10 -> 8: the top 8 bits of the 10-bit value, i.e. the same picture
         * truncated to the 8-bit surface (P010 -> NV12 happens when a future
         * driver returns P010 and the application asked for NV12). */
        for (y = 0; y < height; y++) {
            const USHORT *s = (const USHORT *)(src + (size_t)y * src_stride);
            UINT8 *d = dst + (size_t)y * dst_stride;
            for (x = 0; x < width; x++)
                d[x] = (UINT8)(s[x] >> 8);
        }
        for (y = 0; y < chroma_rows; y++) {
            const USHORT *s = (const USHORT *)(src + (size_t)(height + y) * src_stride);
            UINT8 *d = dst + (size_t)(height + y) * dst_stride;
            for (x = 0; x < width; x++)
                d[x] = (UINT8)(s[x] >> 8);
        }
    }
    return TRUE;
}

/* P2-2i: EndFrame failure policy -- recoverable, not device removal.
 *
 * P2-2b..P2-2h sent every failure through pfnSetErrorCb (tritonSetError_impl).
 * The D3D11 runtime treats an error callback from this VOID DDI as a
 * device-level error: Chromium observed DXGI_ERROR_DEVICE_REMOVED
 * (0x887A0020), its GPU process exited with code 34 and the media pipeline
 * reported PIPELINE_ERROR_DISCONNECTED -- for a frame the host decoder simply
 * did not produce. That is not a lost device.
 *
 * A dropped frame is therefore recorded in object->pending_error and returned
 * by the next DecoderBeginFrame, which is allowed to return an HRESULT to the
 * application. The current frame is dropped, frame state is already reset, and
 * the following BeginFrame/Submit/EndFrame runs normally: the device, the
 * decoder object and the session stay alive.
 *
 * E_FAIL is used on purpose: it is an ordinary application-visible failure, not
 * a device error, and needs no MF header in this DDI translation unit. */
static void decoder_fail_frame(VIRTIO_WDDM_VideoDecoder *object, HRESULT hr) {
    if (!object)
        return;
    object->decode_failures++;
    if (object->pending_error == S_OK)
        object->pending_error = hr;
    /* VP9FIX (2026-10-25): a failed frame must never leave a capture buffer
     * checked out.  Every caller of this helper drops the frame as a whole, and
     * this is the one place they all pass through, so the pairing is enforced
     * here as well (idempotent: no-op when nothing is mapped).  Missing it is
     * what starves the pool and makes the driver refuse the next MAP_FRAME. */
    decoder_drop_direct(object);
    /* Deliberately no tritonSetError_impl()/pfnSetErrorCb here. */
}

/* Annex-B start code at or after `p`: returns its address and stores 3 or 4 in
 * *prefix. NULL when the buffer holds no further start code. */
static const UINT8 *decoder_find_start_code(const UINT8 *p, const UINT8 *end,
                                            UINT *prefix) {
    for (; p + 3 <= end; p++) {
        if (p[0] != 0 || p[1] != 0)
            continue;
        if (p[2] == 1) {
            *prefix = 3;
            return p;
        }
        if (p[2] == 0 && p + 4 <= end && p[3] == 1) {
            *prefix = 4;
            return p;
        }
    }
    return NULL;
}

/* Walk one access unit. Reports whether it carries SPS (NAL 7) and PPS
 * (NAL 8); when it carries both, rebuilds the decoder's parameter-set cache
 * from them (the last IDR of a stream may bring a new SPS, so this replaces
 * rather than appends). Access units without a full pair leave the cache
 * alone -- that is the normal P-frame case. */
static void decoder_update_parameter_sets(VIRTIO_WDDM_VideoDecoder *object,
                                          const UINT8 *au, UINT size, BOOL *has_sps,
                                          BOOL *has_pps) {
    const UINT8 *end = au + size;
    const UINT8 *p = au;
    UINT prefix;
    UINT total = 0;
    BOOL sps = FALSE;
    BOOL pps = FALSE;

    *has_sps = FALSE;
    *has_pps = FALSE;

    while ((p = decoder_find_start_code(p, end, &prefix)) != NULL) {
        const UINT8 *nal = p + prefix;
        const UINT8 *next;
        UINT nal_size;
        UINT type;

        if (nal >= end)
            break;
        type = (UINT)(nal[0] & 0x1Fu);
        next = decoder_find_start_code(nal, end, &prefix);
        nal_size = (UINT)((next ? next : end) - nal);
        p = nal + 1;

        if (type == 7) {
            sps = TRUE;
            total += 4u + nal_size;
        } else if (type == 8) {
            pps = TRUE;
            total += 4u + nal_size;
        }
    }

    *has_sps = sps;
    *has_pps = pps;
    /* 256 KiB is far above any real SPS+PPS; refuse to grow on garbage. */
    if (!sps || !pps || total == 0 || total > 256u * 1024u)
        return;
    if (!decoder_reserve(&object->params, &object->params_cap, total))
        return;

    object->params_size = 0;
    p = au;
    while ((p = decoder_find_start_code(p, end, &prefix)) != NULL) {
        const UINT8 *nal = p + prefix;
        const UINT8 *next;
        UINT nal_size;
        UINT type;

        if (nal >= end)
            break;
        type = (UINT)(nal[0] & 0x1Fu);
        next = decoder_find_start_code(nal, end, &prefix);
        nal_size = (UINT)((next ? next : end) - nal);
        p = nal + 1;
        if (type != 7 && type != 8)
            continue;

        object->params[object->params_size + 0] = 0;
        object->params[object->params_size + 1] = 0;
        object->params[object->params_size + 2] = 0;
        object->params[object->params_size + 3] = 1;
        memcpy(object->params + object->params_size + 4, nal, nal_size);
        object->params_size += 4u + nal_size;
    }
    VERBOSE("%s: cached %u byte(s) of SPS/PPS", __FUNCTION__, object->params_size);
}

/* P3-2: does this access unit start a new sequence, i.e. does its first VCL NAL
 * belong to an IDR picture (NAL type 5)?  SPS/PPS/SEI before the first slice are
 * skipped. */
static BOOL decoder_au_has_idr(const UINT8 *au, UINT size) {
    const UINT8 *end = au + size;
    const UINT8 *p = au;
    UINT prefix;

    while ((p = decoder_find_start_code(p, end, &prefix)) != NULL) {
        const UINT8 *nal = p + prefix;
        UINT type;

        if (nal >= end)
            break;
        type = (UINT)(nal[0] & 0x1Fu);
        if (type == 5)
            return TRUE;
        if (type == 1) /* first VCL NAL is a non-IDR slice */
            return FALSE;
        p = nal + 1;
    }
    return FALSE;
}

static LONGLONG decoder_qpc_freq(void) {
    static LONGLONG freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    return freq;
}

/* P3-5: per-frame budget on the decode side.  vm_send is the host round trip
 * that hands the access unit over, vm_get_frame is the host decode latency and
 * the upload is the CPU staging copy plus DXVK command recording; with the
 * present line in dxgi.c that accounts for every millisecond of a frame. */
static LONGLONG decoder_qpc_now(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static double decoder_qpc_ms(LONGLONG start, LONGLONG end) {
    LONGLONG freq = decoder_qpc_freq();
    if (freq <= 0)
        return 0.0;
    return (double) (end - start) * 1000.0 / (double) freq;
}

/* P3-5: render-thread DDI cadence.
 *
 * A player render thread calls a handful of video DDIs per presented frame
 * (input view, output view, output rate, blt) and then presents. Timing each of
 * them plus the gap since the previous DDI on the same thread splits the present
 * period into "inside our DDIs" and "everywhere else" (the player and the
 * runtime), which is what makes a 90 ms present period attributable or not.
 * Thread local: the decode thread's DDIs must not be mixed into the render
 * thread's gaps. */
static __thread LONGLONG video_ddi_last_qpc;

typedef struct {
    LONGLONG t;
    double gap_ms;
} VIRTIO_WDDM_DdiStamp;

static VIRTIO_WDDM_DdiStamp video_ddi_enter(void) {
    VIRTIO_WDDM_DdiStamp stamp;
    stamp.t = decoder_qpc_now();
    stamp.gap_ms = video_ddi_last_qpc ? decoder_qpc_ms(video_ddi_last_qpc, stamp.t) : 0.0;
    video_ddi_last_qpc = stamp.t;
    return stamp;
}

static void video_ddi_exit(const char *kind, VIRTIO_WDDM_DdiStamp stamp) {
    double dur_ms = decoder_qpc_ms(stamp.t, decoder_qpc_now());
    INFO("ddi %s: t=%lld gap=%.3f dur=%.3f ms", kind, (long long) stamp.t, stamp.gap_ms,
         dur_ms);
}

/* The virtio-media driver that P2-1 put on the guest (1.0.0.7) is one-shot per
 * session: ioctl_send rejects a second SEND with STATUS_INVALID_DEVICE_STATE
 * ("one access unit per session in P2-1") and ioctl_get_frame rejects a second
 * GET_FRAME. The UMD cannot change the driver, so when a SEND is refused the
 * session is closed and a fresh one opened for that frame; configure_and_send
 * re-runs S_FMT/REQBUFS/STREAMON on the new session.
 *
 * This is deliberately failure-driven rather than done before every frame: a
 * driver that allows streaming keeps its session and pays nothing. The cost on
 * 1.0.0.7 is one rejected IOCTL plus the per-frame setup P2-1 measured at
 * 127.6 ms, i.e. roughly 8 frames/s at 720p until the driver is extended. */
static BOOL decoder_reopen_session(VIRTIO_WDDM_VideoDecoder *object) {
    VM_OPEN_OUT open_out;
    VM_SESSION *session;
    UINT open_codec;

    if (!object)
        return FALSE;
    if (object->session) {
        vm_close(object->session);
        object->session = NULL;
    }
    /* The fresh session must be opened for the SAME codec: the codec is a
     * property of the host session (it selects the host decoder at S_FMT), not
     * something a later SEND can correct. */
    switch (object->codec) {
    case VIRTIO_WDDM_CODEC_VP9:
        open_codec = VM_OPEN_CODEC_VP9;
        break;
    case VIRTIO_WDDM_CODEC_HEVC:
        open_codec = VM_OPEN_CODEC_HEVC;
        break;
    case VIRTIO_WDDM_CODEC_AV1:
        open_codec = VM_OPEN_CODEC_AV1;
        break;
    default:
        open_codec = VM_OPEN_CODEC_H264;
        break;
    }
    memset(&open_out, 0, sizeof(open_out));
    session = vm_open_codec(0 /* decoder */, open_codec, &open_out);
    if (!session) {
        ERROR("%s: vm_open_codec(decoder, %s) for a fresh session failed err=%u",
              __FUNCTION__, decoder_codec_name(object->codec), vm_last_error());
        return FALSE;
    }
    object->session = session;
    object->session_id = open_out.SessionId;
    object->reopens++;
    /* P-BATCH AV1: the host decoder's whole DPB (and the sequence header) lived
     * in the session that just went away.  There is nothing to re-send for the
     * pictures already decoded, so the next access unit has to start from a key
     * frame; resetting the mirror makes the sequence header re-emitted and
     * stops a stale catch-up frame from being spliced in. */
    if (object->codec == VIRTIO_WDDM_CODEC_AV1) {
        av1_dpb_reset(&object->av1_dpb);
        object->av1_prev_pending = FALSE;
        object->av1_seq_emitted = FALSE;
        ERROR("%s: AV1 host session reopened -> decoder DPB lost; the next "
              "access unit must be a key frame (sequence header will be "
              "re-emitted)", __FUNCTION__);
    }
    VERBOSE("%s: reopened vm session=%u (%llu reopen(s)) for %s", __FUNCTION__,
            open_out.SessionId, (unsigned long long)object->reopens,
            decoder_codec_name(object->codec));
    return TRUE;
}

/* Minimum standard level that covers a frame of `mbs` macroblocks, with one
 * step of headroom for streams whose num_ref_frames / bitrate sit at the edge
 * of the level (1080p30 is level 4.0/4.1 in the wild, 720p level 3.1).
 *
 * Why a derived level at all: DXVA_PicParams_H264 has no level_idc field (the
 * WDK struct carries only the parsed H.264 values listed in
 * d3d_h264_accelerator.cc's PicParamsFromSPS; profile_idc, level_idc and
 * seq_parameter_set_id are not among them, which is also why Chromium cannot
 * hand us the original SPS bytes). The SPS we synthesise therefore declares
 * the level the coded size needs; declaring a level at or above the stream's
 * real one is legal H.264. */
static unsigned decoder_level_from_frame_mbs(unsigned mbs) {
    if (mbs <= 1620u)
        return 30u; /* level 3.0 */
    if (mbs <= 3600u)
        return 31u; /* 3.1: 720p (80x45 = 3600) */
    if (mbs <= 5120u)
        return 32u;
    if (mbs <= 8192u)
        return 41u; /* 4.1: 1080p (120x68 = 8160) */
    if (mbs <= 8704u)
        return 42u;
    if (mbs <= 22080u)
        return 50u;
    if (mbs <= 36864u)
        return 51u; /* 5.1: 4K (240x136 = 32640) */
    return 52u;
}

/* Copy the values Chromium fills DXVA_PicParams_H264 with into the plain-C
 * field set that h264_params.h consumes. FALSE means this frame cannot be
 * described by the profile we report (NoFGT / 8-bit 4:2:0), so synthesis is
 * skipped and the per-stream SPS/PPS cache is used if one exists.
 *
 * Why the PICTURE_PARAMETERS buffer is the only source: Chromium's D3D11 H.264
 * accelerator submits slices only (3-byte start code + slice NAL,
 * d3d_h264_accelerator.cc SubmitSlice) and keeps the parsed SPS/PPS in these
 * fields -- there is no SPS/PPS byte string anywhere in the D3D11 buffers.
 * The host V4L2 decoder only receives the BITSTREAM bytes, so it needs the
 * parameter sets in-band or it produces no frame at all (P2-2h:
 * no DQBUF(CAPTURE) -> eventq timeout -> 0xC00000B5).
 *
 * Three SPS values are not in the struct (verified against
 * d3d_h264_accelerator.cc PicParamsFromSPS, which is the exact code Edge
 * 155.0.4283.62 runs):
 *   - profile_idc: Chromium's D3D11 H.264 profile GUID covers Baseline/Main/
 *     Extended/High. High is only required when the PPS enables the 8x8
 *     transform (a High-profile tool), which the struct does carry
 *     (transform_8x8_mode_flag); otherwise Main. Both decode any 8-bit 4:2:0
 *     CAVLC/CABAC stream of that family.
 *   - level_idc: derived from the coded size, see decoder_level_from_frame_mbs.
 *   - seq_parameter_set_id: 0; the PPS we synthesise references SPS 0 and the
 *     slice headers reference the PPS id (parsed from the slice itself).
 * This is the honest limit of what DXVA_PicParams_H264 provides. */
static BOOL decoder_fields_from_pic_params(VIRTIO_WDDM_VideoDecoder *object) {
    const DXVA_PicParams_H264 *p = &object->pic;
    H264ParamSetFields *f = &object->pic_fields;
    unsigned map_units;

    memset(f, 0, sizeof(*f));
    f->frame_width_mbs = (unsigned)p->wFrameWidthInMbsMinus1 + 1u;
    /* Chromium fills wFrameHeightInMbsMinus1 from
     * H264SPS::pic_height_in_map_units_minus1 (d3d_h264_accelerator.cc:173),
     * i.e. map units, not macroblock rows. For progressive streams the two are
     * the same; the conversion below keeps field-coded maths honest even
     * though field pictures are refused a few lines later. */
    map_units = (unsigned)p->wFrameHeightInMbsMinus1 + 1u;
    f->frame_height_mbs = map_units * (2u - (unsigned)p->frame_mbs_only_flag);
    f->seq_parameter_set_id = 0;
    f->profile_idc = p->transform_8x8_mode_flag ? 100u : 77u;
    f->level_idc = decoder_level_from_frame_mbs(f->frame_width_mbs * f->frame_height_mbs);
    f->chroma_format_idc = p->chroma_format_idc;
    f->bit_depth_luma_minus8 = p->bit_depth_luma_minus8;
    f->bit_depth_chroma_minus8 = p->bit_depth_chroma_minus8;
    f->log2_max_frame_num_minus4 = p->log2_max_frame_num_minus4;
    f->pic_order_cnt_type = p->pic_order_cnt_type;
    f->log2_max_pic_order_cnt_lsb_minus4 = p->log2_max_pic_order_cnt_lsb_minus4;
    f->delta_pic_order_always_zero_flag = p->delta_pic_order_always_zero_flag;
    f->num_ref_frames = p->num_ref_frames;
    f->frame_mbs_only_flag = p->frame_mbs_only_flag;
    /* mb_adaptive_frame_field_flag is NOT MbaffFrameFlag: Chromium computes
     * MbaffFrameFlag = sps->mb_adaptive_frame_field_flag && field_pic
     * (:185), so a frame picture always reports 0 here. Progressive browser
     * content has the SPS flag 0 too; an MBAFF frame picture would decode
     * incorrectly and is refused below (field_pic_flag / MBAFF). */
    f->mb_adaptive_frame_field_flag = p->MbaffFrameFlag;
    f->direct_8x8_inference_flag = p->direct_8x8_inference_flag;
    f->num_slice_groups_minus1 = p->num_slice_groups_minus1;
    f->num_ref_idx_l0_active_minus1 = p->num_ref_idx_l0_active_minus1;
    f->num_ref_idx_l1_active_minus1 = p->num_ref_idx_l1_active_minus1;
    f->weighted_pred_flag = p->weighted_pred_flag;
    f->weighted_bipred_idc = p->weighted_bipred_idc;
    f->pic_init_qp_minus26 = p->pic_init_qp_minus26;
    f->pic_init_qs_minus26 = p->pic_init_qs_minus26;
    f->chroma_qp_index_offset = p->chroma_qp_index_offset;
    f->second_chroma_qp_index_offset = p->second_chroma_qp_index_offset;
    f->deblocking_filter_control_present_flag =
        p->deblocking_filter_control_present_flag;
    f->constrained_intra_pred_flag = p->constrained_intra_pred_flag;
    f->redundant_pic_cnt_present_flag = p->redundant_pic_cnt_present_flag;
    f->entropy_coding_mode_flag = p->entropy_coding_mode_flag;
    f->pic_order_present_flag = p->pic_order_present_flag;
    f->transform_8x8_mode_flag = p->transform_8x8_mode_flag;
    /* The SPS dimensions are the coded size; crop to the surface the runtime
     * asked us to decode into so the host output matches S_FMT. */
    f->visible_width = object->desc.SampleWidth;
    f->visible_height = object->desc.SampleHeight;

    if (f->frame_width_mbs == 0 || f->frame_height_mbs == 0) {
        ERROR("%s: PICTURE_PARAMETERS has no frame size (%ux%u MB) -> cannot "
              "synthesise SPS", __FUNCTION__, f->frame_width_mbs,
              f->frame_height_mbs);
        return FALSE;
    }
    if (p->field_pic_flag) {
        ERROR("%s: field picture (field_pic_flag=1) is outside this decoder's "
              "progressive NV12 profile -> no SPS synthesis", __FUNCTION__);
        return FALSE;
    }
    if (p->residual_colour_transform_flag) {
        ERROR("%s: separate colour planes (residual_colour_transform_flag) are "
              "outside this decoder's 4:2:0 profile -> no SPS synthesis",
              __FUNCTION__);
        return FALSE;
    }
    if (f->chroma_format_idc != 1u || f->bit_depth_luma_minus8 != 0u ||
        f->bit_depth_chroma_minus8 != 0u) {
        ERROR("%s: unsupported SPS format chroma_format_idc=%u bit_depth=%u/%u "
              "(only 8-bit 4:2:0) -> no SPS synthesis", __FUNCTION__,
              f->chroma_format_idc, f->bit_depth_luma_minus8,
              f->bit_depth_chroma_minus8);
        return FALSE;
    }
    if (f->num_slice_groups_minus1 != 0u) {
        ERROR("%s: FMO/slice groups (num_slice_groups_minus1=%u) are not in the "
              "reported NoFGT profile -> no SPS synthesis", __FUNCTION__,
              f->num_slice_groups_minus1);
        return FALSE;
    }
    return TRUE;
}

/* Synthesise this frame's Annex-B SPS+PPS from the PICTURE_PARAMETERS values
 * and return their size (0 = not available). The PPS id comes from the first
 * slice NAL because DXVA_PicParams_H264 does not carry it. */
static UINT decoder_synth_parameter_sets(VIRTIO_WDDM_VideoDecoder *object,
                                         const UINT8 *au, UINT au_size) {
    unsigned pps_id;
    unsigned sps_bytes = 0;
    unsigned pps_bytes = 0;
    unsigned total;
    int have_slice = 0;

    object->synth_size = 0;
    if (!object->pic_valid)
        return 0;
    if (!decoder_fields_from_pic_params(object))
        return 0;
    if (!decoder_reserve(&object->synth, &object->synth_cap, H264_PARAMS_MAX_BYTES))
        return 0;

    pps_id = h264_first_slice_pps_id(au, au_size, &have_slice);
    if (!have_slice) {
        VERBOSE("%s: no VCL slice in the %u byte access unit, assuming pps id 0",
                __FUNCTION__, au_size);
        pps_id = 0;
    }

    total = h264_synth_sps_pps(&object->pic_fields, pps_id, object->synth,
                               object->synth_cap, &sps_bytes, &pps_bytes);
    if (total == 0) {
        ERROR("%s: SPS/PPS synthesis refused (profile=%u level=%u %ux%u MB, "
              "poc_type=%u) -> falling back to cached parameter sets",
              __FUNCTION__, object->pic_fields.profile_idc,
              object->pic_fields.level_idc, object->pic_fields.frame_width_mbs,
              object->pic_fields.frame_height_mbs,
              object->pic_fields.pic_order_cnt_type);
        return 0;
    }
    object->synth_size = total;
    object->synth_injections++;

    if (object->synth_logs < 4u) {
        object->synth_logs++;
        INFO("%s: synthesized Annex-B SPS+PPS %u B (SPS %u B, PPS %u B), "
             "pps_id=%u profile=%u level=%u %ux%u MB, head=%02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x", __FUNCTION__, total,
             sps_bytes, pps_bytes, pps_id, object->pic_fields.profile_idc,
             object->pic_fields.level_idc, object->pic_fields.frame_width_mbs,
             object->pic_fields.frame_height_mbs, object->synth[0],
             object->synth[1], object->synth[2], object->synth[3],
             object->synth[4], object->synth[5], object->synth[6],
             object->synth[7], object->synth[8], object->synth[9],
             object->synth[10], object->synth[11]);
    } else {
        VERBOSE("%s: synthesized SPS+PPS %u B (pps_id=%u)", __FUNCTION__, total,
                pps_id);
    }
    return total;
}

/* ------------------------------------------------------------------------- */
/* P-BATCH HEVC: parameter sets from DXVA_PicParams_HEVC                     */
/* ------------------------------------------------------------------------- */

/* Copy the values a D3D11 HEVC client puts in DXVA_PicParams_HEVC into the
 * plain-C field set hevc_params.h consumes.  FALSE means the picture cannot be
 * described by the profile we report (Main, 8-bit 4:2:0) -- the caller then
 * falls back to the parameter sets cached from the access unit.
 *
 * Every value here is a direct transcription, which is the whole point:
 * DXVA_PicParams_HEVC is the parsed SPS+PPS a hardware decoder needs, so an SPS
 * rebuilt from it makes the host parse the slice headers at exactly the bit
 * offsets the encoder wrote.  The three things it does NOT carry are handled
 * explicitly (and only where they cannot change the parse):
 *   - conformance window: rebuilt from the surface the runtime asked for;
 *   - profile/level: Main / derived from the coded size (legal);
 *   - sps_max_num_reorder_pics / latency: 0, i.e. decode order out, which keeps
 *     the 1 AU -> 1 vm_get_frame() contract (same choice as the H.264 SPS).
 * The PPS id is not in the struct either; it comes from the first slice header
 * of the access unit (hevc_first_slice_pps_id). */
static BOOL decoder_hevc_fields_from_pic_params(VIRTIO_WDDM_VideoDecoder *object) {
    const DXVA_PicParams_HEVC *p = &object->hevc_pic;
    HevcParamSetFields *f = &object->hevc_fields;

    memset(f, 0, sizeof(*f));
    f->vps_id = 0; /* the SPS we emit references VPS 0 and we emit VPS 0 */
    f->sps_id = 0;
    f->pic_width_in_min_cbs = p->PicWidthInMinCbsY;
    f->pic_height_in_min_cbs = p->PicHeightInMinCbsY;
    f->chroma_format_idc = p->chroma_format_idc;
    f->separate_colour_plane_flag = p->separate_colour_plane_flag;
    f->bit_depth_luma_minus8 = p->bit_depth_luma_minus8;
    f->bit_depth_chroma_minus8 = p->bit_depth_chroma_minus8;
    f->log2_max_pic_order_cnt_lsb_minus4 = p->log2_max_pic_order_cnt_lsb_minus4;
    f->sps_max_dec_pic_buffering_minus1 = p->sps_max_dec_pic_buffering_minus1;
    f->log2_min_luma_coding_block_size_minus3 = p->log2_min_luma_coding_block_size_minus3;
    f->log2_diff_max_min_luma_coding_block_size =
        p->log2_diff_max_min_luma_coding_block_size;
    f->log2_min_luma_transform_block_size_minus2 = p->log2_min_transform_block_size_minus2;
    f->log2_diff_max_min_luma_transform_block_size =
        p->log2_diff_max_min_transform_block_size;
    f->max_transform_hierarchy_depth_inter = p->max_transform_hierarchy_depth_inter;
    f->max_transform_hierarchy_depth_intra = p->max_transform_hierarchy_depth_intra;
    f->num_short_term_ref_pic_sets = p->num_short_term_ref_pic_sets;
    f->num_long_term_ref_pics_sps = p->num_long_term_ref_pics_sps;
    f->long_term_ref_pics_present_flag = p->long_term_ref_pics_present_flag;
    f->scaling_list_enabled_flag = p->scaling_list_enabled_flag;
    f->amp_enabled_flag = p->amp_enabled_flag;
    f->sample_adaptive_offset_enabled_flag = p->sample_adaptive_offset_enabled_flag;
    f->pcm_enabled_flag = p->pcm_enabled_flag;
    f->pcm_sample_bit_depth_luma_minus1 = p->pcm_sample_bit_depth_luma_minus1;
    f->pcm_sample_bit_depth_chroma_minus1 = p->pcm_sample_bit_depth_chroma_minus1;
    f->log2_min_pcm_luma_coding_block_size_minus3 =
        p->log2_min_pcm_luma_coding_block_size_minus3;
    f->log2_diff_max_min_pcm_luma_coding_block_size =
        p->log2_diff_max_min_pcm_luma_coding_block_size;
    f->pcm_loop_filter_disabled_flag = p->pcm_loop_filter_disabled_flag;
    f->sps_temporal_mvp_enabled_flag = p->sps_temporal_mvp_enabled_flag;
    f->strong_intra_smoothing_enabled_flag = p->strong_intra_smoothing_enabled_flag;
    f->dependent_slice_segments_enabled_flag = p->dependent_slice_segments_enabled_flag;
    f->output_flag_present_flag = p->output_flag_present_flag;
    f->num_extra_slice_header_bits = p->num_extra_slice_header_bits;
    f->sign_data_hiding_enabled_flag = p->sign_data_hiding_enabled_flag;
    f->cabac_init_present_flag = p->cabac_init_present_flag;
    f->num_ref_idx_l0_default_active_minus1 = p->num_ref_idx_l0_default_active_minus1;
    f->num_ref_idx_l1_default_active_minus1 = p->num_ref_idx_l1_default_active_minus1;
    f->init_qp_minus26 = p->init_qp_minus26;
    f->constrained_intra_pred_flag = p->constrained_intra_pred_flag;
    f->transform_skip_enabled_flag = p->transform_skip_enabled_flag;
    f->cu_qp_delta_enabled_flag = p->cu_qp_delta_enabled_flag;
    f->diff_cu_qp_delta_depth = p->diff_cu_qp_delta_depth;
    f->pps_cb_qp_offset = p->pps_cb_qp_offset;
    f->pps_cr_qp_offset = p->pps_cr_qp_offset;
    f->pps_slice_chroma_qp_offsets_present_flag =
        p->pps_slice_chroma_qp_offsets_present_flag;
    f->weighted_pred_flag = p->weighted_pred_flag;
    f->weighted_bipred_flag = p->weighted_bipred_flag;
    f->transquant_bypass_enabled_flag = p->transquant_bypass_enabled_flag;
    f->tiles_enabled_flag = p->tiles_enabled_flag;
    f->entropy_coding_sync_enabled_flag = p->entropy_coding_sync_enabled_flag;
    f->uniform_spacing_flag = p->uniform_spacing_flag;
    f->loop_filter_across_tiles_enabled_flag = p->loop_filter_across_tiles_enabled_flag;
    f->pps_loop_filter_across_slices_enabled_flag =
        p->pps_loop_filter_across_slices_enabled_flag;
    f->deblocking_filter_override_enabled_flag =
        p->deblocking_filter_override_enabled_flag;
    f->pps_deblocking_filter_disabled_flag = p->pps_deblocking_filter_disabled_flag;
    f->pps_beta_offset_div2 = p->pps_beta_offset_div2;
    f->pps_tc_offset_div2 = p->pps_tc_offset_div2;
    f->lists_modification_present_flag = p->lists_modification_present_flag;
    f->log2_parallel_merge_level_minus2 = p->log2_parallel_merge_level_minus2;
    f->slice_segment_header_extension_present_flag =
        p->slice_segment_header_extension_present_flag;
    f->num_tile_columns_minus1 = p->num_tile_columns_minus1;
    f->num_tile_rows_minus1 = p->num_tile_rows_minus1;
    if (p->tiles_enabled_flag && !p->uniform_spacing_flag) {
        unsigned i;
        unsigned columns = p->num_tile_columns_minus1;
        unsigned rows = p->num_tile_rows_minus1;
        if (columns > VIRTIO_WDDM_ARRAY_COUNT(f->column_width_minus1) ||
            rows > VIRTIO_WDDM_ARRAY_COUNT(f->row_height_minus1)) {
            ERROR("%s: %u x %u tiles are outside the DXVA tables -> no VPS/SPS/PPS "
                  "synthesis", __FUNCTION__, columns, rows);
            return FALSE;
        }
        for (i = 0; i < columns; i++)
            f->column_width_minus1[i] = p->column_width_minus1[i];
        for (i = 0; i < rows; i++)
            f->row_height_minus1[i] = p->row_height_minus1[i];
    }

    if (f->pic_width_in_min_cbs == 0 || f->pic_height_in_min_cbs == 0) {
        ERROR("%s: DXVA_PicParams_HEVC has no picture size (%ux%u min-CBs) -> no "
              "VPS/SPS/PPS synthesis", __FUNCTION__, f->pic_width_in_min_cbs,
              f->pic_height_in_min_cbs);
        return FALSE;
    }
    /* HEVC-MAIN10: bit_depth_luma_minus8 is a *field the UMD acts on*, not just
     * data it copies.  0 = Main (8-bit), 2 = Main10 (10-bit); both are written
     * into the synthesised SPS and both are accepted here (the PTL profile idc
     * follows the depth in hevc_params.h).  Anything above 2 is Main12 or worse,
     * which no advertised profile covers, and a luma/chroma mismatch is a
     * broken PicParams.  The picture also has to be 4:2:0 single-plane: that is
     * what the NV12/P010 capture surfaces carry. */
    if (f->chroma_format_idc != 1u ||
        f->bit_depth_luma_minus8 != f->bit_depth_chroma_minus8 ||
        f->bit_depth_luma_minus8 > 2u || f->separate_colour_plane_flag) {
        ERROR("%s: chroma_format_idc=%u bit_depth=%u/%u separate_planes=%u is "
              "outside the advertised Main/Main10 profiles (8- or 10-bit 4:2:0) "
              "-> no synthesis", __FUNCTION__, f->chroma_format_idc,
              f->bit_depth_luma_minus8, f->bit_depth_chroma_minus8,
              f->separate_colour_plane_flag);
        return FALSE;
    }
    /* A 10-bit picture on the 8-bit Main profile would be decoded into an NV12
     * session: refuse it here (the caller falls back to the in-band parameter
     * sets, which is what a client that sends them needs) instead of letting the
     * host truncate the two LSBs. */
    if (object->bit_depth < 10u && f->bit_depth_luma_minus8 != 0u) {
        ERROR("%s: %u-bit PicParams on the 8-bit HEVC Main decoder -- the "
              "application should have selected HEVC Main10 with a P010 output "
              "surface -> no synthesis", __FUNCTION__,
              f->bit_depth_luma_minus8 + 8u);
        return FALSE;
    }
    return TRUE;
}

/* Synthesise this frame's Annex-B VPS+SPS+PPS from DXVA_PicParams_HEVC and
 * return their size (0 = refused/unavailable).  The bytes land in object->synth,
 * which is safe because a decoder object decodes exactly one codec (the H.264
 * path is the only other user of that buffer).
 *
 * hevc_params.h refuses streams whose slice headers cannot be reproduced (RPS
 * kept in the SPS, long-term reference pictures); that refusal is counted and
 * logged here and the caller then uses the access unit's own parameter sets. */
static UINT decoder_hevc_synth_parameter_sets(VIRTIO_WDDM_VideoDecoder *object,
                                              const UINT8 *au, UINT au_size) {
    unsigned vps_bytes = 0;
    unsigned sps_bytes = 0;
    unsigned pps_bytes = 0;
    unsigned total;
    unsigned pps_id;
    int have_slice = 0;

    object->synth_size = 0;
    if (!object->hevc_pic_valid)
        return 0;
    if (!decoder_hevc_fields_from_pic_params(object))
        return 0;
    if (!decoder_reserve(&object->synth, &object->synth_cap, HEVC_PARAMS_MAX_BYTES))
        return 0;

    /* The slice header names the PPS; DXVA_PicParams_HEVC does not. */
    pps_id = hevc_first_slice_pps_id(au, au_size, &have_slice);
    if (!have_slice) {
        VERBOSE("%s: no slice segment in the %u byte access unit, assuming pps id 0",
                __FUNCTION__, au_size);
        pps_id = 0;
    }
    object->hevc_fields.pps_id = pps_id;

    /* Crop the coded picture to the surface the runtime gave us, so the host
     * frame is exactly as large as the NV12 texture (a 1366-wide stream is coded
     * 1368 wide; a 1080p surface may be 1088). */
    object->hevc_fields.visible_width = object->desc.SampleWidth;
    object->hevc_fields.visible_height = object->desc.SampleHeight;
    object->hevc_fields.level_idc = 0; /* derive from the coded size */
    hevc_fields_finish(&object->hevc_fields);

    total = hevc_synth_vps_sps_pps(&object->hevc_fields, object->synth,
                                   object->synth_cap, &vps_bytes, &sps_bytes,
                                   &pps_bytes);
    if (total == 0) {
        object->hevc_synth_refusals++;
        if (object->hevc_param_logs < 8u) {
            object->hevc_param_logs++;
            ERROR("%s: VPS/SPS/PPS synthesis refused (sps_max_dec_pic_buffering=%u "
                  "%ux%u min-CBs, num_short_term_ref_pic_sets=%u, "
                  "long_term_ref_pics=%u/%u, tiles=%u) -> %llu refusal(s) so far; "
                  "the access unit's own parameter sets are used instead",
                  __FUNCTION__, object->hevc_fields.sps_max_dec_pic_buffering_minus1,
                  object->hevc_fields.pic_width_in_min_cbs,
                  object->hevc_fields.pic_height_in_min_cbs,
                  object->hevc_fields.num_short_term_ref_pic_sets,
                  object->hevc_fields.long_term_ref_pics_present_flag,
                  object->hevc_fields.num_long_term_ref_pics_sps,
                  object->hevc_fields.tiles_enabled_flag,
                  (unsigned long long)object->hevc_synth_refusals);
        }
        return 0;
    }
    object->synth_size = total;
    object->hevc_synth_injections++;

    if (object->hevc_param_logs < 6u) {
        object->hevc_param_logs++;
        INFO("%s: synthesized Annex-B VPS+SPS+PPS %u B (VPS %u, SPS %u, PPS %u), "
             "pps_id=%u level=%u coded=%ux%u crop=%ux%u, head=%02x %02x %02x %02x "
             "%02x %02x %02x %02x", __FUNCTION__, total, vps_bytes, sps_bytes,
             pps_bytes, pps_id, object->hevc_fields.level_idc,
             hevc_coded_width(&object->hevc_fields),
             hevc_coded_height(&object->hevc_fields),
             object->hevc_fields.visible_width, object->hevc_fields.visible_height,
             object->synth[0], object->synth[1], object->synth[2], object->synth[3],
             object->synth[4], object->synth[5], object->synth[6], object->synth[7]);
    } else {
        VERBOSE("%s: synthesized VPS+SPS+PPS %u B (pps_id=%u)", __FUNCTION__, total,
                pps_id);
    }
    return total;
}

/* P-BATCH HEVC: cache the VPS/SPS/PPS an access unit carries, like the H.264
 * decoder_update_parameter_sets() does for SPS/PPS.  A full triple replaces the
 * cache (a new sequence may bring a new SPS), a partial one is ignored. */
static void decoder_hevc_update_parameter_sets(VIRTIO_WDDM_VideoDecoder *object,
                                                const UINT8 *au, UINT size,
                                                HevcCollectedParams *info) {
    /* Collected on the stack first: the cache must survive an access unit that
     * carries only part of the triple (a P-frame never re-sends VPS/SPS/PPS, and
     * overwriting the cached bytes with nothing would break every later frame). */
    UINT8 collected[HEVC_PARAMS_MAX_BYTES];
    unsigned total;

    memset(info, 0, sizeof(*info));
    memset(collected, 0, sizeof(collected));
    total = hevc_collect_parameter_sets(au, size, collected, (unsigned)sizeof(collected),
                                        info);
    if (!info->vps_present || !info->sps_present || !info->pps_present)
        return;
    if (!decoder_reserve(&object->hevc_params, &object->hevc_params_cap, total))
        return;
    memcpy(object->hevc_params, collected, total);
    object->hevc_params_size = total;
    VERBOSE("%s: cached %u byte(s) of VPS/SPS/PPS from the access unit",
            __FUNCTION__, total);
}

/* ------------------------------------------------------------------------- */
/* P-BATCH AV1: DXVA_PicParams_AV1 -> the field sets av1_params.h consumes    */
/* ------------------------------------------------------------------------- */

/* Fill the sequence parameters from the client's parsed header.  FALSE = the
 * picture is outside the profile this UMD advertises (AV1 VLD Profile 0, 8-bit
 * 4:2:0), or the struct does not describe a usable sequence.  Called once per
 * frame (it is cheap) so a mid-stream resolution or tool change is picked up;
 * the sequence header itself is re-emitted only when these values change. */
static BOOL av1_seq_from_pic(Av1SeqParams *seq, const DXVA_PicParams_AV1 *p) {
    unsigned max_w, max_h;

    memset(seq, 0, sizeof(*seq));
    if (p->seq_profile != 0) {
        ERROR("%s: seq_profile=%u is outside the advertised AV1 VLD Profile 0 "
              "(8-bit 4:2:0) -> E_FAIL", __FUNCTION__, (UINT)p->seq_profile);
        return FALSE;
    }
    if (p->bitdepth != 8) {
        ERROR("%s: bitdepth=%u is outside the advertised AV1 VLD Profile 0 "
              "(8-bit) -> E_FAIL", __FUNCTION__, (UINT)p->bitdepth);
        return FALSE;
    }
    if (p->format.mono_chrome) {
        ERROR("%s: monochrome AV1 cannot be written into the NV12 output "
              "surface -> E_FAIL", __FUNCTION__);
        return FALSE;
    }
    if (p->format.subsampling_x != 1 || p->format.subsampling_y != 1) {
        ERROR("%s: chroma subsampling %ux%u is outside the advertised profile "
              "(4:2:0) -> E_FAIL", __FUNCTION__,
              (UINT)p->format.subsampling_x, (UINT)p->format.subsampling_y);
        return FALSE;
    }
    max_w = p->max_width ? p->max_width : p->width;
    max_h = p->max_height ? p->max_height : p->height;
    if (max_w == 0 || max_h == 0 || max_w > 16384u || max_h > 8704u) {
        ERROR("%s: unusable sequence size %ux%u -> E_FAIL", __FUNCTION__, max_w,
              max_h);
        return FALSE;
    }

    seq->profile = 0;
    seq->bit_depth = 8;
    seq->subsampling_x = 1;
    seq->subsampling_y = 1;
    seq->max_width = max_w;
    seq->max_height = max_h;
    seq->use_128x128_superblock = p->coding.use_128x128_superblock;
    seq->enable_filter_intra = p->coding.filter_intra;
    seq->enable_intra_edge_filter = p->coding.intra_edge_filter;
    seq->enable_interintra_compound = p->coding.interintra_compound;
    seq->enable_masked_compound = p->coding.masked_compound;
    seq->enable_dual_filter = p->coding.dual_filter;
    seq->enable_jnt_comp = p->coding.jnt_comp;
    seq->enable_ref_frame_mvs = p->coding.enable_ref_frame_mvs;
    /* DXVA_PicParams_AV1 has no enable_order_hint field: a non-zero
     * order_hint_bits is the only signal that the sequence enabled order
     * hints (Chromium fills it from seq_header.order_hint_bits). */
    seq->enable_order_hint = p->order_hint_bits ? 1u : 0u;
    seq->order_hint_bits = p->order_hint_bits;
    seq->enable_cdef = p->coding.cdef;
    seq->enable_restoration = p->coding.restoration;
    seq->film_grain_params_present = p->coding.film_grain;
    /* DXVA carries no colour range; studio (0) is what every 8-bit 4:2:0
     * browser stream uses and it does not change the decoded samples. */
    seq->color_range = 0;
    seq->level_idx = av1_level_from_size(max_w, max_h, p->tiles.cols, p->tiles.rows);
    return TRUE;
}

static BOOL av1_seq_same(const Av1SeqParams *a, const Av1SeqParams *b) {
    return memcmp(a, b, sizeof(*a)) == 0 ? TRUE : FALSE;
}

/* Fill one picture's frame-header fields.  `tiles` is the already-converted
 * Av1Tile array (see the in-place conversion in EndFrame). */
static BOOL av1_frame_from_pic(Av1FrameParams *f, const DXVA_PicParams_AV1 *p,
                               const Av1Tile *tiles, UINT ntiles, UINT surface_w,
                               UINT surface_h) {
    unsigned i, j;

    memset(f, 0, sizeof(*f));
    f->frame_type = p->format.frame_type;
    if (f->frame_type > AV1_FRAME_SWITCH) {
        ERROR("%s: frame_type=%u is not an AV1 frame type -> E_FAIL", __FUNCTION__,
              f->frame_type);
        return FALSE;
    }
    /* Forced: the host is a stateful V4L2 decoder and only produces a CAPTURE
     * frame for a shown picture, while D3D11 calls EndFrame once per submitted
     * picture and expects a decoded surface back.  Decoding is identical for a
     * shown and a hidden picture (libva-v4l2 makes the same choice). */
    f->show_frame = 1;
    /* DXVA_PicParams_AV1 carries no error_resilient_mode bit.  The common
     * browser streams use 0 for inter pictures; a shown key frame infers 1
     * from the syntax.  This is a documented limit of the D3D11 AV1 interface
     * (see deliverables/P-BATCH-AV1.md). */
    f->error_resilient_mode = 0;
    f->disable_cdf_update = p->coding.disable_cdf_update;
    f->allow_screen_content_tools = p->coding.screen_content_tools;
    f->force_integer_mv = p->coding.integer_mv;
    f->order_hint = p->order_hint;
    f->primary_ref_frame = p->primary_ref_frame;
    f->width = p->width ? p->width : surface_w;
    f->height = p->height ? p->height : surface_h;
    if (f->width == 0 || f->height == 0 || f->width > surface_w ||
        f->height > surface_h) {
        ERROR("%s: frame %ux%u does not fit the %ux%u descriptor/NV12 surface "
              "-> E_FAIL", __FUNCTION__, f->width, f->height, surface_w, surface_h);
        return FALSE;
    }
    f->superres_denom = p->superres_denom ? p->superres_denom : AV1_SUPERRES_NUM;
    if (f->superres_denom != AV1_SUPERRES_NUM &&
        (f->superres_denom < AV1_SUPERRES_DENOM_MIN || f->superres_denom > 16)) {
        ERROR("%s: superres_denom=%u is outside 8..16 -> E_FAIL", __FUNCTION__,
              f->superres_denom);
        return FALSE;
    }
    for (i = 0; i < AV1_REFS_PER_FRAME; i++)
        f->ref_frame_idx[i] = p->frame_refs[i].Index & 7u;
    f->allow_high_precision_mv = p->coding.high_precision_mv;
    f->interpolation_filter = p->interp_filter;
    f->is_motion_mode_switchable = p->coding.switchable_motion_mode;
    f->use_ref_frame_mvs = p->coding.use_ref_frame_mvs;
    f->allow_intrabc = p->coding.intrabc;
    f->disable_frame_end_update_cdf = p->coding.disable_frame_end_update_cdf;
    f->allow_warped_motion = p->coding.warped_motion;
    f->reduced_tx_set = p->coding.reduced_tx_set;

    f->tile_cols = p->tiles.cols ? p->tiles.cols : 1u;
    f->tile_rows = p->tiles.rows ? p->tiles.rows : 1u;
    if (f->tile_cols > AV1_MAX_TILE_COLS || f->tile_rows > AV1_MAX_TILE_ROWS ||
        f->tile_cols * f->tile_rows > AV1_MAX_TILES) {
        ERROR("%s: tiling %ux%u is outside the AV1 limits -> E_FAIL", __FUNCTION__,
              f->tile_cols, f->tile_rows);
        return FALSE;
    }
    for (i = 0; i < f->tile_cols; i++)
        f->tile_width_sb[i] = p->tiles.widths[i];
    for (i = 0; i < f->tile_rows; i++)
        f->tile_height_sb[i] = p->tiles.heights[i];
    f->context_update_tile_id = p->tiles.context_update_id;
    f->tile_size_bytes = av1_tile_size_bytes(tiles, ntiles);

    f->base_q_idx = p->quantization.base_qindex;
    f->delta_q_y_dc = p->quantization.y_dc_delta_q;
    f->delta_q_u_dc = p->quantization.u_dc_delta_q;
    f->delta_q_u_ac = p->quantization.u_ac_delta_q;
    f->delta_q_v_dc = p->quantization.v_dc_delta_q;
    f->delta_q_v_ac = p->quantization.v_ac_delta_q;
    /* Chromium writes 0xFF into qm_y/qm_u/qm_v when the frame does not use a
     * quantisation matrix; that is the only "using_qmatrix" signal DXVA has. */
    f->using_qmatrix = (p->quantization.qm_y != 0xFFu) ? 1u : 0u;
    if (f->using_qmatrix) {
        f->qm_y = p->quantization.qm_y & 15u;
        f->qm_u = p->quantization.qm_u & 15u;
        f->qm_v = p->quantization.qm_v & 15u;
    }
    f->delta_q_present = p->quantization.ControlFlags & 1u;
    f->delta_q_res = p->quantization.delta_q_res;

    f->segmentation_enabled = p->segmentation.ControlFlags & 1u;
    f->segmentation_update_map = (p->segmentation.ControlFlags >> 1) & 1u;
    f->segmentation_temporal_update = (p->segmentation.ControlFlags >> 3) & 1u;
    for (i = 0; i < AV1_MAX_SEGMENTS; i++) {
        f->feature_mask[i] = p->segmentation.feature_mask[i].mask;
        for (j = 0; j < AV1_SEG_LVL_MAX; j++)
            f->feature_data[i][j] = p->segmentation.feature_data[i][j];
    }

    f->filter_level[0] = p->loop_filter.filter_level[0];
    f->filter_level[1] = p->loop_filter.filter_level[1];
    f->filter_level[2] = p->loop_filter.filter_level_u;
    f->filter_level[3] = p->loop_filter.filter_level_v;
    f->sharpness_level = p->loop_filter.sharpness_level;
    f->mode_ref_delta_enabled = p->loop_filter.ControlFlags & 1u;
    for (i = 0; i < AV1_TOTAL_REFS_PER_FRAME; i++)
        f->ref_deltas[i] = p->loop_filter.ref_deltas[i];
    f->mode_deltas[0] = p->loop_filter.mode_deltas[0];
    f->mode_deltas[1] = p->loop_filter.mode_deltas[1];
    f->delta_lf_present = (p->loop_filter.ControlFlags >> 3) & 1u;
    f->delta_lf_multi = (p->loop_filter.ControlFlags >> 2) & 1u;
    f->delta_lf_res = p->loop_filter.delta_lf_res;
    for (i = 0; i < 3; i++) {
        f->frame_restoration_type[i] = p->loop_filter.frame_restoration_type[i];
        f->log2_restoration_unit_size[i] =
            p->loop_filter.log2_restoration_unit_size[i];
    }

    /* The DXVA field is damping MINUS 3 (Chromium subtracts it explicitly);
     * av1_params.h writes the syntax value, i.e. damping - 3. */
    f->cdef_damping = (p->cdef.ControlFlags & 3u) + 3u;
    f->cdef_bits = (p->cdef.ControlFlags >> 2) & 3u;
    for (i = 0; i < 8; i++) {
        f->cdef_y_strengths[i] = p->cdef.y_strengths[i].combined;
        f->cdef_uv_strengths[i] = p->cdef.uv_strengths[i].combined;
    }

    f->tx_mode = p->coding.tx_mode;
    f->reference_select = p->coding.reference_mode;
    f->skip_mode_present = p->coding.skip_mode;

    for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
        f->wm_type[i] = p->frame_refs[i].wminvalid ? AV1_WM_IDENTITY
                                                   : p->frame_refs[i].wmtype;
        for (j = 0; j < 6; j++)
            f->wm_params[i][j] = p->frame_refs[i].wmmat[j];
    }

    if (p->film_grain.apply_grain) {
        const DXVA_PicParams_AV1 *fgp = p;
        f->apply_grain = 1;
        f->grain_seed = p->film_grain.grain_seed;
        f->scaling_shift_minus8 = p->film_grain.scaling_shift_minus8;
        f->chroma_scaling_from_luma = p->film_grain.chroma_scaling_from_luma;
        f->ar_coeff_lag = p->film_grain.ar_coeff_lag;
        f->ar_coeff_shift_minus6 = p->film_grain.ar_coeff_shift_minus6;
        f->grain_scale_shift = p->film_grain.grain_scale_shift;
        f->overlap_flag = p->film_grain.overlap_flag;
        f->clip_to_restricted_range = p->film_grain.clip_to_restricted_range;
        f->num_y_points = p->film_grain.num_y_points;
        for (i = 0; i < f->num_y_points && i < 14u; i++) {
            f->point_y_value[i] = p->film_grain.scaling_points_y[i][0];
            f->point_y_scaling[i] = p->film_grain.scaling_points_y[i][1];
        }
        f->num_cb_points = p->film_grain.num_cb_points;
        for (i = 0; i < f->num_cb_points && i < 10u; i++) {
            f->point_cb_value[i] = p->film_grain.scaling_points_cb[i][0];
            f->point_cb_scaling[i] = p->film_grain.scaling_points_cb[i][1];
        }
        f->num_cr_points = p->film_grain.num_cr_points;
        for (i = 0; i < f->num_cr_points && i < 10u; i++) {
            f->point_cr_value[i] = p->film_grain.scaling_points_cr[i][0];
            f->point_cr_scaling[i] = p->film_grain.scaling_points_cr[i][1];
        }
        /* DXVA already stores the "+128" biased AR coefficients and the
         * unbiased multipliers/offsets (Chromium adds/subtracts exactly those
         * biases when it fills the struct), so they are written as they are. */
        for (i = 0; i < 24; i++)
            f->ar_coeffs_y[i] = (int)fgp->film_grain.ar_coeffs_y[i];
        for (i = 0; i < 25; i++) {
            f->ar_coeffs_cb[i] = (int)fgp->film_grain.ar_coeffs_cb[i];
            f->ar_coeffs_cr[i] = (int)fgp->film_grain.ar_coeffs_cr[i];
        }
        f->cb_mult = p->film_grain.cb_mult;
        f->cb_luma_mult = p->film_grain.cb_luma_mult;
        f->cb_offset = p->film_grain.cb_offset & 0x1FF;
        f->cr_mult = p->film_grain.cr_mult;
        f->cr_luma_mult = p->film_grain.cr_luma_mult;
        f->cr_offset = p->film_grain.cr_offset & 0x1FF;
    }
    return TRUE;
}

/* DecoderBeginFrame: validate the output view the runtime selected and open the
 * frame. The texture itself is only touched in EndFrame; there is no CPU
 * mapping here. */
static HRESULT APIENTRY virtio_wddm_video_video_decoder_begin_frame(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode,
    CONST D3D11_1DDIARG_VIDEODECODERBEGINFRAME *pArgs) {
    (void)hDevice;
    VIRTIO_WDDM_VideoDecoder *object = hDecode.pDrvPrivate;
    VIRTIO_WDDM_VideoDecoderOutputView *view;

    VIRTIO_WDDM_DDI_ENTRY("hDecode=%p pArgs=%p priv=%p frame_open=%d submits=%llu",
                          (void *)hDecode.pDrvPrivate, (const void *)pArgs, (void *)object,
                          object ? (int)object->frame_open : -1,
                          object ? (unsigned long long)object->submits : 0ull);

    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECODE || !pArgs) {
        ERROR("%s: invalid decoder %p / args %p -> E_INVALIDARG", __FUNCTION__,
              (void *)object, (void *)pArgs);
        return E_INVALIDARG;
    }
    /* P2-2i: surface the previous EndFrame's dropped frame. This is the only
     * HRESULT-bearing DDI in the decode sequence, so the failure reaches the
     * application here instead of through pfnSetErrorCb, which would remove the
     * whole D3D11 device. Only one frame is reported: the next BeginFrame is
     * allowed to proceed normally. */
    if (object->pending_error != S_OK) {
        HRESULT pending = object->pending_error;
        object->pending_error = S_OK;
        ERROR("%s: previous EndFrame dropped a frame (hr=0x%08lx, %llu so far); "
              "returning it here -- device and session stay alive (P2-2i)",
              __FUNCTION__, (unsigned long)pending,
              (unsigned long long)object->decode_failures);
        return pending;
    }
    if (object->frame_open) {
        ERROR("%s: BeginFrame while a frame is still open (submits=%llu) -> E_FAIL",
              __FUNCTION__, (unsigned long long)object->submits);
        return E_FAIL;
    }
    if (pArgs->pContentKey || pArgs->ContentKeySize != 0) {
        ERROR("%s: content-keyed decode is not supported (%u byte key) -> E_FAIL",
              __FUNCTION__, pArgs->ContentKeySize);
        return E_FAIL;
    }

    view = pArgs->hOutputView.pDrvPrivate;
    if (!view || view->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECOUT ||
        !view->resource.pDrvPrivate) {
        ERROR("%s: no usable decoder output view (view=%p) -> E_INVALIDARG",
              __FUNCTION__, (void *)view);
        return E_INVALIDARG;
    }

    object->output_view = pArgs->hOutputView;
    object->frame_open = TRUE;
    object->frame_sent = FALSE;
    object->bitstream_size = 0;
    /* P3-3: a PICTURE_PARAMETERS buffer only describes THIS frame; do not judge
     * the next frame_num delta against a previous frame's picture. */
    object->pic_this_frame = FALSE;
    /* P-BATCH VP9: SLICE_CONTROL records and the assembled access unit belong to
     * one frame only.  Keeping last frame's records would splice its bytes into
     * this frame's access unit (the BITSTREAM buffer is reset above, so the
     * offsets would even look valid). */
    object->slices_size = 0;
    object->vp9_au_size = 0;
    object->vp9_pic_this_frame = FALSE;
    /* P-BATCH HEVC: same rule for the HEVC picture parameters and the assembled
     * access unit. */
    object->hevc_pic_this_frame = FALSE;
    object->hevc_au_size = 0;
    /* P-BATCH AV1: same rule: the picture parameters, the tile records and the
     * assembled temporal unit belong to one frame only.  The DPB mirror and the
     * pending catch-up frame deliberately survive (they describe the stream,
     * not the frame). */
    object->av1_pic_this_frame = FALSE;
    object->av1_tiles_size = 0;
    object->av1_au_size = 0;

    if (object->frames == 0) {
        VIRTIO_WDDM_Resource *ovres = view->resource.pDrvPrivate;
        /* P2-2J diag v2: log the D3D11 texture (not just the DDI wrapper) so the
         * decoder side and the video-processor input view can be compared in the
         * SAME pointer space: res= is the wrapper (VIRTIO_WDDM_Resource*), tex=
         * is the ID3D11Resource*. */
        INFO("%s: first frame: output view=%p res=%p tex=%p (P2-2b decode path active)",
             __FUNCTION__, (void *)view, (void *)ovres,
             ovres ? (void *)ovres->base.pResource : (void *)NULL);
    }
    return S_OK;
}

/* SubmitDecoderBuffers: collect the Annex-B bytes. The application hands us
 * D3D11 buffer resources with DataOffset/DataSize, possibly several per frame
 * (one per NAL/slice); they are copied out and appended into one access unit.
 *
 * The vm_send() itself happens in EndFrame, after the last Submit of the frame:
 * sending from here would push a partial access unit whenever the runtime
 * splits one frame over more than one Submit call, and the host decoder takes
 * one complete access unit per send. */
static HRESULT APIENTRY virtio_wddm_video_video_decoder_submit_buffers(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode, UINT count,
    CONST D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *pBuffers) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    VIRTIO_WDDM_VideoDecoder *object = hDecode.pDrvPrivate;
    UINT i;
    UINT appended = 0;
    UINT segments = 0;

    VIRTIO_WDDM_DDI_ENTRY("hDecode=%p count=%u pBuffers=%p priv=%p frame_open=%d",
                          (void *)hDecode.pDrvPrivate, count, (const void *)pBuffers,
                          (void *)object, object ? (int)object->frame_open : -1);

    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECODE || !device) {
        ERROR("%s: invalid decoder %p -> E_INVALIDARG", __FUNCTION__, (void *)object);
        return E_INVALIDARG;
    }
    if (!object->frame_open) {
        ERROR("%s: SubmitDecoderBuffers outside BeginFrame/EndFrame -> E_FAIL",
              __FUNCTION__);
        return E_FAIL;
    }
    if (!pBuffers || count == 0) {
        ERROR("%s: no buffers (count=%u) -> E_INVALIDARG", __FUNCTION__, count);
        return E_INVALIDARG;
    }

    for (i = 0; i < count; i++) {
        CONST D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *buffer = &pBuffers[i];
        VIRTIO_WDDM_Resource *resource;
        HRESULT hr;

        /* P2-2i: the client's parsed parameter sets. Chromium fills this buffer
         * with DXVA_PicParams_H264 and sends no SPS/PPS in BITSTREAM, so this is
         * the only place the parameter sets exist for a browser decode. A short
         * or unreadable buffer is logged and ignored: the frame can still use
         * the cached/stream parameter sets, if any. */
        if (buffer->BufferType == D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS) {
            UINT copy;

            if (buffer->DataSize == 0)
                continue;
            resource = buffer->hResource.pDrvPrivate;
            if (!resource || !resource->base.pResource) {
                ERROR("%s: PICTURE_PARAMETERS has no resource (hResource=%p) -> "
                      "ignored", __FUNCTION__, (void *)resource);
                continue;
            }
            /* P-BATCH AV1: DXVA_PicParams_AV1 is the parsed sequence + frame
             * header -- the ONLY description of the picture a D3D11 AV1 client
             * sends, because it also sends no header bytes (see av1_params.h).
             * A short buffer is refused loudly: a smaller struct would be
             * copied field-for-field from the wrong offsets. */
            if (object->codec == VIRTIO_WDDM_CODEC_AV1) {
                if (buffer->DataSize < sizeof(DXVA_PicParams_AV1)) {
                    ERROR("%s: PICTURE_PARAMETERS is only %u B (< %u B "
                          "DXVA_PicParams_AV1) -> ignored", __FUNCTION__,
                          buffer->DataSize, (UINT)sizeof(DXVA_PicParams_AV1));
                    continue;
                }
                copy = (UINT)sizeof(DXVA_PicParams_AV1);
                memset(&object->av1_pic, 0, sizeof(object->av1_pic));
                hr = virtio_wddm_dxvk_video_read_buffer(
                    device->base.pDev1, device->base.pCtx1, resource->base.pResource,
                    buffer->DataOffset, copy, &object->av1_pic);
                if (FAILED(hr)) {
                    ERROR("%s: copying %u B of DXVA_PicParams_AV1 (offset %u) failed: "
                          "0x%08lx -> ignored", __FUNCTION__, copy, buffer->DataOffset,
                          (unsigned long)hr);
                    continue;
                }
                object->av1_pic_valid = TRUE;
                object->av1_pic_this_frame = TRUE;
                object->av1_pic_buffers++;
                if (object->av1_pic_buffers == 1) {
                    INFO("%s: DXVA_PicParams_AV1 %u B: profile=%u %ux%u (max %ux%u) "
                         "bitdepth=%u frame_type=%u show=%u err_res=%u "
                         "order_hint=%u/%u primary_ref=%u tiles=%ux%u "
                         "film_grain=%u apply=%u (OBUs will be rebuilt)",
                         __FUNCTION__, buffer->DataSize,
                         (UINT)object->av1_pic.seq_profile,
                         (UINT)object->av1_pic.width, (UINT)object->av1_pic.height,
                         (UINT)object->av1_pic.max_width,
                         (UINT)object->av1_pic.max_height,
                         (UINT)object->av1_pic.bitdepth,
                         (UINT)object->av1_pic.format.frame_type,
                         (UINT)object->av1_pic.format.show_frame,
                         (UINT)object->av1_pic.coding.reference_mode,
                         (UINT)object->av1_pic.order_hint,
                         (UINT)object->av1_pic.order_hint_bits,
                         (UINT)object->av1_pic.primary_ref_frame,
                         (UINT)object->av1_pic.tiles.cols,
                         (UINT)object->av1_pic.tiles.rows,
                         (UINT)object->av1_pic.coding.film_grain,
                         (UINT)object->av1_pic.film_grain.ControlFlags);
                }
                continue;
            }
            /* P-BATCH VP9: the VP9 picture parameters are a different structure
             * (DXVA_PicParams_VP9).  Nothing has to be rebuilt from them -- VP9
             * carries its own headers -- but they are the only place the client
             * tells us the profile, the bit depth and the coded size, so they are
             * kept for validation and for the sequence-start decision, and the
             * first one is logged so the log shows what the client believed. */
            if (object->codec == VIRTIO_WDDM_CODEC_VP9) {
                if (buffer->DataSize < sizeof(DXVA_PicParams_VP9)) {
                    /* VP9FIX: this used to be "ignored", which let the frame
                     * continue with the previous frame's picture parameters (or
                     * none at all) while the client believed it had described
                     * the picture.  A short struct is a client/ABI mismatch and
                     * is now refused with E_INVALIDARG, exactly like the other
                     * out-of-bounds cases in this file. */
                    ERROR("%s: VP9 PICTURE_PARAMETERS is only %u B (< %u B "
                          "DXVA_PicParams_VP9) -> E_INVALIDARG", __FUNCTION__,
                          buffer->DataSize, (UINT)sizeof(DXVA_PicParams_VP9));
                    return E_INVALIDARG;
                }
                copy = (UINT)sizeof(DXVA_PicParams_VP9);
                memset(&object->vp9_pic, 0, sizeof(object->vp9_pic));
                hr = virtio_wddm_dxvk_video_read_buffer(
                    device->base.pDev1, device->base.pCtx1, resource->base.pResource,
                    buffer->DataOffset, copy, &object->vp9_pic);
                if (FAILED(hr)) {
                    ERROR("%s: copying %u B of DXVA_PicParams_VP9 (offset %u) failed: "
                          "0x%08lx -> E_INVALIDARG", __FUNCTION__, copy,
                          buffer->DataOffset, (unsigned long)hr);
                    return E_INVALIDARG;
                }
                object->vp9_pic_valid = TRUE;
                object->vp9_pic_this_frame = TRUE;
                object->vp9_pic_buffers++;
                if (object->vp9_pic_buffers == 1) {
                    INFO("%s: DXVA_PicParams_VP9 %u B: profile=%u %ux%u "
                         "bitdepth_minus8=%u/%u frame_type=%u show_frame=%u "
                         "error_resilient=%u intra_only=%u subsampling=%u%u "
                         "tiles=%ux%u (log2 %u/%u) header=%u B first_partition=%u B",
                         __FUNCTION__, buffer->DataSize,
                         (UINT)object->vp9_pic.profile, object->vp9_pic.width,
                         object->vp9_pic.height,
                         (UINT)object->vp9_pic.BitDepthMinus8Luma,
                         (UINT)object->vp9_pic.BitDepthMinus8Chroma,
                         (UINT)object->vp9_pic.frame_type,
                         (UINT)object->vp9_pic.show_frame,
                         (UINT)object->vp9_pic.error_resilient_mode,
                         (UINT)object->vp9_pic.intra_only,
                         (UINT)object->vp9_pic.subsampling_x,
                         (UINT)object->vp9_pic.subsampling_y,
                         1u << (object->vp9_pic.log2_tile_cols <= 6u
                                    ? object->vp9_pic.log2_tile_cols
                                    : 6u),
                         1u << (object->vp9_pic.log2_tile_rows <= 2u
                                    ? object->vp9_pic.log2_tile_rows
                                    : 2u),
                         (UINT)object->vp9_pic.log2_tile_cols,
                         (UINT)object->vp9_pic.log2_tile_rows,
                         (UINT)object->vp9_pic.uncompressed_header_size_byte_aligned,
                         (UINT)object->vp9_pic.first_partition_size);
                }
                continue;
            }
            /* P-BATCH HEVC: the HEVC picture parameters (DXVA_PicParams_HEVC) are
             * the SPS+PPS transcription the synthesised VPS+SPS+PPS are built
             * from, so they are copied and kept for EndFrame.  A short buffer is
             * refused loudly: a client that sends an older/smaller struct would
             * otherwise make us synthesise headers from the wrong bytes. */
            if (object->codec == VIRTIO_WDDM_CODEC_HEVC) {
                if (buffer->DataSize < sizeof(DXVA_PicParams_HEVC)) {
                    ERROR("%s: PICTURE_PARAMETERS is only %u B (< %u B "
                          "DXVA_PicParams_HEVC) -> ignored", __FUNCTION__,
                          buffer->DataSize, (UINT)sizeof(DXVA_PicParams_HEVC));
                    continue;
                }
                copy = (UINT)sizeof(DXVA_PicParams_HEVC);
                memset(&object->hevc_pic, 0, sizeof(object->hevc_pic));
                hr = virtio_wddm_dxvk_video_read_buffer(
                    device->base.pDev1, device->base.pCtx1, resource->base.pResource,
                    buffer->DataOffset, copy, &object->hevc_pic);
                if (FAILED(hr)) {
                    ERROR("%s: copying %u B of DXVA_PicParams_HEVC (offset %u) failed: "
                          "0x%08lx -> ignored", __FUNCTION__, copy, buffer->DataOffset,
                          (unsigned long)hr);
                    continue;
                }
                object->hevc_pic_valid = TRUE;
                object->hevc_pic_this_frame = TRUE;
                /* HEVC-MAIN10: the depth the client's PicParams declares, once
                 * per decoder, and whether this decoder can carry it.  A 10-bit
                 * PicParams on the 8-bit Main profile is not fatal (a client may
                 * have picked Main for a Main10 asset when no P010 surface was
                 * available): the host session is NV12, so the picture is
                 * decoded at 8 bits and the P010 conversion is the b << 8 one.
                 * > 10 bits is outside every advertised profile and is called
                 * out as an error, but the frame still goes to the host -- it is
                 * the host that has to refuse it. */
                if (object->hevc_pic_buffers == 0) {
                    if (object->hevc_pic.bit_depth_luma_minus8 > 2u) {
                        ERROR("%s: DXVA_PicParams_HEVC declares %u-bit samples "
                              "(bit_depth_luma_minus8=%u); only Main (8-bit) and "
                              "Main10 (10-bit) are advertised -> the host decides "
                              "whether it can decode this", __FUNCTION__,
                              object->hevc_pic.bit_depth_luma_minus8 + 8u,
                              (UINT)object->hevc_pic.bit_depth_luma_minus8);
                    } else if (object->hevc_pic.bit_depth_luma_minus8 != 0u &&
                               object->bit_depth < 10u) {
                        INFO("%s: 10-bit HEVC on the 8-bit Main decoder "
                             "(bit_depth_luma_minus8=%u): the NV12 capture can "
                             "only carry 8 bits -- select HEVC Main10 with a P010 "
                             "output surface for the full depth", __FUNCTION__,
                             (UINT)object->hevc_pic.bit_depth_luma_minus8);
                    } else if (object->hevc_pic.bit_depth_luma_minus8 != 0u) {
                        INFO("%s: HEVC Main10 decoder carrying %u-bit samples "
                             "(bit_depth_luma_minus8=%u, output %s)", __FUNCTION__,
                             object->hevc_pic.bit_depth_luma_minus8 + 8u,
                             (UINT)object->hevc_pic.bit_depth_luma_minus8,
                             (DXGI_FORMAT)object->desc.OutputFormat == DXGI_FORMAT_P010
                                 ? "P010"
                                 : "NV12");
                    }
                }
                object->hevc_pic_buffers++;
                if (object->hevc_pic_buffers == 1) {
                    INFO("%s: DXVA_PicParams_HEVC %u B: %ux%u min-CBs "
                         "chroma=%u depth=%u/%u log2_poc=%u bufs=%u "
                         "num_st_rps=%u lt_sps=%u tiles=%u sao=%u Irap=%u Idr=%u "
                         "Intra=%u poc=%d (VPS/SPS/PPS will be synthesised)",
                         __FUNCTION__, buffer->DataSize,
                         (UINT)object->hevc_pic.PicWidthInMinCbsY,
                         (UINT)object->hevc_pic.PicHeightInMinCbsY,
                         (UINT)object->hevc_pic.chroma_format_idc,
                         (UINT)object->hevc_pic.bit_depth_luma_minus8,
                         (UINT)object->hevc_pic.bit_depth_chroma_minus8,
                         (UINT)object->hevc_pic.log2_max_pic_order_cnt_lsb_minus4,
                         (UINT)object->hevc_pic.sps_max_dec_pic_buffering_minus1,
                         (UINT)object->hevc_pic.num_short_term_ref_pic_sets,
                         (UINT)object->hevc_pic.num_long_term_ref_pics_sps,
                         (UINT)object->hevc_pic.tiles_enabled_flag,
                         (UINT)object->hevc_pic.sample_adaptive_offset_enabled_flag,
                         (UINT)object->hevc_pic.IrapPicFlag,
                         (UINT)object->hevc_pic.IdrPicFlag,
                         (UINT)object->hevc_pic.IntraPicFlag,
                         (int)object->hevc_pic.CurrPicOrderCntVal);
                }
                continue;
            }
            if (buffer->DataSize < sizeof(DXVA_PicParams_H264)) {
                ERROR("%s: PICTURE_PARAMETERS is only %u B (< %u B "
                      "DXVA_PicParams_H264) -> ignored", __FUNCTION__,
                      buffer->DataSize, (UINT)sizeof(DXVA_PicParams_H264));
                continue;
            }
            copy = (UINT)sizeof(DXVA_PicParams_H264);
            memset(&object->pic, 0, sizeof(object->pic));
            hr = virtio_wddm_dxvk_video_read_buffer(
                device->base.pDev1, device->base.pCtx1, resource->base.pResource,
                buffer->DataOffset, copy, &object->pic);
            if (FAILED(hr)) {
                ERROR("%s: copying %u B of PICTURE_PARAMETERS (offset %u) failed: "
                      "0x%08lx -> ignored", __FUNCTION__, copy, buffer->DataOffset,
                      (unsigned long)hr);
                continue;
            }
            object->pic_valid = TRUE;
            object->pic_this_frame = TRUE;
            object->pic_buffers++;
            if (object->pic_buffers == 1) {
                INFO("%s: PICTURE_PARAMETERS %u B: %ux%u map units, chroma=%u "
                     "bitdepth=%u/%u poc_type=%u frame_mbs_only=%u entropy=%u "
                     "transform_8x8=%u (SPS/PPS will be synthesised; profile/level "
                     "are not part of DXVA_PicParams_H264)", __FUNCTION__,
                     buffer->DataSize,
                     (UINT)object->pic.wFrameWidthInMbsMinus1 + 1u,
                     (UINT)object->pic.wFrameHeightInMbsMinus1 + 1u,
                     (UINT)object->pic.chroma_format_idc,
                     (UINT)object->pic.bit_depth_luma_minus8,
                     (UINT)object->pic.bit_depth_chroma_minus8,
                     (UINT)object->pic.pic_order_cnt_type,
                     (UINT)object->pic.frame_mbs_only_flag,
                     (UINT)object->pic.entropy_coding_mode_flag,
                     (UINT)object->pic.transform_8x8_mode_flag);
            }
            continue;
        }

        /* P-BATCH VP9: SLICE_CONTROL carries DXVA_Slice_VPx_Short records (10 B
         * each: BSNALunitDataLocation, SliceBytesInBuffer, wBadSliceChopping) that
         * point into the BITSTREAM buffer.  They are copied verbatim here and
         * resolved in EndFrame, where the access unit is assembled in record
         * order -- the BITSTREAM buffer alone is not reliable for that, because
         * the client may place the frames of a superframe in any order and may
         * leave padding between them. */
        if (buffer->BufferType == D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL &&
            object->codec == VIRTIO_WDDM_CODEC_VP9) {
            UINT copy = buffer->DataSize;

            if (copy == 0)
                continue;
            /* VP9FIX: the client's record list must fit the 16 KiB buffer the
             * decoder advertises and contain a whole number of 10-byte records.
             * The old code truncated a larger buffer to 16 KiB and kept a
             * partial trailing record, i.e. it could resolve a record from half
             * a record's bytes and then index into BITSTREAM with a garbage
             * offset/length.  Both are now E_INVALIDARG. */
            if (copy > VIRTIO_WDDM_VP9_SLICE_CONTROL_BYTES) {
                ERROR("%s: VP9 SLICE_CONTROL is %u B (more than the %u B buffer "
                      "reported by GetVideoDecoderBufferInfo) -> E_INVALIDARG",
                      __FUNCTION__, copy, (UINT)VIRTIO_WDDM_VP9_SLICE_CONTROL_BYTES);
                return E_INVALIDARG;
            }
            if ((copy % (UINT)sizeof(DXVA_Slice_VPx_Short)) != 0) {
                ERROR("%s: VP9 SLICE_CONTROL is %u B, not a whole number of %u B "
                      "DXVA_Slice_VPx_Short records -> E_INVALIDARG", __FUNCTION__,
                      copy, (UINT)sizeof(DXVA_Slice_VPx_Short));
                return E_INVALIDARG;
            }
            if (copy / (UINT)sizeof(DXVA_Slice_VPx_Short) >
                VIRTIO_WDDM_VP9_MAX_SLICE_RECORDS) {
                ERROR("%s: VP9 SLICE_CONTROL has %u records (max %u) -> "
                      "E_INVALIDARG", __FUNCTION__,
                      copy / (UINT)sizeof(DXVA_Slice_VPx_Short),
                      (UINT)VIRTIO_WDDM_VP9_MAX_SLICE_RECORDS);
                return E_INVALIDARG;
            }
            resource = buffer->hResource.pDrvPrivate;
            if (!resource || !resource->base.pResource) {
                /* VP9FIX: an unreadable record list used to be "ignored", which
                 * silently turned this frame into "the whole BITSTREAM is one
                 * frame" while the client believed it had described the slices.
                 * Refuse the submit instead. */
                ERROR("%s: VP9 SLICE_CONTROL has no resource (hResource=%p) -> "
                      "E_INVALIDARG", __FUNCTION__, (void *)resource);
                return E_INVALIDARG;
            }
            if (!decoder_reserve(&object->slices, &object->slices_cap, copy))
                return E_FAIL;
            hr = virtio_wddm_dxvk_video_read_buffer(
                device->base.pDev1, device->base.pCtx1, resource->base.pResource,
                buffer->DataOffset, copy, object->slices);
            if (FAILED(hr)) {
                ERROR("%s: copying %u B of SLICE_CONTROL (offset %u) failed: "
                      "0x%08lx -> E_FAIL", __FUNCTION__, copy, buffer->DataOffset,
                      (unsigned long)hr);
                return E_FAIL;
            }
            object->slices_size = copy;
            if (object->submits < 4u) {
                INFO("%s: SLICE_CONTROL %u B = %u short VPx slice record(s)",
                     __FUNCTION__, copy, copy / (UINT)sizeof(DXVA_Slice_VPx_Short));
            }
            continue;
        }

        /* P-BATCH AV1: SLICE_CONTROL holds DXVA_Tile_AV1 records (16 B each:
         * DataOffset / DataSize / row / column).  They are copied verbatim and
         * resolved against the BITSTREAM buffer in EndFrame, where the
         * tile_group OBU is assembled in TileNum order. */
        if (buffer->BufferType == D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL &&
            object->codec == VIRTIO_WDDM_CODEC_AV1) {
            UINT copy = buffer->DataSize;

            if (copy == 0)
                continue;
            if (copy > 16u * 1024u) {
                ERROR("%s: SLICE_CONTROL is %u B (more than the %u B buffer "
                      "reported by GetVideoDecoderBufferInfo) -> truncated",
                      __FUNCTION__, copy, 16u * 1024u);
                copy = 16u * 1024u;
            }
            /* Whole records only: a partial trailing record cannot describe a
             * tile and would make the tile group write garbage sizes. */
            copy -= copy % (UINT)sizeof(DXVA_Tile_AV1);
            if (copy == 0)
                continue;
            resource = buffer->hResource.pDrvPrivate;
            if (!resource || !resource->base.pResource) {
                ERROR("%s: SLICE_CONTROL has no resource (hResource=%p) -> ignored",
                      __FUNCTION__, (void *)resource);
                continue;
            }
            if (!decoder_reserve((UINT8 **)&object->av1_tiles, &object->av1_tiles_cap,
                                 copy))
                return E_FAIL;
            hr = virtio_wddm_dxvk_video_read_buffer(
                device->base.pDev1, device->base.pCtx1, resource->base.pResource,
                buffer->DataOffset, copy, object->av1_tiles);
            if (FAILED(hr)) {
                ERROR("%s: copying %u B of SLICE_CONTROL (offset %u) failed: "
                      "0x%08lx -> ignored", __FUNCTION__, copy, buffer->DataOffset,
                      (unsigned long)hr);
                continue;
            }
            object->av1_tiles_size = copy;
            object->av1_slice_controls++;
            if (object->av1_slice_controls <= 2u) {
                INFO("%s: SLICE_CONTROL %u B = %u DXVA_Tile_AV1 record(s)",
                     __FUNCTION__, copy, copy / (UINT)sizeof(DXVA_Tile_AV1));
            }
            continue;
        }

        /* P-BATCH HEVC: DXVA_Slice_HEVC_Short records name where each slice
         * segment sits in BITSTREAM.  Unlike VP9 there is nothing to assemble:
         * the client writes the slice NALs into the BITSTREAM buffer in order, so
         * the buffer *is* the access unit.  The records are counted and ignored,
         * exactly like the H.264 DXVA_Slice_H264_Short path. */
        if (buffer->BufferType == D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL &&
            object->codec == VIRTIO_WDDM_CODEC_HEVC) {
            object->hevc_slice_controls++;
            if (object->hevc_slice_controls <= 2u) {
                INFO("%s: SLICE_CONTROL %u B = %u DXVA_Slice_HEVC_Short record(s) "
                     "(BITSTREAM already holds the slices, records not needed)",
                     __FUNCTION__, buffer->DataSize,
                     buffer->DataSize / VIRTIO_WDDM_HEVC_SLICE_SHORT_SIZE);
            }
            continue;
        }

        /* P2-2i: Chromium always submits DXVA_Qmatrix_H264 (d3d_h264_accelerator.cc
         * :311-369), filled with the effective 4x4/8x8 scaling lists. When the
         * stream has no custom lists these are the H.264 defaults, which the
         * host decoder applies anyway because our synthesised SPS/PPS carry no
         * scaling matrices (seq/pic_scaling_matrix_present_flag = 0). A stream
         * with custom quantisation matrices would decode with default lists
         * instead -- logged once so the log shows when that caveat applies. */
        if (buffer->BufferType ==
            D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX) {
            if (!object->qmatrix_seen && buffer->DataSize) {
                object->qmatrix_seen = TRUE;
                INFO("%s: client submitted a %u B quantization matrix (normal for "
                     "Chromium); scaling lists are not forwarded, default lists "
                     "are used", __FUNCTION__, buffer->DataSize);
            }
            continue;
        }

        if (buffer->BufferType != D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM) {
            VERBOSE("%s: buffer %u type=%u ignored (raw-bitstream config only needs "
                    "BITSTREAM)", __FUNCTION__, i, (unsigned)buffer->BufferType);
            continue;
        }
        if (buffer->DataSize == 0)
            continue;
        resource = buffer->hResource.pDrvPrivate;
        if (!resource || !resource->base.pResource) {
            ERROR("%s: BITSTREAM buffer %u has no resource (hResource=%p)", __FUNCTION__,
                  i, (void *)resource);
            return E_FAIL;
        }
        /* VP9FIX (all codecs): bound the access unit before growing for it.  The
         * old code computed `bitstream_size + DataSize` in UINT, so a client that
         * submitted enough buffers could wrap the sum to a small value and make
         * decoder_reserve() allocate less than the CopySubresourceRegion()/
         * memcpy() that follows writes -- a heap overflow inside this DLL. */
        if (buffer->DataSize > VIRTIO_WDDM_MAX_ACCESS_UNIT_BYTES ||
            object->bitstream_size >
                VIRTIO_WDDM_MAX_ACCESS_UNIT_BYTES - buffer->DataSize) {
            ERROR("%s: BITSTREAM buffer %u would grow the access unit to more than "
                  "the %u B cap (%u B + %u B) -> E_INVALIDARG", __FUNCTION__, i,
                  (UINT)VIRTIO_WDDM_MAX_ACCESS_UNIT_BYTES, object->bitstream_size,
                  buffer->DataSize);
            return E_INVALIDARG;
        }
        if (!decoder_reserve(&object->bitstream, &object->bitstream_cap,
                             object->bitstream_size + buffer->DataSize))
            return E_FAIL;

        hr = virtio_wddm_dxvk_video_read_buffer(
            device->base.pDev1, device->base.pCtx1, resource->base.pResource,
            buffer->DataOffset, buffer->DataSize,
            object->bitstream + object->bitstream_size);
        if (FAILED(hr)) {
            ERROR("%s: copying %u bytes (offset %u) out of BITSTREAM buffer %u "
                  "failed: 0x%08lx -> E_FAIL", __FUNCTION__, buffer->DataSize,
                  buffer->DataOffset, i, (unsigned long)hr);
            return E_FAIL;
        }
        object->bitstream_size += buffer->DataSize;
        appended += buffer->DataSize;
        segments++;
    }

    if (appended == 0) {
        ERROR("%s: no BITSTREAM data among %u buffer(s) -> E_FAIL", __FUNCTION__, count);
        return E_FAIL;
    }

    object->submits++;
    if (object->submits == 1) {
        INFO("%s: first submit: %u segment(s) / %u byte(s) appended (access unit "
             "now %u byte(s)); vm_send happens in EndFrame", __FUNCTION__, segments,
             appended, object->bitstream_size);
    } else {
        VERBOSE("%s: %u segment(s) / %u byte(s) appended (access unit now %u byte(s))",
                __FUNCTION__, segments, appended, object->bitstream_size);
    }
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* Decoder: the one send/get_frame round trip both codecs share (P-BATCH VP9) */
/* ------------------------------------------------------------------------- */

typedef struct {
    LONGLONG start_qpc; /* QPC of the vm_send that started this picture */
    double send_ms;
    double get_ms;
    unsigned elapsed_ms;
    /* AV1FIX (2026-10-25): the per-frame budget used to be send / get / up,
     * where `get` covered the host decode AND the NV12 copy because both happen
     * inside VM_IOCTL_GET_FRAME.  These three split it, so the next AV1 run can
     * say whether a frame was late because the host never produced it (wait_ms
     * large, copy_ms 0) or because the copy is slow (wait_ms small, copy_ms
     * large) -- the question "host 等待 / 拷贝 / 上传" of this round. */
    double wait_ms;      /* host decode wait (frame became available)          */
    double copy_ms;      /* NV12 copy inside the get ioctl (0 on the mapped path) */
    UINT polls;          /* poll-once get calls this picture needed             */
    UINT last_error;     /* err= of the failing get call, 0 when it succeeded  */
} VIRTIO_WDDM_DecodeTiming;

/* Hand one complete access unit -- one VP9 picture, or an H.264 access unit --
 * to the host decoder and wait for the decoded NV12 picture.
 *
 * This is the part of DecoderEndFrame that is identical for both codecs, plus
 * the two recovery policies the older drivers need:
 *  - the one-shot 1.0.0.7 driver refuses the second SEND of a session, so a
 *    refused SEND reopens the session and re-sends the same bytes
 *    (decoder_reopen_session);
 *  - a picture that does not fit the NV12 scratch buffer (the host reports
 *    ERROR_INSUFFICIENT_BUFFER) grows it once and retries -- never an unbounded
 *    loop.
 *
 * The decoded picture is left in object->nv12 with its geometry/stride in *info.
 * FALSE means the picture was dropped and the failure is already recorded with
 * decoder_fail_frame() (E_FAIL reaches the application through the next
 * DecoderBeginFrame, see P2-2i). */
/* P-BATCH ZEROCOPY: is the direct-mapped output path allowed in this process?
 *
 * Opt-out, like the log file: VIRTIO_WDDM_ZEROCOPY=0 (or off/no) forces the
 * staged path everywhere, which is what an A/B on one machine needs.  Unset
 * means "try the direct path" -- and the driver/ABI gate below still decides
 * whether it is attempted at all. */
static BOOL decoder_zerocopy_enabled(VIRTIO_WDDM_VideoDecoder *object) {
    char value[8];
    DWORD n;
    (void)object;
    n = GetEnvironmentVariableA("VIRTIO_WDDM_ZEROCOPY", value, (DWORD)sizeof(value));
    if (n == 0 || n >= (DWORD)sizeof(value))
        return TRUE;
    if (value[0] == '0' || value[0] == 'o' || value[0] == 'O' || value[0] == 'n' ||
        value[0] == 'N')
        return FALSE;
    return TRUE;
}

/* P-BATCH ZEROCOPY: fetch the next decoded frame straight out of the media
 * pool.  On success `info` describes it and `object->direct_active` says the
 * capture buffer is checked out (EndFrame must release it, on every path).
 *
 * A refusal is permanent for this session: either the driver predates
 * VM_IOCTL_MAP_FRAME (ERROR_INVALID_FUNCTION) or it could not build the
 * mapping, and either way the staged GET_FRAME path is the correct fallback.
 * One failed ioctl per session instead of one per frame.
 *
 * VP9FIX (2026-10-25): two additional guarantees.
 *  1. Pairing: the driver keeps at most one mapping per session and answers
 *     "the previous frame was never released" to a second MAP_FRAME.  If this
 *     is ever called with a mapping still active (a superframe member, or a
 *     path that forgot to drop), the old frame is released first and counted,
 *     instead of being silently overwritten -- overwriting `direct_map` loses
 *     the index and leaks the capture buffer for the rest of the session.
 *  2. Bounds: the pixels the decode path later reads must fit the mapping the
 *     driver returned.  `MappedLength < Size` (or a zero length/VA) is refused
 *     here, the mapping is given back immediately, and the staged path takes
 *     over; reading past the mapped pages is exactly a 0xC0000005 in this DLL.
 *
 * AV1FIX (2026-10-25): 3. a TIMEOUT is not a REFUSAL.
 *     E173's AV1 ring says it in one line: the first MAP_FRAME of the session
 *     answered err=121 (no frame had been produced yet), and from then on the
 *     UMD reported direct=staging for the whole session -- one slow frame cost
 *     the zero-copy route for every following picture, adding a 3 MB copy per
 *     frame exactly when the decode was already late.  The old code disabled the
 *     route on *any* failure.  Now only a real refusal (an older driver without
 *     VM_IOCTL_MAP_FRAME: ERROR_INVALID_FUNCTION, or an impossible request)
 *     disables it for good; a timeout keeps it enabled and is counted, and a
 *     streak of other failures (a driver without a media pool answers ENOMEM
 *     every time) disables it after VIRTIO_WDDM_DIRECT_FAIL_STREAK_MAX pictures
 *     instead of one.  Either way the buffer is given back before returning. */
#define VIRTIO_WDDM_DIRECT_FAIL_STREAK_MAX 8u

static BOOL decoder_try_direct_frame(VIRTIO_WDDM_VideoDecoder *object, VM_FRAME_INFO *info) {
    VM_FRAME_MAP map;
    UINT64 need;
    LONGLONG t0;
    UINT error;
    double failed_ms;

    if (!object->direct_ok)
        return FALSE;
    if (object->direct_active) {
        object->direct_pairing_fixes++;
        ERROR("%s: MAP_FRAME would replace an unreleased mapping (index %u, %u B) "
              "-> releasing it first (pairing fix #%llu)", __FUNCTION__,
              object->direct_map.Index, object->direct_map.MappedLength,
              (unsigned long long)object->direct_pairing_fixes);
        decoder_drop_direct(object);
    }
    memset(&map, 0, sizeof(map));
    t0 = decoder_qpc_now();
    if (vm_map_frame(object->session, &map, 1000)) {
        need = (UINT64)map.Size;
        if (map.UserVa == 0 || map.MappedLength == 0 || need == 0 ||
            (UINT64)map.MappedLength < need) {
            ERROR("%s: MAP_FRAME returned an unusable mapping (uva=0x%llx len=%u "
                  "size=%u, index %u) -> released, staged GET_FRAME for the rest "
                  "of this session", __FUNCTION__, (unsigned long long)map.UserVa,
                  map.MappedLength, map.Size, map.Index);
            (void)vm_release_frame(object->session, map.Index);
            object->direct_ok = FALSE;
            object->direct_refusals++;
            return FALSE;
        }
        object->direct_fail_streak = 0;
        object->direct_map = map;
        object->direct_active = TRUE;
        object->direct_maps++;
        info->Width = map.Width;
        info->Height = map.Height;
        info->Stride = map.Stride;
        info->Size = map.Size;
        info->BytesUsed = map.BytesUsed;
        info->FourCC = map.FourCC;
        info->SessionId = map.SessionId;
        info->Index = map.Index;
        return TRUE;
    }
    error = vm_last_error();
    failed_ms = decoder_qpc_ms(t0, decoder_qpc_now());
    /* Whatever the reason: nothing may stay checked out.  A failed MAP_FRAME
     * dequeued no buffer, but a previous release may have failed and the driver
     * answers INVALID_DEVICE_STATE when nothing is mapped, so this is a cheap
     * proof rather than a guess (see decoder_release_stray_frame). */
    decoder_release_stray_frame(object);
    if (error == ERROR_SEM_TIMEOUT) {
        object->direct_timeouts++;
        if (object->direct_timeouts <= 4u) {
            INFO("%s: MAP_FRAME waited %.2f ms and no frame arrived (err=%u) -- a "
                 "timeout is not a refusal: the direct route stays enabled and the "
                 "next picture maps again (timeout #%llu)", __FUNCTION__, failed_ms,
                 error, (unsigned long long)object->direct_timeouts);
        }
        return FALSE;
    }
    if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_CALL_NOT_IMPLEMENTED || error == ERROR_INVALID_PARAMETER ||
        error == ERROR_INVALID_DATA || error == ERROR_PROC_NOT_FOUND) {
        object->direct_refusals++;
        object->direct_ok = FALSE;
        INFO("%s: direct-mapped frame refused err=%u after %.2f ms -> staged "
             "GET_FRAME for the rest of this session", __FUNCTION__, error,
             failed_ms);
        return FALSE;
    }
    object->direct_other_failures++;
    object->direct_fail_streak++;
    if (object->direct_fail_streak >= VIRTIO_WDDM_DIRECT_FAIL_STREAK_MAX) {
        object->direct_ok = FALSE;
        INFO("%s: MAP_FRAME failed %u time(s) in a row (last err=%u, %.2f ms) -> "
             "staged GET_FRAME for the rest of this session (failure #%llu)",
             __FUNCTION__, object->direct_fail_streak, error, failed_ms,
             (unsigned long long)object->direct_other_failures);
    } else if (object->direct_other_failures <= 4u) {
        INFO("%s: MAP_FRAME failed err=%u after %.2f ms (streak %u) -> staged "
             "GET_FRAME for this picture only (failure #%llu)", __FUNCTION__, error,
             failed_ms, object->direct_fail_streak,
             (unsigned long long)object->direct_other_failures);
    }
    return FALSE;
}

/* P-BATCH ZEROCOPY: unmap the frame and give its capture buffer back to the
 * device.  Must run on every path that got a frame from
 * decoder_try_direct_frame(), including the failures -- otherwise the buffer
 * stays out of the capture queue and the decoder starves.
 *
 * VP9FIX: the pairing is counted (maps vs releases) so the decoder summary can
 * prove that no buffer was left checked out; a failed RELEASE_FRAME is an ERROR
 * once and counted, not silently ignored. */
static void decoder_drop_direct(VIRTIO_WDDM_VideoDecoder *object) {
    if (!object->direct_active)
        return;
    /* PIPELINE (2026-10-25): the same lock the worker takes around its ioctls,
     * so RELEASE_FRAME and the next MAP_FRAME cannot overlap. */
    pipeline_session_lock(object);
    object->direct_active = FALSE;
    if (!vm_release_frame(object->session, object->direct_map.Index)) {
        object->direct_release_failures++;
        pipeline_session_unlock(object);
        ERROR("%s: vm_release_frame(index %u) failed err=%u (release failure #%llu)",
              __FUNCTION__, object->direct_map.Index, vm_last_error(),
              (unsigned long long)object->direct_release_failures);
        pipeline_on_mapping_freed(object);
        return;
    }
    object->direct_frames++;
    object->direct_releases++;
    pipeline_session_unlock(object);
    /* The driver keeps at most one mapping per session, so the worker cannot map
     * the next frame until this one is back. */
    pipeline_on_mapping_freed(object);
}

/* AV1FIX (2026-10-25): give a capture buffer back without knowing its index.
 *
 * The whole AV1 symptom is "the host stops producing frames": MAP_FRAME and
 * then GET_FRAME answer err=121 (ERROR_SEM_TIMEOUT) for every picture.  A
 * timeout means no DQBUF arrived, so in theory nothing is checked out -- but the
 * only thing that can put a capture buffer back into the device's queue is
 * RELEASE_FRAME, and a buffer that stays checked out (a failed release, a
 * mapping overwritten by an early-return path, a driver that dequeued a frame
 * just as the ioctl timed out) turns a slow decode into a permanently starved
 * queue.  The driver documents Index 0xFFFFFFFF as "whatever is mapped" and
 * answers ST_INVALID_DEVICE_STATE when nothing is, so calling it after every
 * failed acquisition is a cheap *proof* that the queue is intact, and a repair
 * when it is not.  Both outcomes are counted: `stray_releases > 0` in the
 * summary means the queue really was short and this call is what fixed it. */
static void decoder_release_stray_frame(VIRTIO_WDDM_VideoDecoder *object) {
    BOOL released;

    if (!object || !object->session)
        return;
    pipeline_session_lock(object);
    released = vm_release_frame(object->session, 0xFFFFFFFFu) ? TRUE : FALSE;
    pipeline_session_unlock(object);
    if (released) {
        object->stray_releases++;
        INFO("%s: RELEASE_FRAME(0xFFFFFFFF) requeued a capture buffer that was "
             "still checked out of the device -- the CAPTURE queue was short by "
             "one until this call (stray release #%llu)", __FUNCTION__,
             (unsigned long long)object->stray_releases);
    } else {
        object->stray_release_empty++;
    }
}

/* ========================================================================= */
/* PIPELINE (2026-10-25): decode as a two-stage pipeline                     */
/* ========================================================================= */
/* 1 WHY
 *
 * Until this round DecoderEndFrame was strictly serial:
 *
 *   assemble AU N -> vm_send(N) -> vm_get_frame(N)/vm_map_frame(N) -> upload(N)
 *
 * and the measured per-frame budget of the 1080p30 HEVC run in
 * 04-测试记录/证据/p4-13 (umd-hevc3.log, `frame N budget:` lines) is
 *
 *   send = 0.07 ..  1.73 ms   the access-unit handover
 *   get  = 3.30 ..  7.90 ms   the host decode wait (the ioctl blocks)
 *   up   = 24.7 .. 34.10 ms   the 3.1 MB read out of the media pool's
 *                             MmNonCached pages + the DXVK staging copy
 *   sum  = 28   .. 43     ms  -> 25.7 fps against a 33.3 ms frame budget
 *
 * `up` is one 3.1 MB memcpy out of non-cached pages; it cannot be removed
 * here (it is what makes the pixels visible to DXVK), but it does not involve
 * the host, which sits idle for those 25-34 ms.  The pipeline lets the host
 * decode during that window instead of after it.
 *
 * 2 WHAT IT DOES
 *
 *   DDI thread (the app's decode thread)              pipeline worker
 *   ----------------------------------------------    ----------------------
 *   EndFrame N: assemble AU N, copy it into job N     vm_send(job N)  [0-2 ms]
 *     (state QUEUED) -> wake the worker
 *               wait for the OLDEST pending job       wait until no capture
 *                 (N-1) to be READY                     buffer is checked out
 *               upload ITS pixels into ITS              then vm_map_frame /
 *                 subresource (the existing upload       vm_get_frame(job N)
 *                 tail, ~25-34 ms)                        [3-8 ms host wait]
 *               release the capture buffer (existing
 *                 decoder_drop_direct, every path) ->  wake: the mapping is
 *                                                       free, map the next one
 *
 * So the send and the host decode wait of job N happen *while* the DDI thread
 * copies frame N-1 out of the pool and records the DXVK upload.  In steady
 * state the worker is always ready before the DDI asks, so `send + get`
 * (3-10 ms) leaves the per-frame budget and only `up` remains.
 *
 * 3 CONSEQUENCES (all of them are visible in the log)
 *
 *  1. A picture is uploaded one EndFrame late: job N lands in the subresource
 *     BeginFrame N selected, but during EndFrame N+1.  Order and the
 *     picture -> subresource mapping are preserved exactly (job N's pixels go
 *     to job N's subresource, in submission order); only the moment moves, so
 *     this is a one-frame decode delay, the same thing a reorder buffer does.
 *     A player with a decode queue (PotPlayer runs 17-21 frames ahead of its
 *     renderer, Chromium about 8) cannot observe it.
 *  2. The first EndFrame of a sequence (decoder creation, or the first frame
 *     after a flush) has no older job to upload, so it writes a neutral black
 *     picture into its subresource instead of nothing: a zeroed NV12 slice
 *     presents as flat green, which is exactly the P2-2J bug.
 *  3. flush/seek discards the in-flight job and bumps the generation, so a
 *     picture from before the seek can never be written after it.
 *  4. VP9 keeps the serial path: one EndFrame may carry several superframe
 *     members and only the last picture is uploaded, so there is no
 *     one-job-per-EndFrame mapping.  H.264 / HEVC / AV1 are pipelined.
 *  5. VIRTIO_WDDM_PIPELINE=0|off|no restores the old serial path bit for bit;
 *     VIRTIO_WDDM_PIPELINE_DEPTH=1 keeps the worker but drains the job it just
 *     submitted (an A/B point with no overlap and no one-frame delay).
 *
 * 4 CONCURRENCY BOUNDARY
 *
 *  - Every vm_* call the pipeline makes happens under pipe->lock, and the
 *    decoder object is the only owner of the session: that lock is the "one
 *    thread at a time" rule vmedia_client.c documents for the driver's
 *    sequential IOCTL queue.  vm_last_error() is per-thread for the same
 *    reason (see vmedia_client.c).
 *  - The worker never mutates object->session.  A refused SEND fails the job
 *    and the DDI thread decides what to do (a reopen is a session-lifetime
 *    change, and that thread owns the session's lifetime).
 *  - The worker never reads the decoder's scratch buffers (bitstream, send,
 *    hevc_au, av1_au): the access unit is copied into the job at submit time.
 *  - The only decoder state the worker writes is the fetch result, which is
 *    read back under the same lock.
 *  - A mapped capture buffer is released by the DDI thread through the
 *    existing decoder_drop_direct() on every exit path (that is what keeps the
 *    maps == releases audit at zero); the worker waits for the release before
 *    it maps the next frame, because the driver keeps at most one mapping per
 *    session.
 */

#define VIRTIO_WDDM_PIPELINE_JOBS 2u

/* A job is a ring slot.  The states are the worker's and the DDI thread's
 * shared vocabulary; every transition happens under pipe->lock. */
#define VIRTIO_WDDM_JOB_FREE 0u     /* slot usable                                */
#define VIRTIO_WDDM_JOB_QUEUED 1u   /* submitted, the worker has not picked it up */
#define VIRTIO_WDDM_JOB_SENDING 2u  /* worker is inside vm_send                    */
#define VIRTIO_WDDM_JOB_FETCHING 3u /* worker is inside map/get                    */
#define VIRTIO_WDDM_JOB_READY 4u    /* pixels available, waiting for the DDI       */
#define VIRTIO_WDDM_JOB_FAILED 5u   /* a stage failed; see failure/error           */

typedef struct {
    UINT state;
    UINT64 seq;        /* 1-based submission order */
    UINT64 generation; /* pipe->generation at submit time */
    LONGLONG submit_qpc;
    LONGLONG stage_qpc; /* QPC when the worker picked the job up */
    LONGLONG send_qpc;  /* QPC of the vm_send */
    LONGLONG ready_qpc; /* QPC when the pixels became available */

    /* The access unit, copied.  The decoder's scratch buffers are reused by
     * the next frame, so the worker must never read them. */
    UINT8 *au;
    UINT au_size;
    UINT au_cap;
    UINT send_w; /* S_FMT OUTPUT geometry of this access unit */
    UINT send_h;

    /* The target of THIS frame's BeginFrame, snapshotted: by the time the
     * pixels are uploaded the runtime may have moved on to another output
     * view, and the picture still belongs in the subresource BeginFrame N
     * named.  Not a DDI handle: a real reference on the D3D11 texture, so the
     * upload cannot touch a destroyed resource. */
    ID3D11Resource *texture;
    UINT subresource;
    UINT mip_slice;
    UINT first_array_slice;
    UINT array_size;
    UINT mips;
    UINT tex_width;
    UINT tex_height;
    DXGI_FORMAT tex_format;
    UINT desc_w; /* decoder descriptor size at submit time */
    UINT desc_h;

    /* Fetch result.  `map` is the direct-mapped capture buffer (mapped = TRUE)
     * or `slot` holds a staged copy. */
    VM_FRAME_INFO info;
    VM_FRAME_MAP map;
    BOOL mapped;
    UINT8 *slot;
    UINT slot_cap;
    BOOL using_slot;

    double send_ms;
    double fetch_ms;
    UINT error;
    const char *failure;
} VIRTIO_WDDM_PipelineJob;

typedef struct VIRTIO_WDDM_DecodePipeline {
    VIRTIO_WDDM_VideoDecoder *object;
    CRITICAL_SECTION lock; /* the jobs *and* every session call */
    HANDLE thread;
    DWORD thread_id;
    HANDLE wake;  /* auto-reset: work for the worker / state changed        */
    HANDLE ready; /* auto-reset: a job left SENDING/FETCHING                */
    BOOL started;
    BOOL stop;
    BOOL broken; /* a stage timed out: the session cannot be trusted any more */
    BOOL defer;  /* FALSE = PIPELINE_DEPTH 1: drain the job just submitted   */
    UINT timeout_ms;
    UINT64 generation;
    UINT64 submit_seq;
    UINT64 drain_seq;
    UINT jobs_in_flight;
    VIRTIO_WDDM_PipelineJob jobs[VIRTIO_WDDM_PIPELINE_JOBS];
    /* The capture buffer currently checked out of the device by a job whose
     * pixels were handed to the DDI thread.  The worker may not map the next
     * frame until it is back. */
    BOOL mapping_out;
    UINT64 mapping_out_seq;
    /* The neutral black picture written by the fill EndFrame. */
    UINT8 *fill;
    UINT fill_cap;
    UINT fill_size;
    UINT fill_logged;
    /* diagnostics */
    UINT64 submitted;
    UINT64 produced;
    UINT64 drained;
    UINT64 fills;
    UINT64 dropped;
    UINT64 timeouts;
    UINT64 send_failures;
    UINT64 fetch_failures;
    UINT64 send_retries;
    UINT64 failed_jobs;
    UINT64 drained_mapped;
    UINT64 drained_staged;
    UINT64 logged;
    double lead_sum;
    double lead_max;   /* ready -> drain: how far ahead the worker was        */
    double queue_sum;  /* submit -> ready: the job's end-to-end latency        */
    double queue_max;
    double wait_sum;   /* time the DDI thread spent waiting for a job          */
    double wait_max;
} VIRTIO_WDDM_DecodePipeline;

/* What pipeline_frame() hands back to DecoderEndFrame: everything the upload
 * tail needs, taken from the drained job rather than from the current
 * BeginFrame. */
typedef struct {
    BOOL fill;  /* the neutral picture, not a decoded frame */
    BOOL failed;
    const UINT8 *src;
    UINT64 readable;
    VM_FRAME_INFO info;
    LONGLONG start_qpc;
    double send_ms;
    double fetch_ms;
    double queue_ms;
    double lead_ms;
    double wait_ms;
    UINT error;
    const char *failure;
    ID3D11Resource *texture;
    UINT subresource;
    UINT mip_slice;
    UINT first_array_slice;
    UINT array_size;
    UINT mips;
    UINT tex_width;
    UINT tex_height;
    DXGI_FORMAT tex_format;
} VIRTIO_WDDM_PipelineFrame;

static BOOL pipeline_env_flag(const char *name, BOOL fallback) {
    char value[16];
    DWORD n = GetEnvironmentVariableA(name, value, (DWORD)sizeof(value));

    if (n == 0 || n >= (DWORD)sizeof(value))
        return fallback;
    switch (value[0]) {
    case '0':
    case 'f':
    case 'F':
    case 'n':
    case 'N':
        return FALSE;
    case '1':
    case 't':
    case 'T':
    case 'y':
    case 'Y':
        return TRUE;
    case 'o':
    case 'O':
        if (value[1] == 'f' || value[1] == 'F')
            return FALSE;
        if (value[1] == 'n' || value[1] == 'N')
            return TRUE;
        return fallback;
    default:
        return fallback;
    }
}

static UINT pipeline_env_uint(const char *name, UINT fallback, UINT lo, UINT hi) {
    char value[16];
    DWORD n = GetEnvironmentVariableA(name, value, (DWORD)sizeof(value));
    UINT parsed = 0;
    DWORD i;

    if (n == 0 || n >= (DWORD)sizeof(value))
        return fallback;
    for (i = 0; i < n && value[i] >= '0' && value[i] <= '9'; i++) {
        parsed = parsed * 10u + (UINT)(value[i] - '0');
        if (parsed > hi) {
            parsed = hi;
            break;
        }
    }
    if (i == 0)
        return fallback;
    if (parsed < lo)
        parsed = lo;
    return parsed;
}

/* Remaining milliseconds until `deadline`, 0 when it has passed.  Used as the
 * ioctl timeout, so a job never outlives its budget. */
static UINT pipeline_left_ms(LONGLONG deadline) {
    double left = decoder_qpc_ms(decoder_qpc_now(), deadline);
    if (left <= 0.0)
        return 0;
    if (left > 60000.0)
        return 60000;
    return (UINT)left;
}

/* Release everything a job owns except its ring slot (the AU buffer is kept at
 * its high-water mark, exactly like the decoder's own scratch buffers).  Must
 * be called with the lock held. */
static void pipeline_job_release_locked(VIRTIO_WDDM_DecodePipeline *pipe,
                                        VIRTIO_WDDM_PipelineJob *job,
                                        const char *why) {
    if (job->mapped) {
        /* A frame that was mapped but never handed to the DDI thread: the
         * capture buffer can only come back through RELEASE_FRAME. */
        job->mapped = FALSE;
        if (!vm_release_frame(pipe->object->session, job->map.Index)) {
            pipe->object->direct_release_failures++;
            ERROR("%s: job %llu (%s) RELEASE_FRAME(index %u) failed err=%u",
                  __FUNCTION__, (unsigned long long)job->seq, why, job->map.Index,
                  vm_last_error());
        } else {
            pipe->object->direct_releases++;
        }
        if (pipe->mapping_out && pipe->mapping_out_seq == job->seq)
            pipe->mapping_out = FALSE;
    }
    if (job->texture) {
        job->texture->lpVtbl->Release(job->texture);
        job->texture = NULL;
    }
    if (job->state != VIRTIO_WDDM_JOB_FREE) {
        VERBOSE("%s: dropping job %llu (state=%u, %s)", __FUNCTION__,
                (unsigned long long)job->seq, job->state, why);
        pipe->dropped++;
    }
    job->state = VIRTIO_WDDM_JOB_FREE;
    job->au_size = 0;
    job->using_slot = FALSE;
    job->error = 0;
    job->failure = NULL;
}

/* The worker: pick the oldest queued job of the current generation, send it,
 * fetch its frame, mark it READY.  Everything that touches the session does so
 * under the lock; everything that can block is done outside it. */
static DWORD WINAPI pipeline_worker(LPVOID param) {
    VIRTIO_WDDM_DecodePipeline *pipe = (VIRTIO_WDDM_DecodePipeline *)param;
    VIRTIO_WDDM_VideoDecoder *object = pipe->object;

    for (;;) {
        VIRTIO_WDDM_PipelineJob *job = NULL;
        UINT64 generation;
        UINT i;

        EnterCriticalSection(&pipe->lock);
        if (pipe->stop) {
            LeaveCriticalSection(&pipe->lock);
            break;
        }
        for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++) {
            VIRTIO_WDDM_PipelineJob *candidate = &pipe->jobs[i];
            if (candidate->state != VIRTIO_WDDM_JOB_QUEUED)
                continue;
            if (candidate->generation != pipe->generation) {
                pipeline_job_release_locked(pipe, candidate, "stale generation");
                continue;
            }
            job = candidate;
            break;
        }
        if (!job) {
            LeaveCriticalSection(&pipe->lock);
            WaitForSingleObject(pipe->wake, 50);
            continue;
        }
        job->state = VIRTIO_WDDM_JOB_SENDING;
        job->stage_qpc = decoder_qpc_now();
        job->failure = NULL;
        job->error = 0;
        generation = pipe->generation;
        LeaveCriticalSection(&pipe->lock);

        /* ---- stage 1: hand the access unit to the host ---- */
        {
            BOOL ok;
            LONGLONG t0 = decoder_qpc_now();

            EnterCriticalSection(&pipe->lock);
            if (pipe->stop || generation != pipe->generation) {
                pipeline_job_release_locked(pipe, job, "generation changed before send");
                LeaveCriticalSection(&pipe->lock);
                SetEvent(pipe->ready);
                continue;
            }
            job->send_qpc = t0;
            ok = vm_send(object->session, job->au, job->au_size, job->send_w,
                         job->send_h, 0, NULL);
            job->error = ok ? 0u : vm_last_error();
            job->send_ms = decoder_qpc_ms(t0, decoder_qpc_now());
            LeaveCriticalSection(&pipe->lock);

            if (!ok) {
                pipe->send_failures++;
                pipe->failed_jobs++;
                job->failure = "vm_send";
                EnterCriticalSection(&pipe->lock);
                if (generation == pipe->generation)
                    job->state = VIRTIO_WDDM_JOB_FAILED;
                else
                    pipeline_job_release_locked(pipe, job, "generation changed");
                LeaveCriticalSection(&pipe->lock);
                ERROR("%s: job %llu: vm_send(%s, %u B, %ux%u) failed err=%u after "
                      "%.2f ms -> job failed, the DDI thread decides whether to "
                      "reopen the session", __FUNCTION__,
                      (unsigned long long)job->seq,
                      decoder_codec_name(object->codec), job->au_size, job->send_w,
                      job->send_h, job->error, job->send_ms);
                SetEvent(pipe->ready);
                continue;
            }
            object->frame_sent = TRUE;
        }

        /* ---- stage 2: wait for the previous capture buffer to come back ---- */
        {
            BOOL abandon = FALSE;
            LONGLONG deadline = decoder_qpc_now() + (LONGLONG)pipe->timeout_ms *
                                                    decoder_qpc_freq() / 1000;

            EnterCriticalSection(&pipe->lock);
            job->state = VIRTIO_WDDM_JOB_FETCHING;
            LeaveCriticalSection(&pipe->lock);

            for (;;) {
                UINT left;

                EnterCriticalSection(&pipe->lock);
                if (pipe->stop || generation != pipe->generation) {
                    pipeline_job_release_locked(pipe, job, "generation changed before fetch");
                    LeaveCriticalSection(&pipe->lock);
                    abandon = TRUE;
                    break;
                }
                if (!pipe->mapping_out)
                    break; /* no capture buffer is checked out of the device */
                LeaveCriticalSection(&pipe->lock);
                if (pipeline_left_ms(deadline) == 0) {
                    abandon = TRUE;
                    job->error = ERROR_SEM_TIMEOUT;
                    job->failure = "capture buffer of the previous job never came back";
                    pipe->timeouts++;
                    pipe->failed_jobs++;
                    EnterCriticalSection(&pipe->lock);
                    if (generation != pipe->generation)
                        pipeline_job_release_locked(pipe, job, "generation changed");
                    else
                        job->state = VIRTIO_WDDM_JOB_FAILED;
                    LeaveCriticalSection(&pipe->lock);
                    ERROR("%s: job %llu: timed out after %u ms waiting for the "
                          "capture buffer of job %llu to be released (mapping_out=%d) "
                          "-> job failed", __FUNCTION__,
                          (unsigned long long)job->seq, pipe->timeout_ms,
                          (unsigned long long)pipe->mapping_out_seq,
                          (int)pipe->mapping_out);
                    break;
                }
                left = pipeline_left_ms(deadline);
                WaitForSingleObject(pipe->wake, left > 20 ? 20 : left);
            }
            if (abandon) {
                SetEvent(pipe->ready);
                continue;
            }
        }

        /* ---- stage 3: fetch the decoded frame ---- */
        {
            BOOL ok = FALSE;
            BOOL refused_route = FALSE;
            UINT error = 0;
            LONGLONG deadline = job->stage_qpc + (LONGLONG)pipe->timeout_ms *
                                                     decoder_qpc_freq() / 1000;
            LONGLONG t0;
            BOOL abandon = FALSE;

            memset(&job->info, 0, sizeof(job->info));
            memset(&job->map, 0, sizeof(job->map));

            if (object->direct_ok) {
                EnterCriticalSection(&pipe->lock);
                if (pipe->stop || generation != pipe->generation) {
                    pipeline_job_release_locked(pipe, job, "generation changed before map");
                    LeaveCriticalSection(&pipe->lock);
                    abandon = TRUE;
                } else {
                    UINT left = pipeline_left_ms(deadline);
                    t0 = decoder_qpc_now();
                    ok = vm_map_frame(object->session, &job->map,
                                      left ? left : 1u) ? TRUE : FALSE;
                    error = ok ? 0u : vm_last_error();
                    job->fetch_ms = decoder_qpc_ms(t0, decoder_qpc_now());
                    LeaveCriticalSection(&pipe->lock);
                }
                if (abandon) {
                    SetEvent(pipe->ready);
                    continue;
                }
                if (ok) {
                    UINT64 need = (UINT64)job->map.Size;

                    if (job->map.UserVa == 0 || job->map.MappedLength == 0 ||
                        need == 0 || (UINT64)job->map.MappedLength < need) {
                        ERROR("%s: job %llu: MAP_FRAME returned an unusable mapping "
                              "(uva=0x%llx len=%u size=%u index %u) -> released, "
                              "staged GET_FRAME for the rest of this session",
                              __FUNCTION__, (unsigned long long)job->seq,
                              (unsigned long long)job->map.UserVa,
                              job->map.MappedLength, job->map.Size, job->map.Index);
                        EnterCriticalSection(&pipe->lock);
                        (void)vm_release_frame(object->session, job->map.Index);
                        object->direct_refusals++;
                        object->direct_ok = FALSE;
                        LeaveCriticalSection(&pipe->lock);
                        refused_route = TRUE;
                    } else {
                        EnterCriticalSection(&pipe->lock);
                        object->direct_fail_streak = 0;
                        object->direct_maps++;
                        job->mapped = TRUE;
                        LeaveCriticalSection(&pipe->lock);
                        job->info.Width = job->map.Width;
                        job->info.Height = job->map.Height;
                        job->info.Stride = job->map.Stride;
                        job->info.Size = job->map.Size;
                        job->info.BytesUsed = job->map.BytesUsed;
                        job->info.FourCC = job->map.FourCC;
                        job->info.SessionId = job->map.SessionId;
                        job->info.Index = job->map.Index;
                        ok = TRUE;
                    }
                } else if (error == ERROR_SEM_TIMEOUT) {
                    /* AV1FIX rule: a timeout is not a refusal.  The route stays
                     * enabled and the next picture maps again. */
                    EnterCriticalSection(&pipe->lock);
                    object->direct_timeouts++;
                    if (object->direct_timeouts <= 4u)
                        INFO("%s: job %llu: MAP_FRAME waited %.2f ms and no frame "
                             "arrived (err=%u) -- timeout #%llu, the direct route "
                             "stays enabled", __FUNCTION__,
                             (unsigned long long)job->seq, job->fetch_ms, error,
                             (unsigned long long)object->direct_timeouts);
                    LeaveCriticalSection(&pipe->lock);
                } else if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
                           error == ERROR_CALL_NOT_IMPLEMENTED ||
                           error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_DATA ||
                           error == ERROR_PROC_NOT_FOUND) {
                    EnterCriticalSection(&pipe->lock);
                    object->direct_refusals++;
                    object->direct_ok = FALSE;
                    LeaveCriticalSection(&pipe->lock);
                    refused_route = TRUE;
                    INFO("%s: job %llu: direct-mapped frame refused err=%u after "
                         "%.2f ms -> staged GET_FRAME for the rest of this session",
                         __FUNCTION__, (unsigned long long)job->seq, error, job->fetch_ms);
                } else {
                    EnterCriticalSection(&pipe->lock);
                    object->direct_other_failures++;
                    object->direct_fail_streak++;
                    if (object->direct_fail_streak >= VIRTIO_WDDM_DIRECT_FAIL_STREAK_MAX) {
                        object->direct_ok = FALSE;
                        INFO("%s: job %llu: MAP_FRAME failed %u time(s) in a row "
                             "(last err=%u) -> staged GET_FRAME for the rest of this "
                             "session", __FUNCTION__, (unsigned long long)job->seq,
                             object->direct_fail_streak, error);
                    } else if (object->direct_other_failures <= 4u) {
                        INFO("%s: job %llu: MAP_FRAME failed err=%u after %.2f ms "
                             "(streak %u) -> staged path for this picture only",
                             __FUNCTION__, (unsigned long long)job->seq, error,
                             job->fetch_ms, object->direct_fail_streak);
                    }
                    LeaveCriticalSection(&pipe->lock);
                }
            }

            if (!ok && !abandon) {
                /* Staged fallback: the same GET_FRAME the serial path uses, into
                 * this job's own slot so the next job can fill its own.  The
                 * size mirrors decoder_decode_unit's nv12 reservation (3 bytes
                 * per pixel, i.e. P010 headroom on top of an NV12 frame). */
                UINT w = job->send_w ? job->send_w : object->desc.SampleWidth;
                UINT h = job->send_h ? job->send_h : object->desc.SampleHeight;
                UINT cap = w * h * 3u + w * 16u + 4096u;

                if (cap < 0x00400000u)
                    cap = 0x00400000u; /* a 1080p NV12 frame is ~3 MB */
                if (!decoder_reserve(&job->slot, &job->slot_cap, cap)) {
                    job->error = ERROR_NOT_ENOUGH_MEMORY;
                    job->failure = "out of memory for the staged frame";
                    pipe->failed_jobs++;
                    EnterCriticalSection(&pipe->lock);
                    if (generation == pipe->generation)
                        job->state = VIRTIO_WDDM_JOB_FAILED;
                    else
                        pipeline_job_release_locked(pipe, job, "generation changed");
                    LeaveCriticalSection(&pipe->lock);
                    ERROR("%s: job %llu: cannot reserve %u B for the staged frame",
                          __FUNCTION__, (unsigned long long)job->seq, cap);
                    SetEvent(pipe->ready);
                    continue;
                }
                ok = FALSE;
                for (;;) {
                    UINT left = pipeline_left_ms(deadline);
                    BOOL got;

                    EnterCriticalSection(&pipe->lock);
                    if (pipe->stop || generation != pipe->generation) {
                        pipeline_job_release_locked(pipe, job, "generation changed in get");
                        LeaveCriticalSection(&pipe->lock);
                        abandon = TRUE;
                        break;
                    }
                    t0 = decoder_qpc_now();
                    got = vm_get_frame(object->session, job->slot, job->slot_cap,
                                       &job->info, left ? left : 1u)
                              ? TRUE
                              : FALSE;
                    error = got ? 0u : vm_last_error();
                    job->fetch_ms = decoder_qpc_ms(t0, decoder_qpc_now());
                    if (!got && error == ERROR_INSUFFICIENT_BUFFER &&
                        job->slot_cap < 0x08000000u) {
                        UINT bigger = job->slot_cap * 2u;
                        LeaveCriticalSection(&pipe->lock);
                        INFO("%s: job %llu: frame does not fit %u B, retrying with %u",
                             __FUNCTION__, (unsigned long long)job->seq, job->slot_cap,
                             bigger);
                        if (!decoder_reserve(&job->slot, &job->slot_cap, bigger))
                            break;
                        continue;
                    }
                    if (!got && error == ERROR_SEM_TIMEOUT) {
                        object->frame_get_timeouts++;
                        decoder_release_stray_frame(object);
                    }
                    LeaveCriticalSection(&pipe->lock);
                    ok = got;
                    break;
                }
                if (abandon) {
                    SetEvent(pipe->ready);
                    continue;
                }
                if (ok) {
                    job->using_slot = TRUE;
                    refused_route = FALSE;
                }
            }

            if (abandon) {
                SetEvent(pipe->ready);
                continue;
            }
            if (!ok) {
                pipe->fetch_failures++;
                pipe->failed_jobs++;
                job->error = error;
                job->failure = refused_route ? "no fetch route" : "map/get_frame";
                EnterCriticalSection(&pipe->lock);
                if (generation == pipe->generation)
                    job->state = VIRTIO_WDDM_JOB_FAILED;
                else
                    pipeline_job_release_locked(pipe, job, "generation changed");
                LeaveCriticalSection(&pipe->lock);
                ERROR("%s: job %llu: no decoded frame after %.2f ms (err=%u, %s, "
                      "direct_ok=%d) -> job failed", __FUNCTION__,
                      (unsigned long long)job->seq, job->fetch_ms, error,
                      refused_route ? "no route" : "host/fetch failure",
                      (int)object->direct_ok);
                SetEvent(pipe->ready);
                continue;
            }

            EnterCriticalSection(&pipe->lock);
            if (generation != pipe->generation) {
                /* A flush landed while this frame was being fetched: the picture
                 * belongs to the sequence the application just abandoned. */
                pipeline_job_release_locked(pipe, job, "flushed during fetch");
                LeaveCriticalSection(&pipe->lock);
                SetEvent(pipe->ready);
                continue;
            }
            job->ready_qpc = decoder_qpc_now();
            job->state = VIRTIO_WDDM_JOB_READY;
            pipe->produced++;
            LeaveCriticalSection(&pipe->lock);
            SetEvent(pipe->ready);
        }
    }
    return 0;
}

/* Snapshot the target of the frame that is being ended and copy its access unit
 * into a free ring slot.  Returns FALSE only on a hard failure (the caller then
 * drops the frame); the session is not touched here. */
static BOOL pipeline_submit(VIRTIO_WDDM_VideoDecoder *object, const UINT8 *data,
                            UINT size, UINT send_w, UINT send_h) {
    VIRTIO_WDDM_DecodePipeline *pipe = object->pipeline;
    VIRTIO_WDDM_VideoDecoderOutputView *view;
    VIRTIO_WDDM_Resource *resource;
    VIRTIO_WDDM_PipelineJob *job;
    UINT64 seq;
    UINT free_slots = 0;
    UINT i;

    if (!pipe || pipe->broken)
        return FALSE;
    view = object->output_view.pDrvPrivate;
    if (!view || view->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECOUT || !view->resource.pDrvPrivate) {
        ERROR("%s: no usable output view for the pipeline job -> E_FAIL", __FUNCTION__);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    resource = view->resource.pDrvPrivate;
    if (!resource->base.pResource) {
        ERROR("%s: output resource %p has no D3D11 texture -> E_FAIL", __FUNCTION__,
              (void *)resource);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    if (!data || size == 0) {
        ERROR("%s: empty access unit (%u B) -> E_FAIL", __FUNCTION__, size);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }

    EnterCriticalSection(&pipe->lock);
    for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++) {
        if (pipe->jobs[i].state == VIRTIO_WDDM_JOB_FREE)
            free_slots++;
    }
    if (!free_slots) {
        /* Cannot happen with one submit and one drain per EndFrame; refuse
         * instead of overwriting a job that is still in flight. */
        LeaveCriticalSection(&pipe->lock);
        ERROR("%s: all %u pipeline job slots are busy (submit=%llu drain=%llu) -> "
              "E_FAIL for this frame", __FUNCTION__, VIRTIO_WDDM_PIPELINE_JOBS,
              (unsigned long long)pipe->submit_seq, (unsigned long long)pipe->drain_seq);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    seq = pipe->submit_seq + 1u;
    job = &pipe->jobs[seq % VIRTIO_WDDM_PIPELINE_JOBS];
    if (job->state != VIRTIO_WDDM_JOB_FREE) {
        /* find any free slot (the ring is small, keep it simple) */
        job = NULL;
        for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++) {
            if (pipe->jobs[i].state == VIRTIO_WDDM_JOB_FREE) {
                job = &pipe->jobs[i];
                break;
            }
        }
        if (!job) {
            LeaveCriticalSection(&pipe->lock);
            decoder_fail_frame(object, E_FAIL);
            return FALSE;
        }
    }
    memset(&job->map, 0, sizeof(job->map));
    memset(&job->info, 0, sizeof(job->info));
    job->mapped = FALSE;
    job->using_slot = FALSE;
    job->error = 0;
    job->failure = NULL;
    job->send_ms = 0.0;
    job->fetch_ms = 0.0;
    job->ready_qpc = 0;
    job->seq = seq;
    job->generation = pipe->generation;
    job->submit_qpc = decoder_qpc_now();
    job->stage_qpc = 0;
    job->send_w = send_w;
    job->send_h = send_h;
    job->desc_w = object->desc.SampleWidth;
    job->desc_h = object->desc.SampleHeight;
    job->texture = resource->base.pResource;
    job->texture->lpVtbl->AddRef(job->texture);
    job->tex_format = (DXGI_FORMAT)resource->base.Format;
    job->tex_width = resource->base.Width;
    job->tex_height = resource->base.Height;
    job->array_size = resource->base.ArraySize;
    job->mips = resource->base.MipLevels ? resource->base.MipLevels : 1u;
    job->mip_slice = view->mip_slice;
    job->first_array_slice = view->first_array_slice;
    job->subresource = view->mip_slice + view->first_array_slice * job->mips;
    job->au_size = 0;
    if (!decoder_reserve(&job->au, &job->au_cap, size)) {
        job->texture->lpVtbl->Release(job->texture);
        job->texture = NULL;
        LeaveCriticalSection(&pipe->lock);
        ERROR("%s: cannot reserve %u B for the access unit -> E_FAIL", __FUNCTION__,
              size);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    memcpy(job->au, data, size);
    job->au_size = size;
    job->state = VIRTIO_WDDM_JOB_QUEUED;
    pipe->submit_seq = seq;
    pipe->submitted++;
    pipe->jobs_in_flight++;
    LeaveCriticalSection(&pipe->lock);

    object->bytes_sent += size;
    object->last_width = send_w;
    object->last_height = send_h;
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        object->last_send_qpc = now.QuadPart;
    }
    SetEvent(pipe->wake);
    VERBOSE("%s: job %llu queued: %u B, %ux%u -> tex=%p subresource=%u (gen %llu)",
            __FUNCTION__, (unsigned long long)seq, size, send_w, send_h,
            (void *)job->texture, job->subresource, (unsigned long long)job->generation);
    return TRUE;
}

/* Build the neutral picture the fill EndFrame writes: a zeroed NV12 surface is
 * not black, it is flat green (Y=0,U=V=0), which is the P2-2J bug. */
static void pipeline_prepare_fill(VIRTIO_WDDM_DecodePipeline *pipe, VIRTIO_WDDM_PipelineFrame *out) {
    UINT w = pipe->object->desc.SampleWidth ? pipe->object->desc.SampleWidth : 1920u;
    UINT h = pipe->object->desc.SampleHeight ? pipe->object->desc.SampleHeight : 1088u;
    UINT64 need = (UINT64)w * h * 3u / 2u;

    if (need == 0 || need > 0x10000000u)
        return;
    if (!decoder_reserve(&pipe->fill, &pipe->fill_cap, (UINT)need))
        return;
    if (pipe->fill_size != (UINT)need) {
        memset(pipe->fill, 16, (size_t)w * h);
        memset(pipe->fill + (size_t)w * h, 128, (size_t)need - (size_t)w * h);
        pipe->fill_size = (UINT)need;
    }
    out->fill = TRUE;
    out->src = pipe->fill;
    out->readable = need;
    out->info.Width = w;
    out->info.Height = h;
    out->info.Stride = w;
    out->info.Size = (UINT)need;
    out->info.FourCC = VIRTIO_WDDM_FOURCC_NV12;
}

/* Drain one job: wait (bounded) for the oldest pending job to be READY, hand
 * its pixels and its target to the caller, and release the slot.  Returns
 * FALSE when the pipeline cannot deliver a frame at all (the caller then drops
 * the frame); a fill returns TRUE with out->fill set. */
static BOOL pipeline_drain(VIRTIO_WDDM_VideoDecoder *object, VIRTIO_WDDM_PipelineFrame *out) {
    VIRTIO_WDDM_DecodePipeline *pipe = object->pipeline;
    VIRTIO_WDDM_PipelineJob *job = NULL;
    LONGLONG t_wait0;
    double waited;
    UINT i;

    memset(out, 0, sizeof(*out));
    if (!pipe)
        return FALSE;

    t_wait0 = decoder_qpc_now();
    EnterCriticalSection(&pipe->lock);
    /* The oldest job that has not been drained yet.  A single worker produces
     * them in submission order, so waiting for the oldest waits for all of
     * them. */
    for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++) {
        VIRTIO_WDDM_PipelineJob *candidate = &pipe->jobs[i];
        if (candidate->state == VIRTIO_WDDM_JOB_FREE || candidate->seq <= pipe->drain_seq)
            continue;
        if (!job || candidate->seq < job->seq)
            job = candidate;
    }
    if (job && pipe->defer && job->seq == pipe->submit_seq) {
        /* The only pending job is the one submitted a moment ago: with the
         * deferred pipeline this EndFrame must NOT wait for it, otherwise the
         * fetch is back on the critical path and the pipeline degenerates into
         * the serial path with an extra thread.  The frame that is owed is the
         * previous one, which has already been drained. */
        job = NULL;
    }
    if (!job) {
        /* Fill frame: nothing older is pending.  Write the placeholder instead
         * of leaving the subresource untouched. */
        VIRTIO_WDDM_VideoDecoderOutputView *view = object->output_view.pDrvPrivate;
        VIRTIO_WDDM_Resource *resource =
            view && view->resource.pDrvPrivate ? view->resource.pDrvPrivate : NULL;
        /* drain_seq deliberately does NOT move: the job submitted by this
         * EndFrame is still owed, and the next EndFrame has to upload it. */
        pipe->fills++;
        pipeline_prepare_fill(pipe, out);
        if (resource && resource->base.pResource) {
            out->texture = resource->base.pResource;
            out->tex_format = (DXGI_FORMAT)resource->base.Format;
            out->tex_width = resource->base.Width;
            out->tex_height = resource->base.Height;
            out->array_size = resource->base.ArraySize;
            out->mips = resource->base.MipLevels ? resource->base.MipLevels : 1u;
            out->mip_slice = view->mip_slice;
            out->first_array_slice = view->first_array_slice;
            out->subresource = view->mip_slice + view->first_array_slice * out->mips;
        }
        LeaveCriticalSection(&pipe->lock);
        if (pipe->fill_logged < 4u) {
            pipe->fill_logged++;
            INFO("%s: pipeline fill #%llu: no older job to upload, writing the "
                 "neutral %ux%u picture into subresource %u (a zeroed NV12 slice "
                 "would present as flat green)", __FUNCTION__,
                 (unsigned long long)pipe->fills, out->info.Width, out->info.Height,
                 out->subresource);
        }
        out->queue_ms = 0.0;
        out->lead_ms = 0.0;
        return out->src != NULL;
    }

    {
        LONGLONG deadline = decoder_qpc_now() +
                            (LONGLONG)(pipe->timeout_ms + 250u) *
                                decoder_qpc_freq() / 1000;
        for (;;) {
            UINT left;

            if (job->state == VIRTIO_WDDM_JOB_READY ||
                job->state == VIRTIO_WDDM_JOB_FAILED ||
                job->state == VIRTIO_WDDM_JOB_FREE)
                break;
            if (job->generation != pipe->generation)
                break; /* quiesced under us: the job is no longer ours */
            left = pipeline_left_ms(deadline);
            if (left == 0) {
                job->state = VIRTIO_WDDM_JOB_FAILED;
                job->failure = "drain timeout";
                job->error = ERROR_SEM_TIMEOUT;
                pipe->timeouts++;
                pipe->failed_jobs++;
                ERROR("%s: job %llu: the worker did not produce it within %u ms "
                      "-> job failed (submit=%llu drain=%llu state=%u)",
                      __FUNCTION__, (unsigned long long)job->seq,
                      pipe->timeout_ms + 250u, (unsigned long long)pipe->submit_seq,
                      (unsigned long long)pipe->drain_seq, job->state);
                break;
            }
            LeaveCriticalSection(&pipe->lock);
            WaitForSingleObject(pipe->ready, left > 20 ? 20 : left);
            EnterCriticalSection(&pipe->lock);
        }
    }

    waited = decoder_qpc_ms(t_wait0, decoder_qpc_now());
    pipe->wait_sum += waited;
    if (waited > pipe->wait_max)
        pipe->wait_max = waited;

    out->wait_ms = waited;
    out->error = job->error;
    out->failure = job->failure;
    out->send_ms = job->send_ms;
    out->fetch_ms = job->fetch_ms;
    out->start_qpc = job->send_qpc;
    out->queue_ms = job->ready_qpc ? decoder_qpc_ms(job->submit_qpc, job->ready_qpc) : -1.0;
    out->lead_ms = job->ready_qpc ? decoder_qpc_ms(job->ready_qpc, decoder_qpc_now()) : -1.0;
    if (out->queue_ms > 0.0) {
        pipe->queue_sum += out->queue_ms;
        if (out->queue_ms > pipe->queue_max)
            pipe->queue_max = out->queue_ms;
    }
    if (out->lead_ms > 0.0) {
        pipe->lead_sum += out->lead_ms;
        if (out->lead_ms > pipe->lead_max)
            pipe->lead_max = out->lead_ms;
    }

    if (job->state != VIRTIO_WDDM_JOB_READY) {
        /* FAILED, FREE (quiesced) or timed out: the frame is dropped. */
        pipe->failed_jobs += (job->state == VIRTIO_WDDM_JOB_FAILED) ? 0u : 1u;
        out->failed = TRUE;
        ERROR("%s: job %llu dropped: %s (err=%u, waited %.2f ms, state=%u)",
              __FUNCTION__, (unsigned long long)job->seq,
              job->failure ? job->failure : "no frame", job->error, waited,
              job->state);
        pipeline_job_release_locked(pipe, job, "failed at drain");
        pipe->drain_seq = job->seq;
        pipe->jobs_in_flight = pipe->jobs_in_flight ? pipe->jobs_in_flight - 1u : 0u;
        LeaveCriticalSection(&pipe->lock);
        return TRUE;
    }

    /* Hand the pixels over. */
    out->texture = job->texture;
    out->tex_format = job->tex_format;
    out->tex_width = job->tex_width;
    out->tex_height = job->tex_height;
    out->array_size = job->array_size;
    out->mips = job->mips;
    out->mip_slice = job->mip_slice;
    out->first_array_slice = job->first_array_slice;
    out->subresource = job->subresource;
    out->info = job->info;
    if (job->mapped) {
        /* Transfer the capture buffer to the DDI thread: the existing decode
         * tail reads it and decoder_drop_direct() gives it back on every exit
         * path, which also tells the worker that the next map may start. */
        object->direct_map = job->map;
        object->direct_active = TRUE;
        job->mapped = FALSE;
        pipe->mapping_out = TRUE;
        pipe->mapping_out_seq = job->seq;
        out->src = (const UINT8 *)job->map.UserVa;
        out->readable = (UINT64)job->map.MappedLength;
        pipe->drained_mapped++;
    } else {
        out->src = job->slot;
        out->readable = (UINT64)job->slot_cap;
        pipe->drained_staged++;
    }
    if (job->texture) {
        job->texture->lpVtbl->Release(job->texture);
        job->texture = NULL;
    }
    {
        /* Log before the slot is cleared: the state below is what the summary
         * needs, and the job's own fields are gone once it is reused. */
        UINT au_size = job->au_size;
        BOOL staged = job->using_slot;

        job->state = VIRTIO_WDDM_JOB_FREE;
        job->au_size = 0;
        job->using_slot = FALSE;
        pipe->drain_seq = job->seq;
        pipe->drained++;
        pipe->jobs_in_flight = pipe->jobs_in_flight ? pipe->jobs_in_flight - 1u : 0u;
        pipe->logged++;
        if (pipe->logged <= 8u || (job->seq % 120u) == 0u) {
            INFO("%s: job %llu %s: au=%u B send=%.2f fetch=%.2f -> subresource %u "
                 "(tex=%p) queue=%.2f lead=%.2f wait=%.2f ms (gen %llu, %s)",
                 __FUNCTION__, (unsigned long long)job->seq,
                 staged ? "staged" : "pool",
                 au_size, job->send_ms, job->fetch_ms, out->subresource,
                 (void *)out->texture, out->queue_ms, out->lead_ms, waited,
                 (unsigned long long)job->generation,
                 out->lead_ms >= 0.0 ? "worker ahead" : "worker late");
        } else {
            VERBOSE("%s: job %llu: send=%.2f fetch=%.2f queue=%.2f lead=%.2f ms",
                    __FUNCTION__, (unsigned long long)job->seq, job->send_ms,
                    job->fetch_ms, out->queue_ms, out->lead_ms);
        }
    }
    LeaveCriticalSection(&pipe->lock);
    return TRUE;
}

/* Discard everything in flight and make sure no ioctl of the old sequence is
 * still running.  Called before vm_flush (seek / IDR / parameter change) and
 * from DestroyVideoDecoder.  Blocks until the worker is between stages, which
 * is bounded by the job timeout. */
static void pipeline_quiesce(VIRTIO_WDDM_VideoDecoder *object, const char *reason) {
    VIRTIO_WDDM_DecodePipeline *pipe = object ? object->pipeline : NULL;
    LONGLONG t0;
    UINT i;
    UINT64 generation;
    UINT dropped_before;

    if (!pipe)
        return;
    if (pipe->broken) {
        INFO("%s: pipeline already broken (%s) -> nothing to quiesce", __FUNCTION__,
             reason ? reason : "?");
        return;
    }
    t0 = decoder_qpc_now();
    /* Bounded wait for the worker to leave whatever stage it is in: it holds
     * the lock only around an ioctl, and every ioctl has a timeout, so this
     * normally returns immediately.  If it does not, the session state is
     * unknown and the pipeline is marked broken instead of being flushed under
     * a live ioctl. */
    for (;;) {
        if (TryEnterCriticalSection(&pipe->lock))
            break;
        if (decoder_qpc_ms(t0, decoder_qpc_now()) >
            (double)(pipe->timeout_ms + 500u)) {
            pipe->broken = TRUE;
            ERROR("%s: the worker did not reach a stage boundary within %u ms (%s) "
                  "-> pipeline marked broken, the session is no longer trusted",
                  __FUNCTION__, pipe->timeout_ms + 500u, reason ? reason : "?");
            return;
        }
        Sleep(1);
    }
    dropped_before = (UINT)pipe->dropped;
    generation = pipe->generation;
    pipe->generation = generation + 1u;
    for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++) {
        VIRTIO_WDDM_PipelineJob *job = &pipe->jobs[i];
        if (job->state == VIRTIO_WDDM_JOB_FREE)
            continue;
        pipeline_job_release_locked(pipe, job, reason ? reason : "quiesce");
    }
    pipe->drain_seq = pipe->submit_seq;
    pipe->mapping_out = FALSE;
    LeaveCriticalSection(&pipe->lock);
    SetEvent(pipe->wake);
    if (object->direct_active) {
        /* Cannot normally happen: the drain hands a mapping to the DDI thread
         * and the same EndFrame gives it back.  If it does, the flush is about
         * to close the session, so give it back first. */
        ERROR("%s: a capture buffer was still checked out at flush time (%s) -> "
              "releasing it before the session is replaced", __FUNCTION__,
              reason ? reason : "?");
        decoder_drop_direct(object);
    }
    INFO("%s: pipeline quiesced for %s: %llu job(s) dropped, generation %llu -> %llu, "
         "waited %.2f ms for the worker", __FUNCTION__, reason ? reason : "?",
         (unsigned long long)(pipe->dropped - dropped_before),
         (unsigned long long)generation, (unsigned long long)pipe->generation,
         decoder_qpc_ms(t0, decoder_qpc_now()));
}

static void pipeline_destroy(VIRTIO_WDDM_VideoDecoder *object) {
    VIRTIO_WDDM_DecodePipeline *pipe = object ? object->pipeline : NULL;
    UINT i;

    if (!pipe)
        return;
    EnterCriticalSection(&pipe->lock);
    pipe->stop = TRUE;
    LeaveCriticalSection(&pipe->lock);
    SetEvent(pipe->wake);
    if (pipe->thread) {
        DWORD waited = WaitForSingleObject(pipe->thread, pipe->timeout_ms + 2000u);
        if (waited != WAIT_OBJECT_0)
            ERROR("%s: the pipeline worker did not stop within %u ms (state=%lu) "
                  "-> closing the session anyway", __FUNCTION__,
                  pipe->timeout_ms + 2000u, (unsigned long)waited);
        CloseHandle(pipe->thread);
        pipe->thread = NULL;
    }
    EnterCriticalSection(&pipe->lock);
    for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++)
        pipeline_job_release_locked(pipe, &pipe->jobs[i], "destroy");
    LeaveCriticalSection(&pipe->lock);

    INFO("%s: pipeline summary: %llu submitted / %llu produced / %llu uploaded "
         "(%llu pool, %llu staged) / %llu fill(s) / %llu dropped / %llu failed job(s) "
         "| %llu send failure(s) / %llu fetch failure(s) / %llu timeout(s) | "
         "queue avg %.2f max %.2f ms, worker lead avg %.2f max %.2f ms, "
         "DDI wait avg %.2f max %.2f ms | %s, %u job(s), timeout %u ms, "
         "generation %llu",
         __FUNCTION__, (unsigned long long)pipe->submitted,
         (unsigned long long)pipe->produced, (unsigned long long)pipe->drained,
         (unsigned long long)pipe->drained_mapped,
         (unsigned long long)pipe->drained_staged, (unsigned long long)pipe->fills,
         (unsigned long long)pipe->dropped, (unsigned long long)pipe->failed_jobs,
         (unsigned long long)pipe->send_failures,
         (unsigned long long)pipe->fetch_failures, (unsigned long long)pipe->timeouts,
         pipe->drained ? pipe->queue_sum / (double)pipe->drained : 0.0, pipe->queue_max,
         pipe->drained ? pipe->lead_sum / (double)pipe->drained : 0.0, pipe->lead_max,
         pipe->drained ? pipe->wait_sum / (double)pipe->drained : 0.0, pipe->wait_max,
         pipe->defer ? "deferred" : "immediate", VIRTIO_WDDM_PIPELINE_JOBS,
         pipe->timeout_ms, (unsigned long long)pipe->generation);

    for (i = 0; i < VIRTIO_WDDM_PIPELINE_JOBS; i++) {
        if (pipe->jobs[i].au)
            HeapFree(GetProcessHeap(), 0, pipe->jobs[i].au);
        if (pipe->jobs[i].slot)
            HeapFree(GetProcessHeap(), 0, pipe->jobs[i].slot);
        memset(&pipe->jobs[i], 0, sizeof(pipe->jobs[i]));
    }
    if (pipe->fill)
        HeapFree(GetProcessHeap(), 0, pipe->fill);
    if (pipe->wake)
        CloseHandle(pipe->wake);
    if (pipe->ready)
        CloseHandle(pipe->ready);
    DeleteCriticalSection(&pipe->lock);
    HeapFree(GetProcessHeap(), 0, pipe);
    object->pipeline = NULL;
}

/* Arm the pipeline for this decoder.  Opt-out (like the log file and the
 * zero-copy route), so a guest can A/B on one machine without a rebuild. */
static BOOL pipeline_create(VIRTIO_WDDM_VideoDecoder *object) {
    VIRTIO_WDDM_DecodePipeline *pipe;
    BOOL enabled;
    UINT depth;

    if (!object)
        return FALSE;
    enabled = pipeline_env_flag("VIRTIO_WDDM_PIPELINE", TRUE);
    if (!enabled) {
        INFO("%s: pipeline disabled by VIRTIO_WDDM_PIPELINE -> the serial "
             "send/get/upload path", __FUNCTION__);
        return FALSE;
    }
    if (object->codec == VIRTIO_WDDM_CODEC_VP9) {
        INFO("%s: VP9 keeps the serial path (one EndFrame may carry several "
             "superframe members and only the last picture is uploaded)", __FUNCTION__);
        return FALSE;
    }
    if (object->driver_version < VMEDIA_FLUSH_MIN_DRIVER_VERSION) {
        INFO("%s: driver 0x%08x predates VM_IOCTL_FLUSH (0x%08x) -> serial path, a "
             "one-shot driver cannot have two access units in flight",
             __FUNCTION__, object->driver_version, (UINT)VMEDIA_FLUSH_MIN_DRIVER_VERSION);
        return FALSE;
    }
    depth = pipeline_env_uint("VIRTIO_WDDM_PIPELINE_DEPTH", 2u, 1u, 2u);
    pipe = (VIRTIO_WDDM_DecodePipeline *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                   sizeof(*pipe));
    if (!pipe) {
        ERROR("%s: out of memory for the pipeline state -> serial path", __FUNCTION__);
        return FALSE;
    }
    pipe->object = object;
    pipe->defer = depth >= 2u;
    pipe->timeout_ms = pipeline_env_uint("VIRTIO_WDDM_PIPELINE_TIMEOUT_MS", 1000u, 50u,
                                         10000u);
    InitializeCriticalSection(&pipe->lock);
    pipe->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    pipe->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!pipe->wake || !pipe->ready) {
        ERROR("%s: cannot create the pipeline events err=%lu -> serial path",
              __FUNCTION__, (unsigned long)GetLastError());
        goto fail;
    }
    object->pipeline = pipe;
    pipe->thread = CreateThread(NULL, 0, pipeline_worker, pipe, 0, &pipe->thread_id);
    if (!pipe->thread) {
        ERROR("%s: CreateThread failed err=%lu -> serial path", __FUNCTION__,
              (unsigned long)GetLastError());
        object->pipeline = NULL;
        goto fail;
    }
    pipe->started = TRUE;
    INFO("%s: pipeline armed (%s, %u job(s), timeout %u ms, codec %s, drv=0x%08x): "
         "vm_send + fetch on the worker, the upload of the previous job on the "
         "DDI thread -- see 04-测试记录/10-流水线.md",
         __FUNCTION__, pipe->defer ? "deferred by one frame" : "immediate drain",
         VIRTIO_WDDM_PIPELINE_JOBS, pipe->timeout_ms,
         decoder_codec_name(object->codec), object->driver_version);
    return TRUE;

fail:
    if (pipe->wake)
        CloseHandle(pipe->wake);
    if (pipe->ready)
        CloseHandle(pipe->ready);
    DeleteCriticalSection(&pipe->lock);
    HeapFree(GetProcessHeap(), 0, pipe);
    return FALSE;
}

/* decoder_drop_direct() hook: the capture buffer is back in the device's queue,
 * so the worker may map the next frame. */
static void pipeline_on_mapping_freed(VIRTIO_WDDM_VideoDecoder *object) {
    VIRTIO_WDDM_DecodePipeline *pipe = object ? object->pipeline : NULL;

    if (!pipe)
        return;
    EnterCriticalSection(&pipe->lock);
    pipe->mapping_out = FALSE;
    LeaveCriticalSection(&pipe->lock);
    SetEvent(pipe->wake);
}

static void pipeline_session_lock(VIRTIO_WDDM_VideoDecoder *object) {
    if (object && object->pipeline)
        EnterCriticalSection(&object->pipeline->lock);
}

static void pipeline_session_unlock(VIRTIO_WDDM_VideoDecoder *object) {
    if (object && object->pipeline)
        LeaveCriticalSection(&object->pipeline->lock);
}

/* The EndFrame entry point of the pipeline: queue this access unit, then upload
 * the oldest finished job.  FALSE means "the caller must not continue" (the
 * failure is already recorded with decoder_fail_frame). */
static BOOL pipeline_frame(VIRTIO_WDDM_VideoDecoder *object, const UINT8 *data, UINT size,
                           UINT send_w, UINT send_h, VIRTIO_WDDM_PipelineFrame *out) {
    VIRTIO_WDDM_DecodePipeline *pipe = object->pipeline;

    if (!pipe)
        return FALSE;
    if (pipe->broken) {
        ERROR("%s: the pipeline is broken (a stage timed out earlier) -> E_FAIL",
              __FUNCTION__);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    if (!pipeline_submit(object, data, size, send_w, send_h))
        return FALSE;
    if (!pipeline_drain(object, out)) {
        ERROR("%s: pipeline_drain could not deliver a frame (fill buffer "
              "unavailable) -> E_FAIL", __FUNCTION__);
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    if (out->failed) {
        ERROR("%s: the pipeline dropped this frame (%s, err=%u); the SEND may have "
              "left the session in an unknown state, so it is flushed and the "
              "pipeline generation moves on", __FUNCTION__,
              out->failure ? out->failure : "no frame", out->error);
        pipeline_quiesce(object, "job failure");
        (void)decoder_flush_session(object, "pipeline job failure");
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    return TRUE;
}

/* AV1FIX (2026-10-25): fetch one staged frame with the host wait and the copy
 * separated in time.
 *
 * VM_IOCTL_GET_FRAME does both inside one ioctl ("block for one decoded NV12
 * frame"), so a plain blocking call can only report their sum -- that is how the
 * AV1 question "is the host late, or is the copy late?" stayed unanswerable.  The
 * ABI documents TimeoutMs = 0 as "poll once", so the caller can drive the wait
 * itself: each poll returns ERROR_SEM_TIMEOUT (121) until a frame is ready, and
 * the poll that succeeds carries the copy.  `wait_ms` is therefore the time until
 * the host produced the frame, `copy_ms` the time of the successful poll (a
 * 3,110,400 B NV12 copy is ~1-3 ms), and `polls` how often the driver was asked.
 *
 * Only used for AV1 and only after the first timeout (see decoder_decode_unit),
 * so a working session never pays for the extra ioctls.  The caller keeps the
 * existing buffer-growth retry: ERROR_INSUFFICIENT_BUFFER is returned to it
 * unmodified. */
static BOOL decoder_get_frame_polled(VIRTIO_WDDM_VideoDecoder *object,
                                     VM_FRAME_INFO *info, UINT budget_ms,
                                     VIRTIO_WDDM_DecodeTiming *timing) {
    LONGLONG start = decoder_qpc_now();
    LONGLONG before;
    UINT polls = 0;
    UINT error;

    timing->wait_ms = -1.0;
    timing->copy_ms = -1.0;
    timing->polls = 0;
    timing->last_error = 0;
    for (;;) {
        before = decoder_qpc_now();
        if (vm_get_frame(object->session, object->nv12, object->nv12_cap, info, 0)) {
            timing->copy_ms = decoder_qpc_ms(before, decoder_qpc_now());
            timing->wait_ms = decoder_qpc_ms(start, before);
            timing->polls = polls;
            timing->last_error = 0;
            return TRUE;
        }
        error = vm_last_error();
        timing->last_error = error;
        if (error != ERROR_SEM_TIMEOUT) {
            /* A real error (or the buffer-growth retry) belongs to the caller. */
            timing->wait_ms = decoder_qpc_ms(start, decoder_qpc_now());
            timing->polls = polls;
            return FALSE;
        }
        polls++;
        if (decoder_qpc_ms(start, decoder_qpc_now()) >= (double)budget_ms) {
            timing->wait_ms = decoder_qpc_ms(start, decoder_qpc_now());
            timing->copy_ms = 0.0; /* the copy never happened */
            timing->polls = polls;
            return FALSE;
        }
        Sleep(1);
    }
}

/* PIPELINE (2026-10-25): the S_FMT(OUTPUT) geometry an access unit is sent
 * with.  Identical to the inline block decoder_decode_unit() used before this
 * round; it is a function because the pipeline worker performs the vm_send() on
 * its own thread and must be handed exactly the same numbers.
 *
 * AV1 sends the CODED size (DXVA_PicParams_AV1 width/height) rather than the
 * 64-row aligned surface, or the host reconfigures its capture queue
 * (SOURCE_CHANGE) after the first frame.  H.264/HEVC keep the surface size,
 * which is what every good run so far used. */
static void decoder_send_geometry(VIRTIO_WDDM_VideoDecoder *object, UINT width,
                                  UINT height, UINT *send_w, UINT *send_h) {
    UINT w = width;
    UINT h = height;

    if (!object) {
        *send_w = w;
        *send_h = h;
        return;
    }
    if (object->codec == VIRTIO_WDDM_CODEC_AV1 && object->av1_frame.width &&
        object->av1_frame.height && object->av1_frame.width <= width &&
        object->av1_frame.height <= height) {
        w = object->av1_frame.width;
        h = object->av1_frame.height;
        if (object->submits <= 1u && (w != width || h != height)) {
            INFO("%s: AV1 S_FMT OUTPUT geometry %ux%u (coded) instead of the %ux%u "
                 "surface -- the surface's 64-row alignment must not define the "
                 "host's coded size, or the capture format changes mid-stream on "
                 "the first frame (SOURCE_CHANGE)", __FUNCTION__, w, h, width, height);
        }
    }
    *send_w = w;
    *send_h = h;
}

static BOOL decoder_decode_unit(VIRTIO_WDDM_VideoDecoder *object,
                                const UINT8 *data, UINT size,
                                UINT width, UINT height,
                                VIRTIO_WDDM_DecodeTiming *timing,
                                VM_FRAME_INFO *info) {
    LONGLONG t0, t1, g0, g1;
    UINT need, start, attempt, get_error, send_w, send_h;

    timing->start_qpc = 0;
    timing->send_ms = 0.0;
    timing->get_ms = 0.0;
    timing->elapsed_ms = 0;
    timing->wait_ms = -1.0; /* -1 = not separable: wait and copy share one ioctl */
    timing->copy_ms = -1.0;
    timing->polls = 0;
    timing->last_error = 0;

    /* AV1FIX (2026-10-25): which geometry goes into S_FMT OUTPUT.
     *
     * For H.264/HEVC the UMD passes the *surface* size and the host tolerates the
     * mismatch: an HEVC 1080p stream is coded 1920x1088 and the texture is
     * 1920x1152, so the host was configured with 1152 and every SEND logs
     * "geometry 1920x1152 differs from the configured 1920x1088" -- 602 frames
     * decoded anyway (E173 ring-hevc3.txt), i.e. that mismatch alone is not
     * fatal.
     *
     * AV1 is different, and the same ring shows it: the AV1 1080p texture is
     * 1920x1152 (the client 64-row aligns the surface) while the bitstream's
     * frames are 1920x1080, so the host accepted S_FMT OUTPUT 1920x1152 and then
     * *changed* the capture format mid-stream:
     *     S_FMT CAPTURE 1920x1152 ... EVENT SOURCE_CHANGE
     *     G_FMT CAPTURE after SOURCE_CHANGE: 1920x1080
     *     SEND: geometry 1920x1152 differs from the configured 1920x1080   (x21)
     * A stateful V4L2 decoder that reconfigures its capture queue on the first
     * decoded frame is exactly where "first frame OK, then nothing" starts, and
     * the AV1 frame header already carries the real coded size (DXVA_PicParams_AV1
     * width/height, validated against the surface by av1_frame_from_pic), so the
     * UMD now configures the host with the CODED size.  1080 is also what the
     * pool budget wants (3,110,400 B per frame instead of 3,317,760).
     *
     * Only AV1 gets this: changing the geometry for H.264/HEVC would alter the
     * two paths that are known good today (E173: 602 HEVC frames, 557 H.264)
     * without a machine to re-verify them on.
     *
     * PIPELINE (2026-10-25): moved into decoder_send_geometry() because the
     * pipeline worker sends the access unit on its own thread and has to be
     * given the same geometry the serial path computes here. */
    decoder_send_geometry(object, width, height, &send_w, &send_h);

    /* Flags stay 0: bit 0 asks the host to STOP the decoder after STREAMON,
     * which is right for a single-shot test but would tear down a streaming
     * session after the first frame. */
    t0 = decoder_qpc_now();
    timing->start_qpc = t0;
    if (!vm_send(object->session, data, size, send_w, send_h, 0, NULL)) {
        UINT error = vm_last_error();
        VERBOSE("%s: vm_send(%s, %u byte(s)) was refused err=%u; reopening the session",
                __FUNCTION__, decoder_codec_name(object->codec), size, error);
        if (!decoder_reopen_session(object) ||
            !vm_send(object->session, data, size, send_w, send_h, 0, NULL)) {
            UINT retry_error = vm_last_error();
            ERROR("%s: vm_send(%s, %u byte(s)) failed err=%u then err=%u -> E_FAIL",
                  __FUNCTION__, decoder_codec_name(object->codec), size, error,
                  retry_error);
            decoder_fail_frame(object, E_FAIL);
            return FALSE;
        }
    }
    t1 = decoder_qpc_now();
    timing->send_ms = decoder_qpc_ms(t0, t1);
    object->frame_sent = TRUE;
    object->last_width = send_w;
    object->last_height = send_h;
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        object->last_send_qpc = now.QuadPart;
    }
    object->bytes_sent += size;

    /* P-BATCH P010: the scratch buffer has to hold whichever layout the host
     * returns -- 1 byte per sample (NV12) or 2 (P010) -- so it is sized for
     * 3 bytes per pixel, the P010 frame with a padded stride. It is still
     * grown from the high-water mark (decoder_reserve), so steady-state
     * playback allocates nothing. */
    /* P-BATCH ZEROCOPY: prefer the direct-mapped frame.  Nothing is allocated
     * and nothing is copied on this path: the NV12 pixels are read straight out
     * of the media pool's pages, which the driver mapped into this process.
     * The staged buffer below is only reserved when we actually fall back, so a
     * decoder running direct never pays its 3 bytes/pixel. */
    memset(info, 0, sizeof(*info));
    start = GetTickCount();
    g0 = decoder_qpc_now();
    if (decoder_try_direct_frame(object, info)) {
        timing->elapsed_ms = GetTickCount() - start;
        g1 = decoder_qpc_now();
        timing->get_ms = decoder_qpc_ms(g0, g1);
        /* The mapped path waits in MAP_FRAME and copies nothing: the ioctl time
         * IS the host wait and the pixels are uploaded from the pool pages. */
        timing->wait_ms = timing->get_ms;
        timing->copy_ms = 0.0;
        timing->polls = 0;
        object->last_wait_ms = timing->wait_ms;
        object->last_copy_ms = 0.0;
        object->last_get_polls = 0;
        return TRUE;
    }

    need = send_w * send_h * 3u + send_w * 16u + 4096u;
    if (!decoder_reserve(&object->nv12, &object->nv12_cap, need)) {
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    for (attempt = 0;; attempt++) {
        LONGLONG get_start = decoder_qpc_now();
        BOOL got;

        /* AV1FIX: once an AV1 session has timed out, the frame is fetched with
         * TimeoutMs = 0 ("poll once", VM_GET_FRAME_IN) so the host wait and the
         * NV12 copy become two numbers instead of one.  A healthy session keeps
         * the single blocking call: no extra ioctls, no polling. */
        if (object->codec == VIRTIO_WDDM_CODEC_AV1 && object->frame_get_segmented) {
            got = decoder_get_frame_polled(object, info, 1000, timing);
            get_error = timing->last_error;
        } else {
            got = vm_get_frame(object->session, object->nv12, object->nv12_cap, info,
                               1000);
            get_error = got ? 0u : vm_last_error();
            timing->last_error = get_error;
        }
        g1 = decoder_qpc_now();
        timing->get_ms = decoder_qpc_ms(get_start, g1);
        if (got)
            break;
        if (get_error == ERROR_INSUFFICIENT_BUFFER && attempt == 0 &&
            object->nv12_cap < 0x08000000u) {
            UINT bigger = object->nv12_cap * 2u;
            INFO("%s: frame does not fit %u bytes, retrying with %u", __FUNCTION__,
                 object->nv12_cap, bigger);
            if (!decoder_reserve(&object->nv12, &object->nv12_cap, bigger)) {
                decoder_fail_frame(object, E_FAIL);
                return FALSE;
            }
            continue;
        }
        /* AV1FIX (2026-10-25): the queue rule.  A timeout is the AV1 failure mode
         * (err=121 x25 in E173): the host produced no frame, so the picture is
         * dropped -- but before it is dropped, whatever capture buffer may still
         * be checked out is handed back.  decoder_fail_frame() also drops the
         * mapping it knows about; this covers the one it does not. */
        if (get_error == ERROR_SEM_TIMEOUT) {
            object->frame_get_timeouts++;
            if (object->codec == VIRTIO_WDDM_CODEC_AV1 && !object->frame_get_segmented) {
                object->frame_get_segmented = TRUE;
                INFO("%s: AV1 GET_FRAME timed out (err=%u) -> segmented timing armed "
                     "for the rest of this session: every frame now reports host "
                     "wait, NV12 copy and poll count separately", __FUNCTION__,
                     get_error);
            }
        }
        decoder_release_stray_frame(object);
        object->last_wait_ms = timing->wait_ms;
        object->last_copy_ms = timing->copy_ms;
        object->last_get_polls = timing->polls;
        ERROR("%s: vm_get_frame(%s, 1000 ms) failed err=%u after %lu ms -> E_FAIL "
              "(segments: wait=%.2f ms copy=%.2f ms polls=%u, %s)",
              __FUNCTION__, decoder_codec_name(object->codec), get_error,
              (unsigned long)(GetTickCount() - start), timing->wait_ms,
              timing->copy_ms, timing->polls,
              object->frame_get_segmented ? "polled" : "one blocking ioctl");
        decoder_fail_frame(object, E_FAIL);
        return FALSE;
    }
    timing->elapsed_ms = GetTickCount() - start;
    g1 = decoder_qpc_now();
    if (timing->wait_ms < 0.0) {
        /* No split available for this path: report the whole ioctl and say so. */
        timing->wait_ms = timing->get_ms;
        timing->copy_ms = -1.0;
    }
    object->last_wait_ms = timing->wait_ms;
    object->last_copy_ms = timing->copy_ms;
    object->last_get_polls = timing->polls;
    return TRUE;
}

/* VP9FIX (2026-10-25): validate the client's DXVA_PicParams_VP9 against the one
 * profile this decoder advertises -- VP9 VLD Profile0, 8-bit 4:2:0, NV12 output.
 *
 * The struct is only used for logging, the sequence-start decision and the
 * geometry warning, so a wrong field never crashed on its own; but it is the
 * only description of the picture the UMD gets before the bytes go to the host,
 * and a 10-bit / 4:4:4 / oversized picture means the NV12 surface the result is
 * written into cannot be right.  Fields checked:
 *   profile / bit depth  -> a profile-0 decoder must be given 8-bit data;
 *   frame_type/intra_only-> the sequence-start heuristic reads them;
 *   width/height         -> must fit the surface (0 = "client did not fill");
 *   log2_tile_cols/rows  -> VP9 6.2.10 caps them at 6 (64 columns) and 2
 *                           (4 rows); DXVA_PicParams_VP9 carries both fields.
 * A violation returns FALSE with *error naming the field; the caller refuses the
 * frame with E_INVALIDARG.  Fields a client may legitimately leave at 0
 * (subsampling, tile counts with width 0) are logged once, not fatal. */
static BOOL decoder_vp9_pic_params_valid(VIRTIO_WDDM_VideoDecoder *object,
                                         const char **error) {
    const DXVA_PicParams_VP9 *p = &object->vp9_pic;
    UINT surface_width = object->desc.SampleWidth;
    UINT surface_height = object->desc.SampleHeight;

    if (error)
        *error = NULL;
    if (!object->vp9_pic_this_frame)
        return TRUE; /* the client described no picture for this frame */

    if (p->profile != 0u) {
        if (error)
            *error = "DXVA_PicParams_VP9.profile is not 0";
        return FALSE;
    }
    if (p->BitDepthMinus8Luma != 0u || p->BitDepthMinus8Chroma != 0u) {
        if (error)
            *error = "bit depth is not 8 (only profile 0 / NV12 is advertised)";
        return FALSE;
    }
    if ((unsigned)p->frame_type > 1u) {
        if (error)
            *error = "frame_type is neither KEY (0) nor INTER (1)";
        return FALSE;
    }
    if (p->frame_type == 0u && p->intra_only != 0u) {
        if (error)
            *error = "KEY frame with intra_only set";
        return FALSE;
    }
    if (p->log2_tile_cols > VIRTIO_WDDM_VP9_MAX_TILE_COLS_LOG2) {
        if (error)
            *error = "log2_tile_cols is out of range (>6)";
        return FALSE;
    }
    if (p->log2_tile_rows > VIRTIO_WDDM_VP9_MAX_TILE_ROWS_LOG2) {
        if (error)
            *error = "log2_tile_rows is out of range (>2)";
        return FALSE;
    }
    if (p->width == 0u || p->height == 0u) {
        if (object->vp9_pic_warnings < 4u) {
            object->vp9_pic_warnings++;
            INFO("%s: DXVA_PicParams_VP9 has width/height 0 (the client did not "
                 "fill the coded size); geometry is judged from the bitstream "
                 "header instead", __FUNCTION__);
        }
    } else if (p->width > surface_width || p->height > surface_height) {
        if (error)
            *error = "coded size is larger than the output surface";
        return FALSE;
    }
    if (p->subsampling_x == 0u || p->subsampling_y == 0u) {
        if (object->vp9_pic_warnings < 4u) {
            object->vp9_pic_warnings++;
            INFO("%s: DXVA_PicParams_VP9 says subsampling %u/%u (profile 0 is "
                 "4:2:0 = 1/1); the NV12 output surface still governs, so the "
                 "frame is not refused", __FUNCTION__, (UINT)p->subsampling_x,
                 (UINT)p->subsampling_y);
        }
    }
    if (p->extra_plane != 0u) {
        if (object->vp9_pic_warnings < 4u) {
            object->vp9_pic_warnings++;
            INFO("%s: DXVA_PicParams_VP9 has extra_plane set, but profile 0 has "
                 "no alpha plane; the flag is ignored", __FUNCTION__);
        }
    }
    /* VP9 6.2.10: a tile column is at least 256 luma samples wide.  A stream
     * that claims more columns than its coded width can hold will not decode
     * correctly, but this is a conformance check, not a memory-safety one, so it
     * is logged once and left to the host's parser to reject. */
    if (p->width != 0u &&
        (1u << p->log2_tile_cols) > ((p->width + 255u) / 256u) &&
        object->vp9_pic_warnings < 4u) {
        object->vp9_pic_warnings++;
        INFO("%s: DXVA_PicParams_VP9 claims %u tile columns for a %u px wide "
             "picture (minimum tile width is 256 px); the host parser decides",
             __FUNCTION__, 1u << p->log2_tile_cols, p->width);
    }
    return TRUE;
}

/* Assemble a VP9 access unit from the client's control buffers.
 *
 * The bytes of a VP9 picture are in the BITSTREAM buffer, but where they are in
 * it is described by the SLICE_CONTROL records (DXVA_Slice_VPx_Short, 10 bytes:
 * BSNALunitDataLocation / SliceBytesInBuffer / wBadSliceChopping) -- a client
 * may submit several frames of a superframe as several records and may leave
 * padding between them.  The records are therefore resolved in record order;
 * with no records the whole BITSTREAM buffer is the access unit.
 *
 * The result is split at the superframe index into object->vp9_unit, because the
 * D3D11 contract is one decoded picture per DecoderEndFrame: the caller sends
 * each member and keeps the last picture.
 *
 * VP9FIX (2026-10-25): returns an HRESULT instead of a BOOL so the caller can
 * fail the frame with the right code (E_INVALIDARG for a client buffer or a
 * split that does not fit, E_OUTOFMEMORY for an allocation, E_FAIL otherwise).
 * Every length is checked against the buffer it came from before it becomes a
 * pointer/length:
 *   - the record list is a whole number of 10-byte records and no larger than
 *     the 16 KiB SLICE_CONTROL buffer the decoder advertises,
 *   - each record's (location, size) lies inside BITSTREAM; a record outside is
 *     refused, not dropped (dropping bytes sends a truncated frame),
 *   - the concatenation never exceeds the BITSTREAM buffer, which also catches
 *     overlapping records that the old 32-bit sum would have wrapped,
 *   - the superframe split is re-validated by vp9_access_unit_valid() before the
 *     caller dereferences offset[]/size[]. */
static HRESULT decoder_vp9_build_access_unit(VIRTIO_WDDM_VideoDecoder *object,
                                             const char **error) {
    const UINT8 *records = object->slices;
    UINT record_count;
    UINT64 total = 0;
    UINT i;

    if (error)
        *error = NULL;
    object->vp9_au_size = 0;
    memset(&object->vp9_unit, 0, sizeof(object->vp9_unit));
    if (object->bitstream_size == 0) {
        if (error)
            *error = "no BITSTREAM data";
        return E_INVALIDARG;
    }
    if (object->bitstream_size > VIRTIO_WDDM_MAX_ACCESS_UNIT_BYTES) {
        if (error)
            *error = "BITSTREAM is larger than the access-unit cap";
        return E_INVALIDARG;
    }
    if (object->slices_size > VIRTIO_WDDM_VP9_SLICE_CONTROL_BYTES ||
        (object->slices_size % (UINT)sizeof(DXVA_Slice_VPx_Short)) != 0) {
        if (error)
            *error = "SLICE_CONTROL is not a whole number of VPx slice records";
        return E_INVALIDARG;
    }
    record_count = object->slices_size / (UINT)sizeof(DXVA_Slice_VPx_Short);
    if (record_count > VIRTIO_WDDM_VP9_MAX_SLICE_RECORDS) {
        if (error)
            *error = "too many SLICE_CONTROL records";
        return E_INVALIDARG;
    }

    for (i = 0; i < record_count; i++) {
        DXVA_Slice_VPx_Short slice;
        memcpy(&slice, records + i * sizeof(slice), sizeof(slice));
        if (slice.SliceBytesInBuffer == 0)
            continue;
        if ((UINT64)slice.BSNALunitDataLocation + (UINT64)slice.SliceBytesInBuffer >
            (UINT64)object->bitstream_size) {
            ERROR("%s: slice record %u (offset %u + %u B) is outside the %u B "
                  "BITSTREAM buffer -> E_INVALIDARG", __FUNCTION__, i,
                  slice.BSNALunitDataLocation, slice.SliceBytesInBuffer,
                  object->bitstream_size);
            if (error)
                *error = "a SLICE_CONTROL record points outside BITSTREAM";
            return E_INVALIDARG;
        }
        total += (UINT64)slice.SliceBytesInBuffer;
        if (total > (UINT64)object->bitstream_size) {
            ERROR("%s: the %u slice record(s) describe %llu B out of a %u B "
                  "BITSTREAM buffer (overlapping records?) -> E_INVALIDARG",
                  __FUNCTION__, record_count, (unsigned long long)total,
                  object->bitstream_size);
            if (error)
                *error = "SLICE_CONTROL records overlap BITSTREAM";
            return E_INVALIDARG;
        }
    }

    if (total == 0) {
        if (record_count != 0)
            ERROR("%s: %u slice record(s) but none usable -> using the whole %u B "
                  "BITSTREAM buffer", __FUNCTION__, record_count, object->bitstream_size);
        if (!decoder_reserve(&object->vp9_au, &object->vp9_au_cap, object->bitstream_size)) {
            if (error)
                *error = "out of memory for the access unit";
            return E_OUTOFMEMORY;
        }
        memcpy(object->vp9_au, object->bitstream, object->bitstream_size);
        object->vp9_au_size = object->bitstream_size;
    } else {
        UINT at = 0;
        if (total > (UINT64)VP9_MAX_FRAME_BYTES) {
            if (error)
                *error = "the assembled access unit is larger than the frame cap";
            return E_INVALIDARG;
        }
        if (!decoder_reserve(&object->vp9_au, &object->vp9_au_cap, (UINT)total)) {
            if (error)
                *error = "out of memory for the access unit";
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < record_count; i++) {
            DXVA_Slice_VPx_Short slice;
            memcpy(&slice, records + i * sizeof(slice), sizeof(slice));
            if (slice.SliceBytesInBuffer == 0)
                continue;
            /* Already validated above; the copy is bounded by `total`, which the
             * reserve covered.  No further check can fail here. */
            memcpy(object->vp9_au + at, object->bitstream + slice.BSNALunitDataLocation,
                   slice.SliceBytesInBuffer);
            at += slice.SliceBytesInBuffer;
        }
        if ((UINT64)at != total) {
            /* Cannot happen (same records, same order); refuse rather than send
             * an access unit whose size does not match its bytes. */
            ERROR("%s: internal slice accounting mismatch (%u != %llu) -> E_FAIL",
                  __FUNCTION__, at, (unsigned long long)total);
            if (error)
                *error = "internal slice accounting mismatch";
            return E_FAIL;
        }
        object->vp9_au_size = at;
    }
    if (object->vp9_au_size == 0) {
        if (error)
            *error = "the assembled access unit is empty";
        return E_INVALIDARG;
    }

    vp9_parse_superframe(object->vp9_au, object->vp9_au_size, &object->vp9_unit);
    if (!vp9_access_unit_valid(&object->vp9_unit, object->vp9_au_size)) {
        ERROR("%s: superframe split does not fit the %u B access unit "
              "(count=%u superframe=%u) -> E_INVALIDARG", __FUNCTION__,
              object->vp9_au_size, object->vp9_unit.count, object->vp9_unit.superframe);
        if (error)
            *error = "superframe split does not fit the access unit";
        memset(&object->vp9_unit, 0, sizeof(object->vp9_unit));
        return E_INVALIDARG;
    }
    return S_OK;
}

/* DecoderEndFrame: send the assembled access unit to the host decoder, wait for
 * the decoded NV12 frame and write it into the output texture.
 *
 * Failure policy: this entry cannot return an HRESULT, so every hard failure
 * logs a line with the reason and calls the runtime's error callback with
 * E_FAIL. A vm_get_frame() timeout is a failure too: the application asked for
 * a frame and none was produced, so pretending otherwise would leave it with
 * stale pixels. Nothing here blocks longer than the 1000 ms vm_get_frame
 * timeout, and every path clears frame_open so the next BeginFrame can run. */
static VOID APIENTRY virtio_wddm_video_video_decoder_end_frame(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    VIRTIO_WDDM_VideoDecoder *object = hDecode.pDrvPrivate;
    VIRTIO_WDDM_VideoDecoderOutputView *view;
    VIRTIO_WDDM_Resource *resource;
    VM_FRAME_INFO info;
    const UINT8 *send_data;
    UINT send_size;
    UINT elapsed;
    UINT width;
    UINT height;
    UINT stride;
    UINT row_bytes;
    UINT upload_stride;
    UINT upload_fourcc;
    const UINT8 *upload_src;
    /* P-BATCH ZEROCOPY: where this frame's pixels are -- the mmap'd pool pages
     * when the direct path succeeded, the staged `nv12` buffer otherwise. */
    const UINT8 *frame_src;
    BOOL host_p010;
    BOOL dst_p010;
    DXGI_FORMAT dst_format;
    UINT x;
    UINT y;
    UINT mips;
    UINT subresource;
    UINT64 y_sum = 0;
    UINT64 y_nonzero = 0;
    UINT64 y_samples = 0;
    double y_mean;
    HRESULT hr;
    /* P3-5: per-frame budget (see decoder_qpc_now) and the luma-stat gate. */
    LONGLONG t_send0 = 0;
    LONGLONG t_up0 = 0;
    LONGLONG t_up1 = 0;
    double send_ms = 0.0;
    double get_ms = 0.0;
    double up_ms = 0.0;
    UINT64 frame_no = 0;
    BOOL want_stats = FALSE;
    /* P3-3: new-sequence detection (IDR heuristic + frame_num continuity). */
    BOOL idr;
    BOOL fn_gap;
    UINT fn_cur;
    UINT fn_max;
    UINT fn_delta;
    UINT fn_prev;
    char flush_reason[160];
    /* PIPELINE (2026-10-25): with the pipeline armed this EndFrame uploads the
     * picture of the PREVIOUS job (see the pipeline block), so both the pixels
     * and the upload target come from that job instead of from this BeginFrame.
     * `pipeline_used` selects between the two in the upload tail below. */
    BOOL pipeline_used = FALSE;
    BOOL pipeline_fill = FALSE;
    VIRTIO_WDDM_PipelineFrame pipe_frame;
    UINT64 frame_readable = 0;
    /* The upload target: the texture / subresource this frame's picture belongs
     * in.  Serial path: this BeginFrame's output view.  Pipeline path: the
     * drained job's snapshot of its own BeginFrame. */
    ID3D11Resource *target_texture;
    UINT target_w;
    UINT target_h;
    UINT target_array;
    UINT target_mips;
    UINT view_mip;
    UINT view_slice;

    VIRTIO_WDDM_DDI_ENTRY("hDecode=%p priv=%p frame_open=%d bitstream=%u submits=%llu",
                          (void *)hDecode.pDrvPrivate, (void *)object,
                          object ? (int)object->frame_open : -1,
                          object ? object->bitstream_size : 0u,
                          object ? (unsigned long long)object->submits : 0ull);

    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECODE || !device) {
        ERROR("%s: invalid decoder %p -> nothing to end", __FUNCTION__, (void *)object);
        return;
    }
    if (!object->frame_open) {
        ERROR("%s: EndFrame without BeginFrame -> ignored", __FUNCTION__);
        return;
    }
    object->frame_open = FALSE;

    view = object->output_view.pDrvPrivate;
    if (!view || view->magic != VIRTIO_WDDM_VIDEO_MAGIC_DECOUT ||
        !view->resource.pDrvPrivate) {
        ERROR("%s: output view %p vanished before EndFrame -> E_FAIL", __FUNCTION__,
              (void *)view);
        decoder_fail_frame(object, E_FAIL);
        return;
    }
    resource = view->resource.pDrvPrivate;
    if (!resource->base.pResource) {
        ERROR("%s: output resource %p has no D3D11 texture -> E_FAIL", __FUNCTION__,
              (void *)resource);
        decoder_fail_frame(object, E_FAIL);
        return;
    }
    if (object->bitstream_size == 0) {
        ERROR("%s: frame ended with no BITSTREAM data (submits=%llu) -> E_FAIL",
              __FUNCTION__, (unsigned long long)object->submits);
        decoder_fail_frame(object, E_FAIL);
        return;
    }

    width = object->desc.SampleWidth;
    height = object->desc.SampleHeight;
    if (width == 0 || height == 0) {
        ERROR("%s: decoder has no dimensions -> E_FAIL", __FUNCTION__);
        decoder_fail_frame(object, E_FAIL);
        return;
    }

    /* The two codecs diverge here.  VP9 needs no parameter-set work (its
     * headers are in band): the access unit is rebuilt from the control buffers
     * and sent picture by picture.  H.264 goes through the else branch below,
     * which may prepend SPS/PPS synthesised from the client's parsed values.
     * Both end up in the same decoder_decode_unit() round trip. */
    if (object->codec == VIRTIO_WDDM_CODEC_VP9) {
        VIRTIO_WDDM_DecodeTiming timing;
        UINT frame_index;
        const char *vp9_error = NULL;
        HRESULT vp9_hr;

        /* VP9FIX (2026-10-25): the client's DXVA_PicParams_VP9 and the access
         * unit are validated before anything is sent.  A violation refuses the
         * frame with E_INVALIDARG (decoder_fail_frame records it and the next
         * DecoderBeginFrame returns it) instead of letting a bogus length,
         * profile or tile count reach vm_send()/the upload path. */
        if (!decoder_vp9_pic_params_valid(object, &vp9_error)) {
            object->vp9_refusals++;
            ERROR("%s: VP9 PICTURE_PARAMETERS refused (%s): profile=%u %ux%u "
                  "bitdepth=%u/%u tiles=log2 %u/%u -> E_INVALIDARG", __FUNCTION__,
                  vp9_error ? vp9_error : "unknown",
                  (UINT)object->vp9_pic.profile, object->vp9_pic.width,
                  object->vp9_pic.height,
                  (UINT)object->vp9_pic.BitDepthMinus8Luma,
                  (UINT)object->vp9_pic.BitDepthMinus8Chroma,
                  (UINT)object->vp9_pic.log2_tile_cols,
                  (UINT)object->vp9_pic.log2_tile_rows);
            decoder_fail_frame(object, E_INVALIDARG);
            return;
        }
        vp9_hr = decoder_vp9_build_access_unit(object, &vp9_error);
        if (FAILED(vp9_hr)) {
            object->vp9_refusals++;
            ERROR("%s: VP9 access unit refused (%s): BITSTREAM %u B, "
                  "SLICE_CONTROL %u B, %llu submit(s) -> 0x%08lx", __FUNCTION__,
                  vp9_error ? vp9_error : "unknown", object->bitstream_size,
                  object->slices_size, (unsigned long long)object->submits,
                  (unsigned long)vp9_hr);
            decoder_fail_frame(object, vp9_hr);
            return;
        }
        if (object->vp9_unit.superframe) {
            object->vp9_superframes++;
            if (object->vp9_superframes <= 4u) {
                INFO("%s: VP9 superframe: %u frame(s) in %u B (BITSTREAM %u B, "
                     "SLICE_CONTROL %u B -> %u record(s)); sending them one by one "
                     "and keeping the last picture", __FUNCTION__,
                     object->vp9_unit.count, object->vp9_au_size, object->bitstream_size,
                     object->slices_size,
                     object->slices_size / (UINT)sizeof(DXVA_Slice_VPx_Short));
            }
        }
        if (object->vp9_unit.count == 1u && object->slices_size == 0 &&
            object->submits <= 2u) {
            INFO("%s: no SLICE_CONTROL on this VP9 frame: the whole %u B BITSTREAM "
                 "buffer is the access unit", __FUNCTION__, object->bitstream_size);
        }
        memset(&timing, 0, sizeof(timing));
        for (frame_index = 0; frame_index < object->vp9_unit.count; frame_index++) {
            const UINT8 *frame;
            UINT frame_offset;
            UINT frame_size;
            Vp9FrameHeader hdr;
            BOOL intra;

            /* VP9FIX (2026-10-25): the array index and the (offset, size) pair
             * are checked again here, immediately before they become a pointer
             * and a length.  vp9_access_unit_valid() already validated the whole
             * split, so this can only fire if the unit was changed in between;
             * refusing the frame is still the only safe answer. */
            if (frame_index >= VP9_MAX_SUPERFRAME_FRAMES) {
                object->vp9_refusals++;
                ERROR("%s: VP9 frame index %u is outside the %u-entry superframe "
                      "array -> E_INVALIDARG", __FUNCTION__, frame_index,
                      (UINT)VP9_MAX_SUPERFRAME_FRAMES);
                decoder_fail_frame(object, E_INVALIDARG);
                return;
            }
            frame_offset = object->vp9_unit.offset[frame_index];
            frame_size = object->vp9_unit.size[frame_index];
            if (frame_offset > object->vp9_au_size || frame_size == 0 ||
                frame_size > object->vp9_au_size - frame_offset ||
                frame_size > VP9_MAX_FRAME_BYTES) {
                object->vp9_refusals++;
                ERROR("%s: VP9 frame %u/%u (offset %u + %u B) does not fit the %u B "
                      "access unit -> E_INVALIDARG", __FUNCTION__, frame_index,
                      object->vp9_unit.count, frame_offset, frame_size,
                      object->vp9_au_size);
                decoder_fail_frame(object, E_INVALIDARG);
                return;
            }
            frame = object->vp9_au + frame_offset;

            if (!vp9_parse_uncompressed_header(frame, frame_size, &hdr)) {
                object->vp9_bad_headers++;
                if (object->vp9_header_logs < 8u) {
                    object->vp9_header_logs++;
                    ERROR("%s: VP9 frame %u of %u (%u B) did not parse: %s -- sending "
                          "it anyway (the host is the real parser)", __FUNCTION__,
                          frame_index, object->vp9_unit.count, frame_size,
                          hdr.error ? hdr.error : "unknown reason");
                }
            } else {
                intra = vp9_frame_is_intra(&hdr) ? TRUE : FALSE;
                if (intra)
                    object->vp9_keyframes++;
                if (hdr.geometry_valid) {
                    if (hdr.frame_width != width || hdr.frame_height != height) {
                        /* The descriptor is what the NV12 surface and S_FMT were
                         * built from, so it wins; the mismatch is worth knowing. */
                        INFO("%s: VP9 key frame is %ux%u but the decoder descriptor "
                             "is %ux%u (the descriptor wins)", __FUNCTION__,
                             hdr.frame_width, hdr.frame_height, width, height);
                    }
                    if (hdr.frame_width > width || hdr.frame_height > height) {
                        ERROR("%s: VP9 key frame %ux%u is larger than the %ux%u "
                              "surface -> GetFrame will not fit; E_FAIL", __FUNCTION__,
                              hdr.frame_width, hdr.frame_height, width, height);
                        decoder_fail_frame(object, E_FAIL);
                        return;
                    }
                }
                if (object->vp9_header_logs < 3u) {
                    object->vp9_header_logs++;
                    INFO("%s: VP9 frame %u/%u: %s%s profile=%u %ux%u color=%u/%u "
                         "%u B, header %u B", __FUNCTION__, frame_index + 1u,
                         object->vp9_unit.count,
                         hdr.show_existing_frame ? "show_existing" :
                         (hdr.frame_type == VP9_FRAME_TYPE_KEY ? "KEY" :
                          (hdr.intra_only ? "intra-only" : "inter")),
                         intra ? " (intra)" : "", hdr.profile, hdr.frame_width,
                         hdr.frame_height, hdr.color_space, hdr.color_range,
                         frame_size, hdr.header_size_bytes);
                } else {
                    VERBOSE("%s: VP9 frame %u/%u: type=%u intra=%d %u B", __FUNCTION__,
                            frame_index + 1u, object->vp9_unit.count, hdr.frame_type,
                            (int)intra, frame_size);
                }
                /* Same new-sequence rule as the H.264 IDR path: a key frame
                 * after a visible gap, or with a different geometry, means the
                 * old host session still holds references from another position,
                 * so flush it before this picture goes in. */
                if (intra && object->vp9_frames > 0) {
                    LARGE_INTEGER now;
                    LONGLONG gap_ms = 0;
                    BOOL geometry_changed;
                    QueryPerformanceCounter(&now);
                    if (object->last_send_qpc != 0) {
                        LONGLONG freq = decoder_qpc_freq();
                        if (freq > 0)
                            gap_ms = (now.QuadPart - object->last_send_qpc) * 1000 / freq;
                    }
                    geometry_changed = object->last_width != 0 &&
                                       (object->last_width != width ||
                                        object->last_height != height);
                    if (gap_ms >= 120 || geometry_changed) {
                        snprintf(flush_reason, sizeof(flush_reason),
                                 "VP9 key frame, gap=%lld ms, geometry change=%d",
                                 (long long)gap_ms, (int)geometry_changed);
                        decoder_flush_session(object, flush_reason);
                    }
                }
            }

            if (!decoder_decode_unit(object, frame, frame_size, width, height, &timing,
                                     &info))
                return;
            object->vp9_frames++;
            send_data = frame;
            send_size = frame_size;
            elapsed = timing.elapsed_ms;
            send_ms = timing.send_ms;
            get_ms = timing.get_ms;
            if (object->vp9_frames == 1u) {
                INFO("%s: first VP9 picture decoded: %ux%u stride=%u %u B in %lu ms "
                     "(send=%.2f get=%.2f ms)", __FUNCTION__, info.Width, info.Height,
                     info.Stride, info.Size, (unsigned long)elapsed, send_ms, get_ms);
            }
            /* VP9FIX (2026-10-25): a superframe member that is not the final
             * picture is decoded (the host needs it for its DPB) but never
             * uploaded.  Its capture buffer has to go back now: the driver keeps
             * one mapped frame per session, so leaving it mapped makes the next
             * member's MAP_FRAME fail and leaks the buffer for the rest of the
             * session (the old code overwrote `direct_map` instead). */
            if (frame_index + 1u < object->vp9_unit.count) {
                if (object->direct_active) {
                    decoder_drop_direct(object);
                    object->direct_member_releases++;
                }
            }
        }
        t_send0 = timing.start_qpc;
    } else if (object->codec == VIRTIO_WDDM_CODEC_HEVC) {
        /* P-BATCH HEVC.  The access unit is exactly what the client put in
         * BITSTREAM (slice segments, plus AUD/SEI when it sends them).  The
         * parameter sets come from one of two places:
         *
         *   1. the access unit itself -- the ffmpeg / PotPlayer style client
         *      carries VPS+SPS+PPS in band.  Those bytes are exact, so they are
         *      forwarded as they are and cached for the frames that do not
         *      repeat them.
         *   2. DXVA_PicParams_HEVC -- the Chromium style client sends slices
         *      only and keeps the parsed SPS/PPS in the PICTURE_PARAMETERS
         *      buffer; hevc_params.h rebuilds VPS+SPS+PPS from it.
         *
         * (1) wins whenever it is complete, because it cannot be wrong.  (2) is
         * bit-exact too for the streams it accepts (unit test: ffmpeg decodes the
         * re-muxed streams to the identical samples), but it refuses the cases
         * DXVA cannot describe -- reference picture sets kept in the SPS,
         * long-term reference pictures -- and then (1)'s cache is the fallback.
         *
         * A sequence start (IRAP after a gap, or new parameter sets) flushes the
         * host session first, exactly like the H.264 IDR rule. */
        HevcCollectedParams collected;
        const UINT8 *prepend = NULL;
        UINT prepend_size = 0;
        const char *params_source = "no";
        BOOL in_band;
        BOOL irap;
        BOOL params_changed = FALSE;
        UINT before_params_size = object->hevc_params_size;
        UINT8 before_head[16];
        UINT before_head_len = before_params_size < sizeof(before_head)
                                   ? before_params_size
                                   : (UINT)sizeof(before_head);

        if (before_head_len)
            memcpy(before_head, object->hevc_params, before_head_len);
        decoder_hevc_update_parameter_sets(object, object->bitstream,
                                           object->bitstream_size, &collected);
        in_band = (collected.vps_present && collected.sps_present &&
                   collected.pps_present)
                      ? TRUE
                      : FALSE;

        /* Sequence start: the first VCL NAL is BLA/IDR/CRA.  PicParams carries
         * the same flags and is used when the access unit has no slice at all. */
        irap = FALSE;
        {
            unsigned nal_type =
                hevc_first_slice_nal_type(object->bitstream, object->bitstream_size);
            if (nal_type != 0xffffffffu)
                irap = hevc_nal_is_irap(nal_type) ? TRUE : FALSE;
        }
        if (!irap && object->hevc_pic_this_frame && object->hevc_pic.IrapPicFlag)
            irap = TRUE;
        if (irap)
            object->hevc_irap++;

        if (in_band && before_params_size != 0 &&
            (before_params_size != object->hevc_params_size ||
             (before_head_len &&
              memcmp(before_head, object->hevc_params, before_head_len) != 0)))
            params_changed = TRUE;

        if (object->hevc_frames > 0 && irap) {
            LARGE_INTEGER now;
            LONGLONG gap_ms = 0;
            BOOL geometry_changed;

            QueryPerformanceCounter(&now);
            if (object->last_send_qpc != 0) {
                LONGLONG freq = decoder_qpc_freq();
                if (freq > 0)
                    gap_ms = (now.QuadPart - object->last_send_qpc) * 1000 / freq;
            }
            geometry_changed = object->last_width != 0 &&
                               (object->last_width != width || object->last_height != height);
            if (gap_ms >= 120 || params_changed || geometry_changed) {
                snprintf(flush_reason, sizeof(flush_reason),
                         "HEVC IRAP, gap=%lld ms, new VPS/SPS/PPS=%d, geometry "
                         "change=%d", (long long)gap_ms, (int)params_changed,
                         (int)geometry_changed);
                decoder_flush_session(object, flush_reason);
            }
        }

        if (in_band) {
            prepend = object->hevc_params;
            prepend_size = object->hevc_params_size;
            params_source = "in-AU";
            object->hevc_inband_injections++;
        } else {
            UINT synth_size = decoder_hevc_synth_parameter_sets(object, object->bitstream,
                                                                object->bitstream_size);
            if (synth_size) {
                prepend = object->synth;
                prepend_size = synth_size;
                params_source = "synth";
            } else if (object->hevc_params_size) {
                prepend = object->hevc_params;
                prepend_size = object->hevc_params_size;
                params_source = "cached";
            }
        }

        if (prepend && prepend_size &&
            decoder_reserve(&object->hevc_au, &object->hevc_au_cap,
                            prepend_size + object->bitstream_size)) {
            memcpy(object->hevc_au, prepend, prepend_size);
            memcpy(object->hevc_au + prepend_size, object->bitstream,
                   object->bitstream_size);
            object->hevc_au_size = prepend_size + object->bitstream_size;
            send_data = object->hevc_au;
            send_size = object->hevc_au_size;
        } else {
            /* No parameter sets anywhere: send the slices as they are.  The host
             * will not decode them (it has no SPS), which shows up as the
             * GetFrame timeout below; the log line says why. */
            send_data = object->bitstream;
            send_size = object->bitstream_size;
            if (object->hevc_frames == 0)
                ERROR("%s: HEVC frame without parameter sets (PICTURE_PARAMETERS=%d, "
                      "in-AU=%d, cache=%u B) -- the host decoder cannot decode this",
                      __FUNCTION__, (int)object->hevc_pic_valid, (int)in_band,
                      object->hevc_params_size);
        }
        if (object->hevc_frames == 0) {
            INFO("%s: first HEVC access unit %u B -> %u B sent (VPS/SPS/PPS source=%s, "
                 "%u B prefix, Irap=%d, pps=%u)", __FUNCTION__,
                 object->bitstream_size, send_size, params_source, prepend_size,
                 (int)irap, (UINT)object->hevc_fields.pps_id);
        } else {
            VERBOSE("%s: HEVC access unit %u B -> %u B sent (VPS/SPS/PPS source=%s, "
                    "%u B prefix, Irap=%d)", __FUNCTION__, object->bitstream_size,
                    send_size, params_source, prepend_size, (int)irap);
        }
        object->hevc_pic_this_frame = FALSE;
    } else if (object->codec == VIRTIO_WDDM_CODEC_AV1) {
        /* P-BATCH AV1.  The client gives us the parsed headers
         * (DXVA_PicParams_AV1), the tile records (DXVA_Tile_AV1) and the raw
         * tile payloads; the host wants a temporal unit.  This branch rebuilds
         * it and, because refresh_frame_flags is only knowable one frame late,
         * splices in the hidden DPB catch-up frame for the PREVIOUS picture
         * first.  The temporal order inside the buffer is therefore
         *
         *   TD  [seq]  [hidden prev frame + its tile group]  frame  tile group
         *
         * which is one temporal unit: at most one shown frame (5.6), so
         * vm_get_frame() returns exactly this picture's surface.
         *
         * AV1FIX (2026-10-25): AV1 against the HEVC path that decodes 602 frames
         * (E173 ring-hevc3.txt).  "AV1 stops after the first frame" is a
         * difference list, and this is it:
         *
         *  1. AV1's AU is SYNTHESISED (sequence header / frame header / tile
         *     group OBUs from av1_params.h); HEVC's AU is the client's Annex-B
         *     bytes with VPS+SPS+PPS prepended, so HEVC cannot be malformed by
         *     this UMD.  A key frame decodes today (E173: frame 1, Ymean=107.68),
         *     which proves the sequence header, the intra frame header and the
         *     OBU/tile-group wrapper are right; what has never decoded is the
         *     INTER frame header, i.e. the inter-only fields (frame_refs,
         *     frame_size_with_refs, interpolation filter, motion-mode/MV fields,
         *     skip_mode, global motion) or the DPB bookkeeping below.
         *  2. AV1 sends refresh_frame_flags = 0 for an inter picture (DXVA has no
         *     such field) and re-sends the picture HIDDEN one frame later; HEVC
         *     carries its reference picture set in the AU itself.
         *  3. AV1 forces show_frame = 1 on every picture.  t6's real stream is
         *     the exact opposite: every coded picture in it is HIDDEN
         *     (show_frame = 0, showable_frame = 1) and the visible frames are
         *     separate show_existing_frame temporal units.  Verified offline with
         *     `ffmpeg -bsf:v trace_headers` on /srv/tv2/t6.av1: TU1 = shown key,
         *     TU2 = two hidden inter pictures, TU3 = show_existing_frame(idx 6),
         *     TU4 = one shown inter picture, TU5 = show_existing_frame(idx 5).
         *     DXVA never delivers a show_existing_frame picture -- the client
         *     displays that reference itself, without a DecodeFrame call -- so
         *     nothing is "missing" on our side, but the UMD's unit does not have
         *     the hidden-then-shown structure the encoder produced.  If the host
         *     only emits a CAPTURE buffer for a picture its own DPB considers
         *     displayed, that is where it goes silent; 09-AV1修复.md lists the
         *     experiment that settles it.
         *  4. AV1 configures the host from the 64-row aligned SURFACE
         *     (1920x1152) while the bitstream's frames are 1920x1080, so the host
         *     reconfigures the capture queue (SOURCE_CHANGE -> G_FMT 1920x1080)
         *     after the first frame.  HEVC has the same 1152-vs-1088 mismatch and
         *     decodes 602 frames, but AV1 is the codec where the UMD knows the
         *     real coded size.  Fixed in decoder_decode_unit(), which now sends
         *     the coded size for AV1 only.
         *  5. Both codecs get 18 capture buffers and every frame is released, so
         *     the queue depth cannot explain the difference (the AV1 ring shows
         *     `RELEASE_FRAME ok` after frame 1 and then only timeouts).
         *
         * The tile records are validated before they become a tile group: a
         * record outside BITSTREAM used to be skipped by av1_tile_group_obu(),
         * which produced a tile group with fewer tiles than the frame header
         * announces -- the host then reads tile-size fields out of tile payload
         * bytes.  Same rule as the VP9FIX slice records: refuse the frame instead
         * of sending a truncated one. */
        Av1Tile *av1_tiles = (Av1Tile *)object->av1_tiles;
        UINT av1_tiles_n;
        UINT au_at = 0;
        UINT au_n;
        UINT au_need;
        UINT fh_size = 0;      /* AV1FIX: frame-header OBU size, for the log    */
        UINT tg_size = 0;      /* ... tile-group OBU size                       */
        UINT seq_size = 0;     /* ... sequence-header OBU size (0 = not emitted) */
        UINT catchup_bytes = 0;/* ... hidden DPB catch-up frame in this unit    */
        BOOL seq_this_unit = FALSE;
        BOOL av1_is_key;
        BOOL av1_seq_changed = FALSE;
        unsigned av1_refresh;
        UINT i;

        if (!object->av1_pic_valid || !object->av1_pic_this_frame) {
            ERROR("%s: AV1 frame without PICTURE_PARAMETERS (DXVA_PicParams_AV1) "
                  "-> E_FAIL", __FUNCTION__);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        if (object->av1_tiles_size < (UINT)sizeof(DXVA_Tile_AV1)) {
            ERROR("%s: AV1 frame without SLICE_CONTROL (DXVA_Tile_AV1 records, "
                  "%u B) -> E_FAIL", __FUNCTION__, object->av1_tiles_size);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        av1_tiles_n = object->av1_tiles_size / (UINT)sizeof(DXVA_Tile_AV1);
        /* Narrow the records to Av1Tile in place: 12 B < 16 B and the copy
         * walks forward, so the source record is never overwritten. */
        for (i = 0; i < av1_tiles_n; i++) {
            DXVA_Tile_AV1 rec;
            Av1Tile tile;
            memcpy(&rec, (UINT8 *)object->av1_tiles + i * sizeof(DXVA_Tile_AV1),
                   sizeof(rec));
            tile.offset = rec.DataOffset;
            tile.size = rec.DataSize;
            tile.row = rec.row;
            tile.column = rec.column;
            memcpy((UINT8 *)object->av1_tiles + i * sizeof(Av1Tile), &tile,
                   sizeof(tile));
        }
        /* AV1FIX: the tile grid the frame header announces vs the records the
         * client submitted.  One record per tile, offsets inside BITSTREAM. */
        {
            UINT cols = object->av1_pic.tiles.cols ? object->av1_pic.tiles.cols : 1u;
            UINT rows = object->av1_pic.tiles.rows ? object->av1_pic.tiles.rows : 1u;
            UINT64 tiles_total = 0;
            const char *bad = NULL;

            if (cols > AV1_MAX_TILE_COLS || rows > AV1_MAX_TILE_ROWS ||
                (UINT64)cols * (UINT64)rows > AV1_MAX_TILES) {
                bad = "tile grid is outside the AV1 limits";
            } else if (av1_tiles_n != cols * rows) {
                bad = "record count does not match the tile grid";
            }
            for (i = 0; i < av1_tiles_n && !bad; i++) {
                const Av1Tile *t = av1_tiles + i;
                if (t->size == 0 || t->offset > object->bitstream_size ||
                    t->size > object->bitstream_size - t->offset) {
                    bad = "tile payload is outside BITSTREAM";
                } else if (t->row >= rows || t->column >= cols) {
                    bad = "tile row/column is outside the grid";
                } else {
                    tiles_total += t->size;
                }
            }
            if (!bad && (tiles_total == 0 || tiles_total > object->bitstream_size))
                bad = "tile payload sizes do not add up";
            if (bad) {
                object->av1_bad_tile_records++;
                ERROR("%s: AV1 tile records refused (%s): %u record(s), %ux%u grid, "
                      "BITSTREAM %u B, %llu B in records -> E_INVALIDARG",
                      __FUNCTION__, bad, av1_tiles_n, cols, rows,
                      object->bitstream_size, (unsigned long long)tiles_total);
                decoder_fail_frame(object, E_INVALIDARG);
                return;
            }
        }

        if (!av1_seq_from_pic(&object->av1_seq, &object->av1_pic)) {
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        av1_is_key = (object->av1_pic.format.frame_type == AV1_FRAME_KEY);
        if (!object->av1_seq_emitted ||
            !av1_seq_same(&object->av1_seq, &object->av1_seq_last))
            av1_seq_changed = TRUE;
        if (!av1_frame_from_pic(&object->av1_frame, &object->av1_pic, av1_tiles,
                                av1_tiles_n, width, height)) {
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        if (object->av1_pic.format.show_frame == 0) {
            VERBOSE("%s: picture was coded hidden (show_frame=0); it is emitted "
                    "shown so the host produces the surface this EndFrame waits "
                    "for", __FUNCTION__);
        }

        au_need = 64u                                 /* temporal delimiter */
                  + 256u                              /* sequence header     */
                  + object->av1_prev_tile_group_size  /* catch-up tile group */
                  + 8u                                /* catch-up OBU header */
                  + 4096u + 8u                        /* frame header + OBU  */
                  + object->bitstream_size + 64u;     /* tile group + OBU    */
        if (!decoder_reserve(&object->av1_au, &object->av1_au_cap, au_need)) {
            decoder_fail_frame(object, E_FAIL);
            return;
        }

        /* 1. The previous picture's hidden catch-up frame, with the refresh
         *    mask this picture's RefFrameMapTextureIndex just revealed. */
        if (object->av1_prev_pending) {
            unsigned prev_refresh = 0;

            object->av1_prev_pending = FALSE;
            if (av1_is_key) {
                /* A new sequence starts here: the key frame resets the DPB, so
                 * the previous picture's mask can no longer matter. */
                av1_dpb_reset(&object->av1_dpb);
            } else {
                prev_refresh = av1_derive_refresh_flags(
                    object->av1_prev_texture,
                    object->av1_pic.RefFrameMapTextureIndex);
                object->av1_masks_derived++;
                if (prev_refresh != 0) {
                    Av1FrameParams hidden = object->av1_prev_frame;
                    hidden.show_frame = 0;
                    au_n = av1_frame_header_obu(
                        &object->av1_prev_seq, &hidden, &object->av1_dpb, NULL, 0,
                        prev_refresh, object->av1_au + au_at,
                        object->av1_au_cap - au_at);
                    if (au_n == 0 ||
                        object->av1_prev_tile_group_size >
                            object->av1_au_cap - au_at - au_n) {
                        ERROR("%s: rebuilding the hidden catch-up frame for the "
                              "previous picture failed (refresh=0x%02x) -> E_FAIL",
                              __FUNCTION__, prev_refresh);
                        decoder_fail_frame(object, E_FAIL);
                        return;
                    }
                    au_at += au_n;
                    memcpy(object->av1_au + au_at, object->av1_prev_tile_group,
                           object->av1_prev_tile_group_size);
                    catchup_bytes = au_n + object->av1_prev_tile_group_size;
                    au_at += object->av1_prev_tile_group_size;
                    object->av1_catchups++;
                    if (object->av1_catchups <= 3u) {
                        INFO("%s: hidden DPB catch-up for the previous picture: "
                             "refresh_frame_flags=0x%02x (derived from texture %u, "
                             "map %02x %02x %02x %02x %02x %02x %02x %02x), %u B",
                             __FUNCTION__, prev_refresh, object->av1_prev_texture,
                             object->av1_pic.RefFrameMapTextureIndex[0],
                             object->av1_pic.RefFrameMapTextureIndex[1],
                             object->av1_pic.RefFrameMapTextureIndex[2],
                             object->av1_pic.RefFrameMapTextureIndex[3],
                             object->av1_pic.RefFrameMapTextureIndex[4],
                             object->av1_pic.RefFrameMapTextureIndex[5],
                             object->av1_pic.RefFrameMapTextureIndex[6],
                             object->av1_pic.RefFrameMapTextureIndex[7],
                             object->av1_prev_tile_group_size);
                    }
                }
                /* The decoder has now applied the previous picture to its DPB
                 * (or not, with a mask of 0), which is the state this picture
                 * was encoded against. */
                av1_dpb_update(&object->av1_dpb, &object->av1_prev_frame,
                               prev_refresh);
            }
        }

        /* 2. Temporal delimiter, then the sequence header when it is new or
         *    this is a key frame (a flushed host session has no sequence). */
        au_n = av1_temporal_delimiter_obu(object->av1_au + au_at,
                                          object->av1_au_cap - au_at);
        if (au_n == 0) {
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        au_at += au_n;
        if (object->av1_seq_emitted && av1_is_key)
            av1_seq_changed = TRUE;
        if (!object->av1_seq_emitted || av1_seq_changed) {
            au_n = av1_sequence_header_obu(&object->av1_seq,
                                           object->av1_au + au_at,
                                           object->av1_au_cap - au_at);
            if (au_n == 0) {
                ERROR("%s: sequence header synthesis failed (level=%u %ux%u) -> "
                      "E_FAIL", __FUNCTION__, object->av1_seq.level_idx,
                      object->av1_seq.max_width, object->av1_seq.max_height);
                decoder_fail_frame(object, E_FAIL);
                return;
            }
            au_at += au_n;
            object->av1_seq_last = object->av1_seq;
            object->av1_seq_emitted = TRUE;
            seq_this_unit = TRUE;
            seq_size = au_n;
            if (object->av1_frames == 0 || av1_is_key) {
                INFO("%s: rebuilt sequence header (profile=%u level=%u %ux%u, "
                     "order_hint_bits=%u, 128x128_sb=%u, cdef=%u, restoration=%u, "
                     "film_grain=%u), %u B", __FUNCTION__, object->av1_seq.profile,
                     object->av1_seq.level_idx, object->av1_seq.max_width,
                     object->av1_seq.max_height, object->av1_seq.order_hint_bits,
                     object->av1_seq.use_128x128_superblock,
                     object->av1_seq.enable_cdef,
                     object->av1_seq.enable_restoration,
                     object->av1_seq.film_grain_params_present, au_n);
            }
        }

        /* 3. This picture's frame header.  A key frame refreshes all slots by
         *    definition; every other picture goes out with mask 0 and is
         *    re-sent hidden once the next picture reveals its real mask. */
        av1_refresh = av1_is_key ? 0xFFu : 0u;
        au_n = av1_frame_header_obu(&object->av1_seq, &object->av1_frame,
                                    &object->av1_dpb, NULL, 0, av1_refresh,
                                    object->av1_au + au_at,
                                    object->av1_au_cap - au_at);
        if (au_n == 0) {
            ERROR("%s: frame header synthesis failed (type=%u %ux%u tiles=%ux%u "
                  "q=%u) -> E_FAIL", __FUNCTION__,
                  object->av1_frame.frame_type, object->av1_frame.width,
                  object->av1_frame.height, object->av1_frame.tile_cols,
                  object->av1_frame.tile_rows, object->av1_frame.base_q_idx);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        au_at += au_n;
        fh_size = au_n;

        /* 4. The tile group, and a copy of it for the hidden re-send. */
        au_n = av1_tile_group_obu(&object->av1_seq, &object->av1_frame, av1_tiles,
                                  av1_tiles_n, object->bitstream,
                                  object->bitstream_size,
                                  object->av1_au + au_at,
                                  object->av1_au_cap - au_at);
        if (au_n == 0) {
            ERROR("%s: tile group assembly failed (%u record(s), %u B bitstream, "
                  "%ux%u tiles) -> E_FAIL", __FUNCTION__, av1_tiles_n,
                  object->bitstream_size, object->av1_frame.tile_cols,
                  object->av1_frame.tile_rows);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        if (!decoder_reserve(&object->av1_prev_tile_group,
                             &object->av1_prev_tile_group_cap, au_n))
            return;
        memcpy(object->av1_prev_tile_group, object->av1_au + au_at, au_n);
        object->av1_prev_tile_group_size = au_n;
        tg_size = au_n;
        au_at += au_n;

        object->av1_au_size = au_at;
        send_data = object->av1_au;
        send_size = object->av1_au_size;
        /* AV1FIX (2026-10-25): the first four units of a session are logged
         * field by field.  The old line only fired while av1_frames == 0 -- i.e.
         * for EVERY unit of a session that never decoded anything -- and it
         * printed the tile-group size twice (it reused `au_n`), which made the
         * E173 log read "795536 B frame header + 795536 B tile group".  The
         * values below are exactly the ones the host parses, so the next run can
         * be compared against `ffmpeg -bsf:v trace_headers` on the source file
         * without guessing which bit went wrong. */
        if (object->av1_pic_buffers <= 4u) {
            INFO("%s: AV1 unit #%llu: %u B = TD(2) + %sseq(%u) + %scatch-up(%u) + "
                 "frame header(%u) + tile group(%u) | %u tile(s) %ux%u mask=0x%02x "
                 "| pic: type=%u show=%u oh=%u/%u primary_ref=%u refs=%u.%u.%u.%u."
                 "%u.%u.%u",
                 __FUNCTION__, (unsigned long long)object->av1_pic_buffers, send_size,
                 seq_this_unit ? "" : "no ", seq_size,
                 catchup_bytes ? "yes " : "no ", catchup_bytes,
                 fh_size, tg_size, av1_tiles_n, object->av1_frame.width,
                 object->av1_frame.height, av1_refresh,
                 (UINT)object->av1_pic.format.frame_type,
                 (UINT)object->av1_pic.format.show_frame,
                 (UINT)object->av1_pic.order_hint,
                 (UINT)object->av1_pic.order_hint_bits,
                 (UINT)object->av1_pic.primary_ref_frame,
                 (UINT)(object->av1_pic.frame_refs[0].Index & 7u),
                 (UINT)(object->av1_pic.frame_refs[1].Index & 7u),
                 (UINT)(object->av1_pic.frame_refs[2].Index & 7u),
                 (UINT)(object->av1_pic.frame_refs[3].Index & 7u),
                 (UINT)(object->av1_pic.frame_refs[4].Index & 7u),
                 (UINT)(object->av1_pic.frame_refs[5].Index & 7u),
                 (UINT)(object->av1_pic.frame_refs[6].Index & 7u));
        }
    } else {

        /* Build the bytes that go to the host: the assembled access unit, with
         * SPS/PPS prepended when this unit does not carry a full pair. Two
         * sources, in order:
         *   1. P2-2i: parameter sets synthesised from this frame's
         *      PICTURE_PARAMETERS. This is what Chromium needs: its access unit is
         *      slices only, and the host V4L2 decoder cannot decode without
         *      in-band SPS/PPS.
         *   2. the last SPS/PPS pair seen in a stream (probe/ffmpeg-style clients
         *      that carry them in the first access unit). Kept for clients other
         *      than Chromium and as a fallback when synthesis refuses a stream.
         * Both are per-frame: a session that has never seen parameter sets could
         * not decode a P-frame.
         *
         * H.264 only: VP9 has no out-of-band parameter sets at all, so its path
         * above never reaches this block. */
        BOOL has_sps = FALSE;
        BOOL has_pps = FALSE;
        UINT prepend_size = 0;
        const UINT8 *prepend = NULL;
        const char *params_source = "no";
        UINT prev_params_size = object->params_size;
        UINT8 prev_params_head[16];
        UINT prev_head_len = prev_params_size < sizeof(prev_params_head)
                                 ? prev_params_size
                                 : (UINT)sizeof(prev_params_head);

        send_data = object->bitstream;
        send_size = object->bitstream_size;
        if (prev_head_len)
            memcpy(prev_params_head, object->params, prev_head_len);
        decoder_update_parameter_sets(object, object->bitstream,
                                      object->bitstream_size, &has_sps, &has_pps);

        /* P3-2/P3-3: does this access unit start a NEW sequence?  Seek / loop /
         * pause-resume hand us an IDR after a visible gap; resolution change
         * hands us a different SPS/PPS or geometry.  The old host session still
         * holds reference frames and queued AUs, so flush it -- the driver drops
         * them and opens a fresh session, and the AU below is then the first AU
         * that session ever sees (it carries SPS/PPS, from the stream or
         * prepended below), so decoding restarts cleanly instead of waiting on
         * references from the old position. */
        idr = decoder_au_has_idr(object->bitstream, object->bitstream_size);

        /* P3-3 (b): frame_num continuity, for a player that keeps the SAME
         * decoder across a seek -- the IDR rule above cannot catch that one
         * because the player never re-sends an IDR on this handle.
         *
         * The rule is H.264 7.4.3: an IDR sets frame_num = 0, a reference
         * picture advances it by one (mod MaxFrameNum) and a non-reference
         * picture repeats the previous reference picture's value.  Any other
         * delta means the slices come from a different position in the stream.
         * POC is deliberately not used (B frames / reordering make it
         * non-monotone in decode order).
         *
         * Two guards keep this from misfiring:
         *  - it only acts after frame_num has been seen to follow the rule at
         *    least once (frame_num_advances > 0), so a client that leaves
         *    frame_num at 0 (the p2-2i probe, some fillers) never looks like a
         *    seek;
         *  - only an IDR re-baselines without comparing (the one legal reset),
         *    and a delta that is neither the expected 0/1 nor a jump just
         *    re-baselines.  Conservative by design: a missed seek costs one
         *    stale GOP, a false seek costs a host reconfigure. */
        fn_gap = FALSE;
        fn_cur = 0;
        fn_max = 0;
        fn_delta = 0;
        fn_prev = 0;
        if (object->pic_this_frame) {
            fn_max = 1u << ((UINT)object->pic.log2_max_frame_num_minus4 + 4u);
            fn_cur = (UINT)object->pic.frame_num % fn_max;
            if (object->prev_frame_num_valid && !idr &&
                fn_max == object->prev_frame_num_max) {
                UINT expected = object->pic.RefPicFlag ? 1u : 0u;
                fn_prev = object->prev_frame_num;
                fn_delta = (fn_cur + fn_max - fn_prev) % fn_max;
                if (fn_delta == expected) {
                    object->frame_num_advances++;
                    if (!object->fn_live_logged) {
                        object->fn_live_logged = TRUE;
                        INFO("%s: frame_num tracking is live: %u -> %u (MaxFrameNum %u, "
                             "RefPicFlag=%u) -- seek detection can now fire", __FUNCTION__,
                             fn_prev, fn_cur, fn_max, (UINT)object->pic.RefPicFlag);
                    }
                } else if (fn_delta >= 2u && object->frame_num_advances > 0) {
                    fn_gap = TRUE;
                } else {
                    VERBOSE("%s: frame_num %u -> %u (delta %u, expected %u) neither "
                            "advances nor jumps -> re-baselining, no flush",
                            __FUNCTION__, fn_prev, fn_cur, fn_delta, expected);
                }
            }
            object->prev_frame_num = fn_cur;
            object->prev_frame_num_max = fn_max;
            object->prev_frame_num_valid = TRUE;
        } else {
            /* No PICTURE_PARAMETERS this frame: the next delta cannot be judged
             * against a picture that described an older frame. */
            object->prev_frame_num_valid = FALSE;
        }
        object->pic_this_frame = FALSE;

        if (object->frames > 0) {
            LARGE_INTEGER now;
            LONGLONG gap_ms = 0;
            BOOL params_changed = FALSE;
            BOOL geometry_changed;

            QueryPerformanceCounter(&now);
            if (object->last_send_qpc != 0) {
                LONGLONG freq = decoder_qpc_freq();
                if (freq > 0)
                    gap_ms = (now.QuadPart - object->last_send_qpc) * 1000 / freq;
            }
            if (has_sps && has_pps && prev_params_size != 0 &&
                (prev_params_size != object->params_size ||
                 (prev_head_len &&
                  memcmp(prev_params_head, object->params, prev_head_len) != 0)))
                params_changed = TRUE;
            geometry_changed = object->last_width != 0 &&
                               (object->last_width != width || object->last_height != height);
            if (idr && (gap_ms >= 120 || params_changed || geometry_changed)) {
                snprintf(flush_reason, sizeof(flush_reason),
                         "IDR, gap=%lld ms, new SPS/PPS=%d, geometry change=%d",
                         (long long)gap_ms, (int)params_changed, (int)geometry_changed);
                decoder_flush_session(object, flush_reason);
            }
        }
        if (fn_gap) {
            snprintf(flush_reason, sizeof(flush_reason),
                     "frame_num %u -> %u (MaxFrameNum %u, delta %u, after %llu clean "
                     "advance(s))", fn_prev, fn_cur, fn_max, fn_delta,
                     (unsigned long long)object->frame_num_advances);
            if (decoder_flush_session(object, flush_reason))
                object->fn_flushes++;
        }
        if (object->frames == 0 && object->bitstream_size >= 6u) {
            /* One line that proves what the client actually sent: a browser AU
             * starts with a slice (0x41/0x65 after the start code), a probe or
             * ffmpeg-style AU with SPS (0x67) / PPS (0x68). */
            INFO("%s: first AU %u B, head=%02x %02x %02x %02x %02x %02x "
                 "(has_sps=%d has_pps=%d)", __FUNCTION__, object->bitstream_size,
                 object->bitstream[0], object->bitstream[1], object->bitstream[2],
                 object->bitstream[3], object->bitstream[4], object->bitstream[5],
                 (int)has_sps, (int)has_pps);
        }
        if (has_sps && has_pps) {
            params_source = "in-AU";
        } else {
            prepend_size = decoder_synth_parameter_sets(object, object->bitstream,
                                                        object->bitstream_size);
            if (prepend_size) {
                prepend = object->synth;
                params_source = "synth";
            } else if (object->params_size) {
                prepend = object->params;
                prepend_size = object->params_size;
                params_source = "cached";
            }
            if (prepend &&
                decoder_reserve(&object->send, &object->send_cap,
                                prepend_size + object->bitstream_size)) {
                memcpy(object->send, prepend, prepend_size);
                memcpy(object->send + prepend_size, object->bitstream,
                       object->bitstream_size);
                send_data = object->send;
                send_size = prepend_size + object->bitstream_size;
                object->param_set_injections++;
            } else {
                params_source = "no";
            }
        }
        if (object->frames == 0) {
            INFO("%s: first host access unit %u B (SPS/PPS source=%s, %u B "
                 "prefix)", __FUNCTION__, send_size, params_source, prepend_size);
        } else {
            VERBOSE("%s: host access unit %u B (SPS/PPS source=%s, %u B prefix)",
                    __FUNCTION__, send_size, params_source, prepend_size);
        }
    }

    if (object->codec != VIRTIO_WDDM_CODEC_VP9) {
        VIRTIO_WDDM_DecodeTiming timing;

        /* One access unit per frame; the send/get_frame round trip (and the
         * one-shot-driver retry) is the shared decoder_decode_unit().  This is
         * the H.264 path and the HEVC path: both hand over one complete Annex-B
         * access unit with the parameter sets in front. */
        if (object->pipeline != NULL) {
            /* PIPELINE (2026-10-25): queue this access unit for the worker and
             * upload the OLDEST finished job instead of waiting for this one.
             * The worker's send of this AU and its host decode wait therefore
             * overlap the upload below, which is where the frame budget is. */
            UINT pipe_w = width;
            UINT pipe_h = height;

            decoder_send_geometry(object, width, height, &pipe_w, &pipe_h);
            if (!pipeline_frame(object, send_data, send_size, pipe_w, pipe_h, &pipe_frame))
                return; /* the failure is recorded; the next BeginFrame reports it */
            pipeline_used = TRUE;
            pipeline_fill = pipe_frame.fill;
            info = pipe_frame.info;
            frame_readable = pipe_frame.readable;
            t_send0 = pipe_frame.start_qpc;
            send_ms = pipe_frame.send_ms;
            get_ms = pipe_frame.fetch_ms;
            elapsed = pipe_frame.queue_ms > 0.0 ? (UINT)pipe_frame.queue_ms : 0u;
        } else {
            if (!decoder_decode_unit(object, send_data, send_size, width, height,
                                     &timing, &info))
                return;
            t_send0 = timing.start_qpc;
            elapsed = timing.elapsed_ms;
            send_ms = timing.send_ms;
            get_ms = timing.get_ms;
        }
        /* P-BATCH AV1: the picture is now in the host decoder.  A key frame
         * refreshed all eight slots by definition; any other picture is kept
         * for the one-frame lookahead that recovers its real mask when the next
         * picture's RefFrameMapTextureIndex arrives.  (This describes the AU
         * this EndFrame submitted, not the picture the upload writes, so it runs
         * in both modes.) */
        if (object->codec == VIRTIO_WDDM_CODEC_AV1) {
            Av1FrameParams *av1_done = &object->av1_frame;
            object->av1_frames++;
            if (av1_done->frame_type == AV1_FRAME_KEY) {
                object->av1_key_frames++;
                av1_dpb_update(&object->av1_dpb, av1_done, 0xFFu);
                object->av1_prev_pending = FALSE;
            } else {
                object->av1_prev_frame = *av1_done;
                object->av1_prev_seq = object->av1_seq;
                object->av1_prev_texture = object->av1_pic.CurrPicTextureIndex;
                object->av1_prev_pending = TRUE;
            }
        }
    }

    if (info.Width == 0 || info.Height == 0 || info.Width > width ||
        info.Height > height) {
        ERROR("%s: decoded %ux%u does not fit the %ux%u surface -> E_FAIL", __FUNCTION__,
              info.Width, info.Height, width, height);
        decoder_drop_direct(object);
        decoder_fail_frame(object, E_FAIL);
        return;
    }

    /* P-BATCH ZEROCOPY: this frame's pixels live either in the media pool pages
     * the driver mapped into this process (direct path) or in the staged nv12
     * buffer (fallback).  Nothing below cares which, and the captured buffer is
     * released on every exit so the device keeps getting buffers back.
     *
     * PIPELINE: the third source is the drained job's own slot (a staged fetch
     * made by the worker), and the mapping it may hold was transferred into
     * object->direct_map by pipeline_drain(), so the release path below is
     * unchanged. */
    if (pipeline_used) {
        frame_src = pipe_frame.src;
    } else {
        frame_src = object->direct_active ? (CONST UINT8 *)object->direct_map.UserVa
                                          : (CONST UINT8 *)object->nv12;
        frame_readable = object->direct_active ? (UINT64)object->direct_map.MappedLength
                                               : (UINT64)object->nv12_cap;
    }
    if (!frame_src || frame_readable == 0) {
        ERROR("%s: no readable pixel source (%s) -> E_FAIL", __FUNCTION__,
              pipeline_used ? "pipeline job" : "staged/pool frame");
        decoder_drop_direct(object);
        decoder_fail_frame(object, E_FAIL);
        return;
    }

    /* PIPELINE: the upload target snapshot.  In the pipeline the picture being
     * written belongs to the job that BeginFrame selected, which is not
     * necessarily the BeginFrame that is ending now. */
    if (pipeline_used) {
        target_texture = pipe_frame.texture;
        dst_format = pipe_frame.tex_format;
        target_w = pipe_frame.tex_width;
        target_h = pipe_frame.tex_height;
        target_array = pipe_frame.array_size;
        target_mips = pipe_frame.mips;
        mips = pipe_frame.mips;
        subresource = pipe_frame.subresource;
        view_mip = pipe_frame.mip_slice;
        view_slice = pipe_frame.first_array_slice;
    } else {
        target_texture = resource->base.pResource;
        dst_format = (DXGI_FORMAT)resource->base.Format;
        target_w = resource->base.Width;
        target_h = resource->base.Height;
        target_array = resource->base.ArraySize;
        mips = resource->base.MipLevels ? resource->base.MipLevels : 1u;
        subresource = view->mip_slice + view->first_array_slice * mips;
        view_mip = view->mip_slice;
        view_slice = view->first_array_slice;
        target_mips = mips;
    }
    if (!target_texture) {
        ERROR("%s: no upload target texture (%s) -> E_FAIL", __FUNCTION__,
              pipeline_used ? "pipeline job" : "output view");
        decoder_drop_direct(object);
        decoder_fail_frame(object, E_FAIL);
        return;
    }

    /* P-BATCH P010: the source layout the host returned and the layout of the
     * surface it has to land in.
     *
     * vm_get_frame() describes the decoded frame in VM_FRAME_INFO: FourCC names
     * the layout of the bytes ('NV12' from media-wdm 1.0.0.16, whose S_FMT
     * CAPTURE is V4L2_PIX_FMT_NV12; 'P010' from a driver that negotiates
     * V4L2_PIX_FMT_P010), Size is the packed frame size and Stride the row pitch
     * in bytes. Both layouts are stride * height * 3 / 2 bytes (two planes of
     * the same row pitch, the chroma one half as tall), so one consistency
     * check covers either; the row is 1 byte per sample for NV12 and 2 for
     * P010, which is what `row_bytes` below names.
     *
     * The destination is whatever the application asked for in its decoder
     * descriptor (D3D11_1DDI_VIDEO_DECODER_DESC::OutputFormat): the texture the
     * output view names carries it, so EndFrame converts only when the two
     * disagree (decoder_convert_420). */
    host_p010 = info.FourCC == VIRTIO_WDDM_FOURCC_P010;
    /* PIPELINE: dst_format came from the drained job's snapshot above; only the
     * serial path has to read it from the resource that is ending now. */
    if (!pipeline_used)
        dst_format = (DXGI_FORMAT)resource->base.Format;
    if (!decoder_output_format_supported(dst_format)) {
        ERROR("%s: output texture %p has format %u, not NV12 or P010 -> E_FAIL",
              __FUNCTION__, (void *)target_texture, (UINT)dst_format);
        decoder_drop_direct(object);
        decoder_fail_frame(object, E_FAIL);
        return;
    }
    dst_p010 = dst_format == DXGI_FORMAT_P010;

    /* Both planes are stride bytes per row (vm_get_frame's documented layout).
     * If the driver returned a tighter buffer than its Stride claims, fall back
     * to the tight row. */
    row_bytes = host_p010 ? info.Width * 2u : info.Width;
    stride = info.Stride ? info.Stride : row_bytes;
    if (stride < row_bytes)
        stride = row_bytes;
    if ((UINT64)info.Size < (UINT64)stride * info.Height * 3u / 2u)
        stride = row_bytes;

    /* VP9FIX (2026-10-25): the pixel reads below (luma statistics, conversion
     * and UpdateSubresource) use `stride * Height * 3/2` bytes from frame_src.
     * That must fit BOTH the bytes the producer says are valid (info.Size) and
     * the memory actually mapped/copied here -- direct_map.MappedLength for the
     * pool path, nv12_cap for the staged path.  Before this check a short host
     * frame (or a mapping smaller than Size) made the luma loop and the upload
     * read past the end of the mapping, which is a 0xC0000005 in this DLL
     * instead of a visible decode error. */
    {
        UINT64 read_need = (UINT64)stride * info.Height * 3u / 2u;
        if (read_need > (UINT64)info.Size || read_need > frame_readable) {
            ERROR("%s: %ux%u stride=%u needs %llu B but only %u B are valid and "
                  "%llu B are readable (%s) -> E_INVALIDARG", __FUNCTION__,
                  info.Width, info.Height, stride, (unsigned long long)read_need,
                  info.Size, (unsigned long long)frame_readable,
                  pipeline_used ? (pipeline_fill ? "pipeline fill buffer"
                                                 : "pipeline job")
                                : (object->direct_active ? "pool mapping"
                                                         : "staged buffer"));
            decoder_fail_frame(object, E_INVALIDARG); /* also releases the mapping */
            return;
        }
    }

    /* Luma statistics for the on-device log: the same numbers P2-1's test
     * printed, so a frame can be compared against the known-good reference.
     *
     * P3-5: this used to scan every pixel of every frame (921,600 at 1280x720)
     * on the decode thread, and it is only a diagnostic. It is now computed
     * only for the frames whose log line actually carries it (the first three
     * and every 120th, exactly the frames the INFO lines below print) and then
     * by sampling every 8th row and column, i.e. 1/64th of the work. y_mean and
     * nonzero/samples always describe the same pixel set. */
    frame_no = object->frames + 1u;
    want_stats = !pipeline_fill &&
                 ((frame_no <= 3u) || ((frame_no % 120u) == 0u));
    y_mean = 0.0;
    if (want_stats) {
        UINT step = frame_no <= 3u ? 1u : 8u;
        for (y = 0; y < info.Height; y += step) {
            CONST UINT8 *row = frame_src + (size_t)y * stride;
            for (x = 0; x < info.Width; x += step) {
                /* P-BATCH P010: a P010 luma sample is the 16-bit word; its top
                 * 8 bits are the code value, so the statistic stays comparable
                 * with the NV12 runs. */
                UINT sample = host_p010 ? (UINT)(((CONST USHORT *)row)[x] >> 8)
                                        : (UINT)row[x];
                y_sum += sample;
                y_nonzero += (sample != 0);
                y_samples++;
            }
        }
        y_mean = y_samples ? (double)y_sum / (double)y_samples : 0.0;
    }

    /* P2-2j: the frame belongs in the subresource the application selected with
     * DecoderBeginFrame, not in subresource 0. A decode pool is one NV12
     * texture with ArraySize slices and MipLevels mips; D3D11 numbers the
     * subresources mip + slice * MipLevels. Players (PotPlayer) rotate through
     * 22 slices, so writing slice 0 put the picture where nothing samples it and
     * left the sampled slice zeroed -- which a YCbCr->RGB conversion turns into
     * flat green (Y=0, U=V=0).
     *
     * subresource / mips / target_* were resolved above, from this BeginFrame's
     * output view on the serial path and from the drained job's snapshot on the
     * pipeline path; only the range check is left here. */
    if (subresource >= (target_array ? target_array : 1u) * target_mips) {
        ERROR("%s: output view mip=%u slice=%u is outside the %ux%u surface "
              "(mips=%u array=%u) -> E_FAIL",
              __FUNCTION__, view_mip, view_slice,
              target_w, target_h, mips,
              target_array);
        /* VP9FIX: decoder_fail_frame() releases the mapped pool frame, so this
         * early exit cannot leave a capture buffer checked out. */
        decoder_fail_frame(object, E_FAIL);
        return;
    }

    /* P-BATCH P010: the frame already has the surface's layout in the common
     * case -- host NV12 into an NV12 surface today, P010 into a P010 surface
     * once media-wdm negotiates V4L2_PIX_FMT_P010 -- so nothing is copied here
     * beyond what UpdateSubresource stages anyway. Only a mismatch (an 8-bit
     * host picture into a 10-bit surface, or the reverse) is converted, once,
     * into the decoder's scratch buffer, which is sized for the destination
     * layout and kept at its high-water mark. */
    if (host_p010 == dst_p010) {
        upload_src = frame_src;
        upload_stride = stride;
        upload_fourcc = host_p010 ? VIRTIO_WDDM_FOURCC_P010 : VIRTIO_WDDM_FOURCC_NV12;
    } else {
        UINT conv_stride = dst_p010 ? info.Width * 2u : info.Width;
        UINT64 conv_need = (UINT64)conv_stride * info.Height * 3u / 2u;
        if (conv_need > 0x7FFFFFFFu) {
            ERROR("%s: %ux%u needs %llu bytes for the %s surface -> E_FAIL",
                  __FUNCTION__, info.Width, info.Height, (unsigned long long)conv_need,
                  dst_p010 ? "P010" : "NV12");
            decoder_drop_direct(object);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        if (!decoder_reserve(&object->conv, &object->conv_cap, (UINT)conv_need)) {
            decoder_drop_direct(object);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        if (!decoder_convert_420(frame_src, stride, host_p010, object->conv,
                                 conv_stride, dst_p010, info.Width, info.Height)) {
            ERROR("%s: cannot convert the %s host frame %ux%u (stride=%u) into the "
                  "%s surface (stride=%u) -> E_FAIL", __FUNCTION__,
                  host_p010 ? "P010" : "NV12", info.Width, info.Height, stride,
                  dst_p010 ? "P010" : "NV12", conv_stride);
            decoder_drop_direct(object);
            decoder_fail_frame(object, E_FAIL);
            return;
        }
        upload_src = object->conv;
        upload_stride = conv_stride;
        upload_fourcc = dst_p010 ? VIRTIO_WDDM_FOURCC_P010 : VIRTIO_WDDM_FOURCC_NV12;
        if (object->frames < 3u || (object->frames % 120u) == 0u)
            INFO("%s: frame %llu converted %s -> %s (%ux%u, strides %u -> %u)",
                 __FUNCTION__, (unsigned long long)frame_no,
                 host_p010 ? "P010" : "NV12", dst_p010 ? "P010" : "NV12",
                 info.Width, info.Height, stride, conv_stride);
    }

    t_up0 = decoder_qpc_now();
    hr = virtio_wddm_dxvk_video_upload_frame(device->base.pCtx1,
                                            target_texture, subresource,
                                            upload_src, info.Width, info.Height,
                                            upload_stride, upload_fourcc);
    t_up1 = decoder_qpc_now();
    up_ms = decoder_qpc_ms(t_up0, t_up1);
    if (FAILED(hr)) {
        ERROR("%s: %s upload into %ux%u texture (subresource %u = mip %u slice %u) "
              "failed: 0x%08lx -> E_FAIL",
              __FUNCTION__, dst_p010 ? "P010" : "NV12",
              target_w, target_h, subresource,
              view_mip, view_slice, (unsigned long)hr);
        decoder_drop_direct(object);
        decoder_fail_frame(object, E_FAIL);
        return;
    }
    /* P-BATCH ZEROCOPY: the pixels are in the texture now (UpdateSubresource
     * copied them into DXVK's staging buffer synchronously), so the capture
     * buffer can go straight back to the device. */
    decoder_drop_direct(object);

    if (pipeline_fill) {
        /* The placeholder picture is not a decoded frame: it must not move
         * object->frames or the per-frame statistics. */
        INFO("%s: pipeline placeholder written into subresource %u (tex=%p) "
             "while job %llu is still in flight",
             __FUNCTION__, subresource, (void *)target_texture,
             (unsigned long long)object->pipeline->submit_seq);
        return;
    }

    object->frames++;
    if (object->codec == VIRTIO_WDDM_CODEC_HEVC)
        object->hevc_frames++;
    /* P2-2J diag v3: read the subresource back and report its plane means. If
     * this shows the decoded picture, the upload landed where the log says;
     * if it shows zeros, the write never reached the texture the renderer
     * samples. First 3 frames only -- a readback waits for the GPU. */
    if (object->frames <= 3u) {
        double ma = -1.0, mb = -1.0, mc = -1.0;
        UINT sampled = 0;
        HRESULT shr = virtio_wddm_dxvk_video_tex_stats(
            device->base.pDev1, device->base.pCtx1, target_texture, subresource,
            &ma, &mb, &mc, &sampled);
        if (SUCCEEDED(shr)) {
            INFO("%s: (P2-2J diag v3) texture readback tex=%p sub=%u fmt=%u %ux%u: "
                 "Ymean=%.2f Umean=%.2f Vmean=%.2f (%u px)", __FUNCTION__,
                 (void *)target_texture, subresource,
                 (UINT)dst_format, target_w, target_h,
                 ma, mb, mc, sampled);
        } else {
            ERROR("%s: (P2-2J diag v3) texture readback failed: 0x%08lx (fmt=%u)", __FUNCTION__,
                  (unsigned long)shr, (UINT)dst_format);
        }
    }
    /* P2-2J diag v2: one compact line per decoded frame, in the ID3D11 texture
     * pointer space (tex=), so every write can be matched against the
     * input_tex=/input_slice= of the blit that samples it.
     * P3-5: t= is the absolute QPC tick at EndFrame entry, so the decode and
     * present timelines come straight out of the log (no length poller). */
    INFO("%s: frame %llu t=%lld -> tex=%p subresource=%u (mip %u slice %u of %u x %u mips) "
         "(P2-2J diag v2)", __FUNCTION__, (unsigned long long)object->frames,
         (long long) t_send0,
         (void *)target_texture, subresource, view_mip,
         view_slice, target_array, mips);
    if (object->frames == 1 || (object->frames % 120u) == 0) {
        INFO("%s: frame %llu in %lu ms: %ux%u stride=%u size=%u Ymean=%.2f "
             "nonzero=%llu/%llu (1/%u sampled) -> %s texture %ux%u res=%p tex=%p subresource=%u "
             "(mip %u slice %u of %u x %u mips) sent=%u B, SPS/PPS=%s",
             __FUNCTION__, (unsigned long long)object->frames, (unsigned long)elapsed,
             info.Width, info.Height, stride, info.Size, y_mean,
             (unsigned long long)y_nonzero, (unsigned long long)y_samples,
             info.Width && info.Height ? (UINT)(((info.Width + 7u) / 8u) * ((info.Height + 7u) / 8u))
                                       : 1u,
             dst_p010 ? "P010" : "NV12",
             target_w, target_h, (void *)resource,
             (void *)target_texture, subresource, view_mip,
             view_slice, target_array, mips, send_size,
             object->param_set_injections && send_data == object->send ? "yes" : "no");
        /* P3-5: the decode half of the per-frame budget; dxgi.c logs the other
         * half (present). get is the host decode latency, up is the staging
         * copy + DXVK recording, send is the AU handover.
         * AV1FIX (2026-10-25): wait/copy split `get` into the part the host owes
         * (the frame becoming available) and the part the pool copy owes.  -1
         * means "one blocking ioctl, not separable" -- only the AV1 path arms the
         * polled acquisition, and only after its first timeout. */
        INFO("%s: frame %llu budget: send=%.2f host_wait=%.2f copy=%.2f up=%.2f "
             "(sum %.2f ms, polls=%u), au=%u B subresource=%u, direct=%llu maps=%llu "
             "releases=%llu %s (mapped=%u B)%s", __FUNCTION__,
             (unsigned long long)frame_no, send_ms, object->last_wait_ms,
             object->last_copy_ms, up_ms,
             send_ms + get_ms + up_ms, object->last_get_polls, send_size,
             subresource, (unsigned long long)object->direct_frames,
             (unsigned long long)object->direct_maps,
             (unsigned long long)object->direct_releases,
             pipeline_used ? "pool/staged (pipeline job)" :
             (frame_src == (const UINT8 *)object->nv12 ? "staging" : "pool"),
             object->direct_map.MappedLength,
             pipeline_used ? "" : " (serial)");
        if (pipeline_used) {
            /* PIPELINE (2026-10-25): the frame that was uploaded is the job
             * submitted one EndFrame earlier, so its `send`/`fetch` and the
             * queueing delay are what tell the pipeline apart from the serial
             * path.  `lead` > 0 means the worker had finished before the DDI
             * thread asked (the intended steady state). */
            INFO("%s: frame %llu pipeline: send=%.2f fetch=%.2f queue=%.2f lead=%.2f "
                 "drain_wait=%.2f ms, %u job(s) in flight, gen %llu", __FUNCTION__,
                 (unsigned long long)frame_no, pipe_frame.send_ms, pipe_frame.fetch_ms,
                 pipe_frame.queue_ms, pipe_frame.lead_ms, pipe_frame.wait_ms,
                 (unsigned)object->pipeline->jobs_in_flight,
                 (unsigned long long)object->pipeline->generation);
        }
    } else {
        VERBOSE("%s: frame %llu in %lu ms: %ux%u stride=%u size=%u (send=%.2f get=%.2f "
                "up=%.2f ms)", __FUNCTION__, (unsigned long long)object->frames,
                (unsigned long)elapsed, info.Width, info.Height, stride, info.Size,
                send_ms, get_ms, up_ms);
    }
}

/* Minimum answer to pfnQueryVideoCapabilities (2.0/2.1 table only). The runtime
 * uses DECODER_CAPS to ask about downsampling / non-real-time decode: this
 * decoder supports neither, so Caps=0 for the profiles it really decodes and
 * UNSUPPORTED for any other profile.  HEVC-MAIN10: the same profile/format gate
 * decides what "really decodes", so a host without a P010 surface does not
 * report Main10 as available here either. The other query types are
 * downsampling recommendations this driver does not implement, so their output
 * is zeroed (Supported=FALSE). */
static VOID APIENTRY virtio_wddm_video_query_video_capabilities(
    D3D10DDI_HDEVICE hDevice, D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY query_type,
    UINT data_size, VOID *pData) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    VIRTIO_WDDM_DDI_ENTRY("GetVideoDecoderCaps/QueryVideoCapabilities: query_type=%u "
                          "data_size=%u pData=%p",
                          (unsigned)query_type, data_size, pData);
    if (!pData || data_size == 0)
        return;

    if (query_type == D3DWDDM2_0DDI_VIDEO_CAPABILITY_QUERY_DECODER_CAPS &&
        data_size >= sizeof(D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_CAPS)) {
        D3DWDDM2_0DDI_VIDEO_CAPABILITY_DECODER_CAPS *caps = pData;
        const VIRTIO_WDDM_DecoderProfile *profile =
            decoder_profile_from_guid(&caps->DecodeProfile);
        caps->Caps = decoder_profile_advertised(device, profile)
                         ? 0
                         : D3DWDDM2_0DDI_VIDEO_DECODER_CAP_UNSUPPORTED;
        VIRTIO_WDDM_DDI_ENTRY("DECODER_CAPS profile=%08lx-%04x-%04x -> 0x%x",
                              (unsigned long)caps->DecodeProfile.Data1,
                              caps->DecodeProfile.Data2, caps->DecodeProfile.Data3,
                              caps->Caps);
        return;
    }

    memset(pData, 0, data_size);
}

/* ------------------------------------------------------------------------- */
/* Video processor: enumerator, caps, object                                 */
/* ------------------------------------------------------------------------- */

static SIZE_T APIENTRY virtio_wddm_video_calc_private_video_processor_enum_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *pArgs) {
    (void)hDevice;
    (void)pArgs;
    return sizeof(VIRTIO_WDDM_VideoProcessorEnum);
}

static HRESULT APIENTRY virtio_wddm_video_create_video_processor_enum(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *pArgs,
    D3D11_1DDI_HVIDEOPROCESSORENUM hEnum, D3D11_1DDI_HRTVIDEOPROCESSORENUM hRTEnum) {
    (void)hRTEnum;

    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    VIRTIO_WDDM_VideoProcessorEnum *object = hEnum.pDrvPrivate;
    if (!object || !device)
        return E_INVALIDARG;

    memset(object, 0, sizeof(*object));
    object->content = pArgs->Desc;

    void *dxvk_device = NULL;
    void *dxvk_context = NULL;
    HRESULT hr = virtio_wddm_dxvk_video_get(device->base.pDev1, device->base.pCtx1,
                                            &dxvk_device, &dxvk_context);
    if (FAILED(hr)) {
        /* No DXVK video device means no processor; the runtime reports the
         * failure to the app and MF falls back to its software path. */
        ERROR("%s: internal DXVK device has no video interfaces: 0x%08lx",
              __FUNCTION__, hr);
        return hr;
    }

    VIRTIO_WDDM_VideoContentDesc desc = {
        .frame_format = (UINT)pArgs->Desc.InputFrameFormat,
        .input_numerator = pArgs->Desc.InputFrameRate.Numerator,
        .input_denominator = pArgs->Desc.InputFrameRate.Denominator,
        .input_width = pArgs->Desc.InputWidth,
        .input_height = pArgs->Desc.InputHeight,
        .output_numerator = pArgs->Desc.OutputFrameRate.Numerator,
        .output_denominator = pArgs->Desc.OutputFrameRate.Denominator,
        .output_width = pArgs->Desc.OutputWidth,
        .output_height = pArgs->Desc.OutputHeight,
        .usage = (UINT)pArgs->Desc.Usage,
    };

    hr = virtio_wddm_dxvk_video_create_enum(dxvk_device, &desc, &object->dxvk_enum);
    if (FAILED(hr)) {
        ERROR("%s: DXVK CreateVideoProcessorEnumerator failed: 0x%08lx",
              __FUNCTION__, hr);
        virtio_wddm_dxvk_video_release(dxvk_device, dxvk_context);
        return hr;
    }

    object->dxvk_device = dxvk_device;
    object->dxvk_context = dxvk_context;
    object->magic = VIRTIO_WDDM_VIDEO_MAGIC_ENUM;

    INFO("%s: enum=%p %ux%u@%u/%u -> %ux%u@%u/%u fm=%u usage=%u dxvk=%p", __FUNCTION__,
         (void *)object,
         pArgs->Desc.InputWidth, pArgs->Desc.InputHeight,
         pArgs->Desc.InputFrameRate.Numerator, pArgs->Desc.InputFrameRate.Denominator,
         pArgs->Desc.OutputWidth, pArgs->Desc.OutputHeight,
         pArgs->Desc.OutputFrameRate.Numerator, pArgs->Desc.OutputFrameRate.Denominator,
         (unsigned)pArgs->Desc.InputFrameFormat, (unsigned)pArgs->Desc.Usage,
         object->dxvk_enum);
    return S_OK;
}

static VOID APIENTRY virtio_wddm_video_destroy_video_processor_enum(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum) {
    (void)hDevice;

    VIRTIO_WDDM_VideoProcessorEnum *object = hEnum.pDrvPrivate;
    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_ENUM) {
        ERROR("%s: bad enumerator handle %p", __FUNCTION__, (void *)object);
        return;
    }
    object->magic = 0;
    virtio_wddm_dxvk_video_destroy_enum(object->dxvk_enum);
    virtio_wddm_dxvk_video_release(object->dxvk_device, object->dxvk_context);
    object->dxvk_enum = NULL;
    object->dxvk_device = NULL;
    object->dxvk_context = NULL;
    VERBOSE("%s: enum=%p", __FUNCTION__, (void *)object);
}

static VOID APIENTRY virtio_wddm_video_check_video_processor_format(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
    DXGI_FORMAT format, UINT *pFlags) {
    (void)hDevice;
    (void)hEnum;
    if (!pFlags)
        return;

    UINT flags = 0;
    if (video_input_format_supported(format))
        flags |= D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT;
    if (video_output_format_supported(format))
        flags |= D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT;

    *pFlags = flags;
    VERBOSE("%s: format=%u flags=0x%x", __FUNCTION__, (unsigned)format, flags);
}

static VOID APIENTRY virtio_wddm_video_get_video_processor_caps(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
    D3D11_1DDI_VIDEO_PROCESSOR_CAPS *pCaps) {
    (void)hDevice;
    (void)hEnum;
    if (!pCaps)
        return;

    *pCaps = (D3D11_1DDI_VIDEO_PROCESSOR_CAPS) {
        .DeviceCaps = VIRTIO_WDDM_VIDEO_DEVICE_CAPS,
        .FeatureCaps = 0,
        .FilterCaps = 0,
        .InputFormatCaps = 0,
        .AutoStreamCaps = 0,
        .StereoCaps = 0,
        .RateConversionCapsCount = VIRTIO_WDDM_VIDEO_RATE_CONVERSION_CAPS,
        .MaxInputStreams = VIRTIO_WDDM_VIDEO_MAX_INPUT_STREAMS,
        .MaxStreamStates = VIRTIO_WDDM_VIDEO_MAX_STREAM_STATES,
    };

    INFO("%s: dev=0x%x feat=0x%x filter=0x%x rateconv=%u maxin=%u maxstates=%u",
         __FUNCTION__, pCaps->DeviceCaps, pCaps->FeatureCaps, pCaps->FilterCaps,
         pCaps->RateConversionCapsCount, pCaps->MaxInputStreams,
         pCaps->MaxStreamStates);
}

static VOID APIENTRY virtio_wddm_video_get_video_processor_rate_conversion_caps(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum, UINT index,
    D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *pCaps) {
    (void)hDevice;
    (void)hEnum;
    (void)index;
    if (!pCaps)
        return;

    /* No deinterlacing, no inverse telecine, no frame-rate conversion: the
     * conversion caps field stays 0 and no custom rates exist. */
    memset(pCaps, 0, sizeof(*pCaps));
    VERBOSE("%s: index=%u (no frame-rate conversion)", __FUNCTION__, index);
}

static VOID APIENTRY virtio_wddm_video_get_video_processor_custom_rate(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
    UINT rate_conversion_index, UINT custom_rate_index,
    D3D11_1DDI_VIDEO_PROCESSOR_CUSTOM_RATE *pRate) {
    (void)hDevice;
    (void)hEnum;
    (void)rate_conversion_index;
    (void)custom_rate_index;
    if (pRate)
        memset(pRate, 0, sizeof(*pRate));
}

static VOID APIENTRY virtio_wddm_video_get_video_processor_filter_range(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORENUM hEnum,
    D3D11_1DDI_VIDEO_PROCESSOR_FILTER filter,
    D3D11_1DDI_VIDEO_PROCESSOR_FILTER_RANGE *pRange) {
    (void)hDevice;
    (void)hEnum;
    (void)filter;
    if (!pRange)
        return;

    /* FilterCaps is 0, so the runtime should not ask. Answer with an identity
     * range rather than leaving the output undefined. */
    *pRange = (D3D11_1DDI_VIDEO_PROCESSOR_FILTER_RANGE) {
        .Minimum = 0,
        .Maximum = 0,
        .Default = 0,
        .Multiplier = 1.0f,
    };
}

static SIZE_T APIENTRY virtio_wddm_video_calc_private_video_processor_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *pArgs) {
    VIRTIO_WDDM_DDI_ENTRY("hDevice=%p pArgs=%p -> %zu bytes",
                          (void *)hDevice.pDrvPrivate, (const void *)pArgs,
                          sizeof(VIRTIO_WDDM_VideoProcessor));
    (void)hDevice;
    (void)pArgs;
    return sizeof(VIRTIO_WDDM_VideoProcessor);
}

static HRESULT APIENTRY virtio_wddm_video_create_video_processor(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDIARG_CREATEVIDEOPROCESSOR *pArgs,
    D3D11_1DDI_HVIDEOPROCESSOR hProcessor, D3D11_1DDI_HRTVIDEOPROCESSOR hRTProcessor) {
    (void)hRTProcessor;

    VIRTIO_WDDM_VideoProcessor *object = hProcessor.pDrvPrivate;
    VIRTIO_WDDM_VideoProcessorEnum *enumeration = pArgs->hVideoProcessorEnum.pDrvPrivate;

    VIRTIO_WDDM_DDI_ENTRY("hDevice=%p pArgs=%p hProcessor=%p enum=%p rateconv=%u",
                          (void *)hDevice.pDrvPrivate, (const void *)pArgs,
                          (void *)object, (void *)enumeration,
                          pArgs->RateConversionCapsIndex);

    if (!object)
        return E_INVALIDARG;
    if (!enumeration || enumeration->magic != VIRTIO_WDDM_VIDEO_MAGIC_ENUM) {
        ERROR("%s: invalid enumerator %p", __FUNCTION__, (void *)enumeration);
        return E_INVALIDARG;
    }
    if (pArgs->RateConversionCapsIndex >= VIRTIO_WDDM_VIDEO_RATE_CONVERSION_CAPS) {
        ERROR("%s: rate conversion index %u out of range", __FUNCTION__,
              pArgs->RateConversionCapsIndex);
        return E_INVALIDARG;
    }

    memset(object, 0, sizeof(*object));
    object->enumeration = enumeration;
    object->rate_conversion_caps_index = pArgs->RateConversionCapsIndex;

    /* The processor gets its own references: the app is free to release the
     * enumerator while keeping the processor, and the runtime would then call
     * DestroyVideoProcessorEnum before DestroyVideoProcessor. */
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    if (!device)
        return E_INVALIDARG;

    void *dxvk_device = NULL;
    void *dxvk_context = NULL;
    HRESULT hr = virtio_wddm_dxvk_video_get(device->base.pDev1, device->base.pCtx1,
                                            &dxvk_device, &dxvk_context);
    if (FAILED(hr)) {
        ERROR("%s: internal DXVK device has no video interfaces: 0x%08lx",
              __FUNCTION__, hr);
        return hr;
    }

    hr = virtio_wddm_dxvk_video_create_processor(dxvk_device, enumeration->dxvk_enum,
                                                 pArgs->RateConversionCapsIndex,
                                                 &object->dxvk_processor);
    if (FAILED(hr)) {
        ERROR("%s: DXVK CreateVideoProcessor failed: 0x%08lx", __FUNCTION__, hr);
        virtio_wddm_dxvk_video_release(dxvk_device, dxvk_context);
        return hr;
    }

    object->dxvk_device = dxvk_device;
    object->dxvk_context = dxvk_context;
    object->magic = VIRTIO_WDDM_VIDEO_MAGIC_PROC;

    INFO("%s: processor=%p enum=%p rateconv=%u dxvk=%p", __FUNCTION__, (void *)object,
         (void *)enumeration, pArgs->RateConversionCapsIndex, object->dxvk_processor);
    return S_OK;
}

static VOID APIENTRY virtio_wddm_video_destroy_video_processor(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor) {
    (void)hDevice;

    VIRTIO_WDDM_VideoProcessor *object = hProcessor.pDrvPrivate;
    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_PROC) {
        ERROR("%s: bad processor handle %p", __FUNCTION__, (void *)object);
        return;
    }
    object->magic = 0;
    virtio_wddm_dxvk_video_destroy_processor(object->dxvk_processor);
    virtio_wddm_dxvk_video_release(object->dxvk_device, object->dxvk_context);
    object->dxvk_processor = NULL;
    object->dxvk_device = NULL;
    object->dxvk_context = NULL;
    VERBOSE("%s: processor=%p", __FUNCTION__, (void *)object);
}

/* Helper: a processor handle is valid and carries the DXVK objects the state
 * setters and the blit need. Returns NULL for an unknown handle. */
static VIRTIO_WDDM_VideoProcessor *video_processor_from_handle(
    D3D11_1DDI_HVIDEOPROCESSOR hProcessor, const char *function) {
    VIRTIO_WDDM_VideoProcessor *object = hProcessor.pDrvPrivate;
    if (!object || object->magic != VIRTIO_WDDM_VIDEO_MAGIC_PROC) {
        ERROR("%s: bad processor handle %p", function, (void *)object);
        return NULL;
    }
    if (!object->dxvk_processor || !object->dxvk_context) {
        ERROR("%s: processor %p has no DXVK objects", function, (void *)object);
        return NULL;
    }
    return object;
}

/* Output/stream state. Each setter is forwarded to the matching DXVK video
 * context entry, which stores it on the DXVK processor; the blit reads that
 * state when it builds the source/destination rectangles and the YCbCr matrix.
 * The DDI and the D3D11 runtime enums share their numeric values, so they are
 * cast rather than mapped. */
static VOID APIENTRY virtio_wddm_video_set_output_target_rect(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    BOOL enable, CONST RECT *pRect) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_output_target_rect(object->dxvk_context,
                                                  object->dxvk_processor, enable, pRect);
}

static VOID APIENTRY virtio_wddm_video_set_output_background_color(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    BOOL ycbcr, CONST D3D11_1DDI_VIDEO_COLOR *pColor) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    const float *color = pColor ? &pColor->RGBA.R : NULL;
    virtio_wddm_dxvk_video_set_output_background_color(object->dxvk_context,
                                                       object->dxvk_processor, ycbcr, color);
}

static VOID APIENTRY virtio_wddm_video_set_output_color_space(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    CONST D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE *pColorSpace) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object || !pColorSpace)
        return;
    UINT bits = 0;
    memcpy(&bits, pColorSpace, sizeof(bits));
    virtio_wddm_dxvk_video_set_output_color_space(object->dxvk_context,
                                                  object->dxvk_processor, bits);
}

static VOID APIENTRY virtio_wddm_video_set_output_alpha_fill_mode(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    D3D11_1DDI_VIDEO_PROCESSOR_ALPHA_FILL_MODE mode, UINT stream_index) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_output_alpha_fill_mode(object->dxvk_context,
                                                      object->dxvk_processor,
                                                      (UINT)mode, stream_index);
}

static VOID APIENTRY virtio_wddm_video_set_output_constriction(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    BOOL enable, SIZE size) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_output_constriction(object->dxvk_context,
                                                   object->dxvk_processor, enable,
                                                   (UINT)size.cx, (UINT)size.cy);
}

static VOID APIENTRY virtio_wddm_video_set_output_stereo_mode(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor, BOOL enable) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_output_stereo_mode(object->dxvk_context,
                                                  object->dxvk_processor, enable);
}

static HRESULT APIENTRY virtio_wddm_video_set_output_extension(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    CONST GUID *pExtensionGuid, UINT data_size, void *pData) {
    (void)hDevice; (void)hProcessor; (void)pExtensionGuid; (void)data_size; (void)pData;
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_get_output_extension(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    CONST GUID *pExtensionGuid, UINT data_size, void *pData) {
    (void)hDevice; (void)hProcessor; (void)pExtensionGuid; (void)data_size; (void)pData;
    return E_NOTIMPL;
}

/* Stream state. Forwarded exactly like the output state: DXVK's video blit
 * reads these when it builds the coordinate matrix (source/dest rectangle,
 * rotation) and the YCbCr->RGB matrix (color space, nominal range). */
static VOID APIENTRY virtio_wddm_video_set_stream_frame_format(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, D3D11_1DDI_VIDEO_FRAME_FORMAT format) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_frame_format(object->dxvk_context,
                                                   object->dxvk_processor,
                                                   stream_index, (UINT)format);
}

static VOID APIENTRY virtio_wddm_video_set_stream_color_space(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, CONST D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE *pColorSpace) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object || !pColorSpace)
        return;
    UINT bits = 0;
    memcpy(&bits, pColorSpace, sizeof(bits));
    virtio_wddm_dxvk_video_set_stream_color_space(object->dxvk_context,
                                                  object->dxvk_processor,
                                                  stream_index, bits);
}

static VOID APIENTRY virtio_wddm_video_set_stream_output_rate(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, D3D11_1DDI_VIDEO_PROCESSOR_OUTPUT_RATE rate,
    BOOL repeat_frame, CONST DXGI_RATIONAL *pCustom_rate) {
    (void)hDevice;
    VIRTIO_WDDM_DdiStamp ddi = video_ddi_enter();
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    UINT numerator = pCustom_rate ? pCustom_rate->Numerator : 0;
    UINT denominator = pCustom_rate ? pCustom_rate->Denominator : 0;
    virtio_wddm_dxvk_video_set_stream_output_rate(object->dxvk_context,
                                                  object->dxvk_processor, stream_index,
                                                  (UINT)rate, repeat_frame,
                                                  numerator, denominator);
    video_ddi_exit("rate", ddi);
}

static VOID APIENTRY virtio_wddm_video_set_stream_source_rect(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, CONST RECT *pRect) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_source_rect(object->dxvk_context,
                                                  object->dxvk_processor,
                                                  stream_index, enable, pRect);
}

static VOID APIENTRY virtio_wddm_video_set_stream_dest_rect(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, CONST RECT *pRect) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_dest_rect(object->dxvk_context,
                                                object->dxvk_processor,
                                                stream_index, enable, pRect);
}

static VOID APIENTRY virtio_wddm_video_set_stream_alpha(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, FLOAT alpha) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_alpha(object->dxvk_context,
                                            object->dxvk_processor,
                                            stream_index, enable, alpha);
}

static VOID APIENTRY virtio_wddm_video_set_stream_palette(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, UINT count, CONST UINT *pEntries) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_palette(object->dxvk_context,
                                              object->dxvk_processor,
                                              stream_index, count, pEntries);
}

static VOID APIENTRY virtio_wddm_video_set_stream_pixel_aspect_ratio(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, CONST DXGI_RATIONAL *pSource,
    CONST DXGI_RATIONAL *pDestination) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    UINT source_numerator = pSource ? pSource->Numerator : 0;
    UINT source_denominator = pSource ? pSource->Denominator : 0;
    UINT destination_numerator = pDestination ? pDestination->Numerator : 0;
    UINT destination_denominator = pDestination ? pDestination->Denominator : 0;
    virtio_wddm_dxvk_video_set_stream_pixel_aspect_ratio(
        object->dxvk_context, object->dxvk_processor, stream_index, enable,
        source_numerator, source_denominator, destination_numerator, destination_denominator);
}

static VOID APIENTRY virtio_wddm_video_set_stream_luma_key(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, FLOAT lower, FLOAT upper) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_luma_key(object->dxvk_context,
                                               object->dxvk_processor,
                                               stream_index, enable, lower, upper);
}

static VOID APIENTRY virtio_wddm_video_set_stream_stereo_format(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, D3D11_1DDI_VIDEO_PROCESSOR_STEREO_FORMAT format,
    BOOL left_view_frame0, BOOL right_view_frame0,
    D3D11_1DDI_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip_mode, int mono_offset) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_stereo_format(
        object->dxvk_context, object->dxvk_processor, stream_index, enable, (UINT)format,
        left_view_frame0, right_view_frame0, (UINT)flip_mode, mono_offset);
}

static VOID APIENTRY virtio_wddm_video_set_stream_auto_processing_mode(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_auto_processing_mode(object->dxvk_context,
                                                           object->dxvk_processor,
                                                           stream_index, enable);
}

static VOID APIENTRY virtio_wddm_video_set_stream_filter(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, D3D11_1DDI_VIDEO_PROCESSOR_FILTER filter, BOOL enable,
    int value) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_filter(object->dxvk_context,
                                             object->dxvk_processor,
                                             stream_index, (UINT)filter, enable, value);
}

static VOID APIENTRY virtio_wddm_video_set_stream_rotation(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, BOOL enable, D3D11_1DDI_VIDEO_PROCESSOR_ROTATION rotation) {
    (void)hDevice;
    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return;
    virtio_wddm_dxvk_video_set_stream_rotation(object->dxvk_context,
                                               object->dxvk_processor,
                                               stream_index, enable, (UINT)rotation);
}

static HRESULT APIENTRY virtio_wddm_video_set_stream_extension(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, CONST GUID *pExtensionGuid, UINT data_size, void *pData) {
    (void)hDevice; (void)hProcessor; (void)stream_index; (void)pExtensionGuid;
    (void)data_size; (void)pData;
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_get_stream_extension(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    UINT stream_index, CONST GUID *pExtensionGuid, UINT data_size, void *pData) {
    (void)hDevice; (void)hProcessor; (void)stream_index; (void)pExtensionGuid;
    (void)data_size; (void)pData;
    return E_NOTIMPL;
}

/* The blit: translate the DDI stream array and hand the whole thing to DXVK's
 * video processor. DXVK does the YUV->RGB matrix, the plane sampling and the
 * scaling draw into the output view on the internal immediate context.
 *
 * Failure policy: anything this driver does not implement is refused with an
 * explicit error instead of being silently ignored. The interesting cases are
 * deinterlacing (PastFrames/FutureFrames) and stereo (right-hand surfaces),
 * which our caps do not advertise; if the runtime asks anyway it gets
 * E_NOTIMPL and the app can fall back, rather than a wrong-looking frame. */
static HRESULT APIENTRY virtio_wddm_video_video_processor_blt(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOR hProcessor,
    D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW hOutputView, UINT output_frame,
    UINT stream_count, CONST D3D11_1DDI_VIDEO_PROCESSOR_STREAM *pStreams) {
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;
    VIRTIO_WDDM_DdiStamp ddi = video_ddi_enter();

    VIRTIO_WDDM_VideoProcessor *object =
        video_processor_from_handle(hProcessor, __FUNCTION__);
    if (!object)
        return E_INVALIDARG;

    VIRTIO_WDDM_VideoProcessorOutputView *output = hOutputView.pDrvPrivate;
    if (!output || output->magic != VIRTIO_WDDM_VIDEO_MAGIC_VIEW || !output->dxvk_view) {
        ERROR("%s: invalid output view %p", __FUNCTION__, (void *)output);
        return E_INVALIDARG;
    }

    if (!pStreams || stream_count == 0 ||
        stream_count > VIRTIO_WDDM_VIDEO_MAX_INPUT_STREAMS) {
        ERROR("%s: bad stream count %u (max %u)", __FUNCTION__, stream_count,
              VIRTIO_WDDM_VIDEO_MAX_INPUT_STREAMS);
        return E_INVALIDARG;
    }

    VIRTIO_WDDM_VideoStream streams[VIRTIO_WDDM_VIDEO_MAX_INPUT_STREAMS];
    for (UINT i = 0; i < stream_count; i++) {
        CONST D3D11_1DDI_VIDEO_PROCESSOR_STREAM *in = &pStreams[i];

        /* No deinterlacing, no frame-rate conversion, no stereo: caps say so,
         * and DXVK's blit would only log and ignore these anyway. */
        if (in->PastFrames || in->FutureFrames) {
            ERROR("%s: stream %u asks for deinterlacing (past=%u future=%u), not "
                  "supported", __FUNCTION__, i, in->PastFrames, in->FutureFrames);
            return E_NOTIMPL;
        }
        if (in->pPastSurfaces || in->pFutureSurfaces || in->pPastSurfacesRight ||
            in->pFutureSurfacesRight || in->hInputSurfaceRight.pDrvPrivate) {
            ERROR("%s: stream %u uses the stereo/deinterlace surfaces, not supported",
                  __FUNCTION__, i);
            return E_NOTIMPL;
        }

        void *input_view = NULL;
        if (in->hInputSurface.pDrvPrivate) {
            VIRTIO_WDDM_VideoProcessorInputView *input =
                in->hInputSurface.pDrvPrivate;
            if (input->magic != VIRTIO_WDDM_VIDEO_MAGIC_VIEW || !input->dxvk_view) {
                ERROR("%s: stream %u has an invalid input view %p", __FUNCTION__, i,
                      (void *)input);
                return E_INVALIDARG;
            }
            input_view = input->dxvk_view;
        } else if (in->Enable) {
            ERROR("%s: stream %u is enabled but has no input surface", __FUNCTION__, i);
            return E_INVALIDARG;
        }

        streams[i] = (VIRTIO_WDDM_VideoStream) {
            .enable = in->Enable,
            .output_index = in->OutputIndex,
            .input_frame_or_field = in->InputFrameOrField,
            .input_view = input_view,
        };
    }

    HRESULT hr = virtio_wddm_dxvk_video_blt(object->dxvk_context, object->dxvk_processor,
                                            output->dxvk_view, output_frame,
                                            stream_count, streams);

    /* One line per process for the first blit: enough to prove on the guest
     * that pixels went through DXVK, without flooding a per-frame log. */
    static volatile LONG logged;
    if (InterlockedCompareExchange(&logged, 1, 0) == 0) {
        VIRTIO_WDDM_VideoProcessorInputView *iv = NULL;
        VIRTIO_WDDM_Resource *ivres = NULL;
        if (pStreams[0].hInputSurface.pDrvPrivate)
            iv = pStreams[0].hInputSurface.pDrvPrivate;
        if (iv && iv->resource.pDrvPrivate)
            ivres = iv->resource.pDrvPrivate;
        if (SUCCEEDED(hr)) {
            INFO("%s: first blit queued (streams=%u frame=%u output=%p input_res=%p "
                 "input_slice=%u): DXVK video processor does YCbCr->RGB + scaling on "
                 "the internal Vulkan queue (P2-2J diag)",
                 __FUNCTION__, stream_count, output_frame, output->dxvk_view,
                 ivres ? (void *)ivres->base.pResource : (void *)NULL,
                 iv ? iv->first_array_slice : 0u);
        } else {
            ERROR("%s: DXVK VideoProcessorBlt failed: 0x%08lx", __FUNCTION__, hr);
        }
    }

    /* P2-2J diag v2: one line per blit (capped after 400) carrying the input
     * texture pointer and its array slice -- the values the decoder's
     * "frame N -> tex=... subresource=..." lines can be compared against in the
     * SAME pointer space. Also the only way to see whether the renderer samples
     * a different texture than the decoder wrote. */
    {
        static volatile LONG blit_count;
        VIRTIO_WDDM_VideoProcessorInputView *di = NULL;
        VIRTIO_WDDM_Resource *dires = NULL;
        void *input_tex = NULL;
        UINT input_slice = 0;
        LONG blit_no = InterlockedIncrement(&blit_count);

        if (pStreams[0].hInputSurface.pDrvPrivate) {
            di = pStreams[0].hInputSurface.pDrvPrivate;
            dires = di->resource.pDrvPrivate;
            if (dires)
                input_tex = (void *)dires->base.pResource;
            input_slice = di->first_array_slice;
        }
        if (true) {
            INFO("%s: blt #%ld streams=%u frame=%u input_tex=%p input_slice=%u "
                 "output=%p hr=0x%08lx (P2-2J diag v2)", __FUNCTION__, blit_no,
                 stream_count, output_frame, input_tex, input_slice, output->dxvk_view,
                 (unsigned long)hr);
        }

        /* P2-2J diag v3: what did the blit actually write into the render
         * target? First 3 blits only (a readback waits for the GPU). */
        if (blit_no <= 3 && device) {
            VIRTIO_WDDM_Resource *ores = output->resource.pDrvPrivate;
            if (ores && ores->base.pResource) {
                UINT omips = ores->base.MipLevels ? ores->base.MipLevels : 1u;
                UINT osub = output->mip_slice + output->first_array_slice * omips;
                double ra = -1.0, rb = -1.0, rc = -1.0;
                UINT sampled = 0;
                HRESULT shr = virtio_wddm_dxvk_video_tex_stats(
                    device->base.pDev1, device->base.pCtx1, ores->base.pResource, osub,
                    &ra, &rb, &rc, &sampled);
                if (SUCCEEDED(shr)) {
                    INFO("%s: (P2-2J diag v3) output readback blt #%ld tex=%p sub=%u "
                         "fmt=%u %ux%u: Rmean=%.2f Gmean=%.2f Bmean=%.2f (%u px)",
                         __FUNCTION__, blit_no, (void *)ores->base.pResource, osub,
                         (UINT)ores->base.Format, ores->base.Width, ores->base.Height,
                         ra, rb, rc, sampled);
                } else {
                    ERROR("%s: (P2-2J diag v3) output readback failed: 0x%08lx (fmt=%u "
                          "%ux%u sub=%u)", __FUNCTION__, (unsigned long)shr,
                          (UINT)ores->base.Format, ores->base.Width, ores->base.Height,
                          osub);
                }
            }
        }
    }
    video_ddi_exit("blt", ddi);
    return hr;
}

/* ------------------------------------------------------------------------- */
/* Views                                                                     */
/* ------------------------------------------------------------------------- */

/* Input/output views own a DXVK video-processor view, created from the D3D11
 * resource that the DDI resource wraps (VIRTIO_WDDM_Resource::base.pResource)
 * and the DXVK enumerator of the DDI enumerator. The DDI side keeps the
 * logical description (slice, FourCC) for logging; the blit passes the DXVK
 * view pointer straight through. */

static SIZE_T APIENTRY virtio_wddm_video_calc_private_video_decoder_output_view_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *pArgs) {
    VIRTIO_WDDM_DDI_ENTRY("hDevice=%p pArgs=%p -> %zu bytes",
                          (void *)hDevice.pDrvPrivate, (const void *)pArgs,
                          sizeof(VIRTIO_WDDM_VideoDecoderOutputView));
    (void)hDevice;
    (void)pArgs;
    return sizeof(VIRTIO_WDDM_VideoDecoderOutputView);
}

/* CreateVideoDecoderOutputView: remember which texture the application decodes
 * into. The resource must already be a 2D NV12 or P010 texture -- the same
 * formats the decoder reports in CheckVideoDecoderFormat, so refusing anything
 * else here turns a wrong-format application into an immediate D3D11 error
 * instead of a corrupt frame later. No DXVK object is needed: EndFrame writes
 * the texture through the internal D3D11 context by resource pointer. */
static HRESULT APIENTRY virtio_wddm_video_create_video_decoder_output_view(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *pArgs,
    D3D11_1DDI_HVIDEODECODEROUTPUTVIEW hView,
    D3D11_1DDI_HRTVIDEODECODEROUTPUTVIEW hRTView) {
    (void)hDevice;
    (void)hRTView;

    VIRTIO_WDDM_VideoDecoderOutputView *object = hView.pDrvPrivate;
    VIRTIO_WDDM_Resource *resource;

    VIRTIO_WDDM_DDI_ENTRY("hView=%p pArgs=%p resource=%p", (void *)object,
                          (const void *)pArgs,
                          pArgs ? (void *)pArgs->hDrvResource.pDrvPrivate : NULL);

    if (!object || !pArgs)
        return E_INVALIDARG;
    memset(object, 0, sizeof(*object));

    resource = pArgs->hDrvResource.pDrvPrivate;
    if (!resource || !resource->base.pResource) {
        ERROR("%s: resource %p is not backed by a D3D11 texture -> E_FAIL", __FUNCTION__,
              (void *)resource);
        return E_FAIL;
    }
    if (resource->base.Dimension != D3D10DDIRESOURCE_TEXTURE2D ||
        !decoder_output_format_supported((DXGI_FORMAT)resource->base.Format)) {
        ERROR("%s: output surface must be a 2D NV12 or P010 texture (dim=%u format=%u) -> "
              "E_INVALIDARG", __FUNCTION__, (unsigned)resource->base.Dimension,
              (unsigned)resource->base.Format);
        return E_INVALIDARG;
    }

    object->magic = VIRTIO_WDDM_VIDEO_MAGIC_DECOUT;
    object->resource = pArgs->hDrvResource;
    object->format = (DXGI_FORMAT)resource->base.Format;
    object->mip_slice = pArgs->MipSlice;
    object->first_array_slice = pArgs->FirstArraySlice;
    object->array_size = pArgs->ArraySize;
    /* res= is the DDI wrapper, tex= the ID3D11Resource: the second one is the
     * pointer space the video-processor input views are logged in. */
    INFO("%s: view=%p res=%p tex=%p %ux%u %s mip=%u slice=%u/%u", __FUNCTION__,
         (void *)object, (void *)resource, (void *)resource->base.pResource,
         resource->base.Width, resource->base.Height,
         object->format == DXGI_FORMAT_P010 ? "P010" : "NV12",
         pArgs->MipSlice, pArgs->FirstArraySlice, pArgs->ArraySize);
    return S_OK;
}

static VOID APIENTRY virtio_wddm_video_destroy_video_decoder_output_view(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEODECODEROUTPUTVIEW hView) {
    (void)hDevice;
    VIRTIO_WDDM_VideoDecoderOutputView *object = hView.pDrvPrivate;
    if (object && object->magic == VIRTIO_WDDM_VIDEO_MAGIC_DECOUT) {
        VERBOSE("%s: view=%p", __FUNCTION__, (void *)object);
        memset(object, 0, sizeof(*object));
    }
}

static SIZE_T APIENTRY virtio_wddm_video_calc_private_video_processor_input_view_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW *pArgs) {
    (void)hDevice;
    (void)pArgs;
    return sizeof(VIRTIO_WDDM_VideoProcessorInputView);
}

static HRESULT APIENTRY virtio_wddm_video_create_video_processor_input_view(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW *pArgs,
    D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW hView,
    D3D11_1DDI_HRTVIDEOPROCESSORINPUTVIEW hRTView) {
    (void)hDevice;
    (void)hRTView;
    VIRTIO_WDDM_DdiStamp ddi = video_ddi_enter();

    VIRTIO_WDDM_VideoProcessorInputView *object = hView.pDrvPrivate;
    VIRTIO_WDDM_Resource *resource = pArgs->hDrvResource.pDrvPrivate;
    VIRTIO_WDDM_VideoProcessorEnum *enumeration =
        pArgs->hDrvVideoProcessorEnum.pDrvPrivate;

    if (!object)
        return E_INVALIDARG;
    if (!resource || !resource->base.pResource) {
        ERROR("%s: resource %p is not backed by a D3D11 texture", __FUNCTION__,
              (void *)resource);
        return E_FAIL;
    }
    if (!enumeration || enumeration->magic != VIRTIO_WDDM_VIDEO_MAGIC_ENUM ||
        !enumeration->dxvk_enum) {
        ERROR("%s: invalid enumerator %p", __FUNCTION__, (void *)enumeration);
        return E_INVALIDARG;
    }

    memset(object, 0, sizeof(*object));
    object->resource = pArgs->hDrvResource;
    object->four_cc = pArgs->FourCC;
    object->mip_slice = pArgs->MipSlice;
    object->first_array_slice = pArgs->FirstArraySlice;
    object->array_size = pArgs->ArraySize;

    HRESULT hr = virtio_wddm_dxvk_video_create_input_view(
        enumeration->dxvk_device, resource->base.pResource, enumeration->dxvk_enum,
        pArgs->FourCC, pArgs->MipSlice, pArgs->FirstArraySlice, &object->dxvk_view);
    if (FAILED(hr)) {
        ERROR("%s: DXVK CreateVideoProcessorInputView failed: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }

    object->magic = VIRTIO_WDDM_VIDEO_MAGIC_VIEW;
    VERBOSE("%s: view=%p res=%p fourcc=0x%x slice=%u/%u dxvk=%p", __FUNCTION__,
            (void *)object, pArgs->hDrvResource.pDrvPrivate, pArgs->FourCC,
            pArgs->FirstArraySlice, pArgs->ArraySize, object->dxvk_view);
    /* P2-2J diag: the renderer's input view has to name the same texture/array
     * slice the decoder wrote, otherwise the blit samples an empty surface and
     * the picture comes out flat green. Logged at INFO so a real player run
     * proves which surface is actually sampled. */
    INFO("%s: P2-2J diag input view=%p res=%p %ux%u fmt=%u mip=%u slice=%u/%u",
         __FUNCTION__, (void *)object, (void *)resource->base.pResource,
         resource->base.Width, resource->base.Height, (unsigned)resource->base.Format,
         pArgs->MipSlice, pArgs->FirstArraySlice, pArgs->ArraySize);
    video_ddi_exit("inputview", ddi);
    return S_OK;
}

static VOID APIENTRY virtio_wddm_video_destroy_video_processor_input_view(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW hView) {
    (void)hDevice;

    VIRTIO_WDDM_VideoProcessorInputView *object = hView.pDrvPrivate;
    if (object && object->magic == VIRTIO_WDDM_VIDEO_MAGIC_VIEW) {
        virtio_wddm_dxvk_video_destroy_input_view(object->dxvk_view);
        object->dxvk_view = NULL;
        object->magic = 0;
    }
}

static SIZE_T APIENTRY virtio_wddm_video_calc_private_video_processor_output_view_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW *pArgs) {
    (void)hDevice;
    (void)pArgs;
    return sizeof(VIRTIO_WDDM_VideoProcessorOutputView);
}

static HRESULT APIENTRY virtio_wddm_video_create_video_processor_output_view(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW *pArgs,
    D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW hView,
    D3D11_1DDI_HRTVIDEOPROCESSOROUTPUTVIEW hRTView) {
    (void)hDevice;
    (void)hRTView;
    VIRTIO_WDDM_DdiStamp ddi = video_ddi_enter();

    VIRTIO_WDDM_VideoProcessorOutputView *object = hView.pDrvPrivate;
    VIRTIO_WDDM_Resource *resource = pArgs->hDrvResource.pDrvPrivate;
    VIRTIO_WDDM_VideoProcessorEnum *enumeration =
        pArgs->hDrvVideoProcessorEnum.pDrvPrivate;

    if (!object)
        return E_INVALIDARG;
    if (!resource || !resource->base.pResource) {
        ERROR("%s: resource %p is not backed by a D3D11 texture", __FUNCTION__,
              (void *)resource);
        return E_FAIL;
    }
    if (!enumeration || enumeration->magic != VIRTIO_WDDM_VIDEO_MAGIC_ENUM ||
        !enumeration->dxvk_enum) {
        ERROR("%s: invalid enumerator %p", __FUNCTION__, (void *)enumeration);
        return E_INVALIDARG;
    }

    memset(object, 0, sizeof(*object));
    object->resource = pArgs->hDrvResource;
    object->mip_slice = pArgs->MipSlice;
    object->first_array_slice = pArgs->FirstArraySlice;
    object->array_size = pArgs->ArraySize;

    HRESULT hr = virtio_wddm_dxvk_video_create_output_view(
        enumeration->dxvk_device, resource->base.pResource, enumeration->dxvk_enum,
        pArgs->MipSlice, pArgs->FirstArraySlice, pArgs->ArraySize, &object->dxvk_view);
    if (FAILED(hr)) {
        ERROR("%s: DXVK CreateVideoProcessorOutputView failed: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }

    object->magic = VIRTIO_WDDM_VIDEO_MAGIC_VIEW;
    /* P2-2J diag v2: the renderer's present target, in the same tex= space. */
    INFO("%s: view=%p res=%p tex=%p slice=%u/%u dxvk=%p (P2-2J diag v2)",
         __FUNCTION__, (void *)object, (void *)resource,
         (void *)resource->base.pResource, pArgs->FirstArraySlice, pArgs->ArraySize,
         object->dxvk_view);
    video_ddi_exit("outputview", ddi);
    return S_OK;
}

static VOID APIENTRY virtio_wddm_video_destroy_video_processor_output_view(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW hView) {
    (void)hDevice;

    VIRTIO_WDDM_VideoProcessorOutputView *object = hView.pDrvPrivate;
    if (object && object->magic == VIRTIO_WDDM_VIDEO_MAGIC_VIEW) {
        virtio_wddm_dxvk_video_destroy_output_view(object->dxvk_view);
        object->dxvk_view = NULL;
        object->magic = 0;
    }
}

static VOID APIENTRY virtio_wddm_video_video_processor_input_view_read_after_write_hazard(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW hView,
    D3D10DDI_HRESOURCE hResource) {
    (void)hDevice; (void)hView; (void)hResource;
}

/* ------------------------------------------------------------------------- */
/* Content protection: not implemented, and not claimed                      */
/* ------------------------------------------------------------------------- */

static HRESULT APIENTRY virtio_wddm_video_get_content_protection_caps(
    D3D10DDI_HDEVICE hDevice, CONST GUID *pCryptoType, CONST GUID *pDecodeProfile,
    D3D11_1DDI_VIDEO_CONTENT_PROTECTION_CAPS *pCaps) {
    (void)hDevice; (void)pCryptoType; (void)pDecodeProfile;
    if (pCaps)
        memset(pCaps, 0, sizeof(*pCaps));
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_get_crypto_key_exchange_type(
    D3D10DDI_HDEVICE hDevice, CONST GUID *pCryptoType, CONST GUID *pDecodeProfile,
    UINT index, GUID *pKeyExchangeType) {
    (void)hDevice; (void)pCryptoType; (void)pDecodeProfile; (void)index;
    (void)pKeyExchangeType;
    return E_NOTIMPL;
}

static SIZE_T APIENTRY virtio_wddm_video_calc_private_crypto_session_size(
    D3D10DDI_HDEVICE hDevice, const D3D11_1DDIARG_CREATECRYPTOSESSION *pArgs) {
    (void)hDevice; (void)pArgs;
    return 0;
}

static HRESULT APIENTRY virtio_wddm_video_create_crypto_session(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDIARG_CREATECRYPTOSESSION *pArgs,
    D3D11_1DDI_HCRYPTOSESSION hSession, D3D11_1DDI_HRTCRYPTOSESSION hRTSession) {
    (void)hDevice; (void)pArgs; (void)hSession; (void)hRTSession;
    return E_NOTIMPL;
}

static VOID APIENTRY virtio_wddm_video_destroy_crypto_session(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession) {
    (void)hDevice; (void)hSession;
}

static VOID APIENTRY virtio_wddm_video_get_certificate_size(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDI_CERTIFICATE_INFO *pInfo,
    UINT *pCertificateSize) {
    (void)hDevice; (void)pInfo;
    if (pCertificateSize)
        *pCertificateSize = 0;
}

static VOID APIENTRY virtio_wddm_video_get_certificate(
    D3D10DDI_HDEVICE hDevice, CONST D3D11_1DDI_CERTIFICATE_INFO *pInfo,
    UINT certificate_size, BYTE *pCertificate) {
    (void)hDevice; (void)pInfo; (void)certificate_size; (void)pCertificate;
}

static HRESULT APIENTRY virtio_wddm_video_negotiate_crypto_session_key_exchange(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession, UINT data_size,
    BYTE *pData) {
    (void)hDevice; (void)hSession; (void)data_size; (void)pData;
    return E_NOTIMPL;
}

static VOID APIENTRY virtio_wddm_video_encryption_blt(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession,
    D3D10DDI_HRESOURCE hSrcResource, D3D10DDI_HRESOURCE hDstResource,
    UINT iv_size, CONST VOID *pIV) {
    (void)hDevice; (void)hSession; (void)hSrcResource; (void)hDstResource;
    (void)iv_size; (void)pIV;
}

static VOID APIENTRY virtio_wddm_video_decryption_blt(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession,
    D3D10DDI_HRESOURCE hSrcResource, D3D10DDI_HRESOURCE hDstResource,
    CONST D3D11_1DDI_ENCRYPTED_BLOCK_INFO *pBlockInfo, UINT content_key_size,
    CONST VOID *pContentKey, UINT iv_size, CONST VOID *pIV) {
    (void)hDevice; (void)hSession; (void)hSrcResource; (void)hDstResource;
    (void)pBlockInfo; (void)content_key_size; (void)pContentKey; (void)iv_size; (void)pIV;
}

static VOID APIENTRY virtio_wddm_video_start_session_key_refresh(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession,
    UINT random_number_size, VOID *pRandomNumber) {
    (void)hDevice; (void)hSession; (void)random_number_size; (void)pRandomNumber;
}

static VOID APIENTRY virtio_wddm_video_finish_session_key_refresh(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession) {
    (void)hDevice; (void)hSession;
}

static VOID APIENTRY virtio_wddm_video_get_encryption_blt_key(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession,
    UINT key_size, VOID *pReadbackKey) {
    (void)hDevice; (void)hSession; (void)key_size; (void)pReadbackKey;
}

static SIZE_T APIENTRY virtio_wddm_video_calc_private_authenticated_channel_size(
    D3D10DDI_HDEVICE hDevice,
    const D3D11_1DDIARG_CREATEAUTHENTICATEDCHANNEL *pArgs) {
    (void)hDevice; (void)pArgs;
    return 0;
}

static HRESULT APIENTRY virtio_wddm_video_create_authenticated_channel(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDIARG_CREATEAUTHENTICATEDCHANNEL *pArgs,
    D3D11_1DDI_HAUTHCHANNEL hChannel, D3D11_1DDI_HRTAUTHCHANNEL hRTChannel) {
    (void)hDevice; (void)pArgs; (void)hChannel; (void)hRTChannel;
    return E_NOTIMPL;
}

static VOID APIENTRY virtio_wddm_video_destroy_authenticated_channel(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HAUTHCHANNEL hChannel) {
    (void)hDevice; (void)hChannel;
}

static HRESULT APIENTRY virtio_wddm_video_negotiate_authenticated_channel_key_exchange(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HAUTHCHANNEL hChannel, UINT data_size,
    VOID *pData) {
    (void)hDevice; (void)hChannel; (void)data_size; (void)pData;
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_query_authenticated_channel(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HAUTHCHANNEL hChannel,
    UINT input_data_size, CONST VOID *pInputData, UINT output_data_size,
    VOID *pOutputData) {
    (void)hDevice; (void)hChannel; (void)input_data_size; (void)pInputData;
    (void)output_data_size; (void)pOutputData;
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_configure_authenticated_channel(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HAUTHCHANNEL hChannel,
    UINT input_data_size, CONST VOID *pInputData,
    D3D11_1DDI_AUTHENTICATED_CONFIGURE_OUTPUT *pOutputData) {
    (void)hDevice; (void)hChannel; (void)input_data_size; (void)pInputData;
    (void)pOutputData;
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_video_decoder_get_handle(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HDECODE hDecode,
    HANDLE *pContentProtectionHandle) {
    (void)hDevice; (void)hDecode;
    if (pContentProtectionHandle)
        *pContentProtectionHandle = NULL;
    return E_NOTIMPL;
}

static HRESULT APIENTRY virtio_wddm_video_crypto_session_get_handle(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_HCRYPTOSESSION hSession, HANDLE *pHandle) {
    (void)hDevice; (void)hSession;
    if (pHandle)
        *pHandle = NULL;
    return E_NOTIMPL;
}

static VOID APIENTRY virtio_wddm_video_get_capture_handle(
    D3D10DDI_HDEVICE hDevice, D3D11_1DDI_GETCAPTUREHANDLEDATA *pHandleData) {
    (void)hDevice;
    if (pHandleData) {
        pHandleData->hAllocation = 0;
        pHandleData->DataOffset = 0;
        pHandleData->DataSize = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Table population                                                          */
/* ------------------------------------------------------------------------- */

/* The 11.1 table, in WDK order. Kept as one list because the same entries have
 * to land in the 11.1, 2.0 and 2.1 tables, which all start with these 81
 * pointers in this order (2.0 adds 13, 2.1 adds 15 more after them). */
#define VIRTIO_WDDM_VIDEO_FUNCS(X) \
    X(pfnGetVideoDecoderProfileCount, virtio_wddm_video_get_video_decoder_profile_count) \
    X(pfnGetVideoDecoderProfile, virtio_wddm_video_get_video_decoder_profile) \
    X(pfnCheckVideoDecoderFormat, virtio_wddm_video_check_video_decoder_format) \
    X(pfnGetVideoDecoderConfigCount, virtio_wddm_video_get_video_decoder_config_count) \
    X(pfnGetVideoDecoderConfig, virtio_wddm_video_get_video_decoder_config) \
    X(pfnGetVideoDecoderBufferTypeCount, virtio_wddm_video_get_video_decoder_buffer_type_count) \
    X(pfnGetVideoDecoderBufferInfo, virtio_wddm_video_get_video_decoder_buffer_info) \
    X(pfnCalcPrivateVideoDecoderSize, virtio_wddm_video_calc_private_video_decoder_size) \
    X(pfnCreateVideoDecoder, virtio_wddm_video_create_video_decoder) \
    X(pfnDestroyVideoDecoder, virtio_wddm_video_destroy_video_decoder) \
    X(pfnVideoDecoderExtension, virtio_wddm_video_video_decoder_extension) \
    X(pfnVideoDecoderBeginFrame, virtio_wddm_video_video_decoder_begin_frame) \
    X(pfnVideoDecoderEndFrame, virtio_wddm_video_video_decoder_end_frame) \
    X(pfnVideoDecoderSubmitBuffers, virtio_wddm_video_video_decoder_submit_buffers) \
    X(pfnCalcPrivateVideoProcessorEnumSize, virtio_wddm_video_calc_private_video_processor_enum_size) \
    X(pfnCreateVideoProcessorEnum, virtio_wddm_video_create_video_processor_enum) \
    X(pfnDestroyVideoProcessorEnum, virtio_wddm_video_destroy_video_processor_enum) \
    X(pfnCheckVideoProcessorFormat, virtio_wddm_video_check_video_processor_format) \
    X(pfnGetVideoProcessorCaps, virtio_wddm_video_get_video_processor_caps) \
    X(pfnGetVideoProcessorRateConversionCaps, virtio_wddm_video_get_video_processor_rate_conversion_caps) \
    X(pfnGetVideoProcessorCustomRate, virtio_wddm_video_get_video_processor_custom_rate) \
    X(pfnGetVideoProcessorFilterRange, virtio_wddm_video_get_video_processor_filter_range) \
    X(pfnCalcPrivateVideoProcessorSize, virtio_wddm_video_calc_private_video_processor_size) \
    X(pfnCreateVideoProcessor, virtio_wddm_video_create_video_processor) \
    X(pfnDestroyVideoProcessor, virtio_wddm_video_destroy_video_processor) \
    X(pfnVideoProcessorSetOutputTargetRect, virtio_wddm_video_set_output_target_rect) \
    X(pfnVideoProcessorSetOutputBackgroundColor, virtio_wddm_video_set_output_background_color) \
    X(pfnVideoProcessorSetOutputColorSpace, virtio_wddm_video_set_output_color_space) \
    X(pfnVideoProcessorSetOutputAlphaFillMode, virtio_wddm_video_set_output_alpha_fill_mode) \
    X(pfnVideoProcessorSetOutputConstriction, virtio_wddm_video_set_output_constriction) \
    X(pfnVideoProcessorSetOutputStereoMode, virtio_wddm_video_set_output_stereo_mode) \
    X(pfnVideoProcessorSetOutputExtension, virtio_wddm_video_set_output_extension) \
    X(pfnVideoProcessorGetOutputExtension, virtio_wddm_video_get_output_extension) \
    X(pfnVideoProcessorSetStreamFrameFormat, virtio_wddm_video_set_stream_frame_format) \
    X(pfnVideoProcessorSetStreamColorSpace, virtio_wddm_video_set_stream_color_space) \
    X(pfnVideoProcessorSetStreamOutputRate, virtio_wddm_video_set_stream_output_rate) \
    X(pfnVideoProcessorSetStreamSourceRect, virtio_wddm_video_set_stream_source_rect) \
    X(pfnVideoProcessorSetStreamDestRect, virtio_wddm_video_set_stream_dest_rect) \
    X(pfnVideoProcessorSetStreamAlpha, virtio_wddm_video_set_stream_alpha) \
    X(pfnVideoProcessorSetStreamPalette, virtio_wddm_video_set_stream_palette) \
    X(pfnVideoProcessorSetStreamPixelAspectRatio, virtio_wddm_video_set_stream_pixel_aspect_ratio) \
    X(pfnVideoProcessorSetStreamLumaKey, virtio_wddm_video_set_stream_luma_key) \
    X(pfnVideoProcessorSetStreamStereoFormat, virtio_wddm_video_set_stream_stereo_format) \
    X(pfnVideoProcessorSetStreamAutoProcessingMode, virtio_wddm_video_set_stream_auto_processing_mode) \
    X(pfnVideoProcessorSetStreamFilter, virtio_wddm_video_set_stream_filter) \
    X(pfnVideoProcessorSetStreamExtension, virtio_wddm_video_set_stream_extension) \
    X(pfnVideoProcessorGetStreamExtension, virtio_wddm_video_get_stream_extension) \
    X(pfnVideoProcessorBlt, virtio_wddm_video_video_processor_blt) \
    X(pfnCalcPrivateVideoDecoderOutputViewSize, virtio_wddm_video_calc_private_video_decoder_output_view_size) \
    X(pfnCreateVideoDecoderOutputView, virtio_wddm_video_create_video_decoder_output_view) \
    X(pfnDestroyVideoDecoderOutputView, virtio_wddm_video_destroy_video_decoder_output_view) \
    X(pfnCalcPrivateVideoProcessorInputViewSize, virtio_wddm_video_calc_private_video_processor_input_view_size) \
    X(pfnCreateVideoProcessorInputView, virtio_wddm_video_create_video_processor_input_view) \
    X(pfnDestroyVideoProcessorInputView, virtio_wddm_video_destroy_video_processor_input_view) \
    X(pfnCalcPrivateVideoProcessorOutputViewSize, virtio_wddm_video_calc_private_video_processor_output_view_size) \
    X(pfnCreateVideoProcessorOutputView, virtio_wddm_video_create_video_processor_output_view) \
    X(pfnDestroyVideoProcessorOutputView, virtio_wddm_video_destroy_video_processor_output_view) \
    X(pfnVideoProcessorInputViewReadAfterWriteHazard, virtio_wddm_video_video_processor_input_view_read_after_write_hazard) \
    X(pfnGetContentProtectionCaps, virtio_wddm_video_get_content_protection_caps) \
    X(pfnGetCryptoKeyExchangeType, virtio_wddm_video_get_crypto_key_exchange_type) \
    X(pfnCalcPrivateCryptoSessionSize, virtio_wddm_video_calc_private_crypto_session_size) \
    X(pfnCreateCryptoSession, virtio_wddm_video_create_crypto_session) \
    X(pfnDestroyCryptoSession, virtio_wddm_video_destroy_crypto_session) \
    X(pfnGetCertificateSize, virtio_wddm_video_get_certificate_size) \
    X(pfnGetCertificate, virtio_wddm_video_get_certificate) \
    X(pfnNegotiateCryptoSessionKeyExchange, virtio_wddm_video_negotiate_crypto_session_key_exchange) \
    X(pfnEncryptionBlt, virtio_wddm_video_encryption_blt) \
    X(pfnDecryptionBlt, virtio_wddm_video_decryption_blt) \
    X(pfnStartSessionKeyRefresh, virtio_wddm_video_start_session_key_refresh) \
    X(pfnFinishSessionKeyRefresh, virtio_wddm_video_finish_session_key_refresh) \
    X(pfnGetEncryptionBltKey, virtio_wddm_video_get_encryption_blt_key) \
    X(pfnCalcPrivateAuthenticatedChannelSize, virtio_wddm_video_calc_private_authenticated_channel_size) \
    X(pfnCreateAuthenticatedChannel, virtio_wddm_video_create_authenticated_channel) \
    X(pfnDestroyAuthenticatedChannel, virtio_wddm_video_destroy_authenticated_channel) \
    X(pfnNegotiateAuthenticatedChannelKeyExchange, virtio_wddm_video_negotiate_authenticated_channel_key_exchange) \
    X(pfnQueryAuthenticatedChannel, virtio_wddm_video_query_authenticated_channel) \
    X(pfnConfigureAuthenticatedChannel, virtio_wddm_video_configure_authenticated_channel) \
    X(pfnVideoDecoderGetHandle, virtio_wddm_video_video_decoder_get_handle) \
    X(pfnCryptoSessionGetHandle, virtio_wddm_video_crypto_session_get_handle) \
    X(pfnVideoProcessorSetStreamRotation, virtio_wddm_video_set_stream_rotation) \
    X(pfnGetCaptureHandle, virtio_wddm_video_get_capture_handle)

/* Catch-all for the entries WDDM2.0/2.1 append after the 11.1 block. Never
 * reads its arguments, so it is safe to place in slots whose exact prototype it
 * does not have: those are decoder sub-sample mapping, crypto-session status,
 * HDR metadata and mirroring, none of which M1 implements. */
typedef HRESULT (APIENTRY *VIRTIO_WDDM_UnimplementedFunc)(void);

static HRESULT APIENTRY virtio_wddm_video_unimplemented(void) {
    return E_NOTIMPL;
}

/* `table` is the runtime's copy of one of the video tables, `table_size` its
 * sizeof(). The 11.1 table is a prefix of the 2.0 and 2.1 tables (same members,
 * same order: d3d10umddi.h:3959-4039 vs 4299-4379 and 4435-...), so filling the
 * 11.1 view and stubbing the 2.0/2.1-only tail covers all three. */
static void fill_video_table(D3D11_1DDI_VIDEODEVICEFUNCS *table, SIZE_T table_size) {
    D3D11_1DDI_VIDEODEVICEFUNCS *view = table;
#define ASSIGN(name, func) view->name = func;
    VIRTIO_WDDM_VIDEO_FUNCS(ASSIGN)
#undef ASSIGN

    /* P2-2c self-check: prove what actually landed in the table. The runtime
     * dispatches by slot number, so log index + name + pointer for all 81
     * entries; slot[8] is pfnCreateVideoDecoder (the call P2-2b saw fail with
     * 0x8000FFFF) and slot[23] is pfnCreateVideoProcessor (the known-good
     * comparison). The WDDM2.0 tail is logged below with its absolute slot. */
    {
        unsigned slot = 0;
#define LOG_SLOT(name, func) \
        INFO("DDI-TABLE[P2-2C] slot[%02u] %-44s %p", slot++, #name, (void *)(func));
        VIRTIO_WDDM_VIDEO_FUNCS(LOG_SLOT)
#undef LOG_SLOT
        INFO("DDI-TABLE[P2-2C] filled table=%p size=%zu entries=%u", (void *)table,
             (size_t)table_size, slot);
        INFO("DDI-TABLE[P2-2C] check: CreateVideoDecoder[8]=%p "
             "CreateVideoProcessor[23]=%p GetVideoProcessorCaps[18]=%p",
             (void *)view->pfnCreateVideoDecoder,
             (void *)view->pfnCreateVideoProcessor,
             (void *)view->pfnGetVideoProcessorCaps);
    }

    const SIZE_T prefix = sizeof(D3D11_1DDI_VIDEODEVICEFUNCS);
    if (table_size <= prefix)
        return;

    const SIZE_T slots = (table_size - prefix) / sizeof(VIRTIO_WDDM_UnimplementedFunc);
    VIRTIO_WDDM_UnimplementedFunc *tail =
        (VIRTIO_WDDM_UnimplementedFunc *)((char *)table + prefix);
    for (SIZE_T i = 0; i < slots; i++)
        tail[i] = virtio_wddm_video_unimplemented;

    /* The 2.0/2.1 tail is all stubs except the decoder-capability query, which
     * P2-2a answers (the decoder profiles really are visible). The 2.1 struct
     * starts with the 2.0 members, so the 2.0 view is valid for both. */
    if (table_size >= sizeof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS)) {
        D3DWDDM2_0DDI_VIDEODEVICEFUNCS *view20 = (D3DWDDM2_0DDI_VIDEODEVICEFUNCS *)table;
        view20->pfnQueryVideoCapabilities = virtio_wddm_video_query_video_capabilities;
        INFO("DDI-TABLE[P2-2C] check: QueryVideoCapabilities(GetVideoDecoderCaps) "
             "tail[3]/abs[84]=%p", (void *)view20->pfnQueryVideoCapabilities);
    }
}

/* PFND3D10DDI_RETRIEVESUBOBJECT (d3d10umddi.h:7097). The runtime calls *this*,
 * not the other way round: CreateDevice's ppfnRetrieveSubObject is an output
 * slot the driver stores its callback in, and the call happens later, when an
 * application first asks for ID3D11VideoDevice. SubDeviceID identifies the table
 * version, so the ID the runtime passes is authoritative.
 *
 * Parameter shapes (d3d10umddi.h:4042 / 4397 / 4537): pParams is an input
 * struct { BOOL Relocate; VIDEODEVICEFUNCS *p...VideoDeviceFuncs; } whose
 * pointer member addresses the table the runtime allocated and wants filled
 * (Relocate is TRUE when the runtime moved that table and wants it rewritten).
 * The matching OUTPUT struct is a lone "UINT8 Reserved" placeholder, which is
 * why the table normally cannot travel through pOutputParamsBuffer; that buffer
 * is still honoured if a runtime passes one large enough, and both are filled
 * when both are usable. */
static HRESULT APIENTRY virtio_wddm_retrieve_subobject(
    D3D10DDI_HDEVICE hDevice,
    UINT32 SubDeviceID,
    SIZE_T ParamSize,
    void *pParams,
    SIZE_T OutputParamSize,
    void *pOutputParamsBuffer) {
    (void)hDevice;

    SIZE_T need;
    switch (SubDeviceID) {
    case D3D11_1DDI_VIDEO_FUNCTIONS:
        need = sizeof(D3D11_1DDI_VIDEODEVICEFUNCS);
        break;
    case D3DWDDM2_0DDI_VIDEO_FUNCTIONS:
        need = sizeof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS);
        break;
    case D3DWDDM2_1DDI_VIDEO_FUNCTIONS:
        need = sizeof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS);
        break;
    default:
        /* 6/7 are the WDDM2.4 tables, which M1 does not implement: fail the
         * request explicitly instead of handing back a table that would be read
         * through the wrong layout. */
        ERROR("%s: RetrieveSubObject(id=%u) is not a table we implement -> E_INVALIDARG",
              __FUNCTION__, SubDeviceID);
        return E_INVALIDARG;
    }

    /* The 2.0 / 2.1 inputs have the same two members in the same order as the
     * 11.1 one, so this view reads all three correctly. */
    D3D11_1DDI_VIDEO_INPUT *in = (D3D11_1DDI_VIDEO_INPUT *)pParams;
    D3D11_1DDI_VIDEODEVICEFUNCS *table = NULL;
    const char *via = "none";
    BOOL relocate = FALSE;

    if (pOutputParamsBuffer && OutputParamSize >= need) {
        fill_video_table((D3D11_1DDI_VIDEODEVICEFUNCS *)pOutputParamsBuffer, need);
        table = (D3D11_1DDI_VIDEODEVICEFUNCS *)pOutputParamsBuffer;
        via = "output";
    }
    if (in && ParamSize >= sizeof(*in) && in->p11VideoDeviceFuncs) {
        fill_video_table((D3D11_1DDI_VIDEODEVICEFUNCS *)in->p11VideoDeviceFuncs, need);
        if (table) {
            via = "output+input";
        } else {
            table = (D3D11_1DDI_VIDEODEVICEFUNCS *)in->p11VideoDeviceFuncs;
            via = "input";
        }
        relocate = in->Relocate;
    }

    if (!table) {
        ERROR("%s: RetrieveSubObject(id=%u) supplied no table buffer "
              "(params=%p/%zu, output=%p/%zu) -> E_INVALIDARG",
              __FUNCTION__, SubDeviceID, pParams, (size_t)ParamSize,
              pOutputParamsBuffer, (size_t)OutputParamSize);
        return E_INVALIDARG;
    }

    INFO("%s: RetrieveSubObject(id=%u) -> table=%p size=%zu via=%s relocate=%d",
         __FUNCTION__, SubDeviceID, (void *)table, (size_t)need, via, (int)relocate);
    INFO("%s: video DDI installed (build=HEVC-MAIN10-20261025/1.0.0.21, id=%u size=%zu): "
         "decoder=H.264 "
         "VLD NoFGT + VP9 VLD Profile0 + HEVC VLD Main + AV1 VLD Profile0 + HEVC VLD "
         "Main10 when a P010 surface exists / output NV12 or P010, Main10 = P010 only "
         "(1 config each, ConfigBitstreamRaw 2/1/1/1/1), "
         "processor=input NV12/YUY2 -> output BGRA8, blit=DXVK video processor "
         "(YCbCr->RGB + scale), EndFrame=direct-mapped media-pool frame "
         "(VM_IOCTL_MAP_FRAME, staging GET_FRAME fallback), VP9 boundary checks + "
         "paired MAP_FRAME/RELEASE_FRAME, AV1: coded S_FMT geometry + timeout is "
         "not a refusal + capture buffer requeued on every failed get + segmented "
         "host-wait/copy timing, HEVC Main10: VM_OPEN_CODEC_HEVC_MAIN10 (driver "
         "1.0.0.21 asks S_FMT CAPTURE for V4L2_PIX_FMT_P010, NV12 fallback) + "
         "10-bit SPS synthesis (profile_idc 2) + P010 two-plane upload",
         __FUNCTION__, SubDeviceID, (size_t)need);
    return S_OK;
}

void virtio_wddm_fill_video_funcs(D3D10DDIARG_CREATEDEVICE *pArgs) {
    INFO("P2-2C diag build: fill_video_funcs interface=0x%x fns: "
         "CreateVideoDecoder=%p CreateVideoProcessor=%p GetVideoDecoderCaps=%p",
         pArgs->Interface, (void *)virtio_wddm_video_create_video_decoder,
         (void *)virtio_wddm_video_create_video_processor,
         (void *)virtio_wddm_video_query_video_capabilities);
    switch (pArgs->Interface) {
    case D3D11_0_DDI_INTERFACE_VERSION:
        /* The video DDI arrived with 11.1; a 11.0 device has no video table. */
        return;
    case D3D11_1_DDI_INTERFACE_VERSION:
    case D3DWDDM1_3_DDI_INTERFACE_VERSION:
        /* D3DWDDM1.3 has no video table of its own in the WDK (there is no
         * D3DWDDM1_3DDI_VIDEODEVICEFUNCS), it reuses the 11.1 one. */
    case D3DWDDM2_0_DDI_INTERFACE_VERSION:
    case D3DWDDM2_1_DDI_INTERFACE_VERSION:
        break;
    default:
        /* 2.2 and later use their own video tables (sub-object 6/7) that M1
         * does not fill. Say so instead of registering a callback that could
         * only return E_INVALIDARG. */
        INFO("%s: interface 0x%x has no video table in M1 (skipped)",
             __FUNCTION__, pArgs->Interface);
        return;
    }

    /* CreateDevice only has to store the function: *ppfn is the runtime's
     * output slot and is NULL before the call. M1/M2 tested *ppfn here, which
     * therefore always bailed out -- the video table was never reachable. */
    if (!pArgs->ppfnRetrieveSubObject) {
        INFO("%s: runtime passed no ppfnRetrieveSubObject slot; video DDI unavailable",
             __FUNCTION__);
        return;
    }
    *pArgs->ppfnRetrieveSubObject = virtio_wddm_retrieve_subobject;
    INFO("%s: video RetrieveSubObject registered (fn=%p); tables for id=2/3/5",
         __FUNCTION__, (void *)virtio_wddm_retrieve_subobject);
}

/* droidvm_mft.h - shared declarations for the DroidVM virtio-media MFT.
 *
 * One source tree, two build modes (MFT_BUILD_MODE):
 *
 *   decoder  (default)  H.264 -> NV12   MFT_CATEGORY_VIDEO_DECODER
 *                       the working virtio-media decode path (P-BATCH-MFT)
 *   encoder             NV12  -> H.264  MFT_CATEGORY_VIDEO_ENCODER
 *                       IMFTransform + ICodecAPI *skeleton* (this batch); the
 *                       vmedia encoder data path is not wired yet
 *
 * Build examples (llvm-mingw clang):
 *   -DMFT_BUILD_MODE=decoder
 *   -DMFT_BUILD_MODE=encoder
 *   -DMFT_BUILD_MODE=0 / -DMFT_BUILD_MODE=1 are accepted too.
 *
 * Both modes register as *hardware async* MFTs:
 *   MFTFlags (registry)                     = MFT_ENUM_FLAG_HARDWARE (0x4)
 *   MF_TRANSFORM_ASYNC                      = 1
 *   MFT_ENUM_HARDWARE_VENDOR_ID_Attribute  = "VEN_1AF4"
 *   MFT_ENUM_HARDWARE_URL_Attribute        = mode specific
 * That combination is what makes MFTEnumEx(..., MFT_ENUM_FLAG_HARDWARE, ...)
 * return the transform.  See 04-测试记录/13-MFT.md in the VPU workspace.
 */
#ifndef DROIDVM_MFT_H
#define DROIDVM_MFT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

/* mingw-w64 only emits the IMFxxx_Method(This, ...) call macros when this is
 * defined before the Media Foundation headers.  Without it a C build has to
 * spell every call as This->lpVtbl->Method(...). */
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Build mode                                                                 */
/* ------------------------------------------------------------------------- */

#define MFT_BUILD_MODE_decoder 0
#define MFT_BUILD_MODE_encoder 1
#define MFT_BUILD_MODE_0       0
#define MFT_BUILD_MODE_1       1

#ifndef MFT_BUILD_MODE
#define MFT_BUILD_MODE decoder
#endif

#define DVM_MFT_CAT_(a, b) a##b
#define DVM_MFT_CAT(a, b)  DVM_MFT_CAT_(a, b)
#define DVM_MFT_MODE       DVM_MFT_CAT(MFT_BUILD_MODE_, MFT_BUILD_MODE)

#if (DVM_MFT_MODE != 0) && (DVM_MFT_MODE != 1)
#error "MFT_BUILD_MODE must be decoder, encoder, 0 or 1"
#endif

#if DVM_MFT_MODE == 0
/* {9F3C2A11-7D64-4E3B-9A21-6C0D5B48E7A2} */
extern const CLSID CLSID_DroidVmH264DecoderMft;
#define DVM_MFT_ACTIVE_CLSID CLSID_DroidVmH264DecoderMft
#define DVM_MFT_CATEGORY      MFT_CATEGORY_VIDEO_DECODER
#define DVM_MFT_FRIENDLY_NAME L"DroidVM virtio-media H.264 Decoder MFT"
#define DVM_MFT_HW_URL        L"DroidVM virtio-media H264 Decoder HW-MFT"
/* The decode itself runs on the host GPU/MediaCodec, so advertise the
 * hardware-decoder merit. */
#define DVM_MFT_MERIT 0x80000000u
#define DVM_MFT_MODENAME "decoder"
#else
/* {9F3C2A12-7D64-4E3B-9A21-6C0D5B48E7A3} - same prefix as the decoder CLSID,
 * one digit different, so the pair is easy to recognise in the registry. */
extern const CLSID CLSID_DroidVmH264EncoderMft;
#define DVM_MFT_ACTIVE_CLSID CLSID_DroidVmH264EncoderMft
#define DVM_MFT_CATEGORY      MFT_CATEGORY_VIDEO_ENCODER
#define DVM_MFT_FRIENDLY_NAME L"DroidVM virtio-media H.264 Encoder MFT"
#define DVM_MFT_HW_URL        L"DroidVM virtio-media H264 Encoder HW-MFT"
/* Qualcomm's hardware encoder MFTs register MFT_CODEC_MERIT_Attribute = 7
 * (see 05-参考文档/作者原始文档/evidence/gpudriverx2-video-20261001/
 * mft-attributes.json).  Encoder selection is normally explicit, so this
 * value is documentation rather than a ranking decision. */
#define DVM_MFT_MERIT 7u
#define DVM_MFT_MODENAME "encoder"
#endif

/* Vendor id used by the author's plan (QEMU/virtio PCI vendor 0x1AF4).  The
 * attribute is a string; "VEN_1AF4" mirrors Qualcomm's "VEN_QCOM". */
#define DVM_MFT_HW_VENDOR_ID L"VEN_1AF4"

/* The only MFT_ENUM_FLAG bit stored in the transform's MFTFlags value.  It
 * also matches every hardware MFT on a stock Windows 11 install (e.g. AMD's
 * "D3D11 Hardware MFT Playback Decoder" has MFTFlags=4).  A plain
 * MFTRegister(..., 0, ...) writes no MFTFlags at all and never shows up in a
 * MFT_ENUM_FLAG_HARDWARE enumeration - that was the P-BATCH bug. */
#define DVM_MFT_ENUM_FLAGS MFT_ENUM_FLAG_HARDWARE

#define DVM_MFT_VERSION L"1.0.1.0"

/* Event/log hook.  dvm_logf() is a no-op unless DROIDVM_MFT_LOG names a file
 * (same idea as the UMD's VIRTIO_WDDM_LOG_FILE, see AGENT-ENVIRONMENT 16). */
void dvm_logf(const char *fmt, ...);
void dvm_log_flush_close(void);

/* Creates the transform (called by the class factory).  Defined by
 * droidvm_mft_object.c (decoder build) or droidvm_mft_encoder.c (encoder
 * build). */
HRESULT dvm_mft_create_instance(REFIID riid, void **ppv);

/* Incremented/decremented by the class factory so DllCanUnloadNow is honest. */
void dvm_mft_addref_object(void);
void dvm_mft_release_object(void);

/* Registration (droidvm_mft_reg.c).  Writes the COM keys, calls MFTRegister,
 * forces the hardware MFTFlags/category/attributes and mirrors the x86 view
 * into WOW6432Node when the sibling x86 DLL is present. */
const CLSID *dvm_mft_clsid(void);
HRESULT dvm_mft_register(HINSTANCE inst);
HRESULT dvm_mft_unregister(void);

#ifdef __cplusplus
}
#endif

#endif /* DROIDVM_MFT_H */

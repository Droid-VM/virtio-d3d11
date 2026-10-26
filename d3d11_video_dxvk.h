#pragma once

/* M2: bridge between the D3D11 video DDI (d3d11_video.c) and the DXVK video
 * processor that this UMD already links into dx11um_virtio.dll.
 *
 * The UMD owns an internal DXVK ID3D11Device1 / ID3D11DeviceContext1
 * (VIRTIO_WDDM_Device::base.pDev1 / pCtx1, created in device.c). DXVK's
 * D3D11 implementation exposes ID3D11VideoDevice on that device and
 * ID3D11VideoContext on that context; the actual YUV->RGB conversion and
 * scaling are DXVK's video processor (src/d3d11/d3d11_video.cpp), which draws
 * a full-screen triangle with d3d11_video_blit_vert/frag into the output view
 * and scales through the viewport.
 *
 * d3d11_video.c is C and must not include the C++ D3D11 runtime headers that
 * DXVK's interfaces need; this header therefore speaks only plain Windows
 * types (windows.h) and passes every COM object as void *. The .cpp side does
 * the QueryInterface and the argument translation. That keeps the DDI
 * translation unit free of any d3d11.h <-> d3d10umddi.h ordering tricks.
 *
 * Lifetime: every object returned by a create_* call is owned by the caller
 * (one AddRef) and must be released with the matching destroy_* call. Objects
 * are plain DXVK COM pointers, so a 0/NULL pointer is always accepted. */

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC, field for field, so the C side does
 * not have to hand the bridge any DDI-specific type. Enum values (frame
 * format, usage) are passed through unchanged: D3D11_1DDI_VIDEO_FRAME_FORMAT
 * and D3D11_VIDEO_FRAME_FORMAT have identical numeric values, likewise
 * D3D11_1DDI_VIDEO_USAGE / D3D11_VIDEO_USAGE. */
typedef struct {
    UINT frame_format;
    UINT input_numerator;
    UINT input_denominator;
    UINT input_width;
    UINT input_height;
    UINT output_numerator;
    UINT output_denominator;
    UINT output_width;
    UINT output_height;
    UINT usage;
} VIRTIO_WDDM_VideoContentDesc;

/* One D3D11_VIDEO_PROCESSOR_STREAM with the fields this driver supports.
 * Deinterlacing (past/future frames) and stereo (right surface) are refused by
 * the DDI before it gets here, so there are no pointers for them. */
typedef struct {
    BOOL enable;
    UINT output_index;
    UINT input_frame_or_field;
    void *input_view; /* ID3D11VideoProcessorInputView *, or NULL */
} VIRTIO_WDDM_VideoStream;

/* Query the internal DXVK device/context for the video interfaces.
 * d3d11_device / d3d11_context are the UMD's base.pDev1 / base.pCtx1.
 * On success both outputs carry one reference each. */
HRESULT virtio_wddm_dxvk_video_get(void *d3d11_device, void *d3d11_context,
                                   void **video_device, void **video_context);
void virtio_wddm_dxvk_video_release(void *video_device, void *video_context);

/* Video processor enumerator (ID3D11VideoProcessorEnumerator). */
HRESULT virtio_wddm_dxvk_video_create_enum(void *video_device,
                                           const VIRTIO_WDDM_VideoContentDesc *desc,
                                           void **enumeration);
void virtio_wddm_dxvk_video_destroy_enum(void *enumeration);

/* Video processor (ID3D11VideoProcessor). */
HRESULT virtio_wddm_dxvk_video_create_processor(void *video_device, void *enumeration,
                                                UINT rate_conversion_index, void **processor);
void virtio_wddm_dxvk_video_destroy_processor(void *processor);

/* Video processor input view (ID3D11VideoProcessorInputView). `resource` is the
 * ID3D11Resource * of the DDI resource (VIRTIO_WDDM_Resource::base.pResource). */
HRESULT virtio_wddm_dxvk_video_create_input_view(void *video_device, void *resource,
                                                 void *enumeration, UINT four_cc,
                                                 UINT mip_slice, UINT first_array_slice,
                                                 void **view);
void virtio_wddm_dxvk_video_destroy_input_view(void *view);

/* Video processor output view (ID3D11VideoProcessorOutputView). */
HRESULT virtio_wddm_dxvk_video_create_output_view(void *video_device, void *resource,
                                                  void *enumeration, UINT mip_slice,
                                                  UINT first_array_slice, UINT array_size,
                                                  void **view);
void virtio_wddm_dxvk_video_destroy_output_view(void *view);

/* State forwarding. The DDI setters are all void, so these are void too; a
 * NULL context/processor is a no-op rather than a crash. `color_space_bits` is
 * the 32-bit D3D11_VIDEO_PROCESSOR_COLOR_SPACE value (same layout as
 * D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE). `color` is the four float
 * components of D3D11_VIDEO_COLOR (RGBA or YCbCrA). */
void virtio_wddm_dxvk_video_set_output_target_rect(void *video_context, void *processor,
                                                   BOOL enable, const RECT *rect);
void virtio_wddm_dxvk_video_set_output_background_color(void *video_context, void *processor,
                                                        BOOL ycbcr, const float color[4]);
void virtio_wddm_dxvk_video_set_output_color_space(void *video_context, void *processor,
                                                   UINT color_space_bits);
void virtio_wddm_dxvk_video_set_output_alpha_fill_mode(void *video_context, void *processor,
                                                       UINT mode, UINT stream_index);
void virtio_wddm_dxvk_video_set_output_constriction(void *video_context, void *processor,
                                                    BOOL enable, UINT width, UINT height);
void virtio_wddm_dxvk_video_set_output_stereo_mode(void *video_context, void *processor,
                                                   BOOL enable);

void virtio_wddm_dxvk_video_set_stream_frame_format(void *video_context, void *processor,
                                                    UINT stream_index, UINT format);
void virtio_wddm_dxvk_video_set_stream_color_space(void *video_context, void *processor,
                                                   UINT stream_index, UINT color_space_bits);
void virtio_wddm_dxvk_video_set_stream_output_rate(void *video_context, void *processor,
                                                   UINT stream_index, UINT rate, BOOL repeat,
                                                   UINT custom_numerator, UINT custom_denominator);
void virtio_wddm_dxvk_video_set_stream_source_rect(void *video_context, void *processor,
                                                   UINT stream_index, BOOL enable,
                                                   const RECT *rect);
void virtio_wddm_dxvk_video_set_stream_dest_rect(void *video_context, void *processor,
                                                 UINT stream_index, BOOL enable,
                                                 const RECT *rect);
void virtio_wddm_dxvk_video_set_stream_alpha(void *video_context, void *processor,
                                             UINT stream_index, BOOL enable, float alpha);
void virtio_wddm_dxvk_video_set_stream_palette(void *video_context, void *processor,
                                               UINT stream_index, UINT count,
                                               const UINT *entries);
void virtio_wddm_dxvk_video_set_stream_pixel_aspect_ratio(void *video_context, void *processor,
                                                          UINT stream_index, BOOL enable,
                                                          UINT source_numerator,
                                                          UINT source_denominator,
                                                          UINT destination_numerator,
                                                          UINT destination_denominator);
void virtio_wddm_dxvk_video_set_stream_luma_key(void *video_context, void *processor,
                                                UINT stream_index, BOOL enable,
                                                float lower, float upper);
void virtio_wddm_dxvk_video_set_stream_stereo_format(void *video_context, void *processor,
                                                     UINT stream_index, BOOL enable,
                                                     UINT format, BOOL left_view_frame0,
                                                     BOOL right_view_frame0, UINT flip_mode,
                                                     int mono_offset);
void virtio_wddm_dxvk_video_set_stream_auto_processing_mode(void *video_context, void *processor,
                                                             UINT stream_index, BOOL enable);
void virtio_wddm_dxvk_video_set_stream_filter(void *video_context, void *processor,
                                              UINT stream_index, UINT filter, BOOL enable,
                                              int value);
void virtio_wddm_dxvk_video_set_stream_rotation(void *video_context, void *processor,
                                                UINT stream_index, BOOL enable, UINT rotation);

/* The actual blit: queues DXVK's video blit (CSC + scaling) on the internal
 * immediate context. Returns S_OK once the work is queued; GPU completion is
 * ordered by the same DXVK command stream as the app's other D3D11 work, and
 * present waits on the existing D3D11 fence (see dxgi.c). */
HRESULT virtio_wddm_dxvk_video_blt(void *video_context, void *processor, void *output_view,
                                   UINT output_frame, UINT stream_count,
                                   const VIRTIO_WDDM_VideoStream *streams);

/* P2-2b: read a byte range out of the application's compressed-bitstream
 * buffer (SubmitDecoderBuffers gives us a D3D11 buffer resource plus
 * DataOffset/DataSize, not a CPU pointer). Copies through a temporary staging
 * buffer, so it also works when the app created the buffer as
 * D3D11_USAGE_DEFAULT, where ID3D11DeviceContext::Map is not allowed.
 * `resource` is the ID3D11Resource * of the DDI resource
 * (VIRTIO_WDDM_Resource::base.pResource). Returns E_INVALIDARG for a resource
 * that is not a buffer or a range that overflows. */
HRESULT virtio_wddm_dxvk_video_read_buffer(void *d3d11_device, void *d3d11_context,
                                           void *resource, UINT offset, UINT size,
                                           void *dst);

/* P-BATCH P010: write one decoded frame into the decoder output texture, in the
 * layout the texture declares. `fourcc` is the layout of `data`
 * (VM_FRAME_INFO::FourCC, i.e. V4L2's 'NV12' / 'P010') and must match the
 * texture's format: the DDI converts the frame before it gets here when the host
 * layout and the surface layout differ, so this call only stages pixels.
 *
 * The packed layout of both formats is the one vm_get_frame() returns: Y rows,
 * then the interleaved UV rows, `stride` bytes per row for both planes,
 * stride * height * 3 / 2 bytes total (for P010 a row is 2 bytes per sample,
 * width * 2 bytes, with the picture rows first and the chroma rows after
 * height * stride). DXVK's packImageData() walks the two planes in aspect order
 * and advances the source by height * SrcRowPitch between them, which is exactly
 * that split.
 *
 * The write goes through DXVK's ID3D11DeviceContext::UpdateSubresource, which
 * stages the pixels on the internal immediate context and copies them into the
 * texture's Vulkan image on the same queue as every other D3D11 command this
 * UMD issues. That keeps the frame ordered before the video-processor blit that
 * samples it without mapping the texture or taking an extra fence. Returns
 * E_INVALIDARG when the resource is not a 2D texture of `fourcc`'s format at
 * least width x height.
 *
 * P2-2j: `subresource` is D3D11CalcSubresource(MipSlice, ArraySlice, MipLevels)
 * of the decoder output view. A player allocates one texture with many array
 * slices (PotPlayer: 22) and selects a different slice per frame through
 * DecoderBeginFrame; writing slice 0 regardless leaves every other slice zeroed,
 * and a zeroed NV12 surface converts to a flat green (Y=0, U=V=0 -> RGB
 * 0,135,0). Passing the view's own subresource is therefore required. */
HRESULT virtio_wddm_dxvk_video_upload_frame(void *d3d11_context, void *resource,
                                            UINT subresource, const void *data,
                                            UINT width, UINT height, UINT stride,
                                            UINT fourcc);

/* P-BATCH P010: whether the internal DXVK device can create a 2D texture of
 * `format` (a DXGI_FORMAT). The DDI uses it to decide whether advertising P010
 * as a decode output surface is honest: DXVK maps DXGI_FORMAT_P010 to
 * VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 and reports
 * D3D11_FORMAT_SUPPORT_TEXTURE2D only when the Vulkan device supports it, so on
 * a host without that format every client keeps NV12 instead of failing at
 * CreateTexture2D. CPU-side format lookup only: no GPU work, no allocation. */
HRESULT virtio_wddm_dxvk_video_format_supported(void *d3d11_device, UINT format,
                                                BOOL *supported);

/* P2-2J diag v3: read one subresource back through a staging copy and report
 * plane means (NV12 -> Y/U/V, P010 -> the same three in 10-bit code values,
 * B8G8R8A8 -> R/G/B). Read-only; used to prove that the frame really landed in
 * the subresource the video processor samples, and what the blit wrote into the
 * render target. Never fails a frame. */
HRESULT virtio_wddm_dxvk_video_tex_stats(void *d3d11_device, void *d3d11_context,
                                         void *resource, UINT subresource,
                                         double *mean_a, double *mean_b, double *mean_c,
                                         UINT *samples);

#ifdef __cplusplus
}
#endif

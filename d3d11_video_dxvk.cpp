/* M2: D3D11 video DDI -> DXVK video processor bridge.
 *
 * Why this file exists: the UMD already links DXVK's D3D11 implementation and
 * creates an internal DXVK device (device.c: D3D11CreateDevice on the DXVK
 * adapter). DXVK's ID3D11VideoDevice / ID3D11VideoContext implement the whole
 * video processor, including the YUV->RGB conversion and the scaling blit
 * (thirdparty/dxvk/src/d3d11/d3d11_video.cpp + d3d11_video_blit_*.frag/vert).
 * The DDI in d3d11_video.c therefore does not reimplement any of that: it
 * translates the DDI structs into the runtime interfaces and calls them.
 *
 * The whole file is C++ only because the D3D11 runtime headers are. It does
 * not include any WDK/DDI header, so the two worlds cannot collide.
 *
 * Threading: DXVK's immediate context serialises its own command stream; the
 * DDI calls this from the runtime's D3D11 device threads, exactly like
 * resource.c already calls ID3D11Device1 / ID3D11DeviceContext1.
 *
 * Synchronisation: VideoProcessorBlt only queues work. It runs in the same
 * DXVK command stream as the app's other D3D11 commands, so no extra fence is
 * needed here; present signals device->base.pPresentFence on pCtx4, flushes and
 * waits for it (dxgi.c: virtio_wddm_sync_with_present_context), which covers
 * every earlier blit. No busy-wait is introduced by this file. */

#include <cstdint>

#include <d3d11_1.h>
#include <dxgi.h>

#include "d3d11_video_dxvk.h"

namespace {

constexpr UINT kMaxBlitStreams = 8;

static_assert(sizeof(D3D11_VIDEO_PROCESSOR_COLOR_SPACE) == sizeof(UINT),
              "color space is a 32-bit bitfield");
static_assert(sizeof(D3D11_VIDEO_COLOR) == 4 * sizeof(float),
              "D3D11_VIDEO_COLOR is four floats");

inline ID3D11VideoDevice *as_video_device(void *p) {
    return static_cast<ID3D11VideoDevice *>(p);
}

inline ID3D11VideoContext *as_video_context(void *p) {
    return static_cast<ID3D11VideoContext *>(p);
}

inline ID3D11VideoProcessorEnumerator *as_enum(void *p) {
    return static_cast<ID3D11VideoProcessorEnumerator *>(p);
}

inline ID3D11VideoProcessor *as_processor(void *p) {
    return static_cast<ID3D11VideoProcessor *>(p);
}

inline ID3D11VideoProcessorInputView *as_input_view(void *p) {
    return static_cast<ID3D11VideoProcessorInputView *>(p);
}

inline ID3D11VideoProcessorOutputView *as_output_view(void *p) {
    return static_cast<ID3D11VideoProcessorOutputView *>(p);
}

inline ID3D11Resource *as_resource(void *p) {
    return static_cast<ID3D11Resource *>(p);
}

inline D3D11_VIDEO_PROCESSOR_COLOR_SPACE color_space_from_bits(UINT bits) {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

inline D3D11_VIDEO_COLOR color_from_floats(const float color[4]) {
    D3D11_VIDEO_COLOR value = {};
    if (color)
        memcpy(&value, color, sizeof(value));
    return value;
}

inline D3D11_VIDEO_PROCESSOR_STREAM stream_from_ddi(const VIRTIO_WDDM_VideoStream &in) {
    D3D11_VIDEO_PROCESSOR_STREAM out = {};
    out.Enable = in.enable;
    out.OutputIndex = in.output_index;
    out.InputFrameOrField = in.input_frame_or_field;
    out.PastFrames = 0;
    out.FutureFrames = 0;
    out.pInputSurface = as_input_view(in.input_view);
    return out;
}

} /* namespace */

extern "C" HRESULT virtio_wddm_dxvk_video_get(void *d3d11_device, void *d3d11_context,
                                              void **video_device, void **video_context) {
    if (!d3d11_device || !d3d11_context || !video_device || !video_context)
        return E_INVALIDARG;

    *video_device = nullptr;
    *video_context = nullptr;

    ID3D11VideoDevice *device = nullptr;
    HRESULT hr = static_cast<IUnknown *>(d3d11_device)->QueryInterface(
        __uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&device));
    if (FAILED(hr))
        return hr;

    ID3D11VideoContext *context = nullptr;
    hr = static_cast<IUnknown *>(d3d11_context)->QueryInterface(
        __uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&context));
    if (FAILED(hr)) {
        device->Release();
        return hr;
    }

    *video_device = device;
    *video_context = context;
    return S_OK;
}

extern "C" void virtio_wddm_dxvk_video_release(void *video_device, void *video_context) {
    if (video_context)
        as_video_context(video_context)->Release();
    if (video_device)
        as_video_device(video_device)->Release();
}

extern "C" HRESULT virtio_wddm_dxvk_video_create_enum(
        void *video_device, const VIRTIO_WDDM_VideoContentDesc *desc, void **enumeration) {
    if (!video_device || !desc || !enumeration)
        return E_INVALIDARG;

    *enumeration = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = static_cast<D3D11_VIDEO_FRAME_FORMAT>(desc->frame_format);
    content.InputFrameRate.Numerator = desc->input_numerator;
    content.InputFrameRate.Denominator = desc->input_denominator;
    content.InputWidth = desc->input_width;
    content.InputHeight = desc->input_height;
    content.OutputFrameRate.Numerator = desc->output_numerator;
    content.OutputFrameRate.Denominator = desc->output_denominator;
    content.OutputWidth = desc->output_width;
    content.OutputHeight = desc->output_height;
    content.Usage = static_cast<D3D11_VIDEO_USAGE>(desc->usage);

    ID3D11VideoProcessorEnumerator *object = nullptr;
    HRESULT hr;
    try {
        hr = as_video_device(video_device)->CreateVideoProcessorEnumerator(&content, &object);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
        *enumeration = object;
    return hr;
}

extern "C" void virtio_wddm_dxvk_video_destroy_enum(void *enumeration) {
    if (enumeration)
        as_enum(enumeration)->Release();
}

extern "C" HRESULT virtio_wddm_dxvk_video_create_processor(void *video_device, void *enumeration,
                                                           UINT rate_conversion_index,
                                                           void **processor) {
    if (!video_device || !enumeration || !processor)
        return E_INVALIDARG;

    *processor = nullptr;

    ID3D11VideoProcessor *object = nullptr;
    HRESULT hr;
    try {
        hr = as_video_device(video_device)->CreateVideoProcessor(
            as_enum(enumeration), rate_conversion_index, &object);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
        *processor = object;
    return hr;
}

extern "C" void virtio_wddm_dxvk_video_destroy_processor(void *processor) {
    if (processor)
        as_processor(processor)->Release();
}

extern "C" HRESULT virtio_wddm_dxvk_video_create_input_view(
        void *video_device, void *resource, void *enumeration, UINT four_cc,
        UINT mip_slice, UINT first_array_slice, void **view) {
    if (!video_device || !resource || !enumeration || !view)
        return E_INVALIDARG;

    *view = nullptr;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
    desc.FourCC = four_cc;
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = mip_slice;
    desc.Texture2D.ArraySlice = first_array_slice;

    ID3D11VideoProcessorInputView *object = nullptr;
    HRESULT hr;
    try {
        hr = as_video_device(video_device)->CreateVideoProcessorInputView(
            as_resource(resource), as_enum(enumeration), &desc, &object);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
        *view = object;
    return hr;
}

extern "C" void virtio_wddm_dxvk_video_destroy_input_view(void *view) {
    if (view)
        as_input_view(view)->Release();
}

extern "C" HRESULT virtio_wddm_dxvk_video_create_output_view(
        void *video_device, void *resource, void *enumeration, UINT mip_slice,
        UINT first_array_slice, UINT array_size, void **view) {
    if (!video_device || !resource || !enumeration || !view)
        return E_INVALIDARG;

    *view = nullptr;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = {};
    if (array_size > 1) {
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipSlice = mip_slice;
        desc.Texture2DArray.FirstArraySlice = first_array_slice;
        desc.Texture2DArray.ArraySize = array_size;
    } else {
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = mip_slice;
    }

    ID3D11VideoProcessorOutputView *object = nullptr;
    HRESULT hr;
    try {
        hr = as_video_device(video_device)->CreateVideoProcessorOutputView(
            as_resource(resource), as_enum(enumeration), &desc, &object);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
        *view = object;
    return hr;
}

extern "C" void virtio_wddm_dxvk_video_destroy_output_view(void *view) {
    if (view)
        as_output_view(view)->Release();
}

extern "C" void virtio_wddm_dxvk_video_set_output_target_rect(
        void *video_context, void *processor, BOOL enable, const RECT *rect) {
    if (!video_context || !processor)
        return;
    RECT zero = {};
    as_video_context(video_context)->VideoProcessorSetOutputTargetRect(
        as_processor(processor), enable, rect ? rect : &zero);
}

extern "C" void virtio_wddm_dxvk_video_set_output_background_color(
        void *video_context, void *processor, BOOL ycbcr, const float color[4]) {
    if (!video_context || !processor)
        return;
    const D3D11_VIDEO_COLOR value = color_from_floats(color);
    as_video_context(video_context)->VideoProcessorSetOutputBackgroundColor(
        as_processor(processor), ycbcr, &value);
}

extern "C" void virtio_wddm_dxvk_video_set_output_color_space(
        void *video_context, void *processor, UINT color_space_bits) {
    if (!video_context || !processor)
        return;
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE value = color_space_from_bits(color_space_bits);
    as_video_context(video_context)->VideoProcessorSetOutputColorSpace(
        as_processor(processor), &value);
}

extern "C" void virtio_wddm_dxvk_video_set_output_alpha_fill_mode(
        void *video_context, void *processor, UINT mode, UINT stream_index) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetOutputAlphaFillMode(
        as_processor(processor), static_cast<D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE>(mode),
        stream_index);
}

extern "C" void virtio_wddm_dxvk_video_set_output_constriction(
        void *video_context, void *processor, BOOL enable, UINT width, UINT height) {
    if (!video_context || !processor)
        return;
    const SIZE size = { static_cast<LONG>(width), static_cast<LONG>(height) };
    as_video_context(video_context)->VideoProcessorSetOutputConstriction(
        as_processor(processor), enable, size);
}

extern "C" void virtio_wddm_dxvk_video_set_output_stereo_mode(
        void *video_context, void *processor, BOOL enable) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetOutputStereoMode(
        as_processor(processor), enable);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_frame_format(
        void *video_context, void *processor, UINT stream_index, UINT format) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamFrameFormat(
        as_processor(processor), stream_index,
        static_cast<D3D11_VIDEO_FRAME_FORMAT>(format));
}

extern "C" void virtio_wddm_dxvk_video_set_stream_color_space(
        void *video_context, void *processor, UINT stream_index, UINT color_space_bits) {
    if (!video_context || !processor)
        return;
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE value = color_space_from_bits(color_space_bits);
    as_video_context(video_context)->VideoProcessorSetStreamColorSpace(
        as_processor(processor), stream_index, &value);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_output_rate(
        void *video_context, void *processor, UINT stream_index, UINT rate, BOOL repeat,
        UINT custom_numerator, UINT custom_denominator) {
    if (!video_context || !processor)
        return;
    DXGI_RATIONAL custom = { custom_numerator, custom_denominator };
    as_video_context(video_context)->VideoProcessorSetStreamOutputRate(
        as_processor(processor), stream_index,
        static_cast<D3D11_VIDEO_PROCESSOR_OUTPUT_RATE>(rate), repeat,
        custom_numerator ? &custom : nullptr);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_source_rect(
        void *video_context, void *processor, UINT stream_index, BOOL enable,
        const RECT *rect) {
    if (!video_context || !processor)
        return;
    RECT zero = {};
    as_video_context(video_context)->VideoProcessorSetStreamSourceRect(
        as_processor(processor), stream_index, enable, rect ? rect : &zero);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_dest_rect(
        void *video_context, void *processor, UINT stream_index, BOOL enable,
        const RECT *rect) {
    if (!video_context || !processor)
        return;
    RECT zero = {};
    as_video_context(video_context)->VideoProcessorSetStreamDestRect(
        as_processor(processor), stream_index, enable, rect ? rect : &zero);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_alpha(
        void *video_context, void *processor, UINT stream_index, BOOL enable, float alpha) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamAlpha(
        as_processor(processor), stream_index, enable, alpha);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_palette(
        void *video_context, void *processor, UINT stream_index, UINT count,
        const UINT *entries) {
    if (!video_context || !processor || !entries)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamPalette(
        as_processor(processor), stream_index, count, entries);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_pixel_aspect_ratio(
        void *video_context, void *processor, UINT stream_index, BOOL enable,
        UINT source_numerator, UINT source_denominator,
        UINT destination_numerator, UINT destination_denominator) {
    if (!video_context || !processor)
        return;
    const DXGI_RATIONAL source = { source_numerator, source_denominator };
    const DXGI_RATIONAL destination = { destination_numerator, destination_denominator };
    as_video_context(video_context)->VideoProcessorSetStreamPixelAspectRatio(
        as_processor(processor), stream_index, enable, &source, &destination);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_luma_key(
        void *video_context, void *processor, UINT stream_index, BOOL enable,
        float lower, float upper) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamLumaKey(
        as_processor(processor), stream_index, enable, lower, upper);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_stereo_format(
        void *video_context, void *processor, UINT stream_index, BOOL enable, UINT format,
        BOOL left_view_frame0, BOOL right_view_frame0, UINT flip_mode, int mono_offset) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamStereoFormat(
        as_processor(processor), stream_index, enable,
        static_cast<D3D11_VIDEO_PROCESSOR_STEREO_FORMAT>(format), left_view_frame0,
        right_view_frame0, static_cast<D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE>(flip_mode),
        mono_offset);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_auto_processing_mode(
        void *video_context, void *processor, UINT stream_index, BOOL enable) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamAutoProcessingMode(
        as_processor(processor), stream_index, enable);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_filter(
        void *video_context, void *processor, UINT stream_index, UINT filter, BOOL enable,
        int value) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamFilter(
        as_processor(processor), stream_index,
        static_cast<D3D11_VIDEO_PROCESSOR_FILTER>(filter), enable, value);
}

extern "C" void virtio_wddm_dxvk_video_set_stream_rotation(
        void *video_context, void *processor, UINT stream_index, BOOL enable, UINT rotation) {
    if (!video_context || !processor)
        return;
    as_video_context(video_context)->VideoProcessorSetStreamRotation(
        as_processor(processor), stream_index, enable,
        static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(rotation));
}

extern "C" HRESULT virtio_wddm_dxvk_video_blt(void *video_context, void *processor,
                                              void *output_view, UINT output_frame,
                                              UINT stream_count,
                                              const VIRTIO_WDDM_VideoStream *streams) {
    if (!video_context || !processor || !output_view || !streams)
        return E_INVALIDARG;
    if (stream_count == 0 || stream_count > kMaxBlitStreams)
        return E_INVALIDARG;

    D3D11_VIDEO_PROCESSOR_STREAM translated[kMaxBlitStreams] = {};
    for (UINT i = 0; i < stream_count; i++)
        translated[i] = stream_from_ddi(streams[i]);

    /* DXVK's blit allocates its command objects on the fly and does not catch
     * anything itself. An exception must not unwind into the DDI/runtime, so
     * the failure is turned into an HRESULT here instead of taking the app
     * down. Resource errors (unsupported format, relocation failure) are
     * already reported by DXVK as an error and leave nothing to draw; that
     * path still returns S_OK from DXVK, which is its documented behaviour. */
    try {
        return as_video_context(video_context)->VideoProcessorBlt(
            as_processor(processor), as_output_view(output_view), output_frame,
            stream_count, translated);
    } catch (...) {
        return E_FAIL;
    }
}

/* P2-2b: copy a byte range out of the app's bitstream buffer. The DDI hands us
 * the resource and a byte offset, never a pointer, and the buffer is normally
 * D3D11_USAGE_DEFAULT (Map with D3D11_MAP_READ would be rejected), so the range
 * is copied to a throwaway staging buffer first and mapped from there. */
extern "C" HRESULT virtio_wddm_dxvk_video_read_buffer(void *d3d11_device,
                                                      void *d3d11_context,
                                                      void *resource, UINT offset,
                                                      UINT size, void *dst) {
    if (!d3d11_device || !d3d11_context || !resource || !dst || size == 0)
        return E_INVALIDARG;
    if (offset > 0xFFFFFFFFu - size)
        return E_INVALIDARG;

    ID3D11Resource *src = as_resource(resource);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    src->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
        return E_INVALIDARG;

    /* VP9FIX (2026-10-25): the DDI hands over a byte offset/size inside the
     * client's buffer, never a pointer.  CopySubresourceRegion() below reads
     * exactly that range, so it has to lie inside the resource: an offset/size
     * pair past ByteWidth makes the copy read heap memory after the buffer (and
     * the following memcpy() would then stage those bytes).  The runtime is
     * supposed to validate this, but a corrupted slice record or a bad
     * DataOffset must fail here with E_INVALIDARG instead of reaching DXVK. */
    {
        D3D11_BUFFER_DESC src_desc = {};
        static_cast<ID3D11Buffer *>(src)->GetDesc(&src_desc);
        if (offset > src_desc.ByteWidth || size > src_desc.ByteWidth - offset)
            return E_INVALIDARG;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Device *device = static_cast<ID3D11Device *>(d3d11_device);
    ID3D11DeviceContext *context = static_cast<ID3D11DeviceContext *>(d3d11_context);

    ID3D11Buffer *staging = nullptr;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &staging);
    if (FAILED(hr) || !staging)
        return FAILED(hr) ? hr : E_FAIL;

    /* For a buffer resource the source box is expressed in bytes: left..right is
     * the range, top/bottom and front/back are fixed at 0..1. */
    const D3D11_BOX box = { offset, 0, 0, offset + size, 1, 1 };
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, &box);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(dst, mapped.pData, size);
        context->Unmap(staging, 0);
    }
    staging->Release();
    return hr;
}

/* P-BATCH P010: the two packed 4:2:0 layouts the decode path moves between, as
 * V4L2 fourccs (VM_FRAME_INFO::FourCC and, for the texture, its DXGI_FORMAT). */
constexpr UINT kFourCCNv12 = 0x3231564Eu; /* 'NV12' */
constexpr UINT kFourCCP010 = 0x30313050u; /* 'P010' */

/* P-BATCH P010: can the internal DXVK device create a 2D texture of `format`?
 *
 * The DDI asks this before it advertises DXGI_FORMAT_P010 as a decode output
 * surface. DXVK's CheckFormatSupport maps the format through its DXGI table
 * (dxgi_format.cpp:816 -> VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16)
 * and sets D3D11_FORMAT_SUPPORT_TEXTURE2D only when the Vulkan device supports
 * sampled/storage 2D images of it, so the answer also covers the multiplane
 * view formats (d3d11_device.cpp:2337). Pure CPU-side lookup. */
extern "C" HRESULT virtio_wddm_dxvk_video_format_supported(void *d3d11_device, UINT format,
                                                          BOOL *supported) {
    if (!d3d11_device || !supported)
        return E_INVALIDARG;
    *supported = FALSE;

    UINT flags = 0;
    HRESULT hr = static_cast<ID3D11Device *>(d3d11_device)
                     ->CheckFormatSupport(static_cast<DXGI_FORMAT>(format), &flags);
    if (FAILED(hr))
        return hr;
    *supported = (flags & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? TRUE : FALSE;
    return S_OK;
}

/* P-BATCH P010: write one decoded frame into the decoder output texture, in the
 * layout the texture declares.
 *
 * `fourcc` is the layout of `data` and must equal the texture's format; the DDI
 * converts a frame whose host layout differs from the surface before calling
 * this (d3d11_video.c:decoder_convert_420), so only staging is left here. DXVK's
 * UpdateSubresource understands the multi-plane formats (it packs each plane
 * tight into its staging buffer and copies plane by plane) and uses the internal
 * context the UMD already owns, so the pixels land on the same Vulkan queue as
 * the blit that consumes them. A box is always passed: the output texture may be
 * larger than the decoded picture, and the default (whole-subresource) update
 * would read past the end of our buffer.
 *
 * The packed source layout is the same for both formats: 'stride' bytes per row
 * for the Y plane and for the interleaved UV plane, which starts at
 * stride * height. For P010 a row holds width * 2 bytes on both planes (the UV
 * row is width/2 sample pairs of 2 words each), which is why `row_bytes` below
 * doubles for P010.
 *
 * P2-2j: `subresource` comes from the decoder output view (MipSlice +
 * ArraySlice * MipLevels). A player's decode pool is one texture with many
 * array slices and DecoderBeginFrame picks a different slice per frame, so
 * hard-coding 0 wrote every frame into slice 0 and left the slice the renderer
 * actually sampled at zero -> Y=0, U=V=0 -> flat green (measured: 0,135,0). */
extern "C" HRESULT virtio_wddm_dxvk_video_upload_frame(void *d3d11_context, void *resource,
                                                       UINT subresource, const void *data,
                                                       UINT width, UINT height, UINT stride,
                                                       UINT fourcc) {
    if (!d3d11_context || !resource || !data || width == 0 || height == 0)
        return E_INVALIDARG;
    if (fourcc != kFourCCNv12 && fourcc != kFourCCP010)
        return E_INVALIDARG;
    /* 4:2:0 is 2x2-subsampled: odd dimensions have no valid UV plane. */
    if ((width & 1u) || (height & 1u))
        return E_INVALIDARG;

    const bool p010 = fourcc == kFourCCP010;
    const UINT row_bytes = p010 ? width * 2u : width;
    if (stride < row_bytes)
        return E_INVALIDARG;

    ID3D11Resource *dst = as_resource(resource);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    dst->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return E_INVALIDARG;

    ID3D11Texture2D *texture = static_cast<ID3D11Texture2D *>(dst);
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (desc.Format != (p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12) || width > desc.Width ||
        height > desc.Height)
        return E_INVALIDARG;

    /* The subresource must be a slice of this texture, otherwise DXVK would
     * silently write nothing. */
    if (subresource >= desc.MipLevels * desc.ArraySize)
        return E_INVALIDARG;

    const D3D11_BOX box = { 0, 0, 0, width, height, 1 };
    static_cast<ID3D11DeviceContext *>(d3d11_context)->UpdateSubresource(
        dst, subresource, &box, data, stride, stride * height);
    return S_OK;
}

/* P2-2J diag v3: read one subresource back and report plane means. Read-only:
 * a staging copy plus a Map, no state the app can observe. This is the only way
 * to answer two questions on the guest (no debugger there):
 *   1. did the decode upload actually land in the subresource the video-processor
 *      input view samples? (call it on the decoder texture after the upload)
 *   2. what did the blit actually write? (call it on the output view's texture)
 * NV12 -> mean_a/b/c = Y / U / V; P010 -> the same three as 10-bit code values
 * (word >> 6, i.e. 0..1023, so an 8-bit picture shows Y * 4); B8G8R8A8 ->
 * mean_a/b/c = R / G / B. */
extern "C" HRESULT virtio_wddm_dxvk_video_tex_stats(void *d3d11_device,
                                                    void *d3d11_context,
                                                    void *resource, UINT subresource,
                                                    double *mean_a, double *mean_b,
                                                    double *mean_c, UINT *samples) {
    if (!d3d11_device || !d3d11_context || !resource || !mean_a || !mean_b || !mean_c)
        return E_INVALIDARG;

    ID3D11Resource *src = as_resource(resource);
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    src->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return E_INVALIDARG;

    ID3D11Texture2D *texture = static_cast<ID3D11Texture2D *>(src);
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (subresource >= desc.MipLevels * desc.ArraySize)
        return E_INVALIDARG;
    if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010 &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ID3D11Device *device = static_cast<ID3D11Device *>(d3d11_device);
    ID3D11DeviceContext *context = static_cast<ID3D11DeviceContext *>(d3d11_context);

    ID3D11Texture2D *staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || !staging)
        return FAILED(hr) ? hr : E_FAIL;

    context->CopySubresourceRegion(staging, 0, 0, 0, 0, src, subresource, nullptr);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        staging->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    const BYTE *base = static_cast<const BYTE *>(mapped.pData);
    const UINT row_pitch = mapped.RowPitch;
    double sum_a = 0.0, sum_b = 0.0, sum_c = 0.0;
    unsigned long long count = 0;

    if (desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010) {
        /* P-BATCH P010: the same two planes, 1 or 2 bytes per sample. For P010
         * the mean is reported in 10-bit code values (the 16-bit word holds the
         * sample in bits 15:6, hence >> 6), so an 8-bit picture reads Y * 4. */
        const bool p010 = desc.Format == DXGI_FORMAT_P010;
        for (UINT y = 0; y < desc.Height; y++) {
            const BYTE *row = base + (size_t)y * row_pitch;
            for (UINT x = 0; x < desc.Width; x++) {
                if (p010)
                    sum_a += (double)(reinterpret_cast<const uint16_t *>(row)[x] >> 6);
                else
                    sum_a += row[x];
            }
        }
        count = (unsigned long long)desc.Width * desc.Height;
        sum_a /= (double)count;

        const BYTE *uv = base + (size_t)row_pitch * desc.Height;
        unsigned long long uv_count = 0;
        for (UINT y = 0; y + 1 < desc.Height; y += 2) {
            const BYTE *row = uv + (size_t)y * row_pitch;
            for (UINT x = 0; x + 1 < desc.Width; x += 2) {
                if (p010) {
                    const uint16_t *pair = reinterpret_cast<const uint16_t *>(row) + x;
                    sum_b += (double)(pair[0] >> 6);
                    sum_c += (double)(pair[1] >> 6);
                } else {
                    sum_b += row[x];
                    sum_c += row[x + 1];
                }
                uv_count++;
            }
        }
        if (uv_count) {
            sum_b /= (double)uv_count;
            sum_c /= (double)uv_count;
        }
    } else {
        for (UINT y = 0; y < desc.Height; y++) {
            const BYTE *row = base + (size_t)y * row_pitch;
            for (UINT x = 0; x < desc.Width; x++) {
                sum_c += row[(size_t)x * 4 + 0]; /* B */
                sum_b += row[(size_t)x * 4 + 1]; /* G */
                sum_a += row[(size_t)x * 4 + 2]; /* R */
            }
        }
        count = (unsigned long long)desc.Width * desc.Height;
        sum_a /= (double)count;
        sum_b /= (double)count;
        sum_c /= (double)count;
    }

    context->Unmap(staging, 0);
    staging->Release();

    *mean_a = sum_a;
    *mean_b = sum_b;
    *mean_c = sum_c;
    if (samples)
        *samples = (UINT)count;
    return S_OK;
}

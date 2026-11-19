/* C11 Media Foundation Sink Writer integration test; no project changes needed.
 *
 * Build on the build server, NOT on the development workstation. Examples:
 *   cl /nologo /TC /W4 mft_sink_writer.c /Fe:mft_sink_writer.exe \
 *      /link mfreadwrite.lib mfplat.lib mfuuid.lib ole32.lib
 *   aarch64-w64-mingw32-clang -std=c11 -Wall -Wextra -municode \
 *      mft_sink_writer.c -o mft_sink_writer.exe \
 *      -lmfreadwrite -lmfplat -lmfuuid -lole32
 * Use a Windows 8+ SDK (IMFSinkWriterEx), and match the registered COM server's
 * architecture (normally ARM64). No direct dependency on the DroidVM DLL.
 *
 * Guest invocation (not run automatically):
 *   mft_sink_writer.exe --probe-only
 *   mft_sink_writer.exe result.mp4
 *   mft_sink_writer.exe --prefer-local result.mp4
 * Output must not already exist. Failed runs may leave an incomplete MP4.
 * Exit: 0 = requested gate passed, 1 = API/encoding failure, 2 = usage,
 *       3 = actual encoder identity unproven/wrong (never a software PASS).
 *
 * API limits:
 * - MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS permits hardware; it does NOT ban
 *   software fallback or select a CLSID. Only actual-instance checks count.
 * - MFTRegisterLocalByCLSID is process scoped, requires existing COM class
 *   registration and supplies local enumeration preference, not an absolute
 *   Sink Writer selection guarantee. No global registration/merit is changed.
 * - IMFSinkWriterEx is Windows 8+. GetServiceForStream(GUID_NULL, IMFTransform)
 *   is a fallback; it can fail or return a sink stream interface. Require the
 *   NV12 -> H264 types AND exact DroidVM instance identity in that path.
 * - Some MFTs do not expose identity attributes. Such runs are UNPROVEN even
 *   when writing/Finalize would succeed. Enumeration/activation alone is not
 *   evidence of the encoder used by a Sink Writer.
 * - PASS proves 120 accepted input samples, exact rational input times, the
 *   selected encoder, successful Finalize and a nonempty output. Independently
 *   inspect/decode the MP4 to validate output packet count, PTS and pixels.
 * - No asynchronous callback is installed: WriteSample/Finalize are synchronous
 *   and can block in a broken driver. Use an external guest test timeout.
 *
 * Docs: https://learn.microsoft.com/windows/win32/api/mfreadwrite/
 *   nf-mfreadwrite-imfsinkwriterex-gettransformforstream
 *   nf-mfreadwrite-imfsinkwriter-getserviceforstream
 * https://learn.microsoft.com/windows/win32/api/mfapi/
 *   nf-mfapi-mftregisterlocalbyclsid
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <wchar.h>

#define WIDTH 640u
#define HEIGHT 480u
#define FRAME_COUNT 120u
#define FPS_NUM 30u
#define FPS_DEN 1u
#define NV12_BYTES (WIDTH * HEIGHT * 3u / 2u)
#define PACK(a,b) (((UINT64)(a) << 32) | (UINT64)(b))
#define CHECK(call) do { hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "%s failed: 0x%08lX\n", #call, (unsigned long)hr); \
    goto done; } } while (0)

static const CLSID target_clsid = {
    0x9f3c2a12, 0x7d64, 0x4e3b, {0x9a,0x21,0x6c,0x0d,0x5b,0x48,0xe7,0xa3}
};
static const WCHAR target_name[] = L"DroidVM virtio-media H.264 Encoder MFT";

static void print_guid(const char *label, const GUID *guid)
{
    WCHAR text[40];
    if (StringFromGUID2(guid, text, (int)(sizeof(text)/sizeof(text[0]))))
        printf("%s%ls\n", label, text);
}

/* A conflicting CLSID overrides a matching name. Name-only evidence is
 * accepted only when the CLSID is unavailable, and explicitly labeled. */
static int identity(IMFAttributes *attrs, const char *source)
{
    GUID clsid;
    WCHAR *name = NULL;
    UINT32 length = 0;
    HRESULT hc, hn;
    int match = 0;
    hc = IMFAttributes_GetGUID(attrs, &MFT_TRANSFORM_CLSID_Attribute, &clsid);
    hn = IMFAttributes_GetAllocatedString(attrs, &MFT_FRIENDLY_NAME_Attribute,
                                         &name, &length);
    printf("IDENTITY source=%s\n", source);
    if (SUCCEEDED(hc)) {
        print_guid("  CLSID=", &clsid);
        match = IsEqualGUID(&clsid, &target_clsid);
    } else {
        printf("  CLSID unavailable: 0x%08lX\n", (unsigned long)hc);
        if (SUCCEEDED(hn)) match = wcscmp(name, target_name) == 0;
    }
    if (SUCCEEDED(hn)) printf("  name=%ls\n", name);
    else printf("  name unavailable: 0x%08lX\n", (unsigned long)hn);
    if (match) printf("  DROIDVM_PROVEN via %s\n",
                      SUCCEEDED(hc) ? "CLSID" : "exact friendly name only");
    if (name) CoTaskMemFree(name);
    return match;
}

static int transform_identity(IMFTransform *mft, const char *source)
{
    IMFAttributes *attrs = NULL;
    HRESULT hr = IMFTransform_GetAttributes(mft, &attrs);
    int match = 0;
    if (SUCCEEDED(hr)) {
        match = identity(attrs, source);
        IMFAttributes_Release(attrs);
    } else {
        printf("IDENTITY source=%s attributes unavailable: 0x%08lX\n",
               source, (unsigned long)hr);
    }
    return match;
}

/* GetServiceForStream can fall through to the sink: verify encoder types. */
static int encoder_types(IMFTransform *mft)
{
    IMFMediaType *in = NULL, *out = NULL;
    GUID a, b, major_in, major_out;
    DWORD ni = 0, no = 0, ii = 0, oi = 0;
    HRESULT hr = IMFTransform_GetStreamCount(mft, &ni, &no);
    int match = 0;
    if (FAILED(hr) || ni != 1 || no != 1) return 0;
    hr = IMFTransform_GetStreamIDs(mft, 1, &ii, 1, &oi);
    if (FAILED(hr) && hr != E_NOTIMPL) return 0;
    hr = IMFTransform_GetInputCurrentType(mft, ii, &in);
    if (SUCCEEDED(hr)) hr = IMFTransform_GetOutputCurrentType(mft, oi, &out);
    if (SUCCEEDED(hr) &&
        SUCCEEDED(IMFMediaType_GetGUID(in, &MF_MT_MAJOR_TYPE, &major_in)) &&
        SUCCEEDED(IMFMediaType_GetGUID(out, &MF_MT_MAJOR_TYPE, &major_out)) &&
        SUCCEEDED(IMFMediaType_GetGUID(in, &MF_MT_SUBTYPE, &a)) &&
        SUCCEEDED(IMFMediaType_GetGUID(out, &MF_MT_SUBTYPE, &b))) {
        match = IsEqualGUID(&major_in, &MFMediaType_Video) &&
                IsEqualGUID(&major_out, &MFMediaType_Video) &&
                IsEqualGUID(&a, &MFVideoFormat_NV12) &&
                IsEqualGUID(&b, &MFVideoFormat_H264);
    }
    if (in) IMFMediaType_Release(in);
    if (out) IMFMediaType_Release(out);
    return match;
}

static int prove_writer_encoder(IMFSinkWriter *writer, DWORD stream)
{
    IMFSinkWriterEx *ex = NULL;
    IMFTransform *mft = NULL;
    HRESULT hr;
    DWORD i;
    int proven = 0;
    hr = IMFSinkWriter_QueryInterface(writer, &IID_IMFSinkWriterEx, (void **)&ex);
    if (SUCCEEDED(hr)) {
        /* Bound enumeration defensively; a valid writer has very few MFTs. */
        for (i = 0; i < 32; ++i) {
            GUID category;
            hr = IMFSinkWriterEx_GetTransformForStream(ex, stream, i,
                                                       &category, &mft);
            if (FAILED(hr)) {
                printf("GetTransformForStream index=%lu: 0x%08lX\n",
                       (unsigned long)i, (unsigned long)hr);
                break;
            }
            printf("WRITER transform index=%lu\n", (unsigned long)i);
            print_guid("  category=", &category);
            if (IsEqualGUID(&category, &MFT_CATEGORY_VIDEO_ENCODER))
                proven |= transform_identity(mft, "SinkWriterEx encoder");
            IMFTransform_Release(mft);
            mft = NULL;
        }
        IMFSinkWriterEx_Release(ex);
    } else {
        printf("IMFSinkWriterEx unavailable: 0x%08lX\n", (unsigned long)hr);
    }
    if (proven) return 1;
    hr = IMFSinkWriter_GetServiceForStream(writer, stream, &GUID_NULL,
                                          &IID_IMFTransform, (void **)&mft);
    if (SUCCEEDED(hr)) {
        if (encoder_types(mft))
            proven = transform_identity(mft, "GetServiceForStream encoder");
        else printf("GetServiceForStream returned no NV12->H264 encoder\n");
        IMFTransform_Release(mft);
    } else {
        printf("GetServiceForStream IMFTransform: 0x%08lX\n", (unsigned long)hr);
    }
    return proven;
}

static HRESULT make_type(IMFMediaType **type, const GUID *subtype)
{
    HRESULT hr = MFCreateMediaType(type);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(*type, &MF_MT_MAJOR_TYPE,
                                                &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(*type, &MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(*type, &MF_MT_FRAME_SIZE,
                                                  PACK(WIDTH, HEIGHT));
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(*type, &MF_MT_FRAME_RATE,
                                                  PACK(FPS_NUM, FPS_DEN));
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(*type, &MF_MT_PIXEL_ASPECT_RATIO,
                                                  PACK(1, 1));
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(*type, &MF_MT_INTERLACE_MODE,
                                                  MFVideoInterlace_Progressive);
    return hr;
}

/* Compute every boundary independently: durations alternate 333333/333334,
 * with no accumulated truncation. Frame 120 ends at exactly 40000000 ticks. */
static LONGLONG frame_time(UINT32 index)
{
    return (LONGLONG)(((UINT64)index * 10000000u * FPS_DEN) / FPS_NUM);
}

static HRESULT write_frame(IMFSinkWriter *writer, DWORD stream, UINT32 index)
{
    IMFSample *sample = NULL;
    IMFMediaBuffer *buffer = NULL;
    BYTE *pixels = NULL;
    DWORD capacity = 0, length = 0;
    UINT32 x, y;
    HRESULT hr;
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(NV12_BYTES, &buffer);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buffer, &pixels, &capacity, &length);
    if (SUCCEEDED(hr)) {
        for (y = 0; y < HEIGHT; ++y)
            for (x = 0; x < WIDTH; ++x)
                pixels[y * WIDTH + x] = (BYTE)(16u + (x*3u + y*5u + index*7u)%220u);
        for (y = 0; y < HEIGHT/2u; ++y) {
            for (x = 0; x < WIDTH; x += 2) {
                pixels[WIDTH*HEIGHT + y*WIDTH + x] = (BYTE)(64u + (x/2u + index*3u)%128u);
                pixels[WIDTH*HEIGHT + y*WIDTH + x + 1] = (BYTE)(64u + (y + index*5u)%128u);
            }
        }
        hr = IMFMediaBuffer_Unlock(buffer);
    }
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_SetCurrentLength(buffer, NV12_BYTES);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(sample, buffer);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(sample, frame_time(index));
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(sample,
                                           frame_time(index+1u)-frame_time(index));
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(writer, stream, sample);
    if (buffer) IMFMediaBuffer_Release(buffer);
    if (sample) IMFSample_Release(sample);
    return hr;
}

/* This is a separate gate; it does not open a driver session or encode frames. */
static int probe_hardware(void)
{
    MFT_REGISTER_TYPE_INFO input = {0}, output = {0};
    IMFActivate **list = NULL;
    UINT32 count = 0, i;
    HRESULT hr;
    int activated = 0;
    input.guidMajorType = output.guidMajorType = MFMediaType_Video;
    input.guidSubtype = MFVideoFormat_NV12;
    output.guidSubtype = MFVideoFormat_H264;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &input, &output, &list, &count);
    if (FAILED(hr)) {
        fprintf(stderr, "MFTEnumEx hardware failed: 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    printf("HARDWARE_ENUM NV12->H264 count=%u\n", (unsigned)count);
    for (i = 0; i < count; ++i) {
        GUID clsid;
        IMFTransform *mft = NULL;
        printf("HARDWARE candidate=%u\n", (unsigned)i);
        (void)identity((IMFAttributes *)list[i], "hardware activation metadata");
        hr = IMFActivate_GetGUID(list[i], &MFT_TRANSFORM_CLSID_Attribute, &clsid);
        if (SUCCEEDED(hr) && IsEqualGUID(&clsid, &target_clsid)) {
            hr = IMFActivate_ActivateObject(list[i], &IID_IMFTransform, (void **)&mft);
            printf("TARGET ActivateObject: 0x%08lX\n", (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                if (transform_identity(mft, "probe activated target")) activated = 1;
                IMFTransform_Release(mft);
                hr = IMFActivate_ShutdownObject(list[i]);
                if (FAILED(hr)) {
                    printf("TARGET ShutdownObject: 0x%08lX\n", (unsigned long)hr);
                    activated = 0;
                }
            }
        }
        IMFActivate_Release(list[i]);
    }
    CoTaskMemFree(list);
    printf("%s PROBE_ONLY: hardware enumeration + target activation; "
           "Sink Writer/MP4 NOT tested\n", activated ? "PASS" : "FAIL");
    return activated ? 0 : 1;
}

int wmain(int argc, WCHAR **argv)
{
    HRESULT hr = S_OK;
    IMFAttributes *attrs = NULL;
    IMFMediaType *input = NULL, *output = NULL;
    IMFSinkWriter *writer = NULL;
    MFT_REGISTER_TYPE_INFO local_in = {0}, local_out = {0};
    const WCHAR *path = NULL;
    WIN32_FILE_ATTRIBUTE_DATA file;
    DWORD stream = 0;
    UINT32 i;
    int rc = 1, co_started = 0, mf_started = 0, local = 0;
    int prefer_local = 0, probe = 0;
    if (argc == 2 && wcscmp(argv[1], L"--probe-only") == 0) probe = 1;
    else if (argc == 2 && argv[1][0] != L'-') path = argv[1];
    else if (argc == 3 && wcscmp(argv[1], L"--prefer-local") == 0) {
        prefer_local = 1;
        path = argv[2];
    } else {
        fprintf(stderr, "usage: mft_sink_writer.exe [--prefer-local] new-output.mp4\n"
                        "       mft_sink_writer.exe --probe-only\n");
        return 2;
    }
    if (path) {
        size_t n = wcslen(path);
        DWORD error;
        if (n < 4 || _wcsicmp(path+n-4, L".mp4") != 0) {
            fprintf(stderr, "Output must have .mp4 extension\n");
            return 2;
        }
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "Refusing existing output\n");
            return 2;
        }
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "Cannot inspect output path: %lu\n", (unsigned long)error);
            return 1;
        }
    }
    CHECK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    co_started = 1;
    CHECK(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    mf_started = 1;
    if (probe) { rc = probe_hardware(); goto done; }
    if (prefer_local) {
        local_in.guidMajorType = local_out.guidMajorType = MFMediaType_Video;
        local_in.guidSubtype = MFVideoFormat_NV12;
        local_out.guidSubtype = MFVideoFormat_H264;
        CHECK(MFTRegisterLocalByCLSID(&target_clsid, &MFT_CATEGORY_VIDEO_ENCODER,
                  target_name, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT,
                  1, &local_in, 1, &local_out));
        local = 1;
        printf("LOCAL_PREFERENCE registered for this process only; selection still verified\n");
    }
    CHECK(MFCreateAttributes(&attrs, 2));
    CHECK(IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1));
    CHECK(MFCreateSinkWriterFromURL(path, NULL, attrs, &writer));
    CHECK(make_type(&output, &MFVideoFormat_H264));
    CHECK(IMFMediaType_SetUINT32(output, &MF_MT_AVG_BITRATE, 2000000));
    CHECK(IMFMediaType_SetUINT32(output, &MF_MT_MPEG2_PROFILE, 100));
    CHECK(IMFSinkWriter_AddStream(writer, output, &stream));
    CHECK(make_type(&input, &MFVideoFormat_NV12));
    CHECK(IMFMediaType_SetUINT32(input, &MF_MT_DEFAULT_STRIDE, WIDTH));
    CHECK(IMFMediaType_SetUINT32(input, &MF_MT_SAMPLE_SIZE, NV12_BYTES));
    CHECK(IMFMediaType_SetUINT32(input, &MF_MT_FIXED_SIZE_SAMPLES, 1));
    CHECK(IMFMediaType_SetUINT32(input, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1));
    CHECK(IMFSinkWriter_SetInputMediaType(writer, stream, input, NULL));
    CHECK(IMFSinkWriter_BeginWriting(writer));
    if (!prove_writer_encoder(writer, stream)) {
        fprintf(stderr, "UNPROVEN_OR_WRONG_ENCODER: refusing to count software fallback as success\n");
        rc = 3;
        goto done;
    }
    printf("START NV12 %ux%u -> H264 MP4 frames=%u fps=%u/%u\n",
           WIDTH, HEIGHT, FRAME_COUNT, FPS_NUM, FPS_DEN);
    for (i = 0; i < FRAME_COUNT; ++i) {
        CHECK(write_frame(writer, stream, i));
        printf("INPUT index=%u pts100ns=%lld duration100ns=%lld\n", (unsigned)i,
               (long long)frame_time(i), (long long)(frame_time(i+1u)-frame_time(i)));
        fflush(stdout);
    }
    /* Check the actual pipeline again after all samples, before Finalize can
     * release internal transforms. Finalize is required even if this fails. */
    rc = prove_writer_encoder(writer, stream) ? 0 : 3;
    hr = IMFSinkWriter_Finalize(writer);
    if (FAILED(hr)) {
        fprintf(stderr, "Finalize failed: 0x%08lX\n", (unsigned long)hr);
        rc = 1;
        goto done;
    }
    printf("FINALIZE OK\n");
    IMFSinkWriter_Release(writer);
    writer = NULL;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &file) ||
        (file.nFileSizeHigh == 0 && file.nFileSizeLow == 0)) {
        fprintf(stderr, "Output absent or empty after Finalize\n");
        rc = 1;
        goto done;
    }
    if (rc == 0)
        printf("PASS SINK_WRITER_DROIDVM frames=%u end100ns=%lld Finalize=OK output=%ls\n",
               FRAME_COUNT, (long long)frame_time(FRAME_COUNT), path);
    else fprintf(stderr, "UNPROVEN_OR_WRONG_ENCODER after WriteSample; MP4 is not a PASS\n");
done:
    if (writer) IMFSinkWriter_Release(writer);
    if (input) IMFMediaType_Release(input);
    if (output) IMFMediaType_Release(output);
    if (attrs) IMFAttributes_Release(attrs);
    if (local) {
        hr = MFTUnregisterLocalByCLSID(target_clsid);
        if (FAILED(hr)) {
            fprintf(stderr, "MFTUnregisterLocalByCLSID failed: 0x%08lX\n", (unsigned long)hr);
            rc = 1;
        }
    }
    if (mf_started) {
        hr = MFShutdown();
        if (FAILED(hr)) {
            fprintf(stderr, "MFShutdown failed: 0x%08lX\n", (unsigned long)hr);
            rc = 1;
        }
    }
    if (co_started) CoUninitialize();
    return rc;
}

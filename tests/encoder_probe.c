#include <stdint.h>
#include "vmedia_abi.h"
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef void *HANDLE;
typedef uint32_t DWORD;
typedef int32_t BOOL;
typedef void *LPSECURITY_ATTRIBUTES;
typedef struct _OVERLAPPED OVERLAPPED;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define OPEN_EXISTING 3u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
__declspec(dllimport) HANDLE __stdcall CreateFileW(const uint16_t *, DWORD, DWORD,
    LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL __stdcall DeviceIoControl(HANDLE, DWORD, void *, DWORD,
    void *, DWORD, DWORD *, OVERLAPPED *);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, OVERLAPPED *);
__declspec(dllimport) BOOL __stdcall FlushFileBuffers(HANDLE);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, DWORD, DWORD);
__declspec(dllimport) BOOL __stdcall VirtualFree(void *, size_t, DWORD);
__declspec(dllimport) void __stdcall OutputDebugStringA(const char *);
__declspec(dllimport) DWORD __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall ExitProcess(uint32_t);
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_RELEASE 0x00008000u
#define PAGE_READWRITE 0x00000004u
static const uint16_t ENCODER_LINK[] = {
    '\\', '\\', '.', '\\', 'V','M','e','d','i','a','E','n','c','o','d','e','r', 0
};

#ifndef ENCODER_PROBE_WIDTH
#define ENCODER_PROBE_WIDTH 64u
#endif
#ifndef ENCODER_PROBE_HEIGHT
#define ENCODER_PROBE_HEIGHT 48u
#endif
#define WIDTH ENCODER_PROBE_WIDTH
#define HEIGHT ENCODER_PROBE_HEIGHT
#define MAX_STRIDE (WIDTH * 2u)
#define NV12_MAX_BYTES (MAX_STRIDE * HEIGHT * 3u / 2u)
#define PACKET_CAPACITY (1024u * 1024u)
#define PACKET_WAIT_MS 3000u
#define INPUT_FRAMES 4u
#define MIN_PACKETS 2u

typedef struct {
    VM_ENCODER_PACKET_INFO info;
    uint8_t bytes[PACKET_CAPACITY];
} PACKET_BUFFER;

typedef struct {
    uint32_t index;
    uint32_t bytes_used;
    uint32_t flags;
    uint32_t reserved;
    uint64_t timestamp_100ns;
} PACKET_META;

typedef struct {
    uint32_t step;
    uint32_t status;
    uint32_t returned_bytes;
    uint32_t frame_index;
} PROBE_LOG_RECORD;

static void zero_bytes(void *p, size_t n);

static void make_nv12(uint8_t *p, uint32_t size, uint32_t y_stride,
                      uint32_t uv_stride, uint32_t uv_offset, unsigned frame)
{
    unsigned x, y;
    zero_bytes(p, size);
    for (y = 0; y < HEIGHT; ++y) {
        for (x = 0; x < WIDTH; ++x) {
            p[y * y_stride + x] = (uint8_t)((x * 3u + y * 5u + frame * 37u) & 255u);
        }
    }
    for (y = 0; y < HEIGHT / 2; ++y) {
        for (x = 0; x < WIDTH; x += 2) {
            p[uv_offset + y * uv_stride + x] = (uint8_t)(96u + frame * 17u);
            p[uv_offset + y * uv_stride + x + 1] = (uint8_t)(160u - frame * 13u);
        }
    }
}

static void zero_bytes(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    size_t i;
    for (i = 0; i < n; ++i) b[i] = 0;
}

static DWORD g_last_error;
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void log_step(uint32_t step, uint32_t status, uint32_t returned, uint32_t frame)
{
    PROBE_LOG_RECORD r;
    DWORD written = 0;
    r.step = step; r.status = status; r.returned_bytes = returned; r.frame_index = frame;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, &r, sizeof(r), &written, NULL);
}

static int ioctl_ok(HANDLE h, DWORD code, void *in, DWORD in_len,
                    void *out, DWORD out_len, DWORD *returned, const char *name)
{
    if (!DeviceIoControl(h, code, in, in_len, out, out_len, returned, NULL)) {
        g_last_error = GetLastError();
        log_step(code, g_last_error, returned ? *returned : 0, 0);
        (void)name;
        OutputDebugStringA("encoder probe: DeviceIoControl failed\n");
        return 0;
    }
    log_step(code, 0, returned ? *returned : 0, 0);
    return 1;
}

static int probe_run(void)
{
    static const uint16_t output_path[] = {'e','n','c','o','d','e','r','-','p','r','o','b','e','.','h','2','6','4',0};
    static const uint16_t metadata_path[] = {'e','n','c','o','d','e','r','-','p','r','o','b','e','.','m','e','t','a',0};
    static const uint16_t log_path[] = {'e','n','c','o','d','e','r','-','p','r','o','b','e','.','l','o','g',0};
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    HANDLE metadata = INVALID_HANDLE_VALUE;
    uint8_t *frame = NULL;
    uint8_t *input_request = NULL;
    PACKET_BUFFER *packet = NULL;
    VM_OPEN_IN open_in = { 1u, VM_OPEN_CODEC_H264 };
    VM_OPEN_OUT open_out;
    VM_ENCODER_CONFIG config;
    DWORD bytes = 0;
    uint32_t payload_size = 0;
    unsigned submitted = 0, packets = 0;
    int status = 1;

    frame = (uint8_t *)VirtualAlloc(NULL, NV12_MAX_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    input_request = (uint8_t *)VirtualAlloc(NULL, sizeof(VM_ENCODER_INPUT) + NV12_MAX_BYTES,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!frame || !input_request) { OutputDebugStringA("encoder probe: NV12 buffers allocation failed\n"); goto done; }
    g_log = CreateFileW(log_path, GENERIC_WRITE, 0, NULL, 2u, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) { OutputDebugStringA("encoder probe: log file open failed\n"); goto done; }
    log_step(0, 0, 0, 0);
    h = CreateFileW(ENCODER_LINK, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_last_error = GetLastError();
        log_step(0x80000000u, g_last_error, 0, 0);
        OutputDebugStringA("encoder probe: open encoder link failed\n");
        goto done;
    }
    zero_bytes(&open_out, sizeof(open_out));
    if (!ioctl_ok(h, VM_IOCTL_OPEN, &open_in, sizeof(open_in),
                  &open_out, sizeof(open_out), &bytes, "VM_IOCTL_OPEN")) goto done;
    if (bytes < sizeof(open_out) || open_out.IsDecoder || !open_out.SessionToken) {
        OutputDebugStringA("encoder probe: OPEN metadata invalid\n");
        goto close_session;
    }
    OutputDebugStringA("encoder probe: session opened\n");

    zero_bytes(&config, sizeof(config));
    config.Version = VM_ENCODER_ABI_VERSION;
    config.Size = sizeof(config);
    config.SessionToken = open_out.SessionToken;
    config.Width = WIDTH;
    config.Height = HEIGHT;
    config.YStride = WIDTH;
    config.UVStride = WIDTH;
    config.UVOffset = WIDTH * HEIGHT;
    config.FrameRateNum = 30;
    config.FrameRateDen = 1;
    config.Bitrate = 256000;
    config.GopFrames = 30;
    if (!ioctl_ok(h, VM_IOCTL_ENC_CONFIG, &config, sizeof(config),
                  &config, sizeof(config), &bytes, "VM_IOCTL_ENC_CONFIG")) goto close_session;
    if (bytes < sizeof(config) || config.SessionToken != open_out.SessionToken) {
        OutputDebugStringA("encoder probe: CONFIG response invalid\n");
        goto close_session;
    }
    OutputDebugStringA("encoder probe: NV12 configured\n");
    log_step(0x2000u,config.Width,config.Height,config.YStride);
    payload_size = config.UVOffset + config.UVStride * (HEIGHT / 2u);
    if (config.YStride > MAX_STRIDE || config.UVStride > MAX_STRIDE || payload_size > NV12_MAX_BYTES) {
        OutputDebugStringA("encoder probe: negotiated NV12 layout exceeds probe buffer\n");
        goto close_session;
    }

    for (submitted = 0; submitted < INPUT_FRAMES; ++submitted) {
        VM_ENCODER_INPUT input;
        make_nv12(frame, payload_size, config.YStride, config.UVStride, config.UVOffset, submitted);
        zero_bytes(&input, sizeof(input));
        input.Version = VM_ENCODER_ABI_VERSION;
        input.Size = sizeof(input);
        input.SessionToken = open_out.SessionToken;
        input.BufferId = submitted;
        input.DataSize = payload_size;
        input.Timestamp = (uint64_t)submitted * 333333u;
        input.YOffset = 0;
        input.UVOffset = config.UVOffset;
        input.YStride = config.YStride;
        input.UVStride = config.UVStride;
        {
            size_t i;
            for (i = 0; i < sizeof(input); ++i) input_request[i] = ((const uint8_t *)&input)[i];
            for (i = 0; i < payload_size; ++i) input_request[sizeof(input) + i] = frame[i];
            if (!ioctl_ok(h, VM_IOCTL_ENC_QUEUE_INPUT, input_request,
                          (DWORD)(sizeof(input) + payload_size), NULL, 0, &bytes,
                          "VM_IOCTL_ENC_QUEUE_INPUT")) goto close_session;
        }
        OutputDebugStringA("encoder probe: NV12 frame queued\n");
    }

    packet = (PACKET_BUFFER *)VirtualAlloc(NULL, sizeof(*packet), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!packet) { OutputDebugStringA("encoder probe: packet allocation failed\n"); goto close_session; }
    output = CreateFileW(output_path, GENERIC_WRITE, 0, NULL, 2u,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) { OutputDebugStringA("encoder probe: output file open failed\n"); goto close_session; }
    metadata = CreateFileW(metadata_path, GENERIC_WRITE, 0, NULL, 2u,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (metadata == INVALID_HANDLE_VALUE) { OutputDebugStringA("encoder probe: metadata file open failed\n"); goto close_session; }

    while (packets < INPUT_FRAMES) {
        VM_ENCODER_GET_PACKET_IN request;
        zero_bytes(&request, sizeof(request));
        request.Version = VM_ENCODER_ABI_VERSION;
        request.Size = sizeof(request);
        request.SessionToken = open_out.SessionToken;
        request.TimeoutMs = PACKET_WAIT_MS;
        request.Capacity = PACKET_CAPACITY;
        zero_bytes(&packet->info, sizeof(packet->info));
        if (!ioctl_ok(h, VM_IOCTL_ENC_GET_PACKET, &request, sizeof(request), packet,
                      sizeof(*packet), &bytes, "VM_IOCTL_ENC_GET_PACKET")) {
            if (g_last_error == 121u) {
                OutputDebugStringA("encoder probe: packet wait timed out\n");
                break;
            }
            goto close_session;
        }
        if (bytes < sizeof(VM_ENCODER_PACKET_INFO) ||
            packet->info.Version != VM_ENCODER_ABI_VERSION ||
            packet->info.SessionToken != open_out.SessionToken ||
            packet->info.BytesUsed > PACKET_CAPACITY ||
            bytes < sizeof(VM_ENCODER_PACKET_INFO) + packet->info.BytesUsed) {
            OutputDebugStringA("encoder probe: invalid packet response\n");
            goto close_session;
        }
        OutputDebugStringA((packet->info.Flags & VM_ENCODER_PACKET_FLAG_KEYFRAME)
            ? "encoder probe: keyframe packet received\n"
            : "encoder probe: packet received\n");
        if (packet->info.Flags & VM_ENCODER_PACKET_FLAG_DRAIN_DONE) {
            OutputDebugStringA("encoder probe: unexpected DrainDone\n");
            goto close_session;
        }
        if (packet->info.BytesUsed == 0) {
            OutputDebugStringA("encoder probe: empty coded packet\n");
            goto close_session;
        }
        {
            DWORD written = 0;
            if (!WriteFile(output, packet->bytes, packet->info.BytesUsed, &written, NULL) ||
                written != packet->info.BytesUsed) {
                OutputDebugStringA("encoder probe: packet file write failed\n");
                goto close_session;
            }
        }
        {
            PACKET_META meta;
            DWORD written = 0;
            meta.index = packets;
            meta.bytes_used = packet->info.BytesUsed;
            meta.flags = packet->info.Flags;
            meta.reserved = 0;
            meta.timestamp_100ns = packet->info.Timestamp;
            if (!WriteFile(metadata, &meta, sizeof(meta), &written, NULL) || written != sizeof(meta)) {
                OutputDebugStringA("encoder probe: metadata write failed\n");
                goto close_session;
            }
        }
        log_step(0x1000u, 0, packet->info.BytesUsed, packets);
        ++packets;
    }
    if (packets < MIN_PACKETS) {
        OutputDebugStringA("encoder probe: fewer packets than minimum threshold\n");
        goto close_session;
    }
    if (!FlushFileBuffers(output) || !FlushFileBuffers(metadata)) { OutputDebugStringA("encoder probe: output flush failed\n"); goto close_session; }
    OutputDebugStringA("encoder probe: H264 output and packet metadata written\n");
    status = 0;

close_session:
    if (output != INVALID_HANDLE_VALUE) { CloseHandle(output); output = INVALID_HANDLE_VALUE; }
    if (metadata != INVALID_HANDLE_VALUE) { CloseHandle(metadata); metadata = INVALID_HANDLE_VALUE; }
    if (packet) { VirtualFree(packet, 0, MEM_RELEASE); packet = NULL; }
    if (h != INVALID_HANDLE_VALUE) {
        VM_CLOSE_IN close_in;
        zero_bytes(&close_in, sizeof(close_in));
        close_in.SessionId = open_out.SessionId;
        close_in.SessionToken = open_out.SessionToken;
        if (!DeviceIoControl(h, VM_IOCTL_CLOSE, &close_in, sizeof(close_in), NULL, 0, &bytes, NULL)) {
            OutputDebugStringA("encoder probe: CLOSE failed\n");
            if (status == 0) status = 1;
        }
    }
done:
    log_step(0xFFFFu, (uint32_t)status, packets, 0);
    if (g_log != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_log); CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
    if (frame) VirtualFree(frame, 0, MEM_RELEASE);
    if (input_request) VirtualFree(input_request, 0, MEM_RELEASE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return status;
}

void __stdcall probe_entry(void)
{
    ExitProcess((uint32_t)probe_run());
}

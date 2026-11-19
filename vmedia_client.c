/* vmedia_client.c - CreateFile + DeviceIoControl client for virtio-media.
 *
 * One VM_SESSION owns one handle to \\.\VMediaDecoder or \\.\VMediaEncoder.
 * Every call is synchronous; the driver serialises IOCTLs on a sequential
 * queue, so a session must not be used from two threads at once.
 *
 * PIPELINE (2026-10-25): the UMD's decode pipeline calls this client from a
 * worker thread (vm_send + vm_map_frame/vm_get_frame) while the DDI thread may
 * be inside vm_release_frame.  Both sides take the same pipeline lock, so the
 * "one caller at a time" rule above still holds for every session, and
 * g_last_error is thread-local so one thread's failure cannot be reported as
 * the other's.
 */
/* P2-1a builds this file as vmedia_client.dll (VMEDIA_CLIENT_BUILD). P2-2a
 * compiles it into dx11um_virtio.dll instead (VMEDIA_CLIENT_STATIC), so the
 * protocol stays single-sourced while the UMD gains no DLL dependency and no
 * extra exports. */
#if !defined(VMEDIA_CLIENT_STATIC)
#define VMEDIA_CLIENT_BUILD 1
#endif

#include <windows.h>
#include <string.h>

#include "vmedia_client.h"

struct _VM_SESSION {
    HANDLE   handle;
    uint32_t session_id;
    uint32_t kind;
    uint32_t codec; /* VM_OPEN_CODEC_*: what the host session was opened for */
    /* SESSION-OWNERSHIP (1.0.0.24): the driver's generation token for THIS
     * session.  Every IOCTL carries it back, so the driver can tell this
     * session's requests from a replaced decoder's.  0 = the driver predates
     * 1.0.0.24 and every request is served as a legacy one. */
    uint32_t token;
    uint32_t driver_version; /* VM_OPEN_OUT.DriverVersion */
    /* 1 while the driver is known to accept VM_IOCTL_SEND_BITSTREAM_EX; 0 once
     * it answered ERROR_INVALID_FUNCTION (a pre-1.0.0.24 driver), so the
     * fallback costs exactly one failed ioctl per session. */
    uint32_t send_ex;
};

/* Per-thread: see the header comment.  vm_last_error() is only meaningful on
 * the thread that made the call, which is exactly what the pipeline needs. */
static __thread uint32_t g_last_error;

static void set_error(uint32_t error)
{
    g_last_error = error;
}

uint32_t VM_CALL vm_last_error(void)
{
    return g_last_error;
}

uint32_t VM_CALL vm_driver_version(void)
{
    return VMEDIA_DRIVER_VERSION;
}

VM_SESSION * VM_CALL vm_open_codec(uint32_t kind, uint32_t codec, VM_OPEN_OUT *out)
{
    static const wchar_t *const links[2] = {
        VMEDIA_LINK_DECODER,
        VMEDIA_LINK_ENCODER,
    };
    VM_OPEN_IN in;
    VM_OPEN_OUT local_out;
    VM_SESSION *session;
    HANDLE handle;
    DWORD returned = 0;

    if (kind > 1) {
        set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (out == NULL) {
        out = &local_out;
    }
    memset(out, 0, sizeof(*out));

    handle = CreateFileW(links[kind], GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(GetLastError());
        return NULL;
    }

    in.Kind = kind;
    in.Codec = codec;
    if (!DeviceIoControl(handle, VM_IOCTL_OPEN, &in, (DWORD)sizeof(in),
                         out, (DWORD)sizeof(*out), &returned, NULL)) {
        set_error(GetLastError());
        CloseHandle(handle);
        return NULL;
    }
    if (returned < sizeof(*out)) {
        set_error(ERROR_INVALID_DATA);
        CloseHandle(handle);
        return NULL;
    }

    session = (VM_SESSION *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*session));
    if (session == NULL) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        CloseHandle(handle);
        return NULL;
    }
    session->handle = handle;
    session->session_id = out->SessionId;
    session->kind = kind;
    session->codec = codec;
    /* SESSION-OWNERSHIP: the generation token, and whether this driver knows
     * about one at all.  Both come from the OPEN answer, so a UMD built against
     * this header still runs against a 1.0.0.23 driver (token 0 = legacy). */
    session->token = out->SessionToken;
    session->driver_version = out->DriverVersion;
    session->send_ex = (out->DriverVersion >= VMEDIA_SESSION_OWNERSHIP_MIN_VERSION) ? 1u : 0u;
    set_error(0);
    return session;
}

VM_SESSION * VM_CALL vm_open(uint32_t kind, VM_OPEN_OUT *out)
{
    return vm_open_codec(kind, VM_OPEN_CODEC_H264, out);
}

int VM_CALL vm_send(VM_SESSION *session,
                    const void *data, uint32_t size,
                    uint32_t width, uint32_t height,
                    uint32_t flags, VM_SEND_OUT *out)
{
    VM_SEND_IN_EX *in;
    VM_SEND_OUT local_out;
    uint8_t *buffer;
    DWORD returned = 0;
    int ok;
    size_t total;
    size_t header;
    DWORD code;

    if (session == NULL || (data == NULL && size != 0)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (out == NULL) {
        out = &local_out;
    }

    /* SESSION-OWNERSHIP (1.0.0.24): the token travels in the header, which is
     * why SEND has its own IOCTL code -- the access unit follows the header
     * immediately, so a longer header under the old code would shift it. */
    header = session->send_ex ? sizeof(VM_SEND_IN_EX) : sizeof(VM_SEND_IN);
    total = header + (size_t)size;
    if (total > 0xFFFFFFFFu) {
        set_error(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    buffer = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, total);
    if (buffer == NULL) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    in = (VM_SEND_IN_EX *)buffer;
    in->Width = width;
    in->Height = height;
    in->Size = size;
    in->Flags = flags;
    in->SessionToken = session->token;
    in->Reserved0 = 0;
    if (size != 0) {
        memcpy(buffer + header, data, size);
    }

    code = session->send_ex ? VM_IOCTL_SEND_BITSTREAM_EX : VM_IOCTL_SEND_BITSTREAM;
    ok = DeviceIoControl(session->handle, code,
                         buffer, (DWORD)total,
                         out, (DWORD)sizeof(*out), &returned, NULL);
    if (!ok) {
        DWORD err = GetLastError();
        if (session->send_ex && err == ERROR_INVALID_FUNCTION) {
            /* The driver does not know the EX code: a pre-1.0.0.24 one.  Fall
             * back to the legacy header for the rest of the session (the access
             * unit is re-sent unchanged; nothing reached the device). */
            session->send_ex = 0;
            session->token = 0;
            HeapFree(GetProcessHeap(), 0, buffer);
            return vm_send(session, data, size, width, height, flags, out);
        }
        HeapFree(GetProcessHeap(), 0, buffer);
        set_error(err);
        return 0;
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    if (returned < sizeof(*out)) {
        set_error(ERROR_INVALID_DATA);
        return 0;
    }
    set_error(0);
    return 1;
}

static int tag_session(VM_SESSION *s)
{
    if (!s || !s->token) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (s->driver_version < VMEDIA_TAG_MIN_VERSION) { set_error(ERROR_NOT_SUPPORTED); return 0; }
    return 1;
}

int VM_CALL vm_send_tag(VM_SESSION *s, const void *data, uint32_t size,
    uint32_t width, uint32_t height, uint32_t flags, uint64_t tag, VM_SEND_OUT *out)
{
    VM_SEND_OUT local;
    VM_SEND_IN_TAG *in;
    DWORD returned = 0, err;
    size_t total = sizeof(*in) + (size_t)size;
    int ok;
    if (!tag_session(s)) return 0;
    if (!data || !size || !tag || tag > INT64_MAX || (flags & ~1u)) {
        set_error(ERROR_INVALID_PARAMETER); return 0;
    }
    if (total > UINT32_MAX || total < size) { set_error(ERROR_BUFFER_OVERFLOW); return 0; }
    in = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!in) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    in->Width = width; in->Height = height; in->Size = size; in->Flags = flags;
    in->SessionToken = s->token; in->Tag = tag;
    memcpy((uint8_t *)in + sizeof(*in), data, size);
    if (!out) out = &local;
    ok = DeviceIoControl(s->handle, VM_IOCTL_SEND_BITSTREAM_TAG, in, (DWORD)total,
                         out, sizeof(*out), &returned, NULL);
    err = ok ? (returned == sizeof(*out) ? 0 : ERROR_INVALID_DATA) : GetLastError();
    HeapFree(GetProcessHeap(), 0, in);
    set_error(err); return err == 0;
}

int VM_CALL vm_get_frame_tag(VM_SESSION *s, void *pixels, uint32_t capacity,
    VM_FRAME_TAG_OUT *out, uint32_t timeout_ms)
{
    VM_GET_FRAME_IN in = { timeout_ms, 0, 0, 0 };
    VM_FRAME_TAG_OUT *buf;
    DWORD returned = 0, err = 0;
    int ok;
    if (!tag_session(s)) return 0;
    if (!pixels || !out) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (capacity > UINT32_MAX - sizeof(*buf)) { set_error(ERROR_BUFFER_OVERFLOW); return 0; }
    buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*buf) + capacity);
    if (!buf) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    in.SessionToken = s->token;
    ok = DeviceIoControl(s->handle, VM_IOCTL_GET_FRAME_TAG, &in, sizeof(in), buf,
                         (DWORD)(sizeof(*buf) + capacity), &returned, NULL);
    if (!ok) err = GetLastError();
    else if (returned < sizeof(*buf) || buf->Info.Size > capacity ||
             returned - sizeof(*buf) < buf->Info.Size || !buf->Tag || buf->Tag > INT64_MAX ||
             buf->SessionToken != s->token) err = ERROR_INVALID_DATA;
    else { *out = *buf; memcpy(pixels, (uint8_t *)buf + sizeof(*buf), buf->Info.Size); }
    HeapFree(GetProcessHeap(), 0, buf);
    set_error(err); return err == 0;
}

int VM_CALL vm_map_frame_tag(VM_SESSION *s, VM_FRAME_MAP_TAG_OUT *out, uint32_t timeout_ms)
{
    VM_MAP_FRAME_IN in = { timeout_ms, 0, 0, 0 };
    DWORD returned = 0, err;
    if (!tag_session(s)) return 0;
    if (!out) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    memset(out, 0, sizeof(*out)); in.SessionToken = s->token;
    if (!DeviceIoControl(s->handle, VM_IOCTL_MAP_FRAME_TAG, &in, sizeof(in), out,
                         sizeof(*out), &returned, NULL)) { set_error(GetLastError()); return 0; }
    if (returned != sizeof(*out) || !out->Tag || out->Tag > INT64_MAX ||
        out->SessionToken != s->token || !out->Map.UserVa || !out->Map.Size) {
        err = ERROR_INVALID_DATA;
        if (returned >= sizeof(VM_FRAME_MAP)) (void)vm_release_frame(s, out->Map.Index);
        set_error(err); return 0;
    }
    set_error(0); return 1;
}

int VM_CALL vm_get_frame(VM_SESSION *session,
                         void *nv12, uint32_t capacity,
                         VM_FRAME_INFO *info, uint32_t timeout_ms)
{
    VM_GET_FRAME_IN in;
    VM_FRAME_INFO local_info;
    uint8_t *buffer;
    DWORD returned = 0;
    DWORD out_len;
    int ok;

    if (session == NULL || nv12 == NULL) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (info == NULL) {
        info = &local_info;
    }
    if (capacity > 0xFFFFFFFFu - (uint32_t)sizeof(VM_FRAME_INFO)) {
        set_error(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    out_len = (DWORD)(sizeof(VM_FRAME_INFO) + capacity);
    buffer = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, out_len);
    if (buffer == NULL) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    in.TimeoutMs = timeout_ms;
    in.Flags = 0;
    in.SessionToken = session->token;   /* SESSION-OWNERSHIP (1.0.0.24) */
    in.Reserved0 = 0;
    ok = DeviceIoControl(session->handle, VM_IOCTL_GET_FRAME,
                         &in, (DWORD)sizeof(in),
                         buffer, out_len, &returned, NULL);
    if (!ok) {
        set_error(GetLastError());
        HeapFree(GetProcessHeap(), 0, buffer);
        return 0;
    }
    if (returned < sizeof(VM_FRAME_INFO)) {
        set_error(ERROR_INVALID_DATA);
        HeapFree(GetProcessHeap(), 0, buffer);
        return 0;
    }
    memcpy(info, buffer, sizeof(*info));
    if (info->Size > capacity ||
        returned < (DWORD)(sizeof(VM_FRAME_INFO) + info->Size)) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        HeapFree(GetProcessHeap(), 0, buffer);
        return 0;
    }
    memcpy(nv12, buffer + sizeof(VM_FRAME_INFO), info->Size);
    HeapFree(GetProcessHeap(), 0, buffer);
    set_error(0);
    return 1;
}

/* P-BATCH ZEROCOPY: the mapped variant of vm_get_frame.  No pixel buffer is
 * allocated or copied here at all -- the driver hands back a VA into the media
 * pool's own pages, mapped into this process, and the capture buffer stays out
 * of the device until vm_release_frame(). */
int VM_CALL vm_map_frame(VM_SESSION *session, VM_FRAME_MAP *map, uint32_t timeout_ms)
{
    VM_MAP_FRAME_IN in;
    VM_FRAME_MAP local;
    DWORD returned = 0;
    int ok;

    if (session == NULL) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (map == NULL) {
        map = &local;
    }
    memset(map, 0, sizeof(*map));
    in.TimeoutMs = timeout_ms;
    in.Flags = 0;
    in.SessionToken = session->token;   /* SESSION-OWNERSHIP (1.0.0.24) */
    in.Reserved0 = 0;
    ok = DeviceIoControl(session->handle, VM_IOCTL_MAP_FRAME,
                         &in, (DWORD)sizeof(in),
                         map, (DWORD)sizeof(*map), &returned, NULL);
    if (!ok) {
        set_error(GetLastError());
        return 0;
    }
    if (returned < sizeof(*map) || map->UserVa == 0 || map->Size == 0) {
        /* Never leave a capture buffer checked out on a short answer. */
        if (returned >= sizeof(*map) || map->Index != 0)
            (void)vm_release_frame(session, map->Index);
        set_error(ERROR_INVALID_DATA);
        return 0;
    }
    set_error(0);
    return 1;
}

int VM_CALL vm_release_frame(VM_SESSION *session, uint32_t index)
{
    VM_RELEASE_FRAME_IN in;
    DWORD returned = 0;
    int ok;

    if (session == NULL) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    in.SessionId = session->session_id;
    in.Index = index;
    in.SessionToken = session->token;   /* SESSION-OWNERSHIP (1.0.0.24) */
    in.Reserved0 = 0;
    ok = DeviceIoControl(session->handle, VM_IOCTL_RELEASE_FRAME,
                         &in, (DWORD)sizeof(in),
                         NULL, 0, &returned, NULL);
    if (!ok) {
        set_error(GetLastError());
        return 0;
    }
    set_error(0);
    return 1;
}

/* P3-2: drop the queued AUs/frames and take the fresh host session the driver
 * opens.  Same handle, new session id -- no CloseHandle/CreateFile round trip,
 * so the caller's VM_SESSION stays valid. */
int VM_CALL vm_flush(VM_SESSION *session, VM_FLUSH_OUT *out)
{
    VM_FLUSH_IN in;
    VM_FLUSH_OUT local_out;
    DWORD returned = 0;
    int ok;

    if (session == NULL) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (out == NULL) {
        out = &local_out;
    }
    memset(out, 0, sizeof(*out));
    in.SessionId = session->session_id;
    in.Flags = 0;
    in.SessionToken = session->token;   /* SESSION-OWNERSHIP (1.0.0.24) */
    in.Reserved0 = 0;
    ok = DeviceIoControl(session->handle, VM_IOCTL_FLUSH,
                         &in, (DWORD)sizeof(in),
                         out, (DWORD)sizeof(*out), &returned, NULL);
    if (!ok) {
        set_error(GetLastError());
        return 0;
    }
    if (returned < sizeof(*out)) {
        set_error(ERROR_INVALID_DATA);
        return 0;
    }
    /* SESSION-OWNERSHIP: a FLUSH **bumps the generation** -- the old token is
     * dead from here on, and using it would get every later IOCTL refused.  An
     * old driver leaves the field 0 (it was Reserved0), which keeps the session
     * on the legacy path. */
    session->token = out->SessionToken;
    if (out->SessionId != 0 || out->SessionToken != 0) {
        session->session_id = out->SessionId;
    }
    set_error(0);
    return 1;
}

static int encoder_session_ok(VM_SESSION *session)
{
    if (session == NULL || session->kind != 1u || session->token == 0) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

int VM_CALL vm_encoder_configure(VM_SESSION *session, VM_ENCODER_CONFIG *config)
{
    VM_ENCODER_CONFIG request;
    DWORD returned = 0;
    if (!encoder_session_ok(session)) return 0;
    if (config == NULL) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    request = *config;
    request.Version = VM_ENCODER_ABI_VERSION;
    request.Size = sizeof(request);
    request.SessionToken = session->token;
    if (!DeviceIoControl(session->handle, VM_IOCTL_ENC_CONFIG,
                         &request, sizeof(request), config, sizeof(*config),
                         &returned, NULL)) {
        set_error(GetLastError());
        return 0;
    }
    if (returned != sizeof(*config) || config->Version != VM_ENCODER_ABI_VERSION ||
        config->Size != sizeof(*config) || config->SessionToken != session->token) {
        set_error(ERROR_INVALID_DATA);
        return 0;
    }
    set_error(0);
    return 1;
}

int VM_CALL vm_encoder_queue_input(VM_SESSION *session,
                                   const VM_ENCODER_INPUT *input,
                                   const void *nv12, uint32_t size)
{
    VM_ENCODER_INPUT request;
    uint8_t *buffer;
    size_t total;
    DWORD returned = 0, error;
    int ok;
    if (!encoder_session_ok(session)) return 0;
    if (input == NULL || nv12 == NULL || size == 0 || input->DataSize != size ||
        size > UINT32_MAX - sizeof(request)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    request = *input;
    request.Version = VM_ENCODER_ABI_VERSION;
    request.Size = sizeof(request);
    request.SessionToken = session->token;
    total = sizeof(request) + (size_t)size;
    buffer = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, total);
    if (buffer == NULL) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memcpy(buffer, &request, sizeof(request));
    memcpy(buffer + sizeof(request), nv12, size);
    ok = DeviceIoControl(session->handle, VM_IOCTL_ENC_QUEUE_INPUT,
                         buffer, (DWORD)total, NULL, 0, &returned, NULL);
    error = ok ? 0 : GetLastError();
    HeapFree(GetProcessHeap(), 0, buffer);
    if (!ok) { set_error(error); return 0; }
    set_error(0);
    return 1;
}

int VM_CALL vm_encoder_get_packet(VM_SESSION *session,
                                  VM_ENCODER_PACKET_INFO *info, void *packet,
                                  uint32_t capacity, uint32_t timeout_ms)
{
    VM_ENCODER_GET_PACKET_IN request;
    uint8_t *buffer;
    size_t total;
    DWORD returned = 0;
    int ok;
    if (!encoder_session_ok(session)) return 0;
    if (info == NULL || packet == NULL || capacity == 0 || timeout_ms > 30000u ||
        capacity > UINT32_MAX - sizeof(*info)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    total = sizeof(*info) + (size_t)capacity;
    buffer = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, total);
    if (buffer == NULL) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memset(&request, 0, sizeof(request));
    request.Version = VM_ENCODER_ABI_VERSION;
    request.Size = sizeof(request);
    request.SessionToken = session->token;
    request.TimeoutMs = timeout_ms;
    request.Capacity = capacity;
    ok = DeviceIoControl(session->handle, VM_IOCTL_ENC_GET_PACKET,
                         &request, sizeof(request), buffer, (DWORD)total,
                         &returned, NULL);
    if (!ok) {
        DWORD err = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        set_error(err);
        return 0;
    }
    {
        VM_ENCODER_PACKET_INFO *header = (VM_ENCODER_PACKET_INFO *)buffer;
        if (returned < sizeof(*header) || header->Version != VM_ENCODER_ABI_VERSION ||
            header->Size != sizeof(*header) || header->SessionToken != session->token ||
            header->BytesUsed > capacity || returned - sizeof(*header) < header->BytesUsed) {
            HeapFree(GetProcessHeap(), 0, buffer);
            set_error(ERROR_INVALID_DATA);
            return 0;
        }
        *info = *header;
        if (header->BytesUsed != 0) memcpy(packet, buffer + sizeof(*header), header->BytesUsed);
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    set_error(0);
    return 1;
}

int VM_CALL vm_encoder_control(VM_SESSION *session, uint32_t ioctl_code)
{
    VM_ENCODER_CONTROL request;
    DWORD returned = 0;
    if (!encoder_session_ok(session)) return 0;
    if (ioctl_code != VM_IOCTL_ENC_DRAIN && ioctl_code != VM_IOCTL_ENC_FLUSH) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(&request, 0, sizeof(request));
    request.Version = VM_ENCODER_ABI_VERSION;
    request.Size = sizeof(request);
    request.SessionToken = session->token;
    if (!DeviceIoControl(session->handle, ioctl_code, &request, sizeof(request),
                         NULL, 0, &returned, NULL)) {
        set_error(GetLastError());
        return 0;
    }
    set_error(0);
    return 1;
}

int VM_CALL vm_encoder_set_bitrate(VM_SESSION *session,uint32_t bitrate)
{
    VM_ENCODER_RATE_CONTROL request;
    DWORD returned=0;
    if(!encoder_session_ok(session))return 0;
    if(session->driver_version<0x00010024u){set_error(ERROR_NOT_SUPPORTED);return 0;}
    if(!bitrate||bitrate>INT32_MAX){set_error(ERROR_INVALID_PARAMETER);return 0;}
    memset(&request,0,sizeof(request));request.Version=VM_ENCODER_ABI_VERSION;
    request.Size=sizeof(request);request.SessionToken=session->token;request.Bitrate=bitrate;
    if(!DeviceIoControl(session->handle,VM_IOCTL_ENC_SET_BITRATE,&request,sizeof(request),NULL,0,&returned,NULL)){
        set_error(GetLastError());return 0;
    }
    set_error(0);return 1;
}

void VM_CALL vm_close(VM_SESSION *session)
{
    VM_CLOSE_IN in;
    DWORD returned = 0;

    if (session == NULL) {
        return;
    }
    in.SessionId = session->session_id;
    in.Flags = 0;
    in.SessionToken = session->token;   /* SESSION-OWNERSHIP (1.0.0.24) */
    in.Reserved0 = 0;
    DeviceIoControl(session->handle, VM_IOCTL_CLOSE, &in, (DWORD)sizeof(in),
                    NULL, 0, &returned, NULL);
    CloseHandle(session->handle);
    HeapFree(GetProcessHeap(), 0, session);
    set_error(0);
}

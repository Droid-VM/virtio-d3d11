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
    VM_SEND_IN *in;
    VM_SEND_OUT local_out;
    uint8_t *buffer;
    DWORD returned = 0;
    int ok;
    size_t total;

    if (session == NULL || (data == NULL && size != 0)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (out == NULL) {
        out = &local_out;
    }

    total = sizeof(VM_SEND_IN) + (size_t)size;
    if (total > 0xFFFFFFFFu) {
        set_error(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    buffer = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, total);
    if (buffer == NULL) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    in = (VM_SEND_IN *)buffer;
    in->Width = width;
    in->Height = height;
    in->Size = size;
    in->Flags = flags;
    if (size != 0) {
        memcpy(buffer + sizeof(VM_SEND_IN), data, size);
    }

    ok = DeviceIoControl(session->handle, VM_IOCTL_SEND_BITSTREAM,
                         buffer, (DWORD)total,
                         out, (DWORD)sizeof(*out), &returned, NULL);
    HeapFree(GetProcessHeap(), 0, buffer);
    if (!ok) {
        set_error(GetLastError());
        return 0;
    }
    if (returned < sizeof(*out)) {
        set_error(ERROR_INVALID_DATA);
        return 0;
    }
    set_error(0);
    return 1;
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
    ok = DeviceIoControl(session->handle, VM_IOCTL_FLUSH,
                         &in, (DWORD)sizeof(in),
                         out, (DWORD)sizeof(*out), &returned, NULL);
    if (!ok) {
        set_error(GetLastError());
        return 0;
    }
    if (returned < sizeof(*out) || out->SessionId == 0) {
        set_error(ERROR_INVALID_DATA);
        return 0;
    }
    session->session_id = out->SessionId;
    set_error(0);
    return 1;
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
    DeviceIoControl(session->handle, VM_IOCTL_CLOSE, &in, (DWORD)sizeof(in),
                    NULL, 0, &returned, NULL);
    CloseHandle(session->handle);
    HeapFree(GetProcessHeap(), 0, session);
    set_error(0);
}

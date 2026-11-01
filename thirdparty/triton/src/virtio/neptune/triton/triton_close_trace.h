/* Diagnostic only: value snapshots, no ownership tracking or ABI fields. */
#ifndef TRITON_CLOSE_TRACE_H
#define TRITON_CLOSE_TRACE_H
#include <windows.h>
#include <stdio.h>
#include "triton_log.h"
typedef struct {
    void *device, *rtdev, *rtres;
    UINT allocation, kmres, count, handle0;
    void *resource, *owner_rtres;
} TR_CLOSE_SNAPSHOT;
#ifdef TRITON_BUILD_COMMON_TRANSLATION_LAYER
/* One sequence domain shared by the UMD and its statically linked Triton. */
BOOL virtio_close_trace_enabled(void);
ULONGLONG virtio_close_trace_next(void);
void virtio_close_trace_emit(const char *, const char *, ULONGLONG, ULONGLONG,
                            DWORD, DWORD, TR_CLOSE_SNAPSHOT, HRESULT, BOOL);
#else
/* Standalone Triton: domain is TU-local, never compare with UMD sequence. */
static BOOL virtio_close_trace_enabled(void)
{
    static volatile LONG cache;
    LONG value = InterlockedCompareExchange(&cache, 0, 0);
    if (!value) {
        char setting[2] = {0};
        DWORD n = GetEnvironmentVariableA("VIRTIO_WDDM_CLOSE_TRACE", setting, sizeof(setting));
        LONG proposed = n == 1 && setting[0] == '1' ? 2 : 1;
        LONG previous = InterlockedCompareExchange(&cache, proposed, 0);
        value = previous ? previous : proposed;
    }
    return value == 2;
}
static ULONGLONG virtio_close_trace_next(void)
{
    static DECLSPEC_ALIGN(8) volatile LONG64 sequence;
    return (ULONGLONG)InterlockedIncrement64(&sequence);
}
static void virtio_close_trace_emit(const char *path, const char *phase,
    ULONGLONG operation, ULONGLONG seq, DWORD pid, DWORD tid,
    TR_CLOSE_SNAPSHOT s, HRESULT hr, BOOL failed)
{
    char line[1024];
    int length = snprintf(line, sizeof(line),
        "CLOSE_TRACE domain=triton-resource-tu level=%s path=%s phase=%s operation_id=%llu event_seq=%llu pid=%lu tid=%lu device=%p hRTDevice=%p hRTResource=%p alloc=0x%08x KMResource_field=0x%08x NumAllocations=%u HandleList[0]=0x%08x resource=%p owner_hRTResource=%p hr=0x%08lx\n",
        failed ? "ERROR" : "INFO", path, phase, operation, seq, pid, tid,
        s.device, s.rtdev, s.rtres, s.allocation, s.kmres, s.count, s.handle0,
        s.resource, s.owner_rtres, hr);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        snprintf(line, sizeof(line),
            "CLOSE_TRACE domain=triton-resource-tu level=ERROR format_overflow operation_id=%llu event_seq=%llu pid=%lu tid=%lu\n",
            operation, seq, pid, tid);
    }
    triton_log_raw(line);
}
#endif
#endif

/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Logging and DDI-entry macros.
 *
 *   TR_LOG(fmt, ...)  - OutputDebugStringA debug print.
 *   TR_STUB(name)     - logs "Triton: STUB <name>" once per thunk.
 */

#ifndef TRITON_LOG_H_INCLUDED
#define TRITON_LOG_H_INCLUDED

#include <windows.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TRITON_BUILD_COMMON_TRANSLATION_LAYER
extern void triton_log_raw(const char *line);
#else
static inline void triton_log_raw(const char *line)
{
    OutputDebugStringA(line);
}
#endif

#define TR_LOG(fmt, ...) do {                                         \
        char _trbuf[512];                                             \
        _snprintf_s(_trbuf, sizeof(_trbuf), _TRUNCATE,                \
                    "Triton: " fmt "\n", ##__VA_ARGS__);              \
        triton_log_raw(_trbuf);                                       \
    } while (0)


static inline BOOL tritonVerboseLogEnabled(void)
{
    static volatile LONG cached;
    LONG value = InterlockedCompareExchange(&cached, 0, 0);
    if (!value) {
        char setting[2] = {0};
        DWORD length = GetEnvironmentVariableA("VIRTIO_WDDM_VERBOSE", setting, sizeof(setting));
        value = length == 1 && setting[0] == '1' ? 2 : 1;
        InterlockedCompareExchange(&cached, value, 0);
    }
    return value == 2;
}

#define TR_LOG_VERBOSE(fmt, ...) do { \
    if (tritonVerboseLogEnabled()) TR_LOG(fmt, ##__VA_ARGS__); \
} while (0)

/* Keep the first errors and exponential samples without flooding the log. */
#define TR_LOG_LIMITED(fmt, ...) do { \
    static volatile LONG count; \
    ULONG n = (ULONG)InterlockedIncrement(&count); \
    if (n <= 16 || !(n & (n - 1)) || tritonVerboseLogEnabled()) \
        TR_LOG(fmt, ##__VA_ARGS__); \
} while (0)

/* Per-DDI-call trace: compiled OUT by default.  OutputDebugStringA is a
 * RaiseException + global DBWIN handshake per call; at draw/list rates it
 * dominates real work: a loading thread can spend minutes parked
 * inside ODS.  Define TRITON_HOT_LOG to re-enable when tracing a
 * specific run. */
#ifdef TRITON_HOT_LOG
#define TR_LOG_HOT TR_LOG
#else
#define TR_LOG_HOT(fmt, ...) do { } while (0)
#endif

#define TR_STUB(name) do {                                            \
        static LONG _seen = 0;                                        \
        if (InterlockedExchange(&_seen, 1) == 0)                      \
            TR_LOG("STUB %s", (name));                                \
    } while (0)

#define TR_TRACE() TR_LOG_HOT("%s", __FUNCTION__)

/* Opt-in capture for the standalone D2D reproduction. No global DWM trace. */
static inline BOOL tritonShaderDiagEnabled(void)
{
    static volatile LONG cached;
    LONG value = InterlockedCompareExchange(&cached, 0, 0);
    if (!value) {
        char dir[2];
        value = GetEnvironmentVariableA("VIRTIO_WDDM_SHADER_DIAG", dir, sizeof(dir)) ? 2 : 1;
        InterlockedCompareExchange(&cached, value, 0);
    }
    return value == 2;
}

#define TR_SHADER_DIAG(fmt, ...) do { \
    if (tritonShaderDiagEnabled()) TR_LOG("SHDIAG " fmt, ##__VA_ARGS__); \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* TRITON_LOG_H_INCLUDED */

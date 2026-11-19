/* droidvm_mft_log.c - file logging for the decoder MFT.
 *
 * Playing video through Media Foundation means the interesting process is
 * wmplayer/MediaPlayer, not something we can attach to while a batch runs, so
 * every state transition the transform makes is written to a file when
 * DROIDVM_MFT_LOG points at one:
 *
 *     set DROIDVM_MFT_LOG=C:\DroidVM\mft\mft.log
 *
 * Like the UMD's VIRTIO_WDDM_LOG_FILE this is opt-in: with the variable unset
 * the only cost is one string compare per call.  The file is opened in append
 * mode and shared for read, so a second process (e.g. the EVR's mixer inside
 * the same player) appending to the same path does not fail; each line carries
 * the pid and tid so two processes interleaving is still readable.
 */
#include "droidvm_mft.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static CRITICAL_SECTION g_log_lock;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static volatile LONG g_log_state;      /* 0 = not initialised, 1 = busy, 2 = ready */
static DWORD g_log_pid;

static void log_open_once(void)
{
    LONG prev = InterlockedCompareExchange(&g_log_state, 1, 0);
    char path[MAX_PATH];
    DWORD n;

    if (prev == 2)
        return;
    if (prev == 1) {
        /* Another thread is opening the file; wait for it to finish. */
        while (InterlockedCompareExchange(&g_log_state, 2, 2) != 2)
            Sleep(0);
        return;
    }

    InitializeCriticalSection(&g_log_lock);
    g_log_pid = GetCurrentProcessId();

    n = GetEnvironmentVariableA("DROIDVM_MFT_LOG", path, (DWORD)sizeof(path));
    if (n != 0 && n < sizeof(path)) {
        g_log_file = CreateFileA(path, FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_log_file != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        char head[128];
        int len;
        GetLocalTime(&st);
        len = _snprintf(head, sizeof(head),
                        "# DroidVM MFT log opened %04u-%02u-%02u %02u:%02u:%02u "
                        "pid=%lu\r\n",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                        st.wSecond, (unsigned long)g_log_pid);
        if (len > 0) {
            DWORD wrote = 0;
            WriteFile(g_log_file, head, (DWORD)len, &wrote, NULL);
        }
    }
    InterlockedExchange(&g_log_state, 2);
}

void dvm_logf(const char *fmt, ...)
{
    char line[1024];
    int len;
    va_list ap;
    SYSTEMTIME st;

    log_open_once();
    if (g_log_file == INVALID_HANDLE_VALUE)
        return;

    GetLocalTime(&st);
    len = _snprintf(line, sizeof(line), "[mft %02u:%02u:%02u.%03u pid=%lu tid=%lu] ",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    (unsigned long)g_log_pid,
                    (unsigned long)GetCurrentThreadId());
    if (len < 0 || (size_t)len >= sizeof(line))
        return;

    va_start(ap, fmt);
    _vsnprintf(line + len, sizeof(line) - (size_t)len - 2, fmt, ap);
    va_end(ap);

    {
        size_t used = strlen(line);
        DWORD wrote = 0;
        if (used > sizeof(line) - 3)
            used = sizeof(line) - 3;
        line[used++] = '\r';
        line[used++] = '\n';
        line[used] = '\0';
        EnterCriticalSection(&g_log_lock);
        if (g_log_file != INVALID_HANDLE_VALUE)
            WriteFile(g_log_file, line, (DWORD)used, &wrote, NULL);
        LeaveCriticalSection(&g_log_lock);
    }
}

void dvm_log_flush_close(void)
{
    if (InterlockedCompareExchange(&g_log_state, 2, 2) != 2)
        return;
    EnterCriticalSection(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_log_lock);
}

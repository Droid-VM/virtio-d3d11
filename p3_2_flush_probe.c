/* p3_2_flush_probe.c -- flush/EOS probe for driver 1.0.0.11 (P3-2).
 *
 * Sequence: open once, send 20 access units (AU 0..19 of the pack, AU 0 carries
 * SPS/PPS), call VM_IOCTL_FLUSH, then send 10 more starting from AU 0 again
 * (that is what a player does after a seek: the stream restarts from an IDR).
 *
 * PASS criteria:
 *   - all 20 pre-flush AUs send and produce a frame,
 *   - FLUSH returns a NEW session id and reports non-zero DroppedAu,
 *   - all 10 post-flush AUs send and produce a frame, with NO timeout,
 *   - the first post-flush frame is a plausible picture (not a blank/never
 *     written buffer: nonzero luma, more than one distinct value).
 *
 * Build (ARM64, llvm-mingw), from the directory that has vmedia_client.c:
 *   aarch64-w64-mingw32-clang -O2 -Wall -Wextra -DVMEDIA_CLIENT_BUILD \
 *       -o p3_2_flush_probe.exe p3_2_flush_probe.c vmedia_client.c -I. -static
 *
 * Exit: 0 pass, 1 check failed, 2 usage/input, 3 open, 4 pre-flush, 5 flush, 6 post-flush.
 */
#define VMEDIA_CLIENT_BUILD 1

#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmedia_client.h"

#define FRAME_CAP (4u * 1024u * 1024u)
#define PRE_AUS 20u
#define POST_AUS 10u

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static double now_ms(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

typedef struct {
    unsigned char *data;
    uint32_t size;
} Au;

static int load_pack(const char *path, Au **aus_out, uint32_t *count_out)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;
    uint32_t count, i, off;
    Au *aus;

    if (!f) {
        printf("INPUT: cannot open '%s' (errno=%d)\n", path, errno);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    if (len <= 4) {
        printf("INPUT: '%s' too short\n", path);
        fclose(f);
        return 0;
    }
    buf = (unsigned char *)malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        printf("INPUT: short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    count = rd32(buf);
    aus = (Au *)calloc(count ? count : 1, sizeof(Au));
    if (!aus) {
        free(buf);
        return 0;
    }
    off = 4;
    for (i = 0; i < count; i++) {
        uint32_t n;
        if (off + 4 > (uint32_t)len) {
            printf("INPUT: pack truncated at AU %u\n", i);
            break;
        }
        n = rd32(buf + off);
        off += 4;
        if (off + n > (uint32_t)len) {
            printf("INPUT: AU %u overruns the pack\n", i);
            break;
        }
        aus[i].data = buf + off;
        aus[i].size = n;
        off += n;
    }
    if (i != count) {
        free(aus);
        free(buf);
        return 0;
    }
    *aus_out = aus;
    *count_out = count;
    return 1;
}

static int check(const char *what, int ok, const char *detail)
{
    printf("CHECK %-58s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && *detail ? "  " : "", detail ? detail : "");
    return ok;
}

/* Send one AU and take one frame.  Returns 1 on success. */
static int send_get(VM_SESSION *session, const Au *au, uint32_t idx,
                    uint32_t width, uint32_t height, uint32_t timeout_ms,
                    unsigned char *nv12, VM_FRAME_INFO *info, double *wait_ms)
{
    VM_SEND_OUT send_out;
    double t0;

    memset(&send_out, 0, sizeof(send_out));
    memset(info, 0, sizeof(*info));
    if (!vm_send(session, au->data, au->size, width, height, 0, &send_out)) {
        printf("AU %u: SEND FAIL err=%lu size=%u\n", idx,
               (unsigned long)vm_last_error(), au->size);
        return 0;
    }
    t0 = now_ms();
    if (!vm_get_frame(session, nv12, FRAME_CAP, info, timeout_ms)) {
        printf("AU %u: GET_FRAME FAIL err=%lu after %.1f ms\n", idx,
               (unsigned long)vm_last_error(), now_ms() - t0);
        return 0;
    }
    *wait_ms = now_ms() - t0;
    if (info->Width != width || info->Height != height ||
        info->Size < width * height * 3u / 2u) {
        printf("AU %u: BAD FRAME %ux%u size=%u\n", idx, info->Width,
               info->Height, info->Size);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *pack = "p3-aus.bin";
    uint32_t width = 720, height = 1280, timeout_ms = 2000;
    Au *aus = NULL;
    uint32_t count = 0, i;
    VM_SESSION *session;
    VM_OPEN_OUT open_out;
    VM_FLUSH_OUT flush_out;
    unsigned char *nv12;
    int ok = 1, checks = 0, passes = 0;
    int pre_ok = 0, post_ok = 0, timeouts = 0;
    int first_post_valid = 0;
    double t_all, worst_wait = 0.0, flush_ms = 0.0;
    uint32_t old_session;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--pack") && i + 1 < (uint32_t)argc) {
            pack = argv[++i];
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < (uint32_t)argc) {
            timeout_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--width") && i + 1 < (uint32_t)argc) {
            width = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--height") && i + 1 < (uint32_t)argc) {
            height = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else {
            printf("bad/unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!load_pack(pack, &aus, &count) || count < PRE_AUS)
        return 2;
    printf("=== p3_2_flush_probe (driver 1.0.0.11 flush/EOS) ===\n");
    printf("client: vm_driver_version=0x%08lX  pack=%s  aus=%u  geometry=%ux%u\n",
           (unsigned long)vm_driver_version(), pack, count, width, height);

    nv12 = (unsigned char *)malloc(FRAME_CAP);
    if (!nv12) {
        printf("OUT OF MEMORY\n");
        return 2;
    }
    memset(&open_out, 0, sizeof(open_out));
    session = vm_open(0, &open_out);
    if (!session) {
        printf("OPEN: FAIL err=%lu\n", (unsigned long)vm_last_error());
        return 3;
    }
    old_session = open_out.SessionId;
    printf("OPEN: session=%u caps=0x%08lX pool=%u drv=0x%08lX\n",
           open_out.SessionId, (unsigned long)open_out.DeviceCaps,
           open_out.PoolSize, (unsigned long)open_out.DriverVersion);

    t_all = now_ms();

    /* ---- phase 1: 20 access units on one session ---------------------- */
    for (i = 0; i < PRE_AUS; i++) {
        VM_FRAME_INFO info;
        double wait = 0.0;
        if (!send_get(session, &aus[i], i, width, height, timeout_ms, nv12,
                      &info, &wait)) {
            if (vm_last_error() == 121) /* ERROR_TIMEOUT */
                timeouts++;
            break;
        }
        pre_ok++;
        if (wait > worst_wait)
            worst_wait = wait;
        if ((i % 5) == 0 || i + 1 == PRE_AUS)
            printf("PRE  AU %2u: ok  %ux%u  %6.1f ms  y[0]=%u y[mid]=%u\n", i,
                   info.Width, info.Height, wait, nv12[0],
                   nv12[width * height / 2]);
    }
    printf("PRE: %d/%u frame(s), worst wait %.1f ms\n", pre_ok, PRE_AUS,
           worst_wait);

    /* ---- phase 2: FLUSH ------------------------------------------------ */
    memset(&flush_out, 0, sizeof(flush_out));
    {
        double t0 = now_ms();
        int flushed = vm_flush(session, &flush_out);
        flush_ms = now_ms() - t0;
        if (!flushed) {
            printf("FLUSH: FAIL err=%lu\n", (unsigned long)vm_last_error());
            vm_close(session);
            free(nv12);
            return 5;
        }
    }
    printf("FLUSH: dropped_au=%u dropped_frames=%u session %u -> %u (%.1f ms)\n",
           flush_out.DroppedAu, flush_out.DroppedFrames, old_session,
           flush_out.SessionId, flush_ms);

    /* ---- phase 3: 10 access units from AU 0 (IDR with SPS/PPS) -------- */
    for (i = 0; i < POST_AUS && i < count; i++) {
        VM_FRAME_INFO info;
        double wait = 0.0;
        unsigned y0;
        if (!send_get(session, &aus[i], i, width, height, timeout_ms, nv12,
                      &info, &wait)) {
            if (vm_last_error() == 121)
                timeouts++;
            break;
        }
        post_ok++;
        y0 = nv12[0];
        if (i == 0) {
            /* A never-written buffer is all zero; a decoded picture is not.
             * Also sample a few points to catch an all-constant buffer. */
            unsigned m = nv12[width * height / 2], nz = 0, k;
            for (k = 0; k < 64; k++)
                nz += nv12[k * (width * height / 64)] != 0;
            first_post_valid = (nz >= 16) && (y0 != m || y0 != 0);
            printf("POST AU  0: first frame after flush: y[0]=%u y[mid]=%u "
                   "nonzero_samples=%u/64 -> %s\n",
                   y0, m, nz, first_post_valid ? "plausible" : "BLANK");
        }
        printf("POST AU %2u: ok  %ux%u  %6.1f ms  y[0]=%u\n", i, info.Width,
               info.Height, wait, y0);
    }
    printf("POST: %d/%u frame(s), timeouts=%d, total %.1f ms\n", post_ok,
           POST_AUS, timeouts, now_ms() - t_all);

    vm_close(session);

    checks++;
    passes += ok &= check("20 pre-flush AUs decoded", pre_ok == (int)PRE_AUS, "");
    checks++;
    passes += ok &= check("FLUSH opened a NEW session",
                          flush_out.SessionId != 0 && flush_out.SessionId != old_session,
                          "");
    checks++;
    passes += ok &= check("FLUSH reported dropped AUs (DroppedAu > 0)",
                          flush_out.DroppedAu > 0, "");
    checks++;
    passes += ok &= check("10 post-flush AUs decoded",
                          post_ok == (int)POST_AUS, "");
    checks++;
    passes += ok &= check("no GET_FRAME timeout", timeouts == 0, "");
    checks++;
    passes += ok &= check("first frame after flush is a real picture",
                          first_post_valid, "");
    checks++;
    passes += ok &= check("worst wait < timeout", worst_wait < (double)timeout_ms, "");

    printf("RESULT: %s (%d/%d checks)\n", ok ? "PASS" : "FAIL", passes, checks);
    free(aus);
    free(nv12);
    return ok ? 0 : 1;
}

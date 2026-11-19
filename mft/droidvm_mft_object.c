/* droidvm_mft_object.c - asynchronous H.264 decoder MFT backed by virtio-media.
 *
 * Media Foundation shape
 * ----------------------
 * The object implements IMFTransform + IMFMediaEventGenerator and is an
 * *asynchronous* MFT: MF_TRANSFORM_ASYNC is TRUE in its attributes, so the
 * topology loader must unlock it with MF_TRANSFORM_ASYNC_UNLOCK before use and
 * then drive it with METransformNeedInput / METransformHaveOutput events
 * instead of assuming ProcessInput/ProcessOutput block.
 *
 * One input stream (H.264 Annex-B or length-prefixed AVC), one output stream
 * (NV12, system memory, MFT_OUTPUT_STREAM_PROVIDES_SAMPLES).  All methods run
 * on Media Foundation's threads; a single worker thread owns the
 * virtio-media session because VM_IOCTL_SEND_BITSTREAM / VM_IOCTL_GET_FRAME are
 * synchronous IOCTLs on a sequential queue and must never overlap.
 *
 * Data flow
 * ---------
 *   ProcessInput:  sample -> h264_bits_build_au() -> queue (copy, timestamp)
 *   worker:        vm_send() the AU  ->  vm_get_frame() (1 s budget)
 *                  -> row-copy/crop into a display-sized NV12 frame -> queue
 *                  -> METransformHaveOutput
 *   ProcessOutput: dequeue one NV12 frame into the output sample
 *
 * Drain/flush follow the async-MFT contract (P-BATCH-MFT.md 4.4):
 *   MFT_MESSAGE_COMMAND_DRAIN -> METransformDrainComplete when nothing is left
 *                                in the worker, ProcessOutput then returns
 *                                MF_E_TRANSFORM_NEED_MORE_INPUT;
 *   MFT_MESSAGE_COMMAND_FLUSH -> queued AUs/frames are dropped, the driver
 *                                session is flushed (new host session) and
 *                                METransformNeedInput is posted again.
 *
 * Errors: a failing driver (no \\.\VMediaDecoder, IOCTL error) is sticky and is
 * reported from ProcessInput/ProcessOutput, which is how an MFT tells the
 * pipeline that the stream died.  The log file named by DROIDVM_MFT_LOG has the
 * Win32 error for every failure.
 */
#include "droidvm_mft.h"
#include "h264_bits.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(VMEDIA_CLIENT_STATIC)
#include <vmedia_client.h>
#else
#define VMEDIA_CLIENT_STATIC 1
#include <vmedia_client.h>
#endif

#define DVM_AU_QUEUE              4
#define DVM_FRAME_SLOTS           4
#define DVM_GET_FRAME_TIMEOUT_MS  1000
#define DVM_MAX_CONSEC_TIMEOUTS   8

#define DVM_PACK2(hi, lo) ((((UINT64)(hi)) << 32) | ((UINT64)(lo)))
#define DVM_HI32(v)       ((UINT32)(((UINT64)(v)) >> 32))
#define DVM_LO32(v)       ((UINT32)((UINT64)(v)))

typedef struct DvmAu {
    uint8_t *data;
    uint32_t len;
    LONGLONG time;
    LONGLONG duration;
    uint32_t gen;
} DvmAu;

typedef struct DvmFrame {
    uint8_t *data;              /* out_w * out_h * 3 / 2, NV12, top-down */
    uint32_t len;
    LONGLONG time;
    LONGLONG duration;
} DvmFrame;

typedef struct DvmMft {
    /* COM: two interface vtable pointers, IMFTransform first so that an
     * IMFTransform* is the object pointer (no pointer fix-up needed). */
    const IMFTransformVtbl            *lpTransformVtbl;
    const IMFMediaEventGeneratorVtbl  *lpEventVtbl;
    LONG                               ref;

    CRITICAL_SECTION                   lock;
    IMFMediaEventQueue                *events;
    IMFAttributes                     *attributes;
    IMFMediaType                      *in_type;
    IMFMediaType                      *out_type;

    /* Negotiated geometry.  in_* is the display size the input media type /
     * SPS report; send_* is what VM_IOCTL_SEND_BITSTREAM gets (macroblock
     * aligned, because that is what the host decoder emits); out_* is the
     * display size of the NV12 sample we hand to Media Foundation. */
    uint32_t                           in_w, in_h;
    uint32_t                           send_w, send_h;
    uint32_t                           out_w, out_h;
    UINT64                             frame_rate;
    h264_bits                          bits;

    /* Driver session: only the worker thread calls vm_*(). */
    VM_SESSION                        *session;
    int                                session_open;
    uint32_t                           drv_w, drv_h, drv_stride, drv_size;
    uint8_t                           *scratch;
    uint32_t                           scratch_cap;
    int                                first_send_done;

    /* Worker commands and queues, all protected by lock. */
    DvmAu                              au_q[DVM_AU_QUEUE];
    uint32_t                           au_head;
    uint32_t                           au_count;
    int                                in_flight;
    uint32_t                           inflight_gen;
    LONGLONG                           inflight_time;
    LONGLONG                           inflight_dur;
    DvmFrame                           frames[DVM_FRAME_SLOTS];
    uint32_t                           frame_count;
    uint32_t                           gen;
    int                                flush_req;
    int                                reset_req;
    int                                need_input_posted;
    int                                have_output_posted;
    int                                draining;
    int                                drained;
    int                                drain_posted;
    int                                streaming;
    volatile LONG                      stop;
    HANDLE                             thread;
    HANDLE                             wake;

    HRESULT                            fatal;
    uint32_t                           consec_timeouts;
    UINT32                             d3d_manager_seen;
    UINT32                             no_need_log;
    int                                mf_started;
    int                                geometry_warned;

    /* Statistics for the log. */
    UINT64                             aus_in;
    UINT64                             frames_out;
    UINT64                             sends_failed;
    UINT64                             get_timeouts;
    UINT64                             flushes;
    UINT64                             drains;
    UINT64                             resets;
    UINT64                             dropped;
} DvmMft;

/* ------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE mft_QueryInterface(IMFTransform *This, REFIID riid, void **ppv);
static ULONG   STDMETHODCALLTYPE mft_AddRef(IMFTransform *This);
static ULONG   STDMETHODCALLTYPE mft_Release(IMFTransform *This);

static DWORD WINAPI dvm_worker_main(LPVOID param);
static void dvm_free_queues_locked(DvmMft *m);

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void post_event(DvmMft *m, MediaEventType type, HRESULT hr)
{
    if (m->events != NULL)
        IMFMediaEventQueue_QueueEventParamVar(m->events, type, &GUID_NULL, hr, NULL);
}

/* True once the client has set MF_TRANSFORM_ASYNC_UNLOCK on our attributes. */
static int async_unlocked(DvmMft *m)
{
    UINT32 v = 0;

    if (m->attributes == NULL)
        return 0;
    if (FAILED(IMFAttributes_GetUINT32(m->attributes, &MF_TRANSFORM_ASYNC_UNLOCK, &v)))
        return 0;
    return v != 0;
}

static uint32_t align16(uint32_t v)
{
    return (v + 15u) & ~15u;
}

/* Recompose the send and input geometry from the SPS and the input media type.
 * Caller holds the lock. */
static void update_geometry_locked(DvmMft *m)
{
    if (m->bits.width != 0 && m->bits.height != 0) {
        /* The SPS is authoritative: a container can report the coded size
         * (1920x1088) where the picture is really 1920x1080. */
        m->in_w = m->bits.width;
        m->in_h = m->bits.height;
        m->send_w = m->bits.coded_width ? m->bits.coded_width : align16(m->bits.width);
        m->send_h = m->bits.coded_height ? m->bits.coded_height : align16(m->bits.height);
    } else if (m->in_w != 0 && m->in_h != 0) {
        m->send_w = align16(m->in_w);
        m->send_h = align16(m->in_h);
    }
}

static HRESULT copy_attribute_type(IMFMediaType **dst, IMFMediaType *src)
{
    IMFMediaType *copy = NULL;
    HRESULT hr;

    hr = MFCreateMediaType(&copy);
    if (FAILED(hr))
        return hr;
    hr = IMFAttributes_CopyAllItems((IMFAttributes *)copy, (IMFAttributes *)src);
    if (FAILED(hr)) {
        IMFMediaType_Release(copy);
        return hr;
    }
    if (*dst != NULL)
        IMFMediaType_Release(*dst);
    *dst = copy;
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* Driver session (worker thread only)                                        */
/* ------------------------------------------------------------------------- */

static void close_session(DvmMft *m)
{
    if (m->session != NULL) {
        vm_close(m->session);
        m->session = NULL;
    }
    m->session_open = 0;
    m->first_send_done = 0;
    m->drv_w = m->drv_h = m->drv_stride = m->drv_size = 0;
    if (m->scratch != NULL) {
        HeapFree(GetProcessHeap(), 0, m->scratch);
        m->scratch = NULL;
        m->scratch_cap = 0;
    }
}

static int ensure_session(DvmMft *m)
{
    VM_OPEN_OUT open_out;

    if (m->session != NULL)
        return 1;

    memset(&open_out, 0, sizeof(open_out));
    m->session = vm_open_codec(0 /* decoder */, VM_OPEN_CODEC_H264, &open_out);
    if (m->session == NULL) {
        uint32_t err = vm_last_error();
        dvm_logf("vm_open failed err=%u (driver present? \\\\.\\VMediaDecoder)", err);
        EnterCriticalSection(&m->lock);
        if (m->fatal == S_OK)
            m->fatal = E_FAIL;
        LeaveCriticalSection(&m->lock);
        return 0;
    }
    m->session_open = 1;
    dvm_logf("vm session %u opened (caps=0x%08X type=%u pool=%u drv=%u.%u.%u)",
             open_out.SessionId, open_out.DeviceCaps, open_out.DeviceType,
             open_out.PoolSize,
             (open_out.DriverVersion >> 24) & 0xFFu,
             (open_out.DriverVersion >> 16) & 0xFFu,
             open_out.DriverVersion & 0xFFFFu);
    return 1;
}

static int ensure_scratch(DvmMft *m, uint32_t bytes)
{
    if (m->scratch != NULL && m->scratch_cap >= bytes)
        return 1;
    if (m->scratch != NULL) {
        HeapFree(GetProcessHeap(), 0, m->scratch);
        m->scratch = NULL;
        m->scratch_cap = 0;
    }
    m->scratch = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (m->scratch == NULL) {
        dvm_logf("HeapAlloc(%u) for the frame scratch buffer failed", bytes);
        return 0;
    }
    m->scratch_cap = bytes;
    return 1;
}

static int send_au(DvmMft *m, DvmAu *au)
{
    VM_SEND_OUT out;
    uint32_t send_w, send_h;
    int ok;

    EnterCriticalSection(&m->lock);
    send_w = m->send_w;
    send_h = m->send_h;
    LeaveCriticalSection(&m->lock);

    if (send_w == 0 || send_h == 0) {
        dvm_logf("dropping AU: no geometry (input type or SPS not seen)");
        return 0;
    }
    if (!ensure_session(m))
        return 0;

    memset(&out, 0, sizeof(out));
    ok = vm_send(m->session, au->data, au->len, send_w, send_h, 0, &out);
    if (!ok) {
        uint32_t err = vm_last_error();
        dvm_logf("vm_send(%u B, %ux%u) failed err=%u; resetting the session and retrying",
                 au->len, send_w, send_h, err);
        close_session(m);
        EnterCriticalSection(&m->lock);
        m->sends_failed++;
        LeaveCriticalSection(&m->lock);
        if (!ensure_session(m))
            return 0;
        memset(&out, 0, sizeof(out));
        ok = vm_send(m->session, au->data, au->len, send_w, send_h, 0, &out);
        if (!ok) {
            uint32_t err2 = vm_last_error();
            dvm_logf("vm_send retry failed err=%u (AU dropped, %llu total failures)",
                     err2, (unsigned long long)m->sends_failed);
            EnterCriticalSection(&m->lock);
            m->sends_failed++;
            m->dropped++;
            LeaveCriticalSection(&m->lock);
            return 0;
        }
    }

    EnterCriticalSection(&m->lock);
    if (!m->first_send_done) {
        m->first_send_done = 1;
        m->drv_w = out.Width;
        m->drv_h = out.Height;
        m->drv_stride = out.CaptureStride;
        m->drv_size = out.CaptureSizeImage;
        /* The driver reports the capture format it negotiated; keep sending
         * that geometry so later AOUs do not trip its mismatch check. */
        if (out.Width != 0 && out.Height != 0) {
            m->send_w = out.Width;
            m->send_h = out.Height;
        }
    }
    m->inflight_gen = au->gen;
    m->inflight_time = au->time;
    m->inflight_dur = au->duration;
    m->in_flight = 1;
    LeaveCriticalSection(&m->lock);

    if (m->drv_size == 0) {
        dvm_logf("driver accepted %ux%u but reported CaptureSizeImage=0", out.Width, out.Height);
        return 0;
    }
    {
        uint32_t need = m->drv_size;
        uint32_t tight = m->drv_stride ? m->drv_stride * m->drv_h * 3u / 2u : 0;
        if (tight > need)
            need = tight;
        if (!ensure_scratch(m, need + 64u)) {
            EnterCriticalSection(&m->lock);
            m->in_flight = 0;
            LeaveCriticalSection(&m->lock);
            return 0;
        }
    }
    if (m->aus_in == 1) {
        dvm_logf("first SEND ok: capture %ux%u stride=%u sizeimage=%u, sends now %ux%u",
                 out.Width, out.Height, out.CaptureStride, out.CaptureSizeImage,
                 m->send_w, m->send_h);
    }
    return 1;
}

/* Copy one decoded NV12 frame (driver layout: stride bytes/row, coded width x
 * coded height) into a display-sized tight NV12 frame.  Rows/columns beyond the
 * source are filled with limited-range black; rows beyond the display height are
 * dropped (the 1080p case: 1088 visible lines -> 1080). */
static void copy_frame(DvmMft *m, DvmFrame *f, uint32_t out_w, uint32_t out_h,
                       const uint8_t *src, uint32_t src_w, uint32_t src_h,
                       uint32_t stride)
{
    uint32_t y;
    uint32_t cw = out_w / 2u;
    uint32_t ch = out_h / 2u;
    uint8_t *dst_y = f->data;
    uint8_t *dst_uv = f->data + (size_t)out_w * out_h;
    const uint8_t *src_uv = src + (size_t)stride * src_h;
    uint32_t copy_w;

    for (y = 0; y < out_h; y++) {
        uint8_t *row = dst_y + (size_t)y * out_w;
        if (y < src_h) {
            copy_w = (src_w < out_w) ? src_w : out_w;
            memcpy(row, src + (size_t)y * stride, copy_w);
            if (copy_w < out_w)
                memset(row + copy_w, 16, out_w - copy_w);
        } else {
            memset(row, 16, out_w);
        }
    }
    for (y = 0; y < ch; y++) {
        uint8_t *row = dst_uv + (size_t)y * out_w;
        if (y < src_h / 2u) {
            copy_w = (src_w < out_w) ? src_w : out_w;
            memcpy(row, src_uv + (size_t)y * stride, copy_w);
            if (copy_w < out_w)
                memset(row + copy_w, 128, out_w - copy_w);
        } else {
            memset(row, 128, out_w);
        }
    }
    (void)cw;
}

static void recv_frame(DvmMft *m)
{
    VM_FRAME_INFO info;
    uint32_t gen;
    int ok = 0;
    uint32_t out_w, out_h;
    uint32_t err = 0;

    memset(&info, 0, sizeof(info));

    EnterCriticalSection(&m->lock);
    gen = m->inflight_gen;
    out_w = m->out_w;
    out_h = m->out_h;
    LeaveCriticalSection(&m->lock);

    if (m->session != NULL && m->scratch != NULL && m->scratch_cap != 0)
        ok = vm_get_frame(m->session, m->scratch, m->scratch_cap, &info,
                          DVM_GET_FRAME_TIMEOUT_MS);
    if (!ok)
        err = vm_last_error();

    if (ok && (info.Width == 0 || info.Height == 0 || info.Size == 0)) {
        dvm_logf("vm_get_frame returned an empty frame (w=%u h=%u size=%u)",
                 info.Width, info.Height, info.Size);
        ok = 0;
    }

    if (ok) {
        DvmFrame f;
        memset(&f, 0, sizeof(f));
        f.len = out_w * out_h * 3u / 2u;
        f.data = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, f.len);
        if (f.data == NULL) {
            dvm_logf("HeapAlloc(%u) for a decoded frame failed", f.len);
            ok = 0;
        } else {
            copy_frame(m, &f, out_w, out_h, m->scratch, info.Width, info.Height,
                       info.Stride);
            f.time = m->inflight_time;
            f.duration = m->inflight_dur;
        }

        EnterCriticalSection(&m->lock);
        m->in_flight = 0;
        if (f.data != NULL) {
            if (gen != m->gen || m->frame_count >= DVM_FRAME_SLOTS) {
                HeapFree(GetProcessHeap(), 0, f.data);
            } else {
                m->frames[m->frame_count++] = f;
                m->frames_out++;
                m->consec_timeouts = 0;
                if (!m->have_output_posted) {
                    m->have_output_posted = 1;
                    LeaveCriticalSection(&m->lock);
                    post_event(m, METransformHaveOutput, S_OK);
                    return;
                }
            }
        }
        LeaveCriticalSection(&m->lock);
        return;
    }

    EnterCriticalSection(&m->lock);
    m->in_flight = 0;
    m->get_timeouts++;
    if (++m->consec_timeouts >= DVM_MAX_CONSEC_TIMEOUTS) {
        m->consec_timeouts = 0;
        m->reset_req = 1;
    }
    if (m->get_timeouts <= 3 || (m->get_timeouts % 30) == 0) {
        dvm_logf("vm_get_frame(%u ms) failed err=%u (%llu timeouts) - dropping the decoded frame",
                 (unsigned)DVM_GET_FRAME_TIMEOUT_MS, err,
                 (unsigned long long)m->get_timeouts);
    }
    LeaveCriticalSection(&m->lock);
}

/* ------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* ------------------------------------------------------------------------- */

static void dvm_free_queues_locked(DvmMft *m)
{
    uint32_t i;

    while (m->au_count > 0) {
        DvmAu *au = &m->au_q[m->au_head];
        if (au->data != NULL)
            HeapFree(GetProcessHeap(), 0, au->data);
        memset(au, 0, sizeof(*au));
        m->au_head = (m->au_head + 1u) % DVM_AU_QUEUE;
        m->au_count--;
    }
    m->au_head = 0;
    for (i = 0; i < m->frame_count; i++) {
        if (m->frames[i].data != NULL)
            HeapFree(GetProcessHeap(), 0, m->frames[i].data);
        memset(&m->frames[i], 0, sizeof(m->frames[i]));
    }
    m->frame_count = 0;
    m->in_flight = 0;
}

static DWORD WINAPI dvm_worker_main(LPVOID param)
{
    DvmMft *m = (DvmMft *)param;

    for (;;) {
        DvmAu au;
        int do_reset = 0;
        int do_flush = 0;
        int do_send = 0;
        int do_recv = 0;
        int do_drain = 0;
        int post_need = 0;

        memset(&au, 0, sizeof(au));

        EnterCriticalSection(&m->lock);
        if (m->stop) {
            LeaveCriticalSection(&m->lock);
            break;
        }
        if (m->reset_req) {
            m->reset_req = 0;
            do_reset = 1;
        } else if (m->flush_req) {
            m->flush_req = 0;
            do_flush = 1;
        } else if (m->au_count != 0 && !m->in_flight &&
                   m->frame_count < DVM_FRAME_SLOTS) {
            au = m->au_q[m->au_head];
            memset(&m->au_q[m->au_head], 0, sizeof(m->au_q[m->au_head]));
            m->au_head = (m->au_head + 1u) % DVM_AU_QUEUE;
            m->au_count--;
            do_send = 1;
            if (m->au_count < DVM_AU_QUEUE && !m->need_input_posted) {
                m->need_input_posted = 1;
                post_need = 1;
            }
        } else if (m->in_flight && m->frame_count < DVM_FRAME_SLOTS) {
            do_recv = 1;
        } else if (m->draining && !m->drain_posted) {
            m->drain_posted = 1;
            m->drained = 1;
            do_drain = 1;
        }
        LeaveCriticalSection(&m->lock);

        if (do_reset) {
            dvm_logf("worker: resetting the driver session");
            EnterCriticalSection(&m->lock);
            /* The queued AUs belong to the old geometry/stream and the driver
             * session that decodes them is about to disappear. */
            dvm_free_queues_locked(m);
            m->gen++;
            m->draining = 0;
            m->drained = 0;
            m->drain_posted = 0;
            m->have_output_posted = 0;
            m->resets++;
            m->fatal = S_OK;
            m->consec_timeouts = 0;
            LeaveCriticalSection(&m->lock);
            close_session(m);
            EnterCriticalSection(&m->lock);
            if (m->au_count < DVM_AU_QUEUE && !m->need_input_posted) {
                m->need_input_posted = 1;
                post_need = 1;
            }
            LeaveCriticalSection(&m->lock);
        } else if (do_flush) {
            VM_FLUSH_OUT fout;
            uint64_t dropped_au;
            memset(&fout, 0, sizeof(fout));
            EnterCriticalSection(&m->lock);
            dropped_au = m->au_count;
            dvm_free_queues_locked(m);
            m->gen++;
            m->draining = 0;
            m->drained = 0;
            m->drain_posted = 0;
            m->need_input_posted = 0;
            m->have_output_posted = 0;
            m->fatal = S_OK;
            m->consec_timeouts = 0;
            m->flushes++;
            m->dropped += dropped_au;
            LeaveCriticalSection(&m->lock);
            if (m->session != NULL) {
                if (!vm_flush(m->session, &fout))
                    dvm_logf("vm_flush failed err=%u", vm_last_error());
                else
                    dvm_logf("vm_flush ok: session %u, dropped %u AU / %u frames",
                             fout.SessionId, fout.DroppedAu, fout.DroppedFrames);
            }
            EnterCriticalSection(&m->lock);
            if (m->au_count < DVM_AU_QUEUE && !m->need_input_posted) {
                m->need_input_posted = 1;
                post_need = 1;
            }
            LeaveCriticalSection(&m->lock);
        } else if (do_send) {
            if (!send_au(m, &au)) {
                EnterCriticalSection(&m->lock);
                m->in_flight = 0;
                if (m->au_count < DVM_AU_QUEUE && !m->need_input_posted) {
                    m->need_input_posted = 1;
                    post_need = 1;
                }
                LeaveCriticalSection(&m->lock);
            }
            if (au.data != NULL)
                HeapFree(GetProcessHeap(), 0, au.data);
        } else if (do_recv) {
            recv_frame(m);
            EnterCriticalSection(&m->lock);
            if (m->au_count < DVM_AU_QUEUE && !m->need_input_posted) {
                m->need_input_posted = 1;
                post_need = 1;
            }
            LeaveCriticalSection(&m->lock);
        } else if (do_drain) {
            EnterCriticalSection(&m->lock);
            m->drains++;
            LeaveCriticalSection(&m->lock);
            dvm_logf("worker: drain complete");
            post_event(m, METransformDrainComplete, S_OK);
        } else {
            WaitForSingleObject(m->wake, 200);
        }

        if (post_need) {
            post_event(m, METransformNeedInput, S_OK);
            post_need = 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* IMFTransform - stream description                                          */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE mft_GetStreamLimits(IMFTransform *This,
                                                      DWORD *in_min, DWORD *in_max,
                                                      DWORD *out_min, DWORD *out_max)
{
    (void)This;
    if (in_min == NULL || in_max == NULL || out_min == NULL || out_max == NULL)
        return E_POINTER;
    *in_min = 1;
    *in_max = 1;
    *out_min = 1;
    *out_max = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetStreamCount(IMFTransform *This,
                                                     DWORD *inputs, DWORD *outputs)
{
    (void)This;
    if (inputs == NULL || outputs == NULL)
        return E_POINTER;
    *inputs = 1;
    *outputs = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetStreamIDs(IMFTransform *This, DWORD in_size,
                                                   DWORD *in_ids, DWORD out_size,
                                                   DWORD *out_ids)
{
    (void)This;
    (void)in_size;
    (void)in_ids;
    (void)out_size;
    (void)out_ids;
    /* One stream each: the default stream identifiers 0 are used and the
     * caller does not have to enumerate anything. */
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE mft_GetInputStreamInfo(IMFTransform *This, DWORD id,
                                                         MFT_INPUT_STREAM_INFO *info)
{
    (void)This;
    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (info == NULL)
        return E_POINTER;
    memset(info, 0, sizeof(*info));
    info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES |
                    MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
    info->cbSize = 0;          /* an H.264 AU is variable sized */
    info->cbMaxLookahead = 0;
    info->cbAlignment = 0;
    info->hnsMaxLatency = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetOutputStreamInfo(IMFTransform *This, DWORD id,
                                                          MFT_OUTPUT_STREAM_INFO *info)
{
    DvmMft *m = (DvmMft *)This;
    uint32_t out_w, out_h;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (info == NULL)
        return E_POINTER;

    EnterCriticalSection(&m->lock);
    out_w = m->out_w ? m->out_w : (m->in_w ? m->in_w : 1920u);
    out_h = m->out_h ? m->out_h : (m->in_h ? m->in_h : 1080u);
    LeaveCriticalSection(&m->lock);

    memset(info, 0, sizeof(*info));
    info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES |
                    MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                    MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE |
                    MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES;
    info->cbSize = out_w * out_h * 3u / 2u;
    info->cbAlignment = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetAttributes(IMFTransform *This,
                                                    IMFAttributes **attributes)
{
    DvmMft *m = (DvmMft *)This;

    if (attributes == NULL)
        return E_POINTER;
    *attributes = NULL;
    if (m->attributes == NULL)
        return E_UNEXPECTED;
    *attributes = m->attributes;
    IMFAttributes_AddRef(m->attributes);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetInputStreamAttributes(IMFTransform *This,
                                                               DWORD id,
                                                               IMFAttributes **attributes)
{
    (void)This;
    (void)id;
    (void)attributes;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE mft_GetOutputStreamAttributes(IMFTransform *This,
                                                                DWORD id,
                                                                IMFAttributes **attributes)
{
    (void)This;
    (void)id;
    (void)attributes;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE mft_DeleteInputStream(IMFTransform *This, DWORD id)
{
    (void)This;
    (void)id;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE mft_AddInputStreams(IMFTransform *This, DWORD streams,
                                                      DWORD *ids)
{
    (void)This;
    (void)streams;
    (void)ids;
    return E_NOTIMPL;
}

/* ------------------------------------------------------------------------- */
/* IMFTransform - media types                                                 */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE mft_GetInputAvailableType(IMFTransform *This, DWORD id,
                                                            DWORD index,
                                                            IMFMediaType **type)
{
    HRESULT hr;
    IMFMediaType *mt = NULL;

    (void)This;
    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    if (index > 1)
        return MF_E_NO_MORE_TYPES;

    hr = MFCreateMediaType(&mt);
    if (FAILED(hr))
        return hr;
    hr = IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        const GUID *sub = (index == 0) ? &MFVideoFormat_H264 : &MFVideoFormat_H264_ES;
        hr = IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, sub);
    }
    if (FAILED(hr)) {
        IMFMediaType_Release(mt);
        return hr;
    }
    *type = mt;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetOutputAvailableType(IMFTransform *This, DWORD id,
                                                             DWORD index,
                                                             IMFMediaType **type)
{
    DvmMft *m = (DvmMft *)This;
    IMFMediaType *mt = NULL;
    HRESULT hr;
    uint32_t out_w, out_h;
    UINT64 fps;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    if (index != 0)
        return MF_E_NO_MORE_TYPES;

    EnterCriticalSection(&m->lock);
    out_w = m->out_w ? m->out_w : (m->in_w ? m->in_w : 1920u);
    out_h = m->out_h ? m->out_h : (m->in_h ? m->in_h : 1080u);
    fps = m->frame_rate;
    LeaveCriticalSection(&m->lock);

    hr = MFCreateMediaType(&mt);
    if (FAILED(hr))
        return hr;
    hr = IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, DVM_PACK2(out_w, out_h));
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE,
                                    fps ? fps : DVM_PACK2(30, 1));
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_PIXEL_ASPECT_RATIO, DVM_PACK2(1, 1));
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE,
                                    (UINT32)MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_DEFAULT_STRIDE, out_w);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_YUV_MATRIX,
                                    (out_w >= 1024)
                                        ? (UINT32)MFVideoTransferMatrix_BT709
                                        : (UINT32)MFVideoTransferMatrix_BT601);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_VIDEO_NOMINAL_RANGE,
                                    (UINT32)MFNominalRange_16_235);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_VIDEO_PRIMARIES,
                                    (out_w >= 1024)
                                        ? (UINT32)MFVideoPrimaries_BT709
                                        : (UINT32)MFVideoPrimaries_SMPTE170M);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_TRANSFER_FUNCTION,
                                    (out_w >= 1024)
                                        ? (UINT32)MFVideoTransFunc_709
                                        : (UINT32)MFVideoTransFunc_Unknown);

    if (FAILED(hr)) {
        IMFMediaType_Release(mt);
        return hr;
    }
    *type = mt;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_SetInputType(IMFTransform *This, DWORD id,
                                                   IMFMediaType *type, DWORD flags)
{
    DvmMft *m = (DvmMft *)This;
    GUID major = {0}, subtype = {0};
    HRESULT hr;
    UINT64 frame_size = 0;
    BYTE *seq = NULL;
    UINT32 seq_size = 0;
    int geometry_changed = 0;
    uint32_t old_send_w, old_send_h;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (!async_unlocked(m))
        return MF_E_TRANSFORM_ASYNC_LOCKED;

    if (type == NULL) {
        EnterCriticalSection(&m->lock);
        if (m->in_type != NULL) {
            IMFMediaType_Release(m->in_type);
            m->in_type = NULL;
        }
        m->in_w = m->in_h = 0;
        m->send_w = m->send_h = 0;
        h264_bits_init(&m->bits);
        m->reset_req = 1;
        LeaveCriticalSection(&m->lock);
        SetEvent(m->wake);
        return S_OK;
    }

    hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || !IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;
    hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr))
        return MF_E_INVALIDMEDIATYPE;
    if (!IsEqualGUID(&subtype, &MFVideoFormat_H264) &&
        !IsEqualGUID(&subtype, &MFVideoFormat_H264_ES))
        return MF_E_INVALIDMEDIATYPE;

    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    EnterCriticalSection(&m->lock);
    old_send_w = m->send_w;
    old_send_h = m->send_h;

    if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)) &&
        frame_size != 0) {
        uint32_t w = DVM_HI32(frame_size);
        uint32_t h = DVM_LO32(frame_size);
        if (w >= 16 && h >= 16 && w <= 4096 && h <= 4096) {
            m->in_w = w & ~1u;
            m->in_h = h & ~1u;
        }
    }
    if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_RATE, &m->frame_rate))) {
        /* keep it; GetOutputAvailableType republishes it */
    }

    /* The sequence header often carries the SPS/PPS (and therefore the coded
     * size) before the first sample arrives.  Both an
     * AVCDecoderConfigurationRecord and a raw Annex-B pair are accepted. */
    if (SUCCEEDED(IMFAttributes_GetAllocatedBlob((IMFAttributes *)type,
                                                 &MF_MT_MPEG_SEQUENCE_HEADER,
                                                 &seq, &seq_size)) &&
        seq != NULL && seq_size != 0) {
        h264_bits_set_sequence_header(&m->bits, seq, seq_size);
        CoTaskMemFree(seq);
        seq = NULL;
    }
    update_geometry_locked(m);
    geometry_changed = (old_send_w != m->send_w || old_send_h != m->send_h);
    LeaveCriticalSection(&m->lock);

    hr = copy_attribute_type(&m->in_type, type);
    if (FAILED(hr))
        return hr;

    dvm_logf("SetInputType: subtype=%s in=%ux%u send=%ux%u",
             IsEqualGUID(&subtype, &MFVideoFormat_H264_ES) ? "H264_ES" : "H264",
             m->in_w, m->in_h, m->send_w, m->send_h);

    if (geometry_changed && m->session_open) {
        EnterCriticalSection(&m->lock);
        m->reset_req = 1;
        LeaveCriticalSection(&m->lock);
        SetEvent(m->wake);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_SetOutputType(IMFTransform *This, DWORD id,
                                                    IMFMediaType *type, DWORD flags)
{
    DvmMft *m = (DvmMft *)This;
    GUID major = {0}, subtype = {0};
    HRESULT hr;
    UINT64 frame_size = 0;
    uint32_t w = 0, h = 0;
    uint32_t in_w, in_h;
    int post_need = 0;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (!async_unlocked(m))
        return MF_E_TRANSFORM_ASYNC_LOCKED;

    if (type == NULL) {
        EnterCriticalSection(&m->lock);
        if (m->out_type != NULL) {
            IMFMediaType_Release(m->out_type);
            m->out_type = NULL;
        }
        m->out_w = m->out_h = 0;
        LeaveCriticalSection(&m->lock);
        return S_OK;
    }

    hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || !IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;
    hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr) || !IsEqualGUID(&subtype, &MFVideoFormat_NV12))
        return MF_E_INVALIDMEDIATYPE;

    EnterCriticalSection(&m->lock);
    in_w = m->in_w;
    in_h = m->in_h;
    LeaveCriticalSection(&m->lock);

    if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)) &&
        frame_size != 0) {
        w = DVM_HI32(frame_size);
        h = DVM_LO32(frame_size);
    } else if (in_w != 0 && in_h != 0) {
        w = in_w;
        h = in_h;
    } else {
        return MF_E_INVALIDMEDIATYPE;
    }
    if (w < 16 || h < 16 || w > 4096 || h > 4096 || (w & 1u) || (h & 1u))
        return MF_E_INVALIDMEDIATYPE;
    if (in_w != 0 && in_h != 0 && (w != in_w || h != in_h))
        return MF_E_INVALIDMEDIATYPE;

    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    hr = copy_attribute_type(&m->out_type, type);
    if (FAILED(hr))
        return hr;
    /* Fill in the attributes a decoder output type is expected to carry even
     * when the caller's type was sparse. */
    if (FAILED(IMFAttributes_GetUINT64((IMFAttributes *)m->out_type,
                                       &MF_MT_FRAME_SIZE, &frame_size))) {
        (void)IMFAttributes_SetUINT64((IMFAttributes *)m->out_type, &MF_MT_FRAME_SIZE,
                                      DVM_PACK2(w, h));
    }
    (void)IMFAttributes_SetUINT32((IMFAttributes *)m->out_type, &MF_MT_DEFAULT_STRIDE, w);
    (void)IMFAttributes_SetUINT32((IMFAttributes *)m->out_type, &MF_MT_INTERLACE_MODE,
                                  (UINT32)MFVideoInterlace_Progressive);
    (void)IMFAttributes_SetUINT32((IMFAttributes *)m->out_type,
                                  &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);

    EnterCriticalSection(&m->lock);
    m->out_w = w;
    m->out_h = h;
    if (m->streaming && m->in_type != NULL && !m->need_input_posted) {
        m->need_input_posted = 1;
        post_need = 1;
    }
    LeaveCriticalSection(&m->lock);

    dvm_logf("SetOutputType: NV12 %ux%u (%u bytes/frame)", w, h, w * h * 3u / 2u);
    if (post_need)
        post_event(m, METransformNeedInput, S_OK);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetInputCurrentType(IMFTransform *This, DWORD id,
                                                          IMFMediaType **type)
{
    DvmMft *m = (DvmMft *)This;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    EnterCriticalSection(&m->lock);
    if (m->in_type == NULL) {
        LeaveCriticalSection(&m->lock);
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    *type = m->in_type;
    IMFMediaType_AddRef(m->in_type);
    LeaveCriticalSection(&m->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetOutputCurrentType(IMFTransform *This, DWORD id,
                                                           IMFMediaType **type)
{
    DvmMft *m = (DvmMft *)This;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    EnterCriticalSection(&m->lock);
    if (m->out_type == NULL) {
        LeaveCriticalSection(&m->lock);
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    *type = m->out_type;
    IMFMediaType_AddRef(m->out_type);
    LeaveCriticalSection(&m->lock);
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* IMFTransform - status and messages                                         */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE mft_GetInputStatus(IMFTransform *This, DWORD id,
                                                     DWORD *flags)
{
    DvmMft *m = (DvmMft *)This;
    int accept = 0;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (flags == NULL)
        return E_POINTER;

    EnterCriticalSection(&m->lock);
    accept = m->in_type != NULL && m->out_type != NULL && !m->draining &&
             !m->drained && m->fatal == S_OK && m->au_count < DVM_AU_QUEUE;
    LeaveCriticalSection(&m->lock);

    *flags = accept ? MFT_INPUT_STATUS_ACCEPT_DATA : 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_GetOutputStatus(IMFTransform *This, DWORD *flags)
{
    DvmMft *m = (DvmMft *)This;

    if (flags == NULL)
        return E_POINTER;
    EnterCriticalSection(&m->lock);
    *flags = (m->frame_count != 0) ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
    LeaveCriticalSection(&m->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_SetOutputBounds(IMFTransform *This, LONGLONG lower,
                                                      LONGLONG upper)
{
    (void)This;
    (void)lower;
    (void)upper;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE mft_ProcessEvent(IMFTransform *This, DWORD id,
                                                   IMFMediaEvent *event)
{
    (void)This;
    (void)id;
    (void)event;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE mft_ProcessMessage(IMFTransform *This,
                                                     MFT_MESSAGE_TYPE message,
                                                     ULONG_PTR param)
{
    DvmMft *m = (DvmMft *)This;
    int post_need = 0;

    switch (message) {
    case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
        EnterCriticalSection(&m->lock);
        m->streaming = 1;
        m->draining = 0;
        m->drained = 0;
        m->drain_posted = 0;
        if (m->in_type != NULL && m->out_type != NULL && !m->need_input_posted) {
            m->need_input_posted = 1;
            post_need = 1;
        }
        LeaveCriticalSection(&m->lock);
        dvm_logf("BeginStreaming (NeedInput posted=%d)", post_need);
        if (post_need)
            post_event(m, METransformNeedInput, S_OK);
        return S_OK;

    case MFT_MESSAGE_NOTIFY_END_STREAMING:
        EnterCriticalSection(&m->lock);
        m->streaming = 0;
        LeaveCriticalSection(&m->lock);
        dvm_logf("EndStreaming");
        return S_OK;

    case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
        EnterCriticalSection(&m->lock);
        m->draining = 0;
        m->drained = 0;
        m->drain_posted = 0;
        LeaveCriticalSection(&m->lock);
        return S_OK;

    case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
        EnterCriticalSection(&m->lock);
        m->draining = 1;
        m->drain_posted = 0;
        LeaveCriticalSection(&m->lock);
        SetEvent(m->wake);
        return S_OK;

    case MFT_MESSAGE_COMMAND_FLUSH:
        EnterCriticalSection(&m->lock);
        m->flush_req = 1;
        m->draining = 0;
        m->drained = 0;
        m->drain_posted = 0;
        m->need_input_posted = 0;
        m->have_output_posted = 0;
        m->gen++;
        LeaveCriticalSection(&m->lock);
        SetEvent(m->wake);
        dvm_logf("CommandFlush");
        return S_OK;

    case MFT_MESSAGE_COMMAND_DRAIN:
        EnterCriticalSection(&m->lock);
        m->draining = 1;
        m->drained = 0;
        m->drain_posted = 0;
        LeaveCriticalSection(&m->lock);
        SetEvent(m->wake);
        dvm_logf("CommandDrain");
        return S_OK;

    case MFT_MESSAGE_SET_D3D_MANAGER:
        EnterCriticalSection(&m->lock);
        m->d3d_manager_seen++;
        LeaveCriticalSection(&m->lock);
        if (m->d3d_manager_seen == 1)
            dvm_logf("SetD3DManager(%p): accepted and ignored (system-memory NV12 only)",
                     (void *)param);
        return S_OK;

    case MFT_MESSAGE_DROP_SAMPLES:
    case MFT_MESSAGE_COMMAND_MARKER:
    case MFT_MESSAGE_NOTIFY_RELEASE_RESOURCES:
    case MFT_MESSAGE_NOTIFY_REACQUIRE_RESOURCES:
    case MFT_MESSAGE_COMMAND_TICK:
    case MFT_MESSAGE_NOTIFY_EVENT:
    case MFT_MESSAGE_COMMAND_SET_OUTPUT_STREAM_STATE:
    case MFT_MESSAGE_COMMAND_FLUSH_OUTPUT_STREAM:
    default:
        return S_OK;
    }
}

/* ------------------------------------------------------------------------- */
/* IMFTransform - data                                                        */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE mft_ProcessInput(IMFTransform *This, DWORD id,
                                                   IMFSample *sample, DWORD flags)
{
    DvmMft *m = (DvmMft *)This;
    IMFMediaBuffer *buffer = NULL;
    BYTE *ptr = NULL;
    DWORD length = 0;
    DWORD max_len = 0;
    LONGLONG time = 0;
    LONGLONG duration = 0;
    uint8_t *au_data = NULL;
    size_t au_len = 0;
    size_t au_cap;
    HRESULT hr = S_OK;
    int post_need = 0;
    DvmAu au;

    (void)flags;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (sample == NULL)
        return E_INVALIDARG;
    if (!async_unlocked(m))
        return MF_E_TRANSFORM_ASYNC_LOCKED;

    EnterCriticalSection(&m->lock);
    if (m->in_type == NULL || m->out_type == NULL) {
        LeaveCriticalSection(&m->lock);
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    if (m->fatal != S_OK) {
        hr = m->fatal;
        LeaveCriticalSection(&m->lock);
        return hr;
    }
    if (m->drained) {
        /* The client drained and is now feeding new data; accept it again. */
        m->draining = 0;
        m->drained = 0;
        m->drain_posted = 0;
    }
    if (m->draining || m->au_count >= DVM_AU_QUEUE) {
        LeaveCriticalSection(&m->lock);
        return MF_E_NOTACCEPTING;
    }
    LeaveCriticalSection(&m->lock);

    if (SUCCEEDED(IMFSample_GetSampleTime(sample, &time)) &&
        time == (LONGLONG)0x7FFFFFFFFFFFFFFFLL) {
        time = 0;
    }
    (void)IMFSample_GetSampleDuration(sample, &duration);

    hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr))
        return hr;
    hr = IMFMediaBuffer_Lock(buffer, &ptr, &max_len, &length);
    if (FAILED(hr)) {
        IMFMediaBuffer_Release(buffer);
        return hr;
    }
    if (ptr == NULL || length == 0) {
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        dvm_logf("ProcessInput: empty sample (%u bytes)", length);
        return E_INVALIDARG;
    }

    au_cap = (size_t)length + H264_CONFIG_MAX + 16u;
    au_data = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, au_cap);
    if (au_data == NULL) {
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        return E_OUTOFMEMORY;
    }

    EnterCriticalSection(&m->lock);
    /* Learn SPS/PPS (and the geometry) from this AU before normalising it. */
    h264_bits_update(&m->bits, ptr, length);
    update_geometry_locked(m);
    if (!m->geometry_warned && m->out_w && m->in_w &&
        (m->out_w != m->in_w || m->out_h != m->in_h)) {
        m->geometry_warned = 1;
        dvm_logf("output media type is %ux%u but the stream is %ux%u: the frame "
                 "is cropped or padded into the negotiated size",
                 m->out_w, m->out_h, m->in_w, m->in_h);
    }
    au_len = h264_bits_build_au(&m->bits, ptr, length, au_data, au_cap);
    if (au_len == 0) {
        LeaveCriticalSection(&m->lock);
        HeapFree(GetProcessHeap(), 0, au_data);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        dvm_logf("ProcessInput: h264_bits_build_au refused a %u byte sample", length);
        return E_INVALIDARG;
    }
    memset(&au, 0, sizeof(au));
    au.data = au_data;
    au.len = (uint32_t)au_len;
    au.time = time;
    au.duration = duration;
    au.gen = m->gen;
    m->au_q[(m->au_head + m->au_count) % DVM_AU_QUEUE] = au;
    m->au_count++;
    m->aus_in++;
    if (m->need_input_posted)
        m->need_input_posted = 0;
    else if (m->no_need_log < 3) {
        m->no_need_log++;
        dvm_logf("ProcessInput without a pending NeedInput (client is not waiting)");
    }
    if (m->au_count < DVM_AU_QUEUE) {
        m->need_input_posted = 1;
        post_need = 1;
    }
    if (m->aus_in <= 2) {
        dvm_logf("ProcessInput #%llu: %u B in -> %u B AU (%s), %s, time=%lld",
                 (unsigned long long)m->aus_in, length, (unsigned)au_len,
                 h264_bits_au_is_idr(ptr, length) ? "IDR" : "non-IDR",
                 (m->bits.config_len != 0) ? "SPS/PPS cached" : "no SPS/PPS yet",
                 (long long)time);
    }
    LeaveCriticalSection(&m->lock);

    IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);
    SetEvent(m->wake);
    if (post_need)
        post_event(m, METransformNeedInput, S_OK);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mft_ProcessOutput(IMFTransform *This, DWORD flags,
                                                    DWORD count,
                                                    MFT_OUTPUT_DATA_BUFFER *buffers,
                                                    DWORD *status)
{
    DvmMft *m = (DvmMft *)This;
    DvmFrame frame;
    IMFMediaBuffer *buffer = NULL;
    IMFSample *sample = NULL;
    BYTE *ptr = NULL;
    DWORD max_len = 0;
    HRESULT hr;
    int post_have = 0;
    int own_sample = 0;
    int stale = 0;

    (void)flags;

    if (!async_unlocked(m))
        return MF_E_TRANSFORM_ASYNC_LOCKED;
    if (status == NULL)
        return E_POINTER;
    *status = 0;
    if (count != 1 || buffers == NULL)
        return E_INVALIDARG;
    if (buffers[0].dwStreamID != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    buffers[0].dwStatus = 0;
    buffers[0].pEvents = NULL;

    EnterCriticalSection(&m->lock);
    if (m->out_type == NULL) {
        LeaveCriticalSection(&m->lock);
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    if (m->fatal != S_OK) {
        hr = m->fatal;
        LeaveCriticalSection(&m->lock);
        return hr;
    }
    if (m->frame_count == 0) {
        LeaveCriticalSection(&m->lock);
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    frame = m->frames[0];
    LeaveCriticalSection(&m->lock);

    if (buffers[0].pSample != NULL) {
        sample = buffers[0].pSample;
        IMFSample_AddRef(sample);
        hr = IMFSample_GetBufferByIndex(sample, 0, &buffer);
        if (FAILED(hr)) {
            IMFSample_Release(sample);
            return hr;
        }
        hr = IMFMediaBuffer_GetMaxLength(buffer, &max_len);
        if (FAILED(hr) || max_len < frame.len) {
            IMFMediaBuffer_Release(buffer);
            IMFSample_Release(sample);
            dvm_logf("ProcessOutput: provided sample is %u bytes, need %u",
                     (unsigned)max_len, frame.len);
            return MF_E_BUFFERTOOSMALL;
        }
    } else {
        hr = MFCreateSample(&sample);
        if (FAILED(hr))
            return hr;
        own_sample = 1;
        hr = MFCreateMemoryBuffer(frame.len, &buffer);
        if (SUCCEEDED(hr))
            hr = IMFSample_AddBuffer(sample, buffer);
        if (FAILED(hr)) {
            if (buffer != NULL)
                IMFMediaBuffer_Release(buffer);
            IMFSample_Release(sample);
            return hr;
        }
    }

    hr = IMFMediaBuffer_Lock(buffer, &ptr, &max_len, NULL);
    if (FAILED(hr) || ptr == NULL || max_len < frame.len) {
        if (SUCCEEDED(hr))
            (void)IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        IMFSample_Release(sample);
        if (SUCCEEDED(hr))
            return MF_E_BUFFERTOOSMALL;
        return hr;
    }

    /* Copy inside the lock: MFT_MESSAGE_COMMAND_FLUSH and the worker's session
     * reset free the frame ring under the same lock, so a frame we announced
     * can disappear before we get here (seek while the renderer is pulling). */
    EnterCriticalSection(&m->lock);
    if (m->frame_count != 0 && m->frames[0].data == frame.data) {
        memcpy(ptr, frame.data, frame.len);
        HeapFree(GetProcessHeap(), 0, m->frames[0].data);
        memmove(&m->frames[0], &m->frames[1],
                sizeof(m->frames[0]) * (DVM_FRAME_SLOTS - 1));
        memset(&m->frames[DVM_FRAME_SLOTS - 1], 0, sizeof(m->frames[0]));
        m->frame_count--;
    } else {
        stale = 1;
    }
    if (m->frame_count != 0) {
        if (!m->have_output_posted) {
            m->have_output_posted = 1;
            post_have = 1;
        }
    } else {
        m->have_output_posted = 0;
    }
    LeaveCriticalSection(&m->lock);
    (void)IMFMediaBuffer_Unlock(buffer);
    IMFMediaBuffer_Release(buffer);
    if (post_have)
        post_event(m, METransformHaveOutput, S_OK);

    if (stale) {
        IMFSample_Release(sample);
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    {
        IMFMediaBuffer *cur = NULL;
        hr = IMFSample_GetBufferByIndex(sample, 0, &cur);
        if (SUCCEEDED(hr)) {
            (void)IMFMediaBuffer_SetCurrentLength(cur, (DWORD)frame.len);
            IMFMediaBuffer_Release(cur);
        }
    }
    (void)IMFSample_SetSampleTime(sample, frame.time);
    (void)IMFSample_SetSampleDuration(sample, frame.duration);

    if (own_sample) {
        /* The caller owns the reference we created. */
        buffers[0].pSample = sample;
    } else {
        IMFSample_Release(sample);
    }
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* IMFMediaEventGenerator                                                     */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE evt_QueryInterface(IMFMediaEventGenerator *This,
                                                     REFIID riid, void **ppv);
static ULONG STDMETHODCALLTYPE evt_AddRef(IMFMediaEventGenerator *This);
static ULONG STDMETHODCALLTYPE evt_Release(IMFMediaEventGenerator *This);

#define DVM_FROM_EVENT_GEN(p) \
    ((DvmMft *)(void *)((uint8_t *)(p) - offsetof(DvmMft, lpEventVtbl)))

static HRESULT STDMETHODCALLTYPE evt_QueryInterface(IMFMediaEventGenerator *This,
                                                     REFIID riid, void **ppv)
{
    return mft_QueryInterface((IMFTransform *)DVM_FROM_EVENT_GEN(This), riid, ppv);
}

static ULONG STDMETHODCALLTYPE evt_AddRef(IMFMediaEventGenerator *This)
{
    return mft_AddRef((IMFTransform *)DVM_FROM_EVENT_GEN(This));
}

static ULONG STDMETHODCALLTYPE evt_Release(IMFMediaEventGenerator *This)
{
    return mft_Release((IMFTransform *)DVM_FROM_EVENT_GEN(This));
}

static HRESULT STDMETHODCALLTYPE evt_BeginGetEvent(IMFMediaEventGenerator *This,
                                                    IMFAsyncCallback *callback,
                                                    IUnknown *state)
{
    DvmMft *m = DVM_FROM_EVENT_GEN(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_BeginGetEvent(m->events, callback, state);
}

static HRESULT STDMETHODCALLTYPE evt_EndGetEvent(IMFMediaEventGenerator *This,
                                                  IMFAsyncResult *result,
                                                  IMFMediaEvent **event)
{
    DvmMft *m = DVM_FROM_EVENT_GEN(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_EndGetEvent(m->events, result, event);
}

static HRESULT STDMETHODCALLTYPE evt_GetEvent(IMFMediaEventGenerator *This, DWORD flags,
                                               IMFMediaEvent **event)
{
    DvmMft *m = DVM_FROM_EVENT_GEN(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_GetEvent(m->events, flags, event);
}

static HRESULT STDMETHODCALLTYPE evt_QueueEvent(IMFMediaEventGenerator *This,
                                                 MediaEventType type, REFGUID ext_type,
                                                 HRESULT hr, const PROPVARIANT *value)
{
    DvmMft *m = DVM_FROM_EVENT_GEN(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_QueueEventParamVar(m->events, type, ext_type, hr, value);
}

/* ------------------------------------------------------------------------- */
/* IUnknown                                                                   */
/* ------------------------------------------------------------------------- */

static const IMFTransformVtbl g_transform_vtbl = {
    mft_QueryInterface,
    mft_AddRef,
    mft_Release,
    mft_GetStreamLimits,
    mft_GetStreamCount,
    mft_GetStreamIDs,
    mft_GetInputStreamInfo,
    mft_GetOutputStreamInfo,
    mft_GetAttributes,
    mft_GetInputStreamAttributes,
    mft_GetOutputStreamAttributes,
    mft_DeleteInputStream,
    mft_AddInputStreams,
    mft_GetInputAvailableType,
    mft_GetOutputAvailableType,
    mft_SetInputType,
    mft_SetOutputType,
    mft_GetInputCurrentType,
    mft_GetOutputCurrentType,
    mft_GetInputStatus,
    mft_GetOutputStatus,
    mft_SetOutputBounds,
    mft_ProcessEvent,
    mft_ProcessMessage,
    mft_ProcessInput,
    mft_ProcessOutput
};

static const IMFMediaEventGeneratorVtbl g_event_vtbl = {
    evt_QueryInterface,
    evt_AddRef,
    evt_Release,
    evt_GetEvent,
    evt_BeginGetEvent,
    evt_EndGetEvent,
    evt_QueueEvent
};

static HRESULT STDMETHODCALLTYPE mft_QueryInterface(IMFTransform *This, REFIID riid,
                                                     void **ppv)
{
    DvmMft *m = (DvmMft *)This;

    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFTransform)) {
        *ppv = (void *)m;
    } else if (IsEqualIID(riid, &IID_IMFMediaEventGenerator)) {
        *ppv = (void *)&m->lpEventVtbl;
    } else {
        return E_NOINTERFACE;
    }
    mft_AddRef(This);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE mft_AddRef(IMFTransform *This)
{
    DvmMft *m = (DvmMft *)This;
    return (ULONG)InterlockedIncrement(&m->ref);
}

static ULONG STDMETHODCALLTYPE mft_Release(IMFTransform *This)
{
    DvmMft *m = (DvmMft *)This;
    LONG ref = InterlockedDecrement(&m->ref);

    if (ref != 0)
        return (ULONG)ref;

    m->stop = 1;
    if (m->wake != NULL)
        SetEvent(m->wake);
    if (m->thread != NULL) {
        WaitForSingleObject(m->thread, INFINITE);
        CloseHandle(m->thread);
        m->thread = NULL;
    }
    if (m->wake != NULL) {
        CloseHandle(m->wake);
        m->wake = NULL;
    }
    close_session(m);
    EnterCriticalSection(&m->lock);
    dvm_free_queues_locked(m);
    LeaveCriticalSection(&m->lock);
    DeleteCriticalSection(&m->lock);
    if (m->events != NULL) {
        IMFMediaEventQueue_Shutdown(m->events);
        IMFMediaEventQueue_Release(m->events);
    }
    if (m->attributes != NULL)
        IMFAttributes_Release(m->attributes);
    if (m->in_type != NULL)
        IMFMediaType_Release(m->in_type);
    if (m->out_type != NULL)
        IMFMediaType_Release(m->out_type);
    if (m->mf_started)
        MFShutdown();
    dvm_logf("decoder destroyed: aus=%llu frames=%llu timeouts=%llu sends_failed=%llu "
             "flushes=%llu drains=%llu resets=%llu",
             (unsigned long long)m->aus_in, (unsigned long long)m->frames_out,
             (unsigned long long)m->get_timeouts, (unsigned long long)m->sends_failed,
             (unsigned long long)m->flushes, (unsigned long long)m->drains,
             (unsigned long long)m->resets);
    HeapFree(GetProcessHeap(), 0, m);
    dvm_mft_release_object();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Construction                                                               */
/* ------------------------------------------------------------------------- */

HRESULT dvm_mft_create_instance(REFIID riid, void **ppv)
{
    DvmMft *m;
    HRESULT hr;

    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;

    m = (DvmMft *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*m));
    if (m == NULL)
        return E_OUTOFMEMORY;

    m->lpTransformVtbl = &g_transform_vtbl;
    m->lpEventVtbl = &g_event_vtbl;
    m->ref = 1;
    m->fatal = S_OK;
    InitializeCriticalSection(&m->lock);
    h264_bits_init(&m->bits);

    hr = MFCreateEventQueue(&m->events);
    if (FAILED(hr)) {
        /* A host that creates the MFT without MFStartup (possible through plain
         * CoCreateInstance) would otherwise just get a failure here.  Keep our
         * own MFStartup reference and drop it in Release(). */
        if (MFStartup(MF_VERSION, MFSTARTUP_LITE) == S_OK) {
            m->mf_started = 1;
            hr = MFCreateEventQueue(&m->events);
        }
    }
    if (FAILED(hr))
        goto fail;
    hr = MFCreateAttributes(&m->attributes, 8);
    if (FAILED(hr))
        goto fail;

    (void)IMFAttributes_SetUINT32(m->attributes, &MF_TRANSFORM_ASYNC, 1);
    (void)IMFAttributes_SetUINT32(m->attributes, &MF_TRANSFORM_ASYNC_UNLOCK, 0);
    (void)IMFAttributes_SetUINT32(m->attributes, &MFT_CODEC_MERIT_Attribute,
                                  DVM_MFT_MERIT);
    (void)IMFAttributes_SetString(m->attributes, &MFT_FRIENDLY_NAME_Attribute,
                                  DVM_MFT_FRIENDLY_NAME);
    (void)IMFAttributes_SetGUID(m->attributes, &MFT_TRANSFORM_CLSID_Attribute,
                                dvm_mft_clsid());
    /* No MFT_PROCESS_LOCAL_Attribute here: that attribute is for per-process
     * MFTs (MFTRegisterLocal).  Putting it on a globally registered MFT is what
     * hid the P-BATCH transform from MFTEnumEx - see droidvm_mft_reg.c. */

    m->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (m->wake == NULL) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    m->thread = CreateThread(NULL, 0, dvm_worker_main, m, 0, NULL);
    if (m->thread == NULL) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }

    dvm_mft_addref_object();
    dvm_logf("decoder created (async=1, merit=0x%08X)", DVM_MFT_MERIT);
    hr = mft_QueryInterface((IMFTransform *)m, riid, ppv);
    if (FAILED(hr))
        mft_Release((IMFTransform *)m);      /* drops the constructor reference */
    else
        mft_Release((IMFTransform *)m);      /* QueryInterface took its own ref */
    return hr;

fail:
    if (m->thread != NULL) {
        m->stop = 1;
        SetEvent(m->wake);
        WaitForSingleObject(m->thread, INFINITE);
        CloseHandle(m->thread);
    }
    if (m->wake != NULL)
        CloseHandle(m->wake);
    if (m->events != NULL) {
        IMFMediaEventQueue_Shutdown(m->events);
        IMFMediaEventQueue_Release(m->events);
    }
    if (m->attributes != NULL)
        IMFAttributes_Release(m->attributes);
    if (m->mf_started)
        MFShutdown();
    DeleteCriticalSection(&m->lock);
    HeapFree(GetProcessHeap(), 0, m);
    return hr;
}

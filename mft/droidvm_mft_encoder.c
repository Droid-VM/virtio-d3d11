/* droidvm_mft_encoder.c - asynchronous H.264 encoder MFT over canonical vmedia ABI.
 *
 * The transform owns one VM encoder session. ProcessInput copies an NV12 sample
 * into bounded encoder queue slots; a worker obtains coded packets and raises
 * METransformHaveOutput. ProcessOutput copies H.264 bytes and the echoed 100-ns
 * timestamp to the caller's sample. Drain waits for STOP/LAST and delivered
 * output; Flush rebuilds the token-owned session after joining its worker.
 * ForceKeyframe is not enabled until the production backend is verified.
 */
#include "droidvm_mft.h"
#include <vmedia_client.h>

#include <icodecapi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The four properties from 05-参考文档/作者方案-视频硬件编解码.md section 6.
 * Spelled out instead of using CODECAPI_* so the object does not depend on
 * which GUID library the mingw build happens to link. */
static const GUID k_codec_mean_bitrate = {
    0xf7222374, 0x2144, 0x4815,
    { 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52 }
};
static const GUID k_codec_rate_control_mode = {
    0x1c0608e9, 0x370c, 0x4710,
    { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 }
};
static const GUID k_codec_gop_size = {
    0x95f31b26, 0x95a4, 0x41aa,
    { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 }
};
static const GUID k_codec_force_keyframe = {
    0x398c1b98, 0x8353, 0x475a,
    { 0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45 }
};
static const GUID k_iid_icodecapi = {
    0x901db4c7, 0x31ce, 0x41a2,
    { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda }
};

#define DVM_PACK2(hi, lo) ((((UINT64)(hi)) << 32) | ((UINT64)(lo)))
#define DVM_HI32(v)       ((UINT32)(((UINT64)(v)) >> 32))
#define DVM_LO32(v)       ((UINT32)((UINT64)(v)))
#define DVM_MAX_DIM       4096u
#define DVM_MIN_ENC_DIM   128u
#define DVM_MAX_INFLIGHT  4u
#define DVM_PACKET_CAP    (4u * 1024u * 1024u)
/* Poll under io_lock, then wait outside it: a blocking GET_PACKET can starve
 * ProcessInput on Windows critical sections, which do not guarantee fairness. */
#define DVM_PACKET_TIMEOUT_MS 0u

typedef struct DvmPacket {
    struct DvmPacket *next;
    uint32_t size;
    uint32_t flags;
    LONGLONG time;
    LONGLONG duration;
    uint8_t data[1];
} DvmPacket;

typedef struct DvmInputTime {
    uint64_t tag;
    LONGLONG pts;
    LONGLONG duration;
    int used;
} DvmInputTime;

typedef struct DvmEncoder {
    /* COM: three interface vtable pointers; IMFTransform first so the object
     * pointer is an IMFTransform pointer without fix-up. */
    const IMFTransformVtbl           *lpVtbl;
    const IMFMediaEventGeneratorVtbl *lpEventVtbl;
    const ICodecAPIVtbl              *lpCodecVtbl;
    LONG                              ref;

    /* Lock order: api_lock -> io_lock -> lock. The packet worker never takes
     * api_lock; stop joins it without holding io_lock or the state lock. */
    CRITICAL_SECTION                  api_lock;
    CRITICAL_SECTION                  io_lock;
    CRITICAL_SECTION                  lock;
    IMFMediaEventQueue               *events;
    IMFAttributes                    *attributes;
    IMFMediaType                     *in_type;
    IMFMediaType                     *out_type;
    VM_SESSION                        *session;
    VM_ENCODER_CONFIG                 encoder_config;
    HANDLE                            worker;
    HANDLE                            wake_event;
    int                               worker_stop;
    int                               worker_started;
    int                               drain_done;
    int                               drain_requested;
    int                               drain_complete_posted;
    uint32_t                          active_outputs;
    uint32_t                          inflight;
    uint32_t                          next_buffer_id;
    uint64_t                          next_time_tag;
    DvmInputTime                      input_times[DVM_MAX_INFLIGHT];
    uint32_t                          packet_count;
    DvmPacket                        *packet_head;
    DvmPacket                        *packet_tail;

    uint32_t                          width;
    uint32_t                          height;
    UINT64                            frame_rate;

    int                               streaming;
    int                               draining;
    int                               need_input_posted;
    int                               fatal;
    int                               mf_started;
    int                               d3d_manager_seen;
    int                               discard_logged;

    UINT64                            frames_in;
    UINT64                            frames_out;
    UINT32                            mean_bitrate;
    UINT32                            rate_control_mode;
    UINT32                            gop_size;
    UINT32                            force_keyframe;
    UINT32                            set_value_count;
} DvmEncoder;

#define DVM_ENC_FROM_EVENT(p) \
    ((DvmEncoder *)(void *)((uint8_t *)(p) - offsetof(DvmEncoder, lpEventVtbl)))
#define DVM_ENC_FROM_CODEC(p) \
    ((DvmEncoder *)(void *)((uint8_t *)(p) - offsetof(DvmEncoder, lpCodecVtbl)))

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void post_event(DvmEncoder *m, MediaEventType type, HRESULT hr)
{
    if (m->events != NULL)
        IMFMediaEventQueue_QueueEventParamVar(m->events, type, &GUID_NULL, hr,
                                              NULL);
}

static int async_unlocked(DvmEncoder *m)
{
    UINT32 v = 0;

    if (m->attributes == NULL)
        return 0;
    if (FAILED(IMFAttributes_GetUINT32(m->attributes, &MF_TRANSFORM_ASYNC_UNLOCK,
                                       &v)))
        return 0;
    return v != 0;
}

static HRESULT copy_media_type(IMFMediaType **dst, IMFMediaType *src)
{
    IMFMediaType *copy = NULL;
    HRESULT hr;

    hr = MFCreateMediaType(&copy);
    if (FAILED(hr))
        return hr;
    hr = IMFAttributes_CopyAllItems((IMFAttributes *)src, (IMFAttributes *)copy);
    if (FAILED(hr)) {
        IMFMediaType_Release(copy);
        return hr;
    }
    if (*dst != NULL)
        IMFMediaType_Release(*dst);
    *dst = copy;
    return S_OK;
}

static int types_ready(DvmEncoder *m)
{
    return m->in_type != NULL && m->out_type != NULL;
}

static void free_packets(DvmEncoder *m)
{
    DvmPacket *p = m->packet_head;
    while (p != NULL) {
        DvmPacket *next = p->next;
        HeapFree(GetProcessHeap(), 0, p);
        p = next;
    }
    m->packet_head = m->packet_tail = NULL;
    m->packet_count = 0;
}

static HRESULT hr_from_vm_error(void)
{
    DWORD error = vm_last_error();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

/* Called under the state lock. Consuming the completion state prevents the
 * worker and ProcessOutput from publishing the same drain twice. */
static int take_drain_completion(DvmEncoder *m)
{
    if(m->fatal==S_OK&&m->drain_requested&&m->drain_done&&!m->packet_head&&m->active_outputs==0&&!m->drain_complete_posted){
        m->drain_complete_posted=1;
        return 1;
    }
    return 0;
}

static DWORD WINAPI encoder_worker(void *arg)
{
    DvmEncoder *m = (DvmEncoder *)arg;
    for (;;) {
        VM_SESSION *session;
        int stop, can_read, post;
        EnterCriticalSection(&m->lock);
        stop = m->worker_stop;
        session = m->session;
        can_read = (m->inflight != 0 || (m->drain_requested&&!m->drain_done)) &&
                   m->packet_count < DVM_MAX_INFLIGHT;
        LeaveCriticalSection(&m->lock);
        if (stop) break;
        if (!session || !can_read) {
            WaitForSingleObject(m->wake_event, 100);
            continue;
        }
        {
            VM_ENCODER_PACKET_INFO info;
            uint8_t *bytes = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, DVM_PACKET_CAP);
            if (bytes == NULL) {
                EnterCriticalSection(&m->lock);
                m->fatal = E_OUTOFMEMORY;
                LeaveCriticalSection(&m->lock);
                post_event(m, MEError, E_OUTOFMEMORY);
                break;
            }
            int ok;
            DWORD error;
            EnterCriticalSection(&m->io_lock);
            ok=vm_encoder_get_packet(session,&info,bytes,DVM_PACKET_CAP,DVM_PACKET_TIMEOUT_MS);
            error=ok?0:vm_last_error();
            LeaveCriticalSection(&m->io_lock);
            if (!ok) {
                HeapFree(GetProcessHeap(), 0, bytes);
                if (error == ERROR_SEM_TIMEOUT || error == ERROR_TIMEOUT) {
                    WaitForSingleObject(m->wake_event,2);
                    continue;
                }
                EnterCriticalSection(&m->lock);
                m->fatal = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
                LeaveCriticalSection(&m->lock);
                post_event(m, MEError, m->fatal);
                break;
            }
            if ((info.Flags & VM_ENCODER_PACKET_FLAG_DRAIN_DONE) && info.BytesUsed==0) {
                int complete;
                EnterCriticalSection(&m->lock);
                if(m->inflight!=0){
                    m->fatal=MF_E_INVALID_STREAM_DATA;LeaveCriticalSection(&m->lock);
                    HeapFree(GetProcessHeap(),0,bytes);post_event(m,MEError,MF_E_INVALID_STREAM_DATA);break;
                }
                m->drain_done=1;
                complete=take_drain_completion(m);
                LeaveCriticalSection(&m->lock);
                HeapFree(GetProcessHeap(),0,bytes);
                if(complete)post_event(m,METransformDrainComplete,S_OK);
                continue;
            }
            if (info.BytesUsed != 0) {
                DvmPacket *node = (DvmPacket *)HeapAlloc(GetProcessHeap(), 0,
                                                          sizeof(*node) + info.BytesUsed);
                if (node == NULL) {
                    HeapFree(GetProcessHeap(), 0, bytes);
                    EnterCriticalSection(&m->lock);
                    m->fatal = E_OUTOFMEMORY;
                    LeaveCriticalSection(&m->lock);
                    post_event(m, MEError, E_OUTOFMEMORY);
                    break;
                }
                node->next = NULL;
                node->size = info.BytesUsed;
                node->flags = info.Flags;
                memcpy(node->data, bytes, info.BytesUsed);
                EnterCriticalSection(&m->lock);
                {unsigned i;for(i=0;i<DVM_MAX_INFLIGHT;++i)if(m->input_times[i].used&&m->input_times[i].tag==info.Timestamp)break;
                 if(i==DVM_MAX_INFLIGHT){
                    m->fatal=MF_E_INVALID_STREAM_DATA;LeaveCriticalSection(&m->lock);
                    HeapFree(GetProcessHeap(),0,node);HeapFree(GetProcessHeap(),0,bytes);
                    post_event(m,MEError,MF_E_INVALID_STREAM_DATA);break;
                 }
                 node->time=m->input_times[i].pts;node->duration=m->input_times[i].duration;
                 m->input_times[i].used=0;
                }
                if (m->packet_tail) m->packet_tail->next = node;
                else m->packet_head = node;
                m->packet_tail = node;
                ++m->packet_count;
                if (m->inflight) --m->inflight;
                m->frames_out++;
                if(info.Flags&VM_ENCODER_PACKET_FLAG_DRAIN_DONE){
                    if(m->inflight!=0)m->fatal=MF_E_INVALID_STREAM_DATA;
                    else m->drain_done=1;
                }
                stop = m->worker_stop;
                error=m->fatal;
                LeaveCriticalSection(&m->lock);
                HeapFree(GetProcessHeap(), 0, bytes);
                post_event(m, METransformHaveOutput, S_OK);
                if(FAILED((HRESULT)error)){post_event(m,MEError,(HRESULT)error);break;}
                if (!stop) {
                    EnterCriticalSection(&m->lock);
                    post = m->streaming && !m->draining && !m->need_input_posted &&
                           m->inflight + m->packet_count < DVM_MAX_INFLIGHT;
                    if (post) m->need_input_posted = 1;
                    LeaveCriticalSection(&m->lock);
                    if (post) post_event(m, METransformNeedInput, S_OK);
                }
            } else {
                HeapFree(GetProcessHeap(), 0, bytes);
            }
        }
    }
    return 0;
}

static void encoder_stop(DvmEncoder *m)
{
    HANDLE worker;
    VM_SESSION *session;
    EnterCriticalSection(&m->lock);
    m->worker_stop = 1;
    worker = m->worker;
    session = m->session;
    if (m->wake_event) SetEvent(m->wake_event);
    LeaveCriticalSection(&m->lock);
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
    }
    if (session) vm_close(session);
    EnterCriticalSection(&m->lock);
    m->worker = NULL;
    m->session = NULL;
    m->worker_started = 0;
    m->inflight = 0;
    memset(m->input_times,0,sizeof(m->input_times));
    m->worker_stop = 0;
    free_packets(m);
    LeaveCriticalSection(&m->lock);
}

static HRESULT encoder_start(DvmEncoder *m)
{
    VM_OPEN_OUT opened;
    VM_ENCODER_CONFIG cfg;
    VM_SESSION *session;
    UINT64 frame_rate;
    uint32_t width, height, bitrate, gop;
    DWORD tid = 0;
    EnterCriticalSection(&m->lock);
    if (m->session != NULL) { LeaveCriticalSection(&m->lock); return S_OK; }
    if (!types_ready(m)) { LeaveCriticalSection(&m->lock); return MF_E_TRANSFORM_TYPE_NOT_SET; }
    width=m->width; height=m->height; frame_rate=m->frame_rate;
    bitrate=m->mean_bitrate; gop=m->gop_size;
    LeaveCriticalSection(&m->lock);
    memset(&opened, 0, sizeof(opened));
    session=vm_open(1, &opened);
    if (!session) {HRESULT hr=hr_from_vm_error();dvm_logf("encoder OPEN failed: %08lX",(unsigned long)hr);return hr;}
    if(opened.DriverVersion<0x00010022u){
        dvm_logf("encoder controls require driver 1.0.0.34; got %08X",opened.DriverVersion);
        vm_close(session);return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    memset(&cfg,0,sizeof(cfg));
    cfg.Version=VM_ENCODER_ABI_VERSION; cfg.Size=sizeof(cfg); cfg.SessionToken=opened.SessionToken;
    cfg.Width=width; cfg.Height=height; cfg.YStride=width; cfg.UVStride=width;
    cfg.UVOffset=width*height; cfg.FrameRateNum=frame_rate ? DVM_HI32(frame_rate) : 30;
    cfg.FrameRateDen=frame_rate ? DVM_LO32(frame_rate) : 1; cfg.Bitrate=bitrate; cfg.GopFrames=gop;
    if (!vm_encoder_configure(session,&cfg)) { HRESULT hr=hr_from_vm_error();dvm_logf("encoder CONFIG failed: %ux%u hr=%08lX",width,height,(unsigned long)hr); vm_close(session); return hr; }
    if (!m->wake_event) m->wake_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    if (!m->wake_event) { vm_close(session); return HRESULT_FROM_WIN32(GetLastError()); }
    EnterCriticalSection(&m->lock);
    m->session=session; m->encoder_config=cfg; m->worker_stop=0; m->drain_done=0;m->drain_requested=0;m->drain_complete_posted=0;m->active_outputs=0;
    m->worker=CreateThread(NULL,0,encoder_worker,m,0,&tid);
    if (!m->worker) {
        m->session=NULL; LeaveCriticalSection(&m->lock); vm_close(session);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    m->worker_started=1;
    LeaveCriticalSection(&m->lock);
    dvm_logf("encoder opened: %ux%u stride=%u uv=%u driver=%08X",width,height,cfg.YStride,cfg.UVOffset,opened.DriverVersion);
    return S_OK;
}

static int valid_dimensions(uint32_t w, uint32_t h)
{
    return w >= DVM_MIN_ENC_DIM && h >= DVM_MIN_ENC_DIM && w <= DVM_MAX_DIM && h <= DVM_MAX_DIM &&
           (w & 1u) == 0 && (h & 1u) == 0;
}

static void log_property(DvmEncoder *m, const char *name, UINT32 value)
{
    UINT32 n;

    EnterCriticalSection(&m->lock);
    n = ++m->set_value_count;
    LeaveCriticalSection(&m->lock);
    if (n <= 16 || (n % 60) == 0)
        dvm_logf("ICodecAPI::SetValue(%s)=%lu (#%lu, encoded frames=%llu; "
                 "host encoder client path", name, (unsigned long)value,
                 (unsigned long)n, (unsigned long long)m->frames_in);
}

/* ------------------------------------------------------------------------- */
/* IMFTransform - stream description                                          */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE enc_GetStreamLimits(IMFTransform *This,
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

static HRESULT STDMETHODCALLTYPE enc_GetStreamCount(IMFTransform *This,
                                                    DWORD *inputs, DWORD *outputs)
{
    (void)This;
    if (inputs == NULL || outputs == NULL)
        return E_POINTER;
    *inputs = 1;
    *outputs = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetStreamIDs(IMFTransform *This, DWORD in_size,
                                                  DWORD *in_ids, DWORD out_size,
                                                  DWORD *out_ids)
{
    (void)This;
    (void)in_size;
    (void)in_ids;
    (void)out_size;
    (void)out_ids;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE enc_GetInputStreamInfo(IMFTransform *This, DWORD id,
                                                        MFT_INPUT_STREAM_INFO *info)
{
    DvmEncoder *m = (DvmEncoder *)This;
    uint32_t w, h;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (info == NULL)
        return E_POINTER;
    EnterCriticalSection(&m->lock);
    w = m->width;
    h = m->height;
    LeaveCriticalSection(&m->lock);

    memset(info, 0, sizeof(*info));
    info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES |
                    MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
    info->cbSize = (w != 0 && h != 0) ? w * h * 3u / 2u : 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetOutputStreamInfo(IMFTransform *This, DWORD id,
                                                         MFT_OUTPUT_STREAM_INFO *info)
{
    (void)This;
    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (info == NULL)
        return E_POINTER;

    memset(info, 0, sizeof(*info));
    /* H.264 access units are variable sized and the client owns the output
     * samples, which is the normal shape for an encoder MFT. */
    info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES |
                    MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
    info->cbSize = DVM_PACKET_CAP;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetAttributes(IMFTransform *This,
                                                   IMFAttributes **attributes)
{
    DvmEncoder *m = (DvmEncoder *)This;

    if (attributes == NULL)
        return E_POINTER;
    *attributes = NULL;
    if (m->attributes == NULL)
        return E_UNEXPECTED;
    *attributes = m->attributes;
    IMFAttributes_AddRef(m->attributes);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetInputStreamAttributes(IMFTransform *This,
                                                              DWORD id,
                                                              IMFAttributes **attributes)
{
    (void)This;
    (void)id;
    (void)attributes;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE enc_GetOutputStreamAttributes(IMFTransform *This,
                                                               DWORD id,
                                                               IMFAttributes **attributes)
{
    (void)This;
    (void)id;
    (void)attributes;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE enc_DeleteInputStream(IMFTransform *This, DWORD id)
{
    (void)This;
    (void)id;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE enc_AddInputStreams(IMFTransform *This, DWORD streams,
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

static HRESULT STDMETHODCALLTYPE enc_GetInputAvailableType(IMFTransform *This,
                                                           DWORD id, DWORD index,
                                                           IMFMediaType **type)
{
    DvmEncoder *m = (DvmEncoder *)This;
    IMFMediaType *mt = NULL;
    HRESULT hr;
    uint32_t w, h;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    if (index != 0)
        return MF_E_NO_MORE_TYPES;

    hr = MFCreateMediaType(&mt);
    if (FAILED(hr))
        return hr;
    hr = IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    EnterCriticalSection(&m->lock);
    w = m->width;
    h = m->height;
    LeaveCriticalSection(&m->lock);
    if (SUCCEEDED(hr) && w != 0 && h != 0) {
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, DVM_PACK2(w, h));
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetUINT32(mt, &MF_MT_DEFAULT_STRIDE, w);
        if (SUCCEEDED(hr))
            hr = IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE,
                                        (UINT32)MFVideoInterlace_Progressive);
    }
    if (FAILED(hr)) {
        IMFMediaType_Release(mt);
        return hr;
    }
    *type = mt;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetOutputAvailableType(IMFTransform *This,
                                                            DWORD id, DWORD index,
                                                            IMFMediaType **type)
{
    DvmEncoder *m = (DvmEncoder *)This;
    IMFMediaType *mt = NULL;
    HRESULT hr;
    uint32_t w, h;
    UINT64 fps;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    if (index != 0)
        return MF_E_NO_MORE_TYPES;

    EnterCriticalSection(&m->lock);
    w = m->width ? m->width : 1920u;
    h = m->height ? m->height : 1080u;
    fps = m->frame_rate;
    LeaveCriticalSection(&m->lock);

    hr = MFCreateMediaType(&mt);
    if (FAILED(hr))
        return hr;
    hr = IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, DVM_PACK2(w, h));
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE,
                                    fps ? fps : DVM_PACK2(30, 1));
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(mt, &MF_MT_PIXEL_ASPECT_RATIO,
                                    DVM_PACK2(1, 1));
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE,
                                    (UINT32)MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_AVG_BITRATE,
                                    m->mean_bitrate ? m->mean_bitrate : 8000000u);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(mt, &MF_MT_ALL_SAMPLES_INDEPENDENT, 0);
    if (FAILED(hr)) {
        IMFMediaType_Release(mt);
        return hr;
    }
    *type = mt;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_SetInputType_impl(IMFTransform *This, DWORD id,
                                                  IMFMediaType *type, DWORD flags)
{
    DvmEncoder *m = (DvmEncoder *)This;
    GUID major = {0}, subtype = {0};
    UINT64 frame_size = 0;
    HRESULT hr;
    uint32_t w = 0, h = 0;
    int post = 0;

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
        m->width = 0;
        m->height = 0;
        LeaveCriticalSection(&m->lock);
        return S_OK;
    }

    hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || !IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;
    hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr) || !IsEqualGUID(&subtype, &MFVideoFormat_NV12))
        return MF_E_INVALIDMEDIATYPE;

    if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)) &&
        frame_size != 0) {
        w = DVM_HI32(frame_size);
        h = DVM_LO32(frame_size);
        if (!valid_dimensions(w, h))
            return MF_E_INVALIDMEDIATYPE;
        EnterCriticalSection(&m->lock);
        if (m->streaming) { LeaveCriticalSection(&m->lock); return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING; }
        if (m->out_type != NULL) {
            UINT64 out_size = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT64(m->out_type, &MF_MT_FRAME_SIZE, &out_size)) &&
                out_size != 0 && (DVM_HI32(out_size) != w || DVM_LO32(out_size) != h)) {
                LeaveCriticalSection(&m->lock); return MF_E_INVALIDMEDIATYPE;
            }
        }
        LeaveCriticalSection(&m->lock);
    }
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    hr = copy_media_type(&m->in_type, type);
    if (FAILED(hr))
        return hr;

    EnterCriticalSection(&m->lock);
    if (w != 0 && h != 0) {
        m->width = w;
        m->height = h;
    }
    post = m->streaming && !m->need_input_posted && types_ready(m);
    if (post)
        m->need_input_posted = 1;
    LeaveCriticalSection(&m->lock);

    if (post)
        post_event(m, METransformNeedInput, S_OK);
    dvm_logf("encoder SetInputType: NV12 %ux%u", w, h);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_SetOutputType_impl(IMFTransform *This, DWORD id,
                                                   IMFMediaType *type, DWORD flags)
{
    DvmEncoder *m = (DvmEncoder *)This;
    GUID major = {0}, subtype = {0};
    UINT64 frame_size = 0;
    HRESULT hr;
    uint32_t w = 0, h = 0;
    int post = 0;

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
        LeaveCriticalSection(&m->lock);
        return S_OK;
    }

    hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || !IsEqualGUID(&major, &MFMediaType_Video))
        return MF_E_INVALIDMEDIATYPE;
    hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr) || !IsEqualGUID(&subtype, &MFVideoFormat_H264))
        return MF_E_INVALIDMEDIATYPE;

    if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size)) &&
        frame_size != 0) {
        w = DVM_HI32(frame_size);
        h = DVM_LO32(frame_size);
        if (!valid_dimensions(w, h))
            return MF_E_INVALIDMEDIATYPE;
        EnterCriticalSection(&m->lock);
        if (m->streaming) { LeaveCriticalSection(&m->lock); return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING; }
        if (m->in_type != NULL) {
            UINT64 in_size = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT64(m->in_type, &MF_MT_FRAME_SIZE, &in_size)) &&
                in_size != 0 && (DVM_HI32(in_size) != w || DVM_LO32(in_size) != h)) {
                LeaveCriticalSection(&m->lock); return MF_E_INVALIDMEDIATYPE;
            }
        }
        LeaveCriticalSection(&m->lock);
    }
    if (flags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    hr = copy_media_type(&m->out_type, type);
    if (FAILED(hr))
        return hr;
    EnterCriticalSection(&m->lock);
    if (w != 0 && h != 0) {
        m->width = w;
        m->height = h;
    }
    {UINT32 bitrate=0;
     if(SUCCEEDED(IMFMediaType_GetUINT32(type,&MF_MT_AVG_BITRATE,&bitrate))&&bitrate)m->mean_bitrate=bitrate;}
    if (SUCCEEDED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_RATE,
                                         &m->frame_rate)) &&
        m->frame_rate == 0) {
        m->frame_rate = DVM_PACK2(30, 1);
    }
    post = m->streaming && !m->need_input_posted && types_ready(m);
    if (post)
        m->need_input_posted = 1;
    LeaveCriticalSection(&m->lock);

    if (post)
        post_event(m, METransformNeedInput, S_OK);
    dvm_logf("encoder SetOutputType: H264 %ux%u", w, h);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetInputCurrentType(IMFTransform *This, DWORD id,
                                                         IMFMediaType **type)
{
    DvmEncoder *m = (DvmEncoder *)This;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    if (m->in_type == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    *type = m->in_type;
    IMFMediaType_AddRef(m->in_type);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetOutputCurrentType(IMFTransform *This, DWORD id,
                                                          IMFMediaType **type)
{
    DvmEncoder *m = (DvmEncoder *)This;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (type == NULL)
        return E_POINTER;
    *type = NULL;
    if (m->out_type == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    *type = m->out_type;
    IMFMediaType_AddRef(m->out_type);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetInputStatus(IMFTransform *This, DWORD id,
                                                    DWORD *flags)
{
    DvmEncoder *m = (DvmEncoder *)This;
    int accept = 0;

    if (id != 0)
        return MF_E_INVALIDSTREAMNUMBER;
    if (flags == NULL)
        return E_POINTER;
    EnterCriticalSection(&m->lock);
    accept = m->streaming && !m->draining && types_ready(m) &&
             m->inflight + m->packet_count < DVM_MAX_INFLIGHT && m->fatal == S_OK;
    LeaveCriticalSection(&m->lock);
    *flags = accept ? MFT_INPUT_STATUS_ACCEPT_DATA : 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_GetOutputStatus(IMFTransform *This,
                                                     DWORD *flags)
{
    DvmEncoder *m = (DvmEncoder *)This;
    if (flags == NULL) return E_POINTER;
    EnterCriticalSection(&m->lock);
    *flags = m->packet_head ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
    LeaveCriticalSection(&m->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_SetOutputBounds(IMFTransform *This,
                                                     LONGLONG lower,
                                                     LONGLONG upper)
{
    (void)This;
    (void)lower;
    (void)upper;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE enc_ProcessEvent(IMFTransform *This, DWORD id,
                                                  IMFMediaEvent *event)
{
    (void)This;
    (void)id;
    (void)event;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE enc_ProcessMessage_impl(IMFTransform *This,
                                                    MFT_MESSAGE_TYPE message,
                                                    ULONG_PTR param)
{
    DvmEncoder *m = (DvmEncoder *)This;
    HRESULT hr;
    int post = 0;
    (void)param;
    switch (message) {
    case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
        hr = encoder_start(m);
        if (FAILED(hr)) { EnterCriticalSection(&m->lock); m->fatal=hr; LeaveCriticalSection(&m->lock); post_event(m,MEError,hr); return hr; }
        EnterCriticalSection(&m->lock);
        m->streaming=1; m->draining=0; m->need_input_posted=0;
        if (types_ready(m) && m->inflight+m->packet_count<DVM_MAX_INFLIGHT) { m->need_input_posted=1; post=1; }
        LeaveCriticalSection(&m->lock);
        if (post) post_event(m,METransformNeedInput,S_OK);
        return S_OK;
    case MFT_MESSAGE_NOTIFY_END_STREAMING:
        EnterCriticalSection(&m->lock); m->streaming=0; m->need_input_posted=0; LeaveCriticalSection(&m->lock);
        encoder_stop(m);
        return S_OK;
    case MFT_MESSAGE_COMMAND_FLUSH:
        {
            int resume;
            uint32_t old_token;
            IMFMediaEvent *event=NULL;
            EnterCriticalSection(&m->lock);
            resume=m->streaming;old_token=m->encoder_config.SessionToken;
            m->streaming=0;m->need_input_posted=0;
            LeaveCriticalSection(&m->lock);
            /* api_lock excludes every public input/output/lifecycle operation.
             * Joining the worker before discarding output is the epoch barrier. */
            encoder_stop(m);
            while(SUCCEEDED(IMFMediaEventQueue_GetEvent(m->events,MF_EVENT_FLAG_NO_WAIT,&event))){
                IMFMediaEvent_Release(event);event=NULL;
            }
            EnterCriticalSection(&m->lock);
            m->draining=0;m->drain_requested=0;m->drain_done=0;
            m->drain_complete_posted=0;m->active_outputs=0;m->fatal=S_OK;
            LeaveCriticalSection(&m->lock);
            if(!resume)return S_OK;
            hr=encoder_start(m);
            if(FAILED(hr)){EnterCriticalSection(&m->lock);m->fatal=hr;LeaveCriticalSection(&m->lock);return hr;}
            if(m->encoder_config.SessionToken==old_token){encoder_stop(m);return E_UNEXPECTED;}
            EnterCriticalSection(&m->lock);m->streaming=1;m->need_input_posted=1;LeaveCriticalSection(&m->lock);
            post_event(m,METransformNeedInput,S_OK);
            return S_OK;
        }
    case MFT_MESSAGE_COMMAND_DRAIN:
        if(!m->session)return MF_E_TRANSFORM_TYPE_NOT_SET;
        EnterCriticalSection(&m->lock);
        if(m->drain_requested){LeaveCriticalSection(&m->lock);return S_OK;}
        LeaveCriticalSection(&m->lock);
        /* The API lock excludes input/stop. Hold io_lock until the successful
         * control has been published, so the worker cannot outrun this state. */
        EnterCriticalSection(&m->io_lock);
        if(!vm_encoder_control(m->session,VM_IOCTL_ENC_DRAIN)){
            hr=hr_from_vm_error();LeaveCriticalSection(&m->io_lock);return hr;
        }
        EnterCriticalSection(&m->lock);
        m->draining=1;m->drain_requested=1;m->drain_done=0;
        m->drain_complete_posted=0;m->need_input_posted=0;
        LeaveCriticalSection(&m->lock);
        LeaveCriticalSection(&m->io_lock);
        SetEvent(m->wake_event);
        return S_OK;
    case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
        EnterCriticalSection(&m->lock); m->draining=1; m->need_input_posted=0; LeaveCriticalSection(&m->lock);
        return S_OK;
    case MFT_MESSAGE_SET_D3D_MANAGER:
        EnterCriticalSection(&m->lock); m->d3d_manager_seen=1; LeaveCriticalSection(&m->lock); return S_OK;
    default:
        return S_OK;
    }
}

static HRESULT STDMETHODCALLTYPE enc_ProcessInput_impl(IMFTransform *This, DWORD id,
                                                  IMFSample *sample, DWORD flags)
{
    DvmEncoder *m=(DvmEncoder *)This;
    IMFMediaBuffer *buffer=NULL;
    BYTE *src=NULL;
    DWORD max_length=0,current=0;
    uint8_t *nv12=NULL;
    uint32_t w,h,src_stride,y_stride,uv_stride,uv_offset,total,buffer_id;
    uint64_t tag=0;
    LONGLONG sample_time=0,sample_duration=0;
    unsigned timing_slot=DVM_MAX_INFLIGHT;
    VM_ENCODER_INPUT in;
    HRESULT hr;
    int post=0;
    (void)flags;
    if(id!=0)return MF_E_INVALIDSTREAMNUMBER;
    if(!sample)return E_POINTER;
    if(!async_unlocked(m))return MF_E_TRANSFORM_ASYNC_LOCKED;
    hr=IMFSample_GetSampleTime(sample,&sample_time);
    if(FAILED(hr))return hr;
    hr=IMFSample_GetSampleDuration(sample,&sample_duration);
    if(hr==MF_E_NO_SAMPLE_DURATION){
        UINT32 num=DVM_HI32(m->frame_rate),den=DVM_LO32(m->frame_rate);
        if(!num||!den){num=30;den=1;}
        sample_duration=(LONGLONG)((10000000ull*den)/num);
    }else if(FAILED(hr))return hr;
    if(sample_duration<=0)return E_INVALIDARG;
    EnterCriticalSection(&m->lock);
    if(m->fatal!=S_OK){hr=m->fatal;LeaveCriticalSection(&m->lock);return hr;}
    if(!types_ready(m)||!m->session){LeaveCriticalSection(&m->lock);return MF_E_TRANSFORM_TYPE_NOT_SET;}
    if(!m->streaming||m->draining||m->inflight+m->packet_count>=DVM_MAX_INFLIGHT){LeaveCriticalSection(&m->lock);return MF_E_NOTACCEPTING;}
    w=m->width;h=m->height;y_stride=m->encoder_config.YStride;uv_stride=m->encoder_config.UVStride;uv_offset=m->encoder_config.UVOffset;
    buffer_id=m->next_buffer_id++;++m->inflight;m->need_input_posted=0;
    LeaveCriticalSection(&m->lock);
    if(w==0||h==0||y_stride<w||uv_stride<w||uv_offset<y_stride*h){hr=MF_E_INVALIDMEDIATYPE;goto fail_slot;}
    src_stride=w;
    { LONG mf_stride=0; if(SUCCEEDED(IMFMediaType_GetUINT32(m->in_type,&MF_MT_DEFAULT_STRIDE,(UINT32 *)&mf_stride))&&mf_stride>0)src_stride=(uint32_t)mf_stride; }
    if(src_stride<w){hr=MF_E_INVALIDMEDIATYPE;goto fail_slot;}
    if((uint64_t)uv_offset+(uint64_t)uv_stride*(h/2u)>UINT32_MAX){hr=MF_E_INVALIDMEDIATYPE;goto fail_slot;}
    total=uv_offset+uv_stride*(h/2u);
    if(FAILED(IMFSample_ConvertToContiguousBuffer(sample,&buffer))) {hr=E_FAIL;goto fail_slot;}
    hr=IMFMediaBuffer_Lock(buffer,&src,&max_length,&current);
    if(FAILED(hr)){IMFMediaBuffer_Release(buffer);goto fail_slot;}
    if(current<(uint64_t)src_stride*h+(uint64_t)src_stride*(h/2u)){IMFMediaBuffer_Unlock(buffer);IMFMediaBuffer_Release(buffer);hr=MF_E_BUFFERTOOSMALL;goto fail_slot;}
    nv12=(uint8_t *)HeapAlloc(GetProcessHeap(),0,total);
    if(!nv12){IMFMediaBuffer_Unlock(buffer);IMFMediaBuffer_Release(buffer);hr=E_OUTOFMEMORY;goto fail_slot;}
    memset(nv12,0,total);
    { uint32_t y; for(y=0;y<h;++y){memcpy(nv12+(size_t)y*y_stride,src+(size_t)y*src_stride,w);if(y_stride>w)memset(nv12+(size_t)y*y_stride+w,16,y_stride-w);} }
    { uint32_t y; const uint8_t *src_uv=src+(size_t)src_stride*h; for(y=0;y<h/2u;++y){memcpy(nv12+uv_offset+(size_t)y*uv_stride,src_uv+(size_t)y*src_stride,w);if(uv_stride>w)memset(nv12+uv_offset+(size_t)y*uv_stride+w,128,uv_stride-w);} }
    IMFMediaBuffer_Unlock(buffer);IMFMediaBuffer_Release(buffer);
    /* Reserve a unique microsecond-aligned opaque tag before the QBUF. The
     * original signed PTS/duration never cross the lossy V4L2 timeval ABI. */
    EnterCriticalSection(&m->lock);
    for(unsigned i=0;i<DVM_MAX_INFLIGHT;++i)if(!m->input_times[i].used){timing_slot=i;break;}
    if(timing_slot<DVM_MAX_INFLIGHT && m->next_time_tag<(uint64_t)INT64_MAX/10u){
        tag=(++m->next_time_tag)*10u;
        m->input_times[timing_slot]=(DvmInputTime){tag,sample_time,sample_duration,1};
    }
    LeaveCriticalSection(&m->lock);
    if(!tag){HeapFree(GetProcessHeap(),0,nv12);hr=E_UNEXPECTED;goto fail_slot;}
    memset(&in,0,sizeof(in));in.Version=VM_ENCODER_ABI_VERSION;in.Size=sizeof(in);in.SessionToken=m->encoder_config.SessionToken;
    in.BufferId=buffer_id;in.DataSize=total;in.Timestamp=tag;
    EnterCriticalSection(&m->lock);in.Flags=m->force_keyframe?VM_ENCODER_INPUT_FORCE_KEYFRAME:0;LeaveCriticalSection(&m->lock);in.YOffset=0;in.UVOffset=uv_offset;in.YStride=y_stride;in.UVStride=uv_stride;
    EnterCriticalSection(&m->io_lock);
    {int ok=vm_encoder_queue_input(m->session,&in,nv12,total);hr=ok?S_OK:hr_from_vm_error();}
    LeaveCriticalSection(&m->io_lock);
    if(FAILED(hr)){HeapFree(GetProcessHeap(),0,nv12);goto fail_slot;}
    HeapFree(GetProcessHeap(),0,nv12);
    EnterCriticalSection(&m->lock);if(in.Flags&VM_ENCODER_INPUT_FORCE_KEYFRAME)m->force_keyframe=0;m->frames_in++;if(m->inflight+m->packet_count<DVM_MAX_INFLIGHT&&!m->need_input_posted&&!m->draining){m->need_input_posted=1;post=1;}LeaveCriticalSection(&m->lock);
    if(m->wake_event)SetEvent(m->wake_event);if(post)post_event(m,METransformNeedInput,S_OK);
    return S_OK;
fail_slot:
    EnterCriticalSection(&m->lock);
    if(timing_slot<DVM_MAX_INFLIGHT&&m->input_times[timing_slot].tag==tag)m->input_times[timing_slot].used=0;
    if(m->inflight)--m->inflight;if(m->fatal==S_OK)m->fatal=hr;
    LeaveCriticalSection(&m->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE enc_ProcessOutput_impl(IMFTransform *This, DWORD flags,
                                                   DWORD count,
                                                   MFT_OUTPUT_DATA_BUFFER *buffers,
                                                   DWORD *status)
{
    DvmEncoder *m=(DvmEncoder *)This;
    DvmPacket *node;
    IMFSample *sample;
    IMFMediaBuffer *buffer=NULL;
    BYTE *dst=NULL;
    DWORD max_len=0,current=0;
    HRESULT hr;
    (void)flags;
    if(status)*status=0;
    if(count!=1||!buffers)return E_INVALIDARG;
    if(buffers[0].dwStreamID!=0)return MF_E_INVALIDSTREAMNUMBER;
    sample=buffers[0].pSample;
    if(!sample)return E_POINTER;
    EnterCriticalSection(&m->lock);
    node=m->packet_head;
    if(node){m->packet_head=node->next;if(!m->packet_head)m->packet_tail=NULL;if(m->packet_count)--m->packet_count;++m->active_outputs;}
    hr=m->fatal;
    LeaveCriticalSection(&m->lock);
    if(!node)return FAILED(hr)?hr:MF_E_TRANSFORM_NEED_MORE_INPUT;
    hr=IMFSample_ConvertToContiguousBuffer(sample,&buffer);
    if(FAILED(hr))goto requeue;
    hr=IMFMediaBuffer_Lock(buffer,&dst,&max_len,&current);
    if(FAILED(hr)){IMFMediaBuffer_Release(buffer);goto requeue;}
    if(max_len<node->size){IMFMediaBuffer_Unlock(buffer);IMFMediaBuffer_Release(buffer);hr=MF_E_BUFFERTOOSMALL;goto requeue;}
    memcpy(dst,node->data,node->size);
    IMFMediaBuffer_Unlock(buffer);
    hr=IMFMediaBuffer_SetCurrentLength(buffer,node->size);
    IMFMediaBuffer_Release(buffer);
    if(FAILED(hr))goto requeue;
    hr=IMFSample_SetSampleTime(sample,node->time);
    if(FAILED(hr))goto requeue;
    hr=IMFSample_SetSampleDuration(sample,node->duration);
    if(FAILED(hr))goto requeue;
    dvm_logf("encoded output bytes=%u pts=%lld duration=%lld",node->size,(long long)node->time,(long long)node->duration);
    (void)IMFAttributes_SetUINT32((IMFAttributes *)sample,&MFSampleExtension_CleanPoint,
        (node->flags&VM_ENCODER_PACKET_FLAG_KEYFRAME)?1u:0u);
    buffers[0].dwStatus=0;
    {
        int complete;
        HeapFree(GetProcessHeap(),0,node);
        if(m->wake_event)SetEvent(m->wake_event);
        EnterCriticalSection(&m->lock);
        --m->active_outputs;
        complete=take_drain_completion(m);
        if(m->inflight+m->packet_count<DVM_MAX_INFLIGHT&&!m->need_input_posted&&!m->draining){m->need_input_posted=1;hr=S_OK;}else hr=S_FALSE;
        LeaveCriticalSection(&m->lock);
        if(complete)post_event(m,METransformDrainComplete,S_OK);
        if(hr==S_OK)post_event(m,METransformNeedInput,S_OK);
    }
    return S_OK;
requeue:
    EnterCriticalSection(&m->lock);node->next=m->packet_head;m->packet_head=node;if(!m->packet_tail)m->packet_tail=node;++m->packet_count;--m->active_outputs;LeaveCriticalSection(&m->lock);
    if(m->wake_event)SetEvent(m->wake_event);
    return hr;
}

/* ------------------------------------------------------------------------- */
/* IMFMediaEventGenerator                                                     */
/* ------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE enc_QueryInterface(IMFTransform *This, REFIID riid,
                                                    void **ppv);
static ULONG STDMETHODCALLTYPE enc_AddRef(IMFTransform *This);
static ULONG STDMETHODCALLTYPE enc_Release(IMFTransform *This);

static HRESULT STDMETHODCALLTYPE evt_QueryInterface(IMFMediaEventGenerator *This,
                                                    REFIID riid, void **ppv)
{
    return enc_QueryInterface((IMFTransform *)DVM_ENC_FROM_EVENT(This), riid, ppv);
}

static ULONG STDMETHODCALLTYPE evt_AddRef(IMFMediaEventGenerator *This)
{
    return enc_AddRef((IMFTransform *)DVM_ENC_FROM_EVENT(This));
}

static ULONG STDMETHODCALLTYPE evt_Release(IMFMediaEventGenerator *This)
{
    return enc_Release((IMFTransform *)DVM_ENC_FROM_EVENT(This));
}

static HRESULT STDMETHODCALLTYPE evt_GetEvent(IMFMediaEventGenerator *This,
                                              DWORD flags, IMFMediaEvent **event)
{
    DvmEncoder *m = DVM_ENC_FROM_EVENT(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_GetEvent(m->events, flags, event);
}

static HRESULT STDMETHODCALLTYPE evt_BeginGetEvent(IMFMediaEventGenerator *This,
                                                   IMFAsyncCallback *callback,
                                                   IUnknown *state)
{
    DvmEncoder *m = DVM_ENC_FROM_EVENT(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_BeginGetEvent(m->events, callback, state);
}

static HRESULT STDMETHODCALLTYPE evt_EndGetEvent(IMFMediaEventGenerator *This,
                                                 IMFAsyncResult *result,
                                                 IMFMediaEvent **event)
{
    DvmEncoder *m = DVM_ENC_FROM_EVENT(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_EndGetEvent(m->events, result, event);
}

static HRESULT STDMETHODCALLTYPE evt_QueueEvent(IMFMediaEventGenerator *This,
                                                MediaEventType type,
                                                REFGUID ext_type, HRESULT hr,
                                                const PROPVARIANT *value)
{
    DvmEncoder *m = DVM_ENC_FROM_EVENT(This);

    if (m->events == NULL)
        return MF_E_SHUTDOWN;
    return IMFMediaEventQueue_QueueEventParamVar(m->events, type, ext_type, hr,
                                                 value);
}

/* ------------------------------------------------------------------------- */
/* ICodecAPI                                                                  */
/* ------------------------------------------------------------------------- */

static int codec_api_supported(REFGUID api)
{
    return IsEqualGUID(api, &k_codec_mean_bitrate) ||
           IsEqualGUID(api, &k_codec_gop_size) ||
           IsEqualGUID(api, &k_codec_force_keyframe);
}

static const char *codec_api_name(REFGUID api)
{
    if (IsEqualGUID(api, &k_codec_mean_bitrate))
        return "AVEncCommonMeanBitRate";
    if (IsEqualGUID(api, &k_codec_rate_control_mode))
        return "AVEncCommonRateControlMode";
    if (IsEqualGUID(api, &k_codec_gop_size))
        return "AVEncMPVGOPSize";
    if (IsEqualGUID(api, &k_codec_force_keyframe))
        return "AVEncVideoForceKeyFrame";
    return "unknown";
}

static HRESULT STDMETHODCALLTYPE codec_QueryInterface(ICodecAPI *This, REFIID riid,
                                                      void **ppv)
{
    return enc_QueryInterface((IMFTransform *)DVM_ENC_FROM_CODEC(This), riid, ppv);
}

static ULONG STDMETHODCALLTYPE codec_AddRef(ICodecAPI *This)
{
    return enc_AddRef((IMFTransform *)DVM_ENC_FROM_CODEC(This));
}

static ULONG STDMETHODCALLTYPE codec_Release(ICodecAPI *This)
{
    return enc_Release((IMFTransform *)DVM_ENC_FROM_CODEC(This));
}

static HRESULT STDMETHODCALLTYPE codec_IsSupported(ICodecAPI *This, const GUID *Api)
{
    (void)This;
    if (Api == NULL)
        return E_POINTER;
    return codec_api_supported(Api) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE codec_IsModifiable(ICodecAPI *This, const GUID *Api)
{
    (void)This;
    if (Api == NULL)
        return E_POINTER;
    return codec_api_supported(Api) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE codec_GetParameterRange(ICodecAPI *This,
                                                         const GUID *Api,
                                                         VARIANT *ValueMin,
                                                         VARIANT *ValueMax,
                                                         VARIANT *SteppingDelta)
{
    (void)This;
    (void)Api;
    (void)ValueMin;
    (void)ValueMax;
    (void)SteppingDelta;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_GetParameterValues(ICodecAPI *This,
                                                          const GUID *Api,
                                                          VARIANT **Values,
                                                          ULONG *ValuesCount)
{
    (void)This;
    (void)Api;
    if (Values != NULL)
        *Values = NULL;
    if (ValuesCount != NULL)
        *ValuesCount = 0;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_GetDefaultValue(ICodecAPI *This,
                                                       const GUID *Api,
                                                       VARIANT *Value)
{
    DvmEncoder *m = DVM_ENC_FROM_CODEC(This);

    (void)m;
    if (Api == NULL || Value == NULL)
        return E_POINTER;
    if (!codec_api_supported(Api))
        return S_FALSE;
    V_VT(Value) = VT_UI4;
    if (IsEqualGUID(Api, &k_codec_mean_bitrate))
        V_UI4(Value) = 8000000u;
    else if (IsEqualGUID(Api, &k_codec_rate_control_mode))
        V_UI4(Value) = 0u;        /* eAVEncCommonRateControlMode_CBR */
    else if (IsEqualGUID(Api, &k_codec_gop_size))
        V_UI4(Value) = 30u;
    else
        V_UI4(Value) = 0u;        /* force-keyframe is a one-shot command */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE codec_GetValue(ICodecAPI *This, const GUID *Api,
                                                VARIANT *Value)
{
    DvmEncoder *m = DVM_ENC_FROM_CODEC(This);

    if (Api == NULL || Value == NULL)
        return E_POINTER;
    if (!codec_api_supported(Api))
        return S_FALSE;
    V_VT(Value) = VT_UI4;
    EnterCriticalSection(&m->lock);
    if (IsEqualGUID(Api, &k_codec_mean_bitrate))
        V_UI4(Value) = m->mean_bitrate;
    else if (IsEqualGUID(Api, &k_codec_rate_control_mode))
        V_UI4(Value) = m->rate_control_mode;
    else if (IsEqualGUID(Api, &k_codec_gop_size))
        V_UI4(Value) = m->gop_size;
    else
        V_UI4(Value) = m->force_keyframe;
    LeaveCriticalSection(&m->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE codec_SetValue(ICodecAPI *This, const GUID *Api,
                                                VARIANT *Value)
{
    DvmEncoder *m = DVM_ENC_FROM_CODEC(This);
    UINT32 v;

    if (Api == NULL || Value == NULL)
        return E_POINTER;
    if (!codec_api_supported(Api))
        return E_NOTIMPL;
    if (V_VT(Value) == VT_UI4)
        v = V_UI4(Value);
    else if (V_VT(Value) == VT_I4)
        v = (UINT32)V_I4(Value);
    else if (V_VT(Value) == VT_BOOL)
        v = (V_BOOL(Value) != VARIANT_FALSE) ? 1u : 0u;
    else
        return E_INVALIDARG;
    if(IsEqualGUID(Api,&k_codec_rate_control_mode))return E_NOTIMPL;
    if((IsEqualGUID(Api,&k_codec_mean_bitrate)||IsEqualGUID(Api,&k_codec_gop_size))&&(!v||v>INT32_MAX))return E_INVALIDARG;
    EnterCriticalSection(&m->api_lock);
    if(m->session&&!IsEqualGUID(Api,&k_codec_force_keyframe)){
        LeaveCriticalSection(&m->api_lock);return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    EnterCriticalSection(&m->lock);
    if (IsEqualGUID(Api, &k_codec_mean_bitrate)) {
        m->mean_bitrate = v;
        if (m->out_type != NULL)
            (void)IMFMediaType_SetUINT32(m->out_type, &MF_MT_AVG_BITRATE, v);
    } else if (IsEqualGUID(Api, &k_codec_rate_control_mode)) {
        m->rate_control_mode = v;
    } else if (IsEqualGUID(Api, &k_codec_gop_size)) {
        m->gop_size = v;
    } else {
        m->force_keyframe = v ? 1u : 0u;
    }
    LeaveCriticalSection(&m->lock);

    log_property(m, codec_api_name(Api), v);
    LeaveCriticalSection(&m->api_lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE codec_RegisterForEvent(ICodecAPI *This,
                                                        const GUID *Api,
                                                        LONG_PTR userData)
{
    (void)This;
    (void)Api;
    (void)userData;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_UnregisterForEvent(ICodecAPI *This,
                                                          const GUID *Api)
{
    (void)This;
    (void)Api;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_SetAllDefaults(ICodecAPI *This)
{
    DvmEncoder *m = DVM_ENC_FROM_CODEC(This);

    EnterCriticalSection(&m->api_lock);
    if(m->session){LeaveCriticalSection(&m->api_lock);return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);}
    EnterCriticalSection(&m->lock);
    m->mean_bitrate = 8000000u;
    m->rate_control_mode = 0u;
    m->gop_size = 30u;
    m->force_keyframe = 0u;
    LeaveCriticalSection(&m->lock);
    dvm_logf("ICodecAPI::SetAllDefaults (encoder defaults)");
    LeaveCriticalSection(&m->api_lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE codec_SetValueWithNotify(ICodecAPI *This,
                                                          const GUID *Api,
                                                          VARIANT *Value,
                                                          GUID **ChangedParam,
                                                          ULONG *ChangedParamCount)
{
    (void)This;
    (void)Api;
    (void)Value;
    if (ChangedParam != NULL)
        *ChangedParam = NULL;
    if (ChangedParamCount != NULL)
        *ChangedParamCount = 0;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_SetAllDefaultsWithNotify(ICodecAPI *This,
                                                                GUID **ChangedParam,
                                                                ULONG *ChangedParamCount)
{
    (void)This;
    if (ChangedParam != NULL)
        *ChangedParam = NULL;
    if (ChangedParamCount != NULL)
        *ChangedParamCount = 0;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_GetAllSettings(ICodecAPI *This, IStream *stream)
{
    (void)This;
    (void)stream;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_SetAllSettings(ICodecAPI *This, IStream *stream)
{
    (void)This;
    (void)stream;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE codec_SetAllSettingsWithNotify(ICodecAPI *This,
                                                                IStream *stream,
                                                                GUID **ChangedParam,
                                                                ULONG *ChangedParamCount)
{
    (void)This;
    (void)stream;
    if (ChangedParam != NULL)
        *ChangedParam = NULL;
    if (ChangedParamCount != NULL)
        *ChangedParamCount = 0;
    return E_NOTIMPL;
}

/* ------------------------------------------------------------------------- */
/* IUnknown                                                                   */
/* ------------------------------------------------------------------------- */

/* Public state-changing calls are serialized independently of the worker's
 * bounded device IO. Every early return remains inside the implementation. */
static HRESULT STDMETHODCALLTYPE enc_SetInputType(IMFTransform *t,DWORD id,IMFMediaType *type,DWORD flags)
{DvmEncoder *m=(DvmEncoder *)t;HRESULT hr;EnterCriticalSection(&m->api_lock);hr=enc_SetInputType_impl(t,id,type,flags);LeaveCriticalSection(&m->api_lock);return hr;}
static HRESULT STDMETHODCALLTYPE enc_SetOutputType(IMFTransform *t,DWORD id,IMFMediaType *type,DWORD flags)
{DvmEncoder *m=(DvmEncoder *)t;HRESULT hr;EnterCriticalSection(&m->api_lock);hr=enc_SetOutputType_impl(t,id,type,flags);LeaveCriticalSection(&m->api_lock);return hr;}
static HRESULT STDMETHODCALLTYPE enc_ProcessMessage(IMFTransform *t,MFT_MESSAGE_TYPE msg,ULONG_PTR param)
{DvmEncoder *m=(DvmEncoder *)t;HRESULT hr;EnterCriticalSection(&m->api_lock);dvm_logf("ProcessMessage msg=%u",(unsigned)msg);hr=enc_ProcessMessage_impl(t,msg,param);if(FAILED(hr))dvm_logf("ProcessMessage failed hr=%08lX",(unsigned long)hr);LeaveCriticalSection(&m->api_lock);return hr;}
static HRESULT STDMETHODCALLTYPE enc_ProcessInput(IMFTransform *t,DWORD id,IMFSample *sample,DWORD flags)
{DvmEncoder *m=(DvmEncoder *)t;HRESULT hr;EnterCriticalSection(&m->api_lock);hr=enc_ProcessInput_impl(t,id,sample,flags);if(FAILED(hr))dvm_logf("ProcessInput failed hr=%08lX frames_in=%llu",(unsigned long)hr,(unsigned long long)m->frames_in);LeaveCriticalSection(&m->api_lock);return hr;}
static HRESULT STDMETHODCALLTYPE enc_ProcessOutput(IMFTransform *t,DWORD flags,DWORD count,MFT_OUTPUT_DATA_BUFFER *buffers,DWORD *status)
{DvmEncoder *m=(DvmEncoder *)t;HRESULT hr;EnterCriticalSection(&m->api_lock);hr=enc_ProcessOutput_impl(t,flags,count,buffers,status);if(FAILED(hr)&&hr!=MF_E_TRANSFORM_NEED_MORE_INPUT)dvm_logf("ProcessOutput failed hr=%08lX frames_out=%llu",(unsigned long)hr,(unsigned long long)m->frames_out);LeaveCriticalSection(&m->api_lock);return hr;}

static const IMFTransformVtbl g_transform_vtbl = {
    enc_QueryInterface,
    enc_AddRef,
    enc_Release,
    enc_GetStreamLimits,
    enc_GetStreamCount,
    enc_GetStreamIDs,
    enc_GetInputStreamInfo,
    enc_GetOutputStreamInfo,
    enc_GetAttributes,
    enc_GetInputStreamAttributes,
    enc_GetOutputStreamAttributes,
    enc_DeleteInputStream,
    enc_AddInputStreams,
    enc_GetInputAvailableType,
    enc_GetOutputAvailableType,
    enc_SetInputType,
    enc_SetOutputType,
    enc_GetInputCurrentType,
    enc_GetOutputCurrentType,
    enc_GetInputStatus,
    enc_GetOutputStatus,
    enc_SetOutputBounds,
    enc_ProcessEvent,
    enc_ProcessMessage,
    enc_ProcessInput,
    enc_ProcessOutput
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

static const ICodecAPIVtbl g_codec_vtbl = {
    codec_QueryInterface,
    codec_AddRef,
    codec_Release,
    codec_IsSupported,
    codec_IsModifiable,
    codec_GetParameterRange,
    codec_GetParameterValues,
    codec_GetDefaultValue,
    codec_GetValue,
    codec_SetValue,
    codec_RegisterForEvent,
    codec_UnregisterForEvent,
    codec_SetAllDefaults,
    codec_SetValueWithNotify,
    codec_SetAllDefaultsWithNotify,
    codec_GetAllSettings,
    codec_SetAllSettings,
    codec_SetAllSettingsWithNotify
};

static HRESULT STDMETHODCALLTYPE enc_QueryInterface(IMFTransform *This, REFIID riid,
                                                    void **ppv)
{
    DvmEncoder *m = (DvmEncoder *)This;

    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFTransform)) {
        *ppv = (void *)m;
    } else if (IsEqualIID(riid, &IID_IMFMediaEventGenerator)) {
        *ppv = (void *)&m->lpEventVtbl;
    } else if (IsEqualIID(riid, &k_iid_icodecapi)) {
        *ppv = (void *)&m->lpCodecVtbl;
    } else {
        return E_NOINTERFACE;
    }
    enc_AddRef(This);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE enc_AddRef(IMFTransform *This)
{
    DvmEncoder *m = (DvmEncoder *)This;

    return (ULONG)InterlockedIncrement(&m->ref);
}

static ULONG STDMETHODCALLTYPE enc_Release(IMFTransform *This)
{
    DvmEncoder *m = (DvmEncoder *)This;
    LONG ref = InterlockedDecrement(&m->ref);

    if (ref != 0)
        return (ULONG)ref;

    encoder_stop(m);
    if (m->wake_event != NULL) {
        CloseHandle(m->wake_event);
        m->wake_event = NULL;
    }
    if (m->events != NULL) {
        IMFMediaEventQueue_Shutdown(m->events);
        IMFMediaEventQueue_Release(m->events);
        m->events = NULL;
    }
    if (m->attributes != NULL)
        IMFAttributes_Release(m->attributes);
    if (m->in_type != NULL)
        IMFMediaType_Release(m->in_type);
    if (m->out_type != NULL)
        IMFMediaType_Release(m->out_type);
    if (m->mf_started)
        MFShutdown();
    DeleteCriticalSection(&m->lock);
    DeleteCriticalSection(&m->io_lock);
    DeleteCriticalSection(&m->api_lock);
    dvm_logf("encoder destroyed: frames_in=%llu frames_out=%llu",
             (unsigned long long)m->frames_in,
             (unsigned long long)m->frames_out);
    HeapFree(GetProcessHeap(), 0, m);
    dvm_mft_release_object();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Construction                                                               */
/* ------------------------------------------------------------------------- */

HRESULT dvm_mft_create_instance(REFIID riid, void **ppv)
{
    DvmEncoder *m;
    HRESULT hr;

    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;

    m = (DvmEncoder *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*m));
    if (m == NULL)
        return E_OUTOFMEMORY;

    m->lpVtbl = &g_transform_vtbl;
    m->lpEventVtbl = &g_event_vtbl;
    m->lpCodecVtbl = &g_codec_vtbl;
    m->ref = 1;
    m->fatal = S_OK;
    m->mean_bitrate = 8000000u;
    m->rate_control_mode = 0u;
    m->gop_size = 30u;
    InitializeCriticalSection(&m->api_lock);
    InitializeCriticalSection(&m->io_lock);
    InitializeCriticalSection(&m->lock);

    hr = MFCreateEventQueue(&m->events);
    if (FAILED(hr)) {
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

    dvm_mft_addref_object();
    dvm_logf("encoder MFT created (async=1, merit=0x%08X, client=encoder ABI)", DVM_MFT_MERIT);
    hr = enc_QueryInterface((IMFTransform *)m, riid, ppv);
    enc_Release((IMFTransform *)m);
    return hr;

fail:
    if (m->events != NULL) {
        IMFMediaEventQueue_Shutdown(m->events);
        IMFMediaEventQueue_Release(m->events);
    }
    if (m->attributes != NULL)
        IMFAttributes_Release(m->attributes);
    if (m->mf_started)
        MFShutdown();
    DeleteCriticalSection(&m->lock);
    DeleteCriticalSection(&m->io_lock);
    DeleteCriticalSection(&m->api_lock);
    HeapFree(GetProcessHeap(), 0, m);
    return hr;
}

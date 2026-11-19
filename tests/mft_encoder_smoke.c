/* Server-built ARM64 direct-activation test. No COM/MFT registration changes. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

static const CLSID encoder_clsid = {0x9f3c2a12,0x7d64,0x4e3b,{0x9a,0x21,0x6c,0x0d,0x5b,0x48,0xe7,0xa3}};
typedef HRESULT (WINAPI *GetClassObjectFn)(REFCLSID,REFIID,void **);
#define W 640u
#define H 480u
#define FRAMES 4u
#ifndef SMOKE_LONG_FRAMES
#define SMOKE_LONG_FRAMES 120u
#endif
#if SMOKE_LONG_FRAMES < 120
#error SMOKE_LONG_FRAMES must be at least 120
#endif
#define MAX_PENDING 4u
#define CAP (4u*1024u*1024u)
#define PACK(a,b) (((UINT64)(a)<<32)|(b))
#define CHECK(call) do { hr=(call); if(FAILED(hr)){fprintf(stderr,"%s: 0x%08lX\n",#call,(unsigned long)hr);goto done;} } while(0)

/* All times are native MF 100ns ticks. Never round through microseconds. */
static LONGLONG frame_time(unsigned index,int timing_mode)
{
    if(timing_mode==2){
        /* Deliberately repeat a PTS once per ten frames. The +7 also tests
         * sub-microsecond precision on the very first output. */
        if(index%10u==1u)--index;
        return (LONGLONG)(((UINT64)index*10000000u*1001u)/30000u)+7;
    }
    if(timing_mode==1)return (LONGLONG)(((UINT64)index*10000000u*1001u)/30000u);
    return (LONGLONG)index*333333;
}

static LONGLONG frame_duration(unsigned index,int timing_mode)
{
    static const LONGLONG variable_duration[]={166667,333337,500003,250009};
    if(timing_mode==2)return variable_duration[index%4u];
    if(timing_mode==1)return frame_time(index+1u,timing_mode)-frame_time(index,timing_mode);
    return 333333;
}

static HRESULT media_type(IMFMediaType **type,const GUID *subtype,unsigned width,unsigned height,int timing_mode)
{
    HRESULT hr=MFCreateMediaType(type);
    if(FAILED(hr))return hr;
    hr=IMFMediaType_SetGUID(*type,&MF_MT_MAJOR_TYPE,&MFMediaType_Video);
    if(SUCCEEDED(hr))hr=IMFMediaType_SetGUID(*type,&MF_MT_SUBTYPE,subtype);
    if(SUCCEEDED(hr))hr=IMFMediaType_SetUINT64(*type,&MF_MT_FRAME_SIZE,PACK(width,height));
    if(SUCCEEDED(hr))hr=IMFMediaType_SetUINT64(*type,&MF_MT_FRAME_RATE,PACK(timing_mode?30000:30,timing_mode?1001:1));
    if(SUCCEEDED(hr))hr=IMFMediaType_SetUINT32(*type,&MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
    return hr;
}

static int parse_dimension(const wchar_t *text,unsigned *value)
{
    wchar_t *end=NULL;
    unsigned long parsed;
    if(*text<L'0'||*text>L'9')return 0;
    parsed=wcstoul(text,&end,10);
    /* Bound allocation arithmetic and require an even NV12 size. */
    if(*end||parsed<128||parsed>8192||(parsed&1))return 0;
    *value=(unsigned)parsed;return 1;
}

int wmain(int argc,wchar_t **argv)
{
    HRESULT hr=S_OK;
    HMODULE dll=NULL;
    GetClassObjectFn get_class;
    IClassFactory *factory=NULL;
    IMFTransform *mft=NULL;
    IMFMediaEventGenerator *events=NULL;
    IMFAttributes *attrs=NULL;
    IMFMediaType *input=NULL,*output=NULL;
    IMFSample *sample=NULL;
    IMFMediaBuffer *buffer=NULL;
    FILE *stream=NULL,*meta=NULL;
    unsigned frames=0,submitted=0,max_pending=0,need_input=0,have_output=0,drain_events=0;
    unsigned target=FRAMES,width=W,height=H;
    int flush_test=0,flushed=0,long_test=0,drain_sent=0;
    int retry_test=0,retried=0,hold_test=0,exit_test=0,timing_mode=0;
    int rc=1,com_started=0,mf_started=0,streaming=0;
    ULONGLONG deadline;
    if(argc<2||argc>5)goto usage;
    if(argc>=3){
        if(wcscmp(argv[2],L"flush")==0)flush_test=1;
        else if(wcscmp(argv[2],L"empty")==0)target=0;
        else if(wcscmp(argv[2],L"retry")==0)retry_test=1;
        else if(wcscmp(argv[2],L"long")==0){long_test=1;target=SMOKE_LONG_FRAMES;}
        else if(wcscmp(argv[2],L"timing")==0){timing_mode=1;target=120u;}
        else if(wcscmp(argv[2],L"variable")==0){timing_mode=2;target=120u;}
        else if(wcscmp(argv[2],L"hold")==0)hold_test=1;
        else if(wcscmp(argv[2],L"exit")==0)exit_test=1;
        else goto usage;
    }
    if(argc>3){
        if(!long_test||argc!=5||!parse_dimension(argv[3],&width)||!parse_dimension(argv[4],&height))goto usage;
    }
    CHECK(CoInitializeEx(NULL,COINIT_MULTITHREADED));com_started=1;
    CHECK(MFStartup(MF_VERSION,MFSTARTUP_FULL));mf_started=1;
    dll=LoadLibraryW(argv[1]);
    if(!dll){fprintf(stderr,"LoadLibrary failed %lu\n",GetLastError());goto done;}
    get_class=(GetClassObjectFn)(void *)GetProcAddress(dll,"DllGetClassObject");
    if(!get_class){fprintf(stderr,"no class factory export\n");goto done;}
    CHECK(get_class(&encoder_clsid,&IID_IClassFactory,(void **)&factory));
    CHECK(IClassFactory_CreateInstance(factory,NULL,&IID_IMFTransform,(void **)&mft));
    CHECK(IMFTransform_QueryInterface(mft,&IID_IMFMediaEventGenerator,(void **)&events));
    CHECK(IMFTransform_GetAttributes(mft,&attrs));
    CHECK(IMFAttributes_SetUINT32(attrs,&MF_TRANSFORM_ASYNC_UNLOCK,1));
    CHECK(media_type(&input,&MFVideoFormat_NV12,width,height,timing_mode));
    CHECK(IMFMediaType_SetUINT32(input,&MF_MT_DEFAULT_STRIDE,width));
    CHECK(media_type(&output,&MFVideoFormat_H264,width,height,timing_mode));
    CHECK(IMFMediaType_SetUINT32(output,&MF_MT_AVG_BITRATE,2000000));
    CHECK(IMFTransform_SetInputType(mft,0,input,0));
    CHECK(IMFTransform_SetOutputType(mft,0,output,0));
    CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0));streaming=1;
    stream=fopen("mft-smoke.h264","wb");meta=fopen("mft-smoke.csv","w");
    if(!stream||!meta){perror("output files");goto done;}
    fprintf(meta,"index,bytes,pts100ns,duration100ns,keyframe\n");
    printf("START frames=%u size=%ux%u pending_limit=%u timing=%s\n",target,width,height,MAX_PENDING,
           timing_mode==2?"variable":timing_mode==1?"30000/1001":"legacy");
    deadline=GetTickCount64()+((long_test||timing_mode)?120000:20000);
    while((frames<target||drain_events==0)&&GetTickCount64()<deadline){
        /* Keep at most four accepted samples without consumed output, even if
         * the transform's internal queue grows or NeedInput events coalesce. */
        while(submitted<target&&submitted-frames<MAX_PENDING){
            unsigned i=submitted;
            BYTE *pixels=NULL;DWORD max_len=0,len=0;
            CHECK(MFCreateSample(&sample));
            CHECK(MFCreateMemoryBuffer(width*height*3u/2u,&buffer));
            CHECK(IMFMediaBuffer_Lock(buffer,&pixels,&max_len,&len));
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)pixels[y*width+x]=(BYTE)(16u+(x*3u+y*5u+i*37u)%220u);
            for(unsigned j=width*height;j<width*height*3u/2u;j+=2){pixels[j]=(BYTE)(96u+i*17u);pixels[j+1]=(BYTE)(160u-i*13u);}
            CHECK(IMFMediaBuffer_Unlock(buffer));
            CHECK(IMFMediaBuffer_SetCurrentLength(buffer,width*height*3u/2u));
            CHECK(IMFSample_AddBuffer(sample,buffer));
            IMFMediaBuffer_Release(buffer);buffer=NULL;
            CHECK(IMFSample_SetSampleTime(sample,frame_time(i,timing_mode)+(flushed?10000000:0)));
            CHECK(IMFSample_SetSampleDuration(sample,frame_duration(i,timing_mode)));
            hr=IMFTransform_ProcessInput(mft,0,sample,0);
            IMFSample_Release(sample);sample=NULL;
            if(hr==MF_E_NOTACCEPTING)break;
            if(FAILED(hr)){fprintf(stderr,"ProcessInput %08lX at %u\n",(unsigned long)hr,i);goto done;}
            ++submitted;
            if(submitted-frames>max_pending)max_pending=submitted-frames;
            printf("INPUT %u OK pts=%lld duration=%lld\n",i,
                   (long long)(frame_time(i,timing_mode)+(flushed?10000000:0)),
                   (long long)frame_duration(i,timing_mode));fflush(stdout);
            if(submitted==FRAMES&&(hold_test||exit_test)){
                /* Leave the transform open for forced/process-exit cleanup tests. */
                if(hold_test){
                    printf("HOLD_READY pid=%lu submitted=%u\n",(unsigned long)GetCurrentProcessId(),submitted);
                    fflush(stdout);
                    Sleep(120000);
                }else{
                    printf("EXIT_WITHOUT_CLOSE\n");fflush(stdout);
                }
                /* Also skip done if the hold expires without an external kill. */
                ExitProcess(0);
            }
        }
        if(flush_test&&!flushed&&submitted==target){
            CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_COMMAND_FLUSH,0));
            flushed=1;submitted=0;
            printf("FLUSH OK; old output must be discarded\n");continue;
        }
        if(submitted==target&&!drain_sent){
            CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_COMMAND_DRAIN,0));
            drain_sent=1;
        }
        IMFMediaEvent *event=NULL;
        while(SUCCEEDED(IMFMediaEventGenerator_GetEvent(events,MF_EVENT_FLAG_NO_WAIT,&event))){
            MediaEventType type;HRESULT event_hr;
            CHECK(IMFMediaEvent_GetType(event,&type));CHECK(IMFMediaEvent_GetStatus(event,&event_hr));
            IMFMediaEvent_Release(event);event=NULL;
            if(type==METransformNeedInput)++need_input;
            if(type==METransformHaveOutput)++have_output;
            if(type==METransformDrainComplete){
                ++drain_events;
                if(!drain_sent||frames!=target||drain_events!=1){fprintf(stderr,"early/duplicate drain completion at %u/%u (count=%u)\n",frames,target,drain_events);goto done;}
            }
            if(FAILED(event_hr)){hr=event_hr;fprintf(stderr,"MF event failure %08lX\n",(unsigned long)hr);goto done;}
        }
        CHECK(MFCreateSample(&sample));
        CHECK(MFCreateMemoryBuffer(retry_test&&!retried?1u:CAP,&buffer));
        CHECK(IMFSample_AddBuffer(sample,buffer));IMFMediaBuffer_Release(buffer);buffer=NULL;
        MFT_OUTPUT_DATA_BUFFER out={0};DWORD status=0;out.pSample=sample;
        hr=IMFTransform_ProcessOutput(mft,0,1,&out,&status);
        if(out.pEvents)IMFCollection_Release(out.pEvents);
        if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT){IMFSample_Release(sample);sample=NULL;Sleep(10);continue;}
        if(retry_test&&!retried){
            if(hr!=MF_E_BUFFERTOOSMALL){fprintf(stderr,"expected MF_E_BUFFERTOOSMALL, got %08lX\n",(unsigned long)hr);goto done;}
            retried=1;IMFSample_Release(sample);sample=NULL;
            printf("BUFFER_TOO_SMALL OK; retrying same output with full capacity\n");continue;
        }
        if(FAILED(hr)){fprintf(stderr,"ProcessOutput %08lX\n",(unsigned long)hr);goto done;}
        LONGLONG pts=0,duration=0;UINT32 key=0;BYTE *bytes=NULL;DWORD max_len=0,len=0;
        LONGLONG expected=frame_time(frames,timing_mode)+(flushed?10000000:0);
        LONGLONG expected_duration=frame_duration(frames,timing_mode);
        CHECK(IMFSample_GetSampleTime(sample,&pts));
        CHECK(IMFSample_GetSampleDuration(sample,&duration));
        CHECK(IMFAttributes_GetUINT32((IMFAttributes *)sample,&MFSampleExtension_CleanPoint,&key));
        CHECK(IMFSample_ConvertToContiguousBuffer(sample,&buffer));
        CHECK(IMFMediaBuffer_Lock(buffer,&bytes,&max_len,&len));
        if(!len||fwrite(bytes,1,len,stream)!=len){fprintf(stderr,"packet write failed\n");goto done;}
        CHECK(IMFMediaBuffer_Unlock(buffer));IMFMediaBuffer_Release(buffer);buffer=NULL;
        fprintf(meta,"%u,%lu,%lld,%lld,%u\n",frames,(unsigned long)len,(long long)pts,(long long)duration,key);
        printf("OUTPUT %u bytes=%lu pts=%lld duration=%lld key=%u\n",frames,(unsigned long)len,(long long)pts,(long long)duration,key);fflush(stdout);
        if(frames>=submitted||pts!=expected||duration!=expected_duration||(frames==0&&!key)){
            fprintf(stderr,"output count/PTS/duration/keyframe mismatch index=%u submitted=%u expected_pts=%lld actual_pts=%lld expected_duration=%lld actual_duration=%lld\n",frames,submitted,(long long)expected,(long long)pts,(long long)expected_duration,(long long)duration);goto done;
        }
        ++frames;IMFSample_Release(sample);sample=NULL;
    }
    if(submitted!=target||frames!=target||drain_events!=1||(target&&!have_output)||(retry_test&&!retried)){
        fprintf(stderr,"timeout/count mismatch submitted=%u frames=%u HaveOutput=%u DrainComplete=%u\n",submitted,frames,have_output,drain_events);goto done;
    }
    /* Repeated drain is idempotent and must not emit another completion.
     * Observe briefly so an asynchronously posted duplicate cannot pass. */
    CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_COMMAND_DRAIN,0));
    deadline=GetTickCount64()+250;
    do{
        IMFMediaEvent *event=NULL;
        while(SUCCEEDED(IMFMediaEventGenerator_GetEvent(events,MF_EVENT_FLAG_NO_WAIT,&event))){
            MediaEventType type;HRESULT event_hr;
            CHECK(IMFMediaEvent_GetType(event,&type));CHECK(IMFMediaEvent_GetStatus(event,&event_hr));
            IMFMediaEvent_Release(event);
            if(FAILED(event_hr)){hr=event_hr;fprintf(stderr,"MF event failure after drain %08lX\n",(unsigned long)hr);goto done;}
            if(type==METransformDrainComplete){fprintf(stderr,"duplicate DrainComplete after drain\n");goto done;}
        }
        Sleep(10);
    }while(GetTickCount64()<deadline);
    printf("PASS frames=%u NeedInput=%u HaveOutput=%u DrainComplete=%u MaxPending=%u\n",frames,need_input,have_output,drain_events,max_pending);rc=0;
    goto done;
usage:
    fprintf(stderr,"usage: mft_encoder_smoke.exe encoder.dll [empty|flush|retry|hold|exit|timing|variable|long [width height]]\n");
    return 2;
done:
    if(buffer)IMFMediaBuffer_Release(buffer);
    if(sample)IMFSample_Release(sample);
    if(streaming&&mft)IMFTransform_ProcessMessage(mft,MFT_MESSAGE_NOTIFY_END_STREAMING,0);
    if(stream&&fclose(stream)!=0)rc=1;
    if(meta&&fclose(meta)!=0)rc=1;
    if(input)IMFMediaType_Release(input);if(output)IMFMediaType_Release(output);
    if(attrs)IMFAttributes_Release(attrs);if(events)IMFMediaEventGenerator_Release(events);
    if(mft)IMFTransform_Release(mft);if(factory)IClassFactory_Release(factory);
    if(dll)FreeLibrary(dll);if(mf_started)MFShutdown();if(com_started)CoUninitialize();
    return rc;
}

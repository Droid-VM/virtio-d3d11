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

static const CLSID encoder_clsid = {0x9f3c2a12,0x7d64,0x4e3b,{0x9a,0x21,0x6c,0x0d,0x5b,0x48,0xe7,0xa3}};
typedef HRESULT (WINAPI *GetClassObjectFn)(REFCLSID,REFIID,void **);
#define W 640u
#define H 480u
#define FRAMES 4u
#define CAP (4u*1024u*1024u)
#define PACK(a,b) (((UINT64)(a)<<32)|(b))
#define CHECK(call) do { hr=(call); if(FAILED(hr)){fprintf(stderr,"%s: 0x%08lX\n",#call,(unsigned long)hr);goto done;} } while(0)

static HRESULT media_type(IMFMediaType **type,const GUID *subtype)
{
    HRESULT hr=MFCreateMediaType(type);
    if(FAILED(hr))return hr;
    hr=IMFMediaType_SetGUID(*type,&MF_MT_MAJOR_TYPE,&MFMediaType_Video);
    if(SUCCEEDED(hr))hr=IMFMediaType_SetGUID(*type,&MF_MT_SUBTYPE,subtype);
    if(SUCCEEDED(hr))hr=IMFMediaType_SetUINT64(*type,&MF_MT_FRAME_SIZE,PACK(W,H));
    if(SUCCEEDED(hr))hr=IMFMediaType_SetUINT64(*type,&MF_MT_FRAME_RATE,PACK(30,1));
    if(SUCCEEDED(hr))hr=IMFMediaType_SetUINT32(*type,&MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
    return hr;
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
    unsigned frames=0,need_input=0,have_output=0,drain_events=0;
    unsigned target=FRAMES;
    int rc=1,com_started=0,mf_started=0,streaming=0;
    ULONGLONG deadline;
    if(argc<2||argc>3){fprintf(stderr,"usage: mft_encoder_smoke.exe encoder.dll [empty]\n");return 2;}
    if(argc==3)target=0;
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
    CHECK(media_type(&input,&MFVideoFormat_NV12));
    CHECK(IMFMediaType_SetUINT32(input,&MF_MT_DEFAULT_STRIDE,W));
    CHECK(media_type(&output,&MFVideoFormat_H264));
    CHECK(IMFMediaType_SetUINT32(output,&MF_MT_AVG_BITRATE,2000000));
    CHECK(IMFTransform_SetInputType(mft,0,input,0));
    CHECK(IMFTransform_SetOutputType(mft,0,output,0));
    CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0));streaming=1;
    stream=fopen("mft-smoke.h264","wb");meta=fopen("mft-smoke.csv","w");
    if(!stream||!meta){perror("output files");goto done;}
    fprintf(meta,"index,bytes,pts100ns,keyframe\n");
    for(unsigned i=0;i<target;++i){
        BYTE *pixels=NULL;DWORD max_len=0,len=0;
        CHECK(MFCreateSample(&sample));
        CHECK(MFCreateMemoryBuffer(W*H*3u/2u,&buffer));
        CHECK(IMFMediaBuffer_Lock(buffer,&pixels,&max_len,&len));
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)pixels[y*W+x]=(BYTE)(16u+(x*3u+y*5u+i*37u)%220u);
        for(unsigned j=W*H;j<W*H*3u/2u;j+=2){pixels[j]=(BYTE)(96u+i*17u);pixels[j+1]=(BYTE)(160u-i*13u);}
        CHECK(IMFMediaBuffer_Unlock(buffer));
        CHECK(IMFMediaBuffer_SetCurrentLength(buffer,W*H*3u/2u));
        CHECK(IMFSample_AddBuffer(sample,buffer));
        IMFMediaBuffer_Release(buffer);buffer=NULL;
        CHECK(IMFSample_SetSampleTime(sample,(LONGLONG)i*333333));
        CHECK(IMFSample_SetSampleDuration(sample,333333));
        CHECK(IMFTransform_ProcessInput(mft,0,sample,0));
        IMFSample_Release(sample);sample=NULL;
        printf("INPUT %u OK\n",i);fflush(stdout);
    }
    CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_COMMAND_DRAIN,0));
    CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_COMMAND_DRAIN,0));
    deadline=GetTickCount64()+20000;
    while((frames<target||drain_events==0)&&GetTickCount64()<deadline){
        IMFMediaEvent *event=NULL;
        while(SUCCEEDED(IMFMediaEventGenerator_GetEvent(events,MF_EVENT_FLAG_NO_WAIT,&event))){
            MediaEventType type;HRESULT event_hr;
            IMFMediaEvent_GetType(event,&type);IMFMediaEvent_GetStatus(event,&event_hr);
            if(type==METransformNeedInput)++need_input;
            if(type==METransformHaveOutput)++have_output;
            if(type==METransformDrainComplete){++drain_events;if(frames!=target){fprintf(stderr,"early drain completion at %u/%u\n",frames,target);IMFMediaEvent_Release(event);goto done;}}
            IMFMediaEvent_Release(event);event=NULL;
            if(FAILED(event_hr)){hr=event_hr;fprintf(stderr,"MF event failure %08lX\n",(unsigned long)hr);goto done;}
        }
        CHECK(MFCreateSample(&sample));CHECK(MFCreateMemoryBuffer(CAP,&buffer));
        CHECK(IMFSample_AddBuffer(sample,buffer));IMFMediaBuffer_Release(buffer);buffer=NULL;
        MFT_OUTPUT_DATA_BUFFER out={0};DWORD status=0;out.pSample=sample;
        hr=IMFTransform_ProcessOutput(mft,0,1,&out,&status);
        if(out.pEvents)IMFCollection_Release(out.pEvents);
        if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT){IMFSample_Release(sample);sample=NULL;Sleep(10);continue;}
        if(FAILED(hr)){fprintf(stderr,"ProcessOutput %08lX\n",(unsigned long)hr);goto done;}
        LONGLONG pts=0;UINT32 key=0;BYTE *bytes=NULL;DWORD max_len=0,len=0;
        CHECK(IMFSample_GetSampleTime(sample,&pts));
        CHECK(IMFAttributes_GetUINT32((IMFAttributes *)sample,&MFSampleExtension_CleanPoint,&key));
        CHECK(IMFSample_ConvertToContiguousBuffer(sample,&buffer));
        CHECK(IMFMediaBuffer_Lock(buffer,&bytes,&max_len,&len));
        if(!len||fwrite(bytes,1,len,stream)!=len){fprintf(stderr,"packet write failed\n");goto done;}
        CHECK(IMFMediaBuffer_Unlock(buffer));IMFMediaBuffer_Release(buffer);buffer=NULL;
        fprintf(meta,"%u,%lu,%lld,%u\n",frames,(unsigned long)len,(long long)pts,key);
        printf("OUTPUT %u bytes=%lu pts=%lld key=%u\n",frames,(unsigned long)len,(long long)pts,key);fflush(stdout);
        if(pts!=(LONGLONG)frames*333330 || (frames==0&&!key)){fprintf(stderr,"PTS/keyframe mismatch\n");goto done;}
        ++frames;IMFSample_Release(sample);sample=NULL;
    }
    if(frames!=target||drain_events!=1||(target&&!have_output)){fprintf(stderr,"timeout/count mismatch frames=%u HaveOutput=%u DrainComplete=%u\n",frames,have_output,drain_events);goto done;}
    /* Repeated drain is idempotent and must not emit another completion. */
    CHECK(IMFTransform_ProcessMessage(mft,MFT_MESSAGE_COMMAND_DRAIN,0));
    {IMFMediaEvent *event=NULL;while(SUCCEEDED(IMFMediaEventGenerator_GetEvent(events,MF_EVENT_FLAG_NO_WAIT,&event))){MediaEventType type;IMFMediaEvent_GetType(event,&type);IMFMediaEvent_Release(event);if(type==METransformDrainComplete){fprintf(stderr,"duplicate DrainComplete\n");goto done;}}}
    printf("PASS frames=%u NeedInput=%u HaveOutput=%u DrainComplete=%u\n",frames,need_input,have_output,drain_events);rc=0;
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

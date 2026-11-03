#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxva.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include "idr_input.h"
static std::string pending_log;
static int logprintf(const char* format, ...) {
    char line[4096]; va_list args; va_start(args,format);
    int n=vsnprintf(line,sizeof(line),format,args); va_end(args);
    if(n<0 || size_t(n)>=sizeof(line)) throw std::runtime_error("log formatting");
    pending_log.append(line,size_t(n)); return n;
}
static int logputs(const char* text) { pending_log.append(text); pending_log.push_back('\n'); return 0; }
static bool flushlog() {
    bool ok=fwrite(pending_log.data(),1,pending_log.size(),stdout)==pending_log.size();
    pending_log.clear(); return fflush(stdout)==0 && !ferror(stdout) && ok;
}
#define printf logprintf
#define puts logputs
static LONGLONG first_end_return_qpc=0, second_end_call_qpc=0;
static LONGLONG qpc_now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
#include <string>
#include "idr_input.h"
using Microsoft::WRL::ComPtr;
static void ck(HRESULT hr, const char* op) {
    printf("API %s hr=0x%08lx tick=%llu\n",op,(unsigned long)hr,GetTickCount64());
    if(FAILED(hr)) throw std::runtime_error(op);
}
static std::vector<uint8_t> load(const char* path) {
    FILE* f=fopen(path,"rb"); if(!f) throw std::runtime_error("open input");
    std::vector<uint8_t> a; int c;
    while((c=fgetc(f))!=EOF) { a.push_back(uint8_t(c)); if(a.size()>65536) { fclose(f); throw std::runtime_error("input limit"); } }
    bool bad=ferror(f)!=0; fclose(f); if(bad) throw std::runtime_error("read input"); return a;
}
static void put(ID3D11VideoContext* vc, ID3D11VideoDecoder* dec,
                D3D11_VIDEO_DECODER_BUFFER_TYPE type, const void* data, UINT bytes,
                D3D11_VIDEO_DECODER_BUFFER_DESC& desc) {
    UINT cap=0; void* dst=nullptr;
    ck(vc->GetDecoderBuffer(dec,type,&cap,&dst),"GetDecoderBuffer");
    if(!dst || cap<bytes) { vc->ReleaseDecoderBuffer(dec,type); throw std::runtime_error("decoder buffer capacity"); }
    memcpy(dst,data,bytes); ck(vc->ReleaseDecoderBuffer(dec,type),"ReleaseDecoderBuffer");
    desc={}; desc.BufferType=type; desc.DataSize=bytes;
}
static void submit(ID3D11VideoContext* vc, ID3D11VideoDecoder* dec,
                   ID3D11VideoDecoderOutputView* view, const IdrInput& x, UINT seq) {
    DXVA_PicParams_H264 p{};
    p.wFrameWidthInMbsMinus1=WORD(x.wmb); p.wFrameHeightInMbsMinus1=WORD(x.hmb);
    p.Reserved16Bits=3; // DXVA H264 specification requires this reserved value
    p.CurrPic.bPicEntry=0; // each output view is its own one-slice texture
    for(auto& r:p.RefFrameList) r.bPicEntry=0xff; // independent IDR: no active reference surfaces
    p.num_ref_frames=UCHAR(x.refs); p.RefPicFlag=1; p.IntraPicFlag=1;
    p.chroma_format_idc=1; p.frame_mbs_only_flag=1; p.direct_8x8_inference_flag=x.direct;
    p.log2_max_frame_num_minus4=UCHAR(x.logfn); p.pic_order_cnt_type=UCHAR(x.poc);
    p.log2_max_pic_order_cnt_lsb_minus4=UCHAR(x.logpoc); p.frame_num=USHORT(x.frame_num);
    p.CurrFieldOrderCnt[0]=p.CurrFieldOrderCnt[1]=int(x.poc_lsb);
    p.num_ref_idx_l0_active_minus1=UCHAR(x.l0); p.num_ref_idx_l1_active_minus1=UCHAR(x.l1);
    p.pic_init_qp_minus26=CHAR(x.qp); p.pic_init_qs_minus26=CHAR(x.qs);
    p.chroma_qp_index_offset=p.second_chroma_qp_index_offset=CHAR(x.chroma);
    p.deblocking_filter_control_present_flag=x.deblock; p.constrained_intra_pred_flag=x.constrained;
    p.redundant_pic_cnt_present_flag=x.redundant; p.ContinuationFlag=1;
    p.MinLumaBipredSize8x8Flag=0; // SPS level_idc=10; flag is set for level >=3.1
    p.MbsConsecutiveFlag=1; // one complete raster-order slice, FMO disabled
    p.StatusReportFeedbackNumber=seq;
    DXVA_Qmatrix_H264 q{}; memset(&q,16,sizeof(q)); // baseline default flat scaling lists
    DXVA_Slice_H264_Short sc{}; sc.BSNALunitDataLocation=x.slice_offset;
    // D3D11 raw=2: one short slice, 3-byte start code, 128-byte aligned bitstream payload.
    std::vector<uint8_t> bs=x.bitstream; bs.resize((bs.size()+127)&~size_t(127),0);
    sc.SliceBytesInBuffer=UINT(bs.size()-x.slice_offset);
    printf("SUBMIT seq=%u decoder=%p view=%p frame_num=%u idr_id=%u poc_type=%u poc_lsb=%u refs=%u bytes=%zu\n",
        seq,dec,view,x.frame_num,x.idr_id,x.poc,x.poc_lsb,x.refs,bs.size());
    ck(vc->DecoderBeginFrame(dec,view,0,nullptr),"DecoderBeginFrame");
    bool ended=false;
    try {
        D3D11_VIDEO_DECODER_BUFFER_DESC d[4]{};
        put(vc,dec,D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,&p,sizeof(p),d[0]);
        put(vc,dec,D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,&q,sizeof(q),d[1]);
        put(vc,dec,D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,&sc,sizeof(sc),d[2]);
        put(vc,dec,D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,bs.data(),UINT(bs.size()),d[3]);
        ck(vc->SubmitDecoderBuffers(dec,4,d),"SubmitDecoderBuffers");
        if(seq==2) second_end_call_qpc=qpc_now();
        HRESULT hr=vc->DecoderEndFrame(dec);
        if(seq==1) first_end_return_qpc=qpc_now();
        if(seq==2) printf("PRODUCER_GAP first_end_return_qpc=%lld second_end_call_qpc=%lld\n",
            (long long)first_end_return_qpc,(long long)second_end_call_qpc);
        ended=true; ck(hr,"DecoderEndFrame");
        printf("API_ACCEPTED seq=%u tick=%llu (confirm pipeline accepted in UMD log)\n",seq,GetTickCount64());
    } catch(...) { if(!ended) vc->DecoderEndFrame(dec); throw; }
}
static bool readback(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                     const char* path, int ey, int eu, int ev) {
    // CopyResource triggers the production texture-read synchronization hook.
    // Driver/API calls themselves can block: the parent process watchdog is mandatory.
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; td.MiscFlags=0;
    ComPtr<ID3D11Texture2D> stage; ck(dev->CreateTexture2D(&td,nullptr,stage.GetAddressOf()),"Create staging");
    ctx->CopyResource(stage.Get(),tex);
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0}; ComPtr<ID3D11Query> q;
    ck(dev->CreateQuery(&qd,q.GetAddressOf()),"Create event query"); ctx->End(q.Get()); ctx->Flush();
    ULONGLONG deadline=GetTickCount64()+5000; BOOL ready=FALSE;
    for(;;) {
        HRESULT hr=ctx->GetData(q.Get(),&ready,sizeof(ready),0);
        if(FAILED(hr)) { printf("READBACK query fail=0x%08lx\n",(unsigned long)hr); return false; }
        if(hr==S_OK && ready) break;
        if(GetTickCount64()>=deadline) { puts("READBACK event timeout"); return false; } Sleep(1);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    for(;;) {
        HRESULT hr=ctx->Map(stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64()<deadline) { Sleep(1); continue; }
        if(FAILED(hr)) { printf("READBACK map fail=0x%08lx\n",(unsigned long)hr); return false; } break;
    }
    if(m.RowPitch<td.Width || !m.pData) { ctx->Unmap(stage.Get(),0); return false; }
    std::vector<uint8_t> nv12(td.Width*td.Height*3/2);
    for(UINT r=0;r<td.Height*3/2;++r) memcpy(nv12.data()+r*td.Width,(uint8_t*)m.pData+r*m.RowPitch,td.Width);
    ctx->Unmap(stage.Get(),0);
    FILE* f=fopen(path,"wb"); if(!f) throw std::runtime_error("open output");
    bool written=fwrite(nv12.data(),1,nv12.size(),f)==nv12.size(); written=fclose(f)==0 && written;
    unsigned sums[3]{}; unsigned bad=0;
    for(UINT i=0;i<td.Width*td.Height;++i) { sums[0]+=nv12[i]; bad+=abs(int(nv12[i])-ey)>3; }
    for(UINT i=td.Width*td.Height;i<nv12.size();i+=2) {
        sums[1]+=nv12[i]; sums[2]+=nv12[i+1]; bad+=abs(int(nv12[i])-eu)>3 || abs(int(nv12[i+1])-ev)>3;
    }
    printf("READBACK %s tex=%p subresource=0 YUVmean=%.3f,%.3f,%.3f bad=%u written=%d\n",
        path,tex,double(sums[0])/(td.Width*td.Height),double(sums[1])/(td.Width*td.Height/4),
        double(sums[2])/(td.Width*td.Height/4),bad,int(written));
    return written && bad==0;
}
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    pending_log.reserve(65536);
    if(argc!=4 || strcmp(argv[1],"--baseline")) {
        puts("usage: h264-production-flush-probe.exe --baseline red.h264 blue.h264"); flushlog(); return 2;
    }
    const bool flush_mode=false;
    try {
        LARGE_INTEGER freq; if(!QueryPerformanceFrequency(&freq) || freq.QuadPart<=0) throw std::runtime_error("QPC frequency");
        auto ra=load(argv[2]), ba=load(argv[3]); auto red=parse_idr(ra), blue=parse_idr(ba);
        // These exact SPS/PPS were obtained from ffmpeg, independently soft decoded.
        const std::vector<uint8_t> expected_sps={0x67,0x42,0xc0,0xa,0xdc,0x63,0x6c,0x4,0x40,0x0,0x0,0x3,0x0,0x40,0x0,0x0,0x3,0x0,0x83,0xc4,0x89,0xe0};
        const std::vector<uint8_t> expected_pps={0x68,0xce,0xf,0x2c,0x80};
        const std::vector<uint8_t> red_slice={0x65,0x88,0x84,0x4,0xbc,0x46,0x28,0x0,0xa,0x8b,0xc7,0x0,0x1,0x28,0xd8,0xe0,0x0,0x2f,0xad,0x27,0x27,0x27,0x27,0x27,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x75,0xd7,0x5d,0x78};
        auto blue_slice=red_slice;
        blue_slice[8]=0x0c; blue_slice[9]=0xc4; blue_slice[13]=0x7d;
        blue_slice[14]=0x68; blue_slice[17]=0x22; blue_slice[18]=0x43;
        if(red.sps!=expected_sps || blue.sps!=expected_sps || red.pps!=expected_pps || blue.pps!=expected_pps ||
           red.slice!=red_slice || blue.slice!=blue_slice)
            throw std::runtime_error("requires exact independently decoded bundled red/blue NAL payloads");
        printf("A06 real D3D11 pid=%lu input=%zu/%zu; gen/token unavailable at public API\n",GetCurrentProcessId(),ra.size(),ba.size());
        SetEnvironmentVariableA("VIRTIO_WDDM_TAG_LIFE_EXPERIMENT","0");
        SetEnvironmentVariableA("VIRTIO_WDDM_MEDIA_TAG_EXPERIMENT","0");
        SetEnvironmentVariableA("VIRTIO_WDDM_PIPELINE","1");
        SetEnvironmentVariableA("VIRTIO_WDDM_PIPELINE_TRACE","1");
        ComPtr<IDXGIFactory1> fac; ck(CreateDXGIFactory1(IID_PPV_ARGS(fac.GetAddressOf())),"Create factory");
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> a; HRESULT hr=fac->EnumAdapters1(i,a.GetAddressOf());
            if(hr==DXGI_ERROR_NOT_FOUND) break; ck(hr,"EnumAdapters1");
            DXGI_ADAPTER_DESC1 d{}; ck(a->GetDesc1(&d),"adapter desc");
            printf("ADAPTER index=%u name=%ls vendor=%x device=%x flags=%x LUID=%08lx:%08lx\n",i,d.Description,d.VendorId,d.DeviceId,d.Flags,(unsigned long)d.AdapterLuid.HighPart,d.AdapterLuid.LowPart);
            if(d.VendorId==0x1af4 && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter=a; break; }
        }
        if(!adapter) throw std::runtime_error("no virtio hardware adapter; no fallback");
        ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL fl;
        ck(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,dev.GetAddressOf(),&fl,ctx.GetAddressOf()),"D3D11CreateDevice");
        ComPtr<ID3D11VideoDevice> vd; ComPtr<ID3D11VideoContext> vc;
        ck(dev.As(&vd),"QI video device"); ck(ctx.As(&vc),"QI video context");
        D3D11_VIDEO_DECODER_DESC dd{D3D11_DECODER_PROFILE_H264_VLD_NOFGT,(red.wmb+1)*16,(red.hmb+1)*16,DXGI_FORMAT_NV12};
        BOOL supported=FALSE; ck(vd->CheckVideoDecoderFormat(&dd.Guid,dd.OutputFormat,&supported),"Check format");
        if(!supported) throw std::runtime_error("NV12 unsupported");
        UINT n=0; ck(vd->GetVideoDecoderConfigCount(&dd,&n),"Get config count");
        D3D11_VIDEO_DECODER_CONFIG config{}; bool found=false;
        for(UINT i=0;i<n;++i) { D3D11_VIDEO_DECODER_CONFIG c{}; ck(vd->GetVideoDecoderConfig(&dd,i,&c),"Get config");
            if(c.ConfigBitstreamRaw==2 && IsEqualGUID(c.guidConfigBitstreamEncryption,DXVA_NoEncrypt)) { config=c; found=true; break; } }
        if(!found) throw std::runtime_error("no raw2 unencrypted config");
        printf("SOURCE_EXPECTED geometry=%ux%u SPS/PicParams/texture matched; host96 observed not advertised-min; runtime negotiation requires external evidence\n",dd.SampleWidth,dd.SampleHeight);
        ComPtr<ID3D11VideoDecoder> dec; ck(vd->CreateVideoDecoder(&dd,&config,dec.GetAddressOf()),"CreateVideoDecoder");
        ComPtr<ID3D11Texture2D> tex[2]; ComPtr<ID3D11VideoDecoderOutputView> view[2];
        for(int i=0;i<2;++i) {
            D3D11_TEXTURE2D_DESC td{}; td.Width=dd.SampleWidth; td.Height=dd.SampleHeight; td.MipLevels=td.ArraySize=1;
            td.Format=DXGI_FORMAT_NV12; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_DECODER;
            ck(dev->CreateTexture2D(&td,nullptr,tex[i].GetAddressOf()),"Create output texture");
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC ov{}; ov.DecodeProfile=dd.Guid; ov.ViewDimension=D3D11_VDOV_DIMENSION_TEXTURE2D;
            ck(vd->CreateVideoDecoderOutputView(tex[i].Get(),&ov,view[i].GetAddressOf()),"Create output view");
        }
        submit(vc.Get(),dec.Get(),view[0].Get(),red,1);
        // Accepted is sufficient for entry. Do not attempt pre-flush readback:
        // a delayed first host output must not prevent reaching second EndFrame.
        if(flush_mode) Sleep(250);
        submit(vc.Get(),dec.Get(),view[1].Get(),blue,2);
        double producer_gap_ms=1000.0*double(second_end_call_qpc-first_end_return_qpc)/double(freq.QuadPart);
        printf("MODE %s producer_end1_return_qpc=%lld end2_call_qpc=%lld freq=%lld gap_ms=%.6f decoder=%p\n",
            argv[1],(long long)first_end_return_qpc,(long long)second_end_call_qpc,(long long)freq.QuadPart,producer_gap_ms,dec.Get());
        printf("CONTROL %s UMD_SAME_CTX_GENERATION_EVIDENCE_REQUIRED; baseline requires gen0, otherwise INVALID_CONTROL\n",argv[1]);
        bool ok=readback(dev.Get(),ctx.Get(),tex[1].Get(),"after-blue.nv12",41,240,110);
        ck(dev->GetDeviceRemovedReason(),"device status");
        printf("RESULT api_and_pixels=%s; A06_GEN_TOKEN_VERDICT=EXTERNAL_EVIDENCE_REQUIRED\n",ok?"PASS":"FAIL");
        view[0].Reset(); view[1].Reset(); dec.Reset(); // exercise normal decoder destruction before exit
        ctx->ClearState(); ctx->Flush();
        puts("NORMAL_EXIT cleanup completed");
        if(!flushlog()) return 1;
        return ok?0:1;
    } catch(const std::exception& e) { printf("FAIL %s\n",e.what()); flushlog(); return 1; }
}

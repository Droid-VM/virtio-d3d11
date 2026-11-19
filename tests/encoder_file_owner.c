/* Server-built negative checks for encoder WDFFILEOBJECT/generation ownership. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../vmedia_abi.h"
int main(void)
{
    HANDLE a=INVALID_HANDLE_VALUE,b=INVALID_HANDLE_VALUE;
    VM_OPEN_IN open_in={1,VM_OPEN_CODEC_H264};
    VM_OPEN_OUT first={0},second={0},other={0};
    VM_CLOSE_IN close_in={0};
    VM_ENCODER_CONFIG cfg={0};
    DWORD used=0,error;
    int rc=1;
    a=CreateFileW(VMEDIA_LINK_ENCODER,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    b=CreateFileW(VMEDIA_LINK_ENCODER,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(a==INVALID_HANDLE_VALUE||b==INVALID_HANDLE_VALUE)goto done;
    if(!DeviceIoControl(a,VM_IOCTL_OPEN,&open_in,sizeof(open_in),&first,sizeof(first),&used,NULL))goto done;
    if(first.DriverVersion<0x00010021u){fprintf(stderr,"requires driver .33\n");goto done;}
    close_in.SessionId=first.SessionId;close_in.SessionToken=first.SessionToken;
    if(DeviceIoControl(b,VM_IOCTL_CLOSE,&close_in,sizeof(close_in),NULL,0,&used,NULL)){fprintf(stderr,"wrong file CLOSE accepted\n");goto done;}
    error=GetLastError();printf("WRONG_FILE_CLOSE rejected=%lu\n",(unsigned long)error);
    if(DeviceIoControl(b,VM_IOCTL_OPEN,&open_in,sizeof(open_in),&other,sizeof(other),&used,NULL)){fprintf(stderr,"competing OPEN accepted\n");goto done;}
    CloseHandle(b);b=INVALID_HANDLE_VALUE;
    /* Cleanup of a non-owner file must not close a's session. */
    if(!DeviceIoControl(a,VM_IOCTL_CLOSE,&close_in,sizeof(close_in),NULL,0,&used,NULL))goto done;
    if(!DeviceIoControl(a,VM_IOCTL_OPEN,&open_in,sizeof(open_in),&second,sizeof(second),&used,NULL))goto done;
    if(second.SessionToken==first.SessionToken){fprintf(stderr,"generation reused\n");goto done;}
    /* Same file, old token: acknowledged stale CLOSE, current owner untouched. */
    if(!DeviceIoControl(a,VM_IOCTL_CLOSE,&close_in,sizeof(close_in),NULL,0,&used,NULL))goto done;
    cfg.Version=VM_ENCODER_ABI_VERSION;cfg.Size=sizeof(cfg);cfg.SessionToken=second.SessionToken;
    cfg.Width=640;cfg.Height=480;cfg.YStride=640;cfg.UVStride=640;cfg.UVOffset=640*480;
    cfg.FrameRateNum=30;cfg.FrameRateDen=1;cfg.Bitrate=2000000;cfg.GopFrames=30;
    if(!DeviceIoControl(a,VM_IOCTL_ENC_CONFIG,&cfg,sizeof(cfg),&cfg,sizeof(cfg),&used,NULL))goto done;
    printf("PASS wrong-file cleanup isolated; stale token %u cannot close current token %u\n",first.SessionToken,second.SessionToken);
    close_in.SessionId=second.SessionId;close_in.SessionToken=second.SessionToken;
    if(!DeviceIoControl(a,VM_IOCTL_CLOSE,&close_in,sizeof(close_in),NULL,0,&used,NULL))goto done;
    rc=0;
done:
    if(rc)fprintf(stderr,"owner check failed Win32=%lu\n",(unsigned long)GetLastError());
    if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);
    return rc;
}

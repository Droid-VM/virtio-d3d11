# Windows VPU P1 probes

## Existing transport probe

`../p3_2_flush_probe.c` is retained as a low-level `vmedia_client` transport probe. It validates OPEN, repeated H.264 AU SEND/GET_FRAME, FLUSH and post-flush decoded NV12 metadata/pixels. It does **not** call the D3D11 video DDI and therefore cannot be counted as the full author P2/P1 D3D11 client gate.

Expected build is ARM64 with `VMEDIA_CLIENT_BUILD`, linking `p3_2_flush_probe.c` and `vmedia_client.c`. Output belongs in `02-编译产物`; raw guest output belongs in `04-测试记录`.

## Next probe

Add a separate native D3D11 video API client only after reviewing the existing H264 production probe fixture and legal DXVA PicParams. It must use the current real D3D11CreateDevice/ID3D11VideoDevice/ID3D11VideoContext path, then assert the same SEND/CAPTURE and target NV12 pixel result. Do not turn the transport probe into a fake DDI test.

# Native ARM64 D3D11 H.264 probe

This probe was recovered from the verified R3 historical experiment into the current Windows VPU development worktree. The original source, fixture and complete R3 evidence remain under `04-测试记录/AV1历史实验-20261101/h264-production-flush-probe/out-r3`; this worktree copy contains source inputs only.

## What it exercises

- Real `D3D11CreateDevice` on the VirtIO GPU adapter.
- `ID3D11VideoDevice` H.264 VLD decoder configuration.
- Two NV12 output textures/views.
- Legal red/blue baseline IDR inputs with exact PicParams and slice metadata derived from the fixture.
- `--baseline`: both inputs without an intentional delay, expected UMD generation 0 control.
- `--flush`: second input after the existing 250ms delay, intended to reach the real non-tag H.264 LIFE flush entry.
- GPU readback and exact blue NV12 pixel verification.

This is not a fake IOCTL probe and does not call `DeviceIoControl` directly. It is not a complete §5.4 old-completion race or direct token observation test; the public D3D11 API cannot expose the UMD token.

## Build

Build only in a clean external output directory on the ARM64 llvm-mingw server. Do not write `.o`, `.exe`, `.dll` or build directories into this source worktree. The expected link inputs are `d3d11`, `dxgi`, `dxguid`, `kernel32`, and the ARM64 Windows headers/WRL available in the existing toolchain.

Example command, with paths adapted by the build host:

```sh
aarch64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra \
  -I/path/to/windows/include \
  -o /srv/vpu-builds/d3d11-h264-probe-YYYYMMDD/h264-production-flush-probe.exe \
  probe.cpp -ld3d11 -ldxgi -ldxguid
```

Keep command, stdout, stderr, exit code, PE machine/import output and SHA256 in the matching `02-编译产物` directory. Do not deploy or run until the parent task explicitly releases the artifact.

## Source identity

- Recovered R3 source SHA256: see the historical `out-r3/SHA256SUMS.txt`.
- R3 baseline/flush results remain historical evidence, not a new acceptance result.

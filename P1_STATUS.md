# Windows VPU P1/P2 development status

- Worktree branch: `vpu-windows-p1-20261103`
- Base commit: `4a516ab`
- Parent repository: `01-源码/virtio-d3d11`
- Host crosvm/media feature and APK runtime are already declared complete by the owner; this worktree does not modify host code.

## Existing implementation

- `d3d11_video.c` contains real H.264 decoder object creation, output-view selection, `DecoderBeginFrame`, `SubmitDecoderBuffers` AU collection, and `DecoderEndFrame` submission.
- `vmedia_client.c/.h` contains OPEN/SEND/GET_FRAME/MAP_FRAME/RELEASE/FLUSH and tag/generation-capable calls.
- NV12/P010 resource format and VideoProcessor code exists in the current UMD.
- Recent VP resource-identity refresh fix is represented in the parent base commit history/worktree lineage and was validated separately as F01.

## P1/P2 next implementation boundary

1. Build a small native ARM64 D3D11 H.264 client/probe from this worktree or a sibling `02-编译产物` test source, using the existing legal 96x96 red/blue fixtures and real D3D11 video API.
2. Exercise one decoder/session: Begin/Submit/End -> SEND -> host CAPTURE -> NV12 readback; record exact HRESULT, UMD generation/job, MediaCodec host evidence and pixel hash.
3. Add explicit EOS/flush/timeout/error assertions without changing production behavior.
4. Only after the minimal client path is repeatable, connect Chrome/Edge/camera application evidence.

## Explicit non-goals for this worktree

- No host/crosvm merge or rebuild; owner confirmed APK already includes media feature and startup parameters.
- No generated object/library/DLL/SYS/PDB files in this source worktree.
- No long soak; user-run stability scripts remain last.
- No OOBE changes.

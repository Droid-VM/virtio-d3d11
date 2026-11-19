# DroidVM encoder MFT source

This directory contains the COM/Media Foundation MFT sources and the server-only build script. The MFT build consumes `../vmedia_client.c`, `../vmedia_client.h`, and `../vmedia_abi.h` from the repository root; it does not compile the historical `mft/vmedia_client.*` copies.

Build on the F01 server only:

```sh
cd /srv/mft-fix
ARCH=arm64 MODES="decoder encoder" bash tools/build-mft.sh
```

The encoder MFT now opens the encoder function, configures the canonical ABI, copies/pads NV12 input into the negotiated stride layout, queues input, obtains H.264 packets on a worker, raises `METransformHaveOutput`, and writes packet bytes/timestamps/keyframe metadata into caller-provided output samples. It accepts only dimensions at least 128x128 for the current Qualcomm AVC encoder capability. The host currently rejects ForceKeyframe and the private Drain/Flush controls; those operations return explicit errors rather than reporting false completion. Runtime COM/MFT activation and ProcessInput/ProcessOutput behavior still require an ARM64 Windows VM test.

# DroidVM encoder MFT source

This directory contains the COM/Media Foundation MFT sources and the server-only build script. The MFT build consumes `../vmedia_client.c`, `../vmedia_client.h`, and `../vmedia_abi.h` from the repository root; it does not compile the historical `mft/vmedia_client.*` copies.

Build on the F01 server only:

```sh
cd /srv/mft-fix
ARCH=arm64 MODES="decoder encoder" bash tools/build-mft.sh
```

The encoder MFT now opens the encoder function, configures the canonical ABI, copies/pads NV12 input into the negotiated stride layout, queues input, obtains H.264 packets on a worker, raises `METransformHaveOutput`, and writes packet bytes/timestamps/keyframe metadata into caller-provided output samples. It accepts only dimensions at least 128x128 for the current Qualcomm AVC encoder capability. Driver 1.0.0.32 supports encoder STOP/LAST drain, including an empty-stream completion. The MFT raises DrainComplete once, only after all tail packets have been delivered. MFT Flush joins the worker, closes the old token-owned session, discards its output/events, and opens/configures a fresh session. The private ENC_FLUSH IOCTL remains unsupported. Direct activation on the ARM64 Windows VM passed four-frame encoding, empty/nonempty drain, and Flush with distinct old/new PTS ranges; global registration/application compatibility and sustained throughput are separate checks. ForceKeyframe is not enabled in the guest yet: the actual virtio-media SET path exists, but the production backend behavior still needs verification.

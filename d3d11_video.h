#pragma once

#include <winddk_compat.h>
#include <d3d10umddi.h>

/* D3D11 video DDI (DXVA / ID3D11VideoDevice) -- decoder decode + caps, and
 * processor caps + a real blit.
 *
 * D3D11 keeps video in a function table of its own, separate from the 3D
 * device funcs: D3D11_1DDI_VIDEODEVICEFUNCS (d3d10umddi.h:3957), and the
 * 2.0/2.1 variants with the same prefix. The driver owns the retrieval
 * callback: virtio_wddm_fill_video_funcs() stores
 * virtio_wddm_retrieve_subobject() into
 * D3D10DDIARG_CREATEDEVICE::ppfnRetrieveSubObject, and the runtime calls it
 * later with SubDeviceID D3D11_1DDI_VIDEO_FUNCTIONS (2) /
 * D3DWDDM2_0DDI_VIDEO_FUNCTIONS (3) / D3DWDDM2_1DDI_VIDEO_FUNCTIONS (5),
 * depending on the device's DDI version. The callback fills the table the
 * runtime points it at. See deliverables/RETRIEVE-SUBOBJECT-ANALYSIS.md (E104).
 *
 * Before M1 this driver never implemented or registered that callback, so the
 * runtime had no video table at all and reported a device without video
 * hardware: dxdiag shows "Video Accel: Unknown" / no DXVA modes, and Media
 * Foundation's Video Processor MFT (msvproc.dll) finds no processor and stops.
 * M1/M2 then read *ppfnRetrieveSubObject as if the runtime had supplied it,
 * which is always NULL; M3 fixes that direction. See deliverables/M1-D3D11-
 * VIDEO-CAPS.md and deliverables/M3-RETRIEVE-SUBOBJECT-FIX.md.
 *
 * M2 implements the processor blit on top of DXVK's video processor, which this
 * UMD already links in; see deliverables/M2-VIDEO-BLIT.md and
 * d3d11_video_dxvk.h.
 *
 * P2-2a gives the decoder side caps + object lifetime: one profile (H.264 VLD
 * NoFGT), NV12 output, one config, and CreateVideoDecoder opens a virtio-media
 * decoder session through vmedia_client's vm_open() (vmedia_client.c is compiled
 * into this DLL; see deliverables/P2-2A-DECODER-DDI.md). DestroyVideoDecoder
 * closes it with vm_close().
 *
 * P2-2b makes that decoder actually decode: CreateVideoDecoderOutputView binds
 * the application's NV12 texture, DecoderBeginFrame selects it,
 * SubmitDecoderBuffers appends the Annex-B bytes from the submitted BITSTREAM
 * buffers into one access unit, and DecoderEndFrame vm_send()s that unit,
 * vm_get_frame()s the decoded NV12 and uploads it into the texture through the
 * internal DXVK immediate context (UpdateSubresource). See
 * deliverables/P2-2B-DECODER-DECODE.md.
 *
 * Called from virtio_wddm_create_device() after the 3D funcs are installed.
 * A failure inside this call never fails device creation: the 3D device works
 * without video, which is exactly the pre-M1 state. */
void virtio_wddm_fill_video_funcs(D3D10DDIARG_CREATEDEVICE *pArgs);

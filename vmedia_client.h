/* vmedia_client.h - user-mode client for the DroidVM virtio-media driver.
 *
 * Thin CreateFile + DeviceIoControl wrapper around vmedia_abi.h.  The handle
 * is the driver's per-function symbolic link (\\.\VMediaDecoder or
 * \\.\VMediaEncoder); the device interface GUID in vmedia_abi.h is what an
 * application can use to discover the function before opening the link.
 *
 * Build (ARM64):  aarch64-w64-mingw32-clang -shared -DVMEDIA_CLIENT_BUILD ...
 * Link:           vmedia_client.lib / vmedia_client.dll (ARM64, cdecl).
 */
#ifndef VMEDIA_CLIENT_H
#define VMEDIA_CLIENT_H

#include <stdint.h>
#include "vmedia_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(VMEDIA_CLIENT_STATIC)
/* Compiled into the D3D11 UMD (P2-2a): plain C linkage, no export/import
 * decoration. dllexport would add vm_* to the UMD's export table, and
 * dllimport would not link against an object file. */
#  define VM_CLIENT_API
#elif defined(VMEDIA_CLIENT_BUILD)
#  define VM_CLIENT_API __declspec(dllexport)
#else
#  define VM_CLIENT_API __declspec(dllimport)
#endif
#define VM_CALL __cdecl

/* Opaque session; one open handle + the driver session id. */
typedef struct _VM_SESSION VM_SESSION;

/* kind: 0 = decoder (\\.\VMediaDecoder), 1 = encoder (\\.\VMediaEncoder).
 * codec: VM_OPEN_CODEC_H264 (0), VM_OPEN_CODEC_VP9 (1) or VM_OPEN_CODEC_HEVC
 * (2) -- the coded format the host session is configured for.  The codec has to
 * be chosen at OPEN because the host decoder picks its backend from the V4L2
 * OUTPUT format the guest driver sets when it configures the session, and
 * VM_IOCTL_SEND_BITSTREAM carries no codec field.  VM_OPEN_CODEC_H264 keeps the
 * pre-VP9 behaviour, so an old driver that ignores the field keeps working for
 * H.264.
 * Returns NULL on failure; call vm_last_error() for the Win32 error. */
VM_CLIENT_API VM_SESSION * VM_CALL vm_open_codec(uint32_t kind, uint32_t codec,
                                                VM_OPEN_OUT *out);

/* vm_open(kind, out) == vm_open_codec(kind, VM_OPEN_CODEC_H264, out). */
VM_CLIENT_API VM_SESSION * VM_CALL vm_open(uint32_t kind, VM_OPEN_OUT *out);

/* Send one H.264 access unit.  `data`/`size` are copied into a single
 * METHOD_BUFFERED input: VM_SEND_IN followed by the bytes.  The driver accepts
 * exactly one access unit per session in P2-1. */
VM_CLIENT_API int VM_CALL vm_send(VM_SESSION *session,
                                  const void *data, uint32_t size,
                                  uint32_t width, uint32_t height,
                                  uint32_t flags, VM_SEND_OUT *out);

/* Block for one decoded NV12 frame and copy it into `nv12` (capacity bytes,
 * tight frame = stride * height * 3 / 2).  Fills `info` with width/height/
 * stride/size.  timeout_ms = 0 polls once.  Returns 1 on success. */
VM_CLIENT_API int VM_CALL vm_get_frame(VM_SESSION *session,
                                       void *nv12, uint32_t capacity,
                                       VM_FRAME_INFO *info, uint32_t timeout_ms);

/* P-BATCH ZEROCOPY: block for one decoded frame and return the capture
 * buffer's pool pages mapped into THIS process instead of a copy.  Fills `map`
 * (its first eight fields are a VM_FRAME_INFO) including UserVa/MappedLength,
 * and keeps the capture buffer checked out of the device.  The caller must call
 * vm_release_frame(session, map->Index) once it has uploaded the pixels; until
 * then the driver refuses another map.  Driver 1.0.0.18+ only: an older driver
 * answers ERROR_INVALID_FUNCTION and the caller falls back to vm_get_frame().
 * Returns 1 on success. */
VM_CLIENT_API int VM_CALL vm_map_frame(VM_SESSION *session,
                                       VM_FRAME_MAP *map, uint32_t timeout_ms);

/* P-BATCH ZEROCOPY: unmap the frame and give its capture buffer back to the
 * device.  `index` is VM_FRAME_MAP.Index (0xFFFFFFFF = whatever is mapped).
 * Returns 1 on success. */
VM_CLIENT_API int VM_CALL vm_release_frame(VM_SESSION *session, uint32_t index);

/* Close the driver session and the handle.  Safe on NULL. */
VM_CLIENT_API void VM_CALL vm_close(VM_SESSION *session);

/* P3-2: flush the session -- the driver drops the queued access units and
 * undelivered frames and opens a FRESH host session, so the decoder loses every
 * reference frame.  The next vm_send() must be an IDR that carries SPS/PPS.
 * Updates the session's driver session id.  Returns 1 on success. */
VM_CLIENT_API int VM_CALL vm_flush(VM_SESSION *session, VM_FLUSH_OUT *out);

/* Last Win32 error (0 after a successful call). */
VM_CLIENT_API uint32_t VM_CALL vm_last_error(void);

/* Driver ABI version the client was compiled against (0x00010007). */
VM_CLIENT_API uint32_t VM_CALL vm_driver_version(void);

#ifdef __cplusplus
}
#endif

#endif /* VMEDIA_CLIENT_H */

#pragma once
#include <windows.h>
/* Diagnostic process-wide scalar state, never part of a rotated resource ABI.
 * Off by default. Existing external DDI serialization is assumed, not added.
 * category: 0=rotation member (100), 1=VP (150), 2=present (50).
 * Mismatches have an independent 32-event quota. */
BOOL virtio_vp_identity_enabled(void);
void virtio_vp_identity_emit(unsigned category, BOOL mismatch, const char *format, ...);
void virtio_vp_identity_rotation(void);

/* Internal contexts only: never call this on an application-owned context.
 * Always enabled, independently of the default-OFF media experiments.
 * ID3D10Multithread and ID3D11Multithread share IID and COM ABI. */
#ifndef VIRTIO_CONTEXT_PROTECTION_H
#define VIRTIO_CONTEXT_PROTECTION_H
#ifndef CONTEXT_PROTECTION_TEST_TYPES
#include <d3d10.h>
#include <d3d11.h>
#endif

typedef struct ContextProtectionState {
    HRESULT query_hr;
    BOOL before, previous, after;
} ContextProtectionState;

static HRESULT protect_internal_context(ID3D11DeviceContext *ctx,
                                         ContextProtectionState *state) {
    static const IID multithread_iid = {
        0x9b7e4e00, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}
    };
    ID3D10Multithread *mt = NULL;
    *state = (ContextProtectionState){ .query_hr = E_POINTER };
    if (!ctx) return E_POINTER;
    state->query_hr = ID3D11DeviceContext_QueryInterface(ctx, &multithread_iid, (void **)&mt);
    if (FAILED(state->query_hr) || !mt) {
        if (mt) ID3D10Multithread_Release(mt);
        return FAILED(state->query_hr) ? state->query_hr : E_NOINTERFACE;
    }
    state->before = ID3D10Multithread_GetMultithreadProtected(mt);
    /* Return value is PREVIOUS state, not success. FALSE is normal initially. */
    state->previous = ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
    state->after = ID3D10Multithread_GetMultithreadProtected(mt);
    ID3D10Multithread_Release(mt);
    return state->after ? S_OK : E_FAIL;
}
#endif

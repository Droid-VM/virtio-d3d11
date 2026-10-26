#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winddk_compat.h>
#include <d3d9.h>
#include <virtio_wddm_uapi.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#undef ERROR
#include "adapter.h"
#include "device.h"
#include "dxgi.h"
#include "resource.h"

/*

Presentable formats:
DXGI_FORMAT_B5G6R5_UNORM => VK_FORMAT_B5G6R5_UNORM_PACK16 => PIPE_FORMAT_R5G6B5_UNORM
DXGI_FORMAT_B5G5R5A1_UNORM => VK_FORMAT_B5G5R5A1_UNORM_PACK16 => PIPE_FORMAT_A1R5G5B5_UNORM
DXGI_FORMAT_B8G8R8A8_UNORM => VK_FORMAT_B8G8R8A8_UNORM => PIPE_FORMAT_B8G8R8A8_UNORM
DXGI_FORMAT_B8G8R8X8_UNORM => VK_FORMAT_B8G8R8A8_UNORM => PIPE_FORMAT_B8G8R8A8_UNORM
DXGI_FORMAT_R16G16B16A16_FLOAT => VK_FORMAT_R16G16B16A16_SFLOAT => PIPE_FORMAT_R16G16B16A16_FLOAT
DXGI_FORMAT_R10G10B10A2_UNORM => VK_FORMAT_A2B10G10R10_UNORM_PACK32 => PIPE_FORMAT_R10G10B10A2_UNORM
DXGI_FORMAT_R8G8B8A8_UNORM => VK_FORMAT_R8G8B8A8_UNORM => PIPE_FORMAT_R8G8B8A8_UNORM
DXGI_FORMAT_R8G8B8A8_UNORM_SRGB => VK_FORMAT_R8G8B8A8_SRGB => PIPE_FORMAT_R8G8B8A8_SRGB

virtio_wddm_present() {
    TODO:
    ID3D11DeviceContext4_Signal present fence
    ID3D11DeviceContext_Flush
    pfnwaitfromgpu,
    pfnpresent,

    blit is the same, just with blit before signal/flush
}
*/

/* P3-5: present-path accounting.
 *
 * A present used to be an opaque 100-150 ms during a seek drag (E145 measured
 * 8.8 presents/s while the decoder kept producing 24-30 frames/s) and nothing
 * inside it was timed.  Everything below is QPC only: no allocation, no
 * syscall, so enabling it does not perturb what it measures.  The log line in
 * virtio_wddm_present splits one present into
 *
 *     flush   = Signal + ID3D11DeviceContext1_Flush (submit our own commands)
 *     fence   = SetEventOnCompletion + WaitForSingleObject (wait for the GPU)
 *     cb      = pfnPresentCb (the KMD/host present)
 *     amort   = tritonPerformAmortizedProcessing (resource retirement)
 *
 * and the EndFrame line in d3d11_video.c splits the decode side into
 * send/get/upload, so a frame's whole cost is accounted for. */
static LONGLONG present_qpc_freq(void) {
    static LONGLONG freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart ? f.QuadPart : 1;
    }
    return freq;
}

static LONGLONG present_qpc_now(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static double present_qpc_ms(LONGLONG start, LONGLONG end) {
    return (double) (end - start) * 1000.0 / (double) present_qpc_freq();
}

/* How far behind the newest fence a present is allowed to wait, and for how
 * long.  Defaults reproduce the pre-P3-5 behaviour exactly (wait for our own
 * fence, 10 s cap); the drag experiments override them per process so a single
 * build can measure "present blocks on its own frame" against "present only
 * orders itself behind older work" back to back.
 *
 *   VIRTIO_WDDM_PRESENT_LAG      older fence values to wait for (default 0)
 *   VIRTIO_WDDM_PRESENT_WAIT_MS  upper bound on that wait (default 10000) */
static void present_wait_policy(uint64_t *lag, DWORD *budget_ms) {
    static uint64_t cached_lag;
    static DWORD cached_budget;
    static LONG cached = 0;

    if (cached == 0) {
        char text[32] = {0};
        uint64_t lag_value = 0;
        DWORD budget = 10000;

        if (GetEnvironmentVariableA("VIRTIO_WDDM_PRESENT_LAG", text, sizeof(text)) > 0)
            lag_value = (uint64_t) _strtoui64(text, NULL, 10);
        if (GetEnvironmentVariableA("VIRTIO_WDDM_PRESENT_WAIT_MS", text, sizeof(text)) > 0) {
            unsigned long value = strtoul(text, NULL, 10);
            if (value > 0 && value <= 600000ul)
                budget = (DWORD) value;
        }
        cached_lag = lag_value;
        cached_budget = budget;
        InterlockedExchange(&cached, 1);
        INFO("%s: present wait policy lag=%llu wait_ms=%lu%s", __FUNCTION__,
             (unsigned long long) lag_value, (unsigned long) budget,
             lag_value ? " (fence used for ordering only)" : " (wait for own fence)");
    }
    *lag = cached_lag;
    *budget_ms = cached_budget;
}

static HRESULT virtio_wddm_sync_with_present_context(VIRTIO_WDDM_Device *device,
                                                    double *out_flush_ms,
                                                    double *out_fence_ms,
                                                    BOOL *out_fence_pending) {
    LONGLONG t_signal = present_qpc_now();
    uint64_t value = atomic_fetch_add_explicit((volatile _Atomic uint64_t *) &device->base.presentFenceValue, 1, memory_order_acq_rel);

    if (out_fence_pending)
        *out_fence_pending = FALSE;

    HRESULT hr = ID3D11DeviceContext4_Signal(device->base.pCtx4, device->base.pPresentFence, value);
    if (FAILED(hr)) {
        ERROR("%s: Failed to signal D3D11 fence: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }
    ID3D11DeviceContext1_Flush(device->base.pCtx1);
    LONGLONG t_flushed = present_qpc_now();

    /* The fence is what orders the KMD present (a different context/queue) after
     * the Turnip/virtio-gpu work this UMD just submitted.  Waiting for `value`
     * means waiting for *this* frame, which serialises present with the whole
     * decode+upload queue in front of it; waiting for `value - lag` only keeps
     * the queue from growing without bound. */
    uint64_t lag = 0;
    DWORD budget_ms = 10000;
    present_wait_policy(&lag, &budget_ms);
    uint64_t target = value > lag ? value - lag : 0;

    HANDLE event = NULL;
    DWORD wait = WAIT_OBJECT_0;
    if (target != 0) {
        event = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!event)
            return HRESULT_FROM_WIN32(GetLastError());
        hr = ID3D11Fence_SetEventOnCompletion(device->base.pPresentFence, target, event);
        if (FAILED(hr)) {
            CloseHandle(event);
            ERROR("%s: Failed to arm D3D11 fence event: 0x%08lx", __FUNCTION__, hr);
            return hr;
        }
        wait = WaitForSingleObject(event, budget_ms);
        CloseHandle(event);
    }
    LONGLONG t_waited = present_qpc_now();

    if (out_flush_ms)
        *out_flush_ms = present_qpc_ms(t_signal, t_flushed);
    if (out_fence_ms)
        *out_fence_ms = present_qpc_ms(t_flushed, t_waited);

    if (wait != WAIT_OBJECT_0) {
        /* P3-5: a bounded wait is not an error.  The present is still ordered
         * behind the Signal above; only the CPU refuses to block past the
         * budget, and the next present (which waits for a later fence value)
         * catches the queue up.  The legacy policy (lag 0, 10 s cap) keeps its
         * old failure behaviour so nothing changes unless it is opted into. */
        device->present.fence_timeouts++;
        if (lag != 0 || budget_ms < 10000) {
            if (out_fence_pending)
                *out_fence_pending = TRUE;
            return S_OK;
        }
        ERROR("%s: Timed out waiting for D3D11 fence: %lu", __FUNCTION__, wait);
        return E_FAIL;
    }
    return S_OK;
}

HRESULT APIENTRY virtio_wddm_present(DXGI_DDI_ARG_PRESENT *pArgs) {
    LONGLONG t_entry = present_qpc_now();
    TRACE();
    VIRTIO_WDDM_Device *device = (void *) pArgs->hDevice;
    VIRTIO_WDDM_Resource *src = (void *) pArgs->hSurfaceToPresent;
    HRESULT hr = S_OK;
    double flush_ms = 0.0;
    double fence_ms = 0.0;
    double cb_ms = 0.0;
    double amort_ms = 0.0;
    BOOL fence_pending = FALSE;

    VERBOSE("present detail: src=%p kmalloc=0x%08x dst=0x%08x flags=0x%08x fence=%llu", src,
         src ? src->base.hKMAllocation : 0, (unsigned) pArgs->hDstResource,
         pArgs->Flags.Value, (unsigned long long)device->base.presentFenceValue);

    // Looks like Blt could be zero for primaries
    //ASSERT(pArgs->Flags.Blt);
    ASSERT(pArgs->hDstResource == 0);
    ASSERT(src->base.hKMAllocation != 0);

    if (false) {
        char name[256];
        snprintf(name, sizeof(name), "present-%llu.ppm", device->base.presentFenceValue);
        virtio_wddm_save_texture_to_ppm(device, src, name);
    }

    hr = virtio_wddm_sync_with_present_context(device, &flush_ms, &fence_ms, &fence_pending);
    LONGLONG t_synced = present_qpc_now();
    VERBOSE("present sync result: hr=0x%08lx fence=%llu", hr,
         (unsigned long long)device->base.presentFenceValue);
    if (FAILED(hr)) {
        ERROR("%s: Failed to flush: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }

    DXGIDDICB_PRESENT present = {
        .hSrcAllocation = src->base.hKMAllocation,
        .hDstAllocation = 0,
        .pDXGIContext = pArgs->pDXGIContext,
        .hContext = device->present.context,
    };
    hr = device->dxgi_callbacks->pfnPresentCb(device->base.hRTDevice.handle, &present);
    LONGLONG t_presented = present_qpc_now();
    cb_ms = present_qpc_ms(t_synced, t_presented);
    VERBOSE("present callback result: hr=0x%08lx src_km=0x%08x ctx=%p", hr,
         present.hSrcAllocation, present.hContext);
    if (FAILED(hr)) {
        ERROR("%s: Failed to present: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }

    /* Present flushes the internal context directly, without entering the
     * Flush DDI. Let the D3D11 runtime retire released resources here too,
     * before it refreshes bindings for the next frame. */
    LONGLONG t_before_amort = present_qpc_now();
    tritonPerformAmortizedProcessing(&device->base);
    LONGLONG t_done = present_qpc_now();
    amort_ms = present_qpc_ms(t_before_amort, t_done);

    device->present.count++;
    INFO("present #%llu t=%lld: flush=%.3f fence=%.3f cb=%.3f amort=%.3f sync=%.3f total=%.3f ms "
         "(fence=%llu lag_pending=%d timeouts=%llu src_km=0x%x)",
         (unsigned long long) device->present.count, (long long) t_entry,
         flush_ms, fence_ms, cb_ms, amort_ms,
         present_qpc_ms(t_entry, t_synced), present_qpc_ms(t_entry, t_done),
         (unsigned long long) device->base.presentFenceValue, (int) fence_pending,
         (unsigned long long) device->present.fence_timeouts, src->base.hKMAllocation);

    return S_OK;
}

HRESULT APIENTRY virtio_wddm_get_gamma_caps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *pArgs) {
    TRACE();
    pArgs->pGammaCapabilities->ScaleAndOffsetSupported = false;
    pArgs->pGammaCapabilities->MinConvertedValue = 0.f;
    pArgs->pGammaCapabilities->MaxConvertedValue = 1.f;
    pArgs->pGammaCapabilities->NumGammaControlPoints = 17;

    for (size_t i = 0; i < pArgs->pGammaCapabilities->NumGammaControlPoints; i++) {
        pArgs->pGammaCapabilities->ControlPointPositions[i] = i / (float) (pArgs->pGammaCapabilities->NumGammaControlPoints - 1);
    }

    return S_OK;
}

HRESULT APIENTRY virtio_wddm_set_display_mode(DXGI_DDI_ARG_SETDISPLAYMODE *pArgs) {
    TRACE();
    VIRTIO_WDDM_Device *device = (void *) pArgs->hDevice;
    VIRTIO_WDDM_Resource *resource = (void *) pArgs->hResource;

    ASSERT(resource->base.hKMAllocation != 0);
    ASSERT(resource->base.IsPresentable);

    D3DDDICB_SETDISPLAYMODE set_mode = {
        .hPrimaryAllocation = resource->base.hKMAllocation,
        .PrivateDriverFormatAttribute = 0,
    };

    HRESULT hr = device->base.KTCallbacks.pfnSetDisplayModeCb(device->base.hRTDevice.handle, &set_mode);
    if (FAILED(hr)) {
        ERROR("%s: failed to set display mode: 0x%08lx", __FUNCTION__, hr);
        return hr;
    }
    return S_OK;
}

HRESULT APIENTRY virtio_wddm_set_resource_priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *pArgs) {
    TRACE();
    return S_OK;
}

HRESULT APIENTRY virtio_wddm_query_resource_residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *pArgs) {
    TRACE();
    HRESULT hr = S_OK;
    VIRTIO_WDDM_Device *device = (void *) pArgs->hDevice;

    for (size_t i = 0; i < pArgs->Resources; i++) {
        VIRTIO_WDDM_Resource *resource = (void *) pArgs->pResources[i];

        if (resource->base.hKMAllocation == 0) {
            pArgs->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
            continue;
        }

        D3DDDI_RESIDENCYSTATUS status;
        {
            D3DDDICB_QUERYRESIDENCY query = {
                .NumAllocations = 1,
                .HandleList = &resource->base.hKMAllocation,
                .pResidencyStatus = &status,
            };
            HRESULT hr = device->base.KTCallbacks.pfnQueryResidencyCb(device->base.hRTDevice.handle, &query);
            if (FAILED(hr)) {
                ERROR("%s: query residency failed: 0x%08lx", __FUNCTION__, hr);
                return hr;
            }
        }

        switch (status) {
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINGPUMEMORY:
                pArgs->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
                break;
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY:
                pArgs->pStatus[i] = DXGI_DDI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY;
                hr = S_RESIDENT_IN_SHARED_MEMORY;
                break;
            case D3DDDI_RESIDENCYSTATUS_NOTRESIDENT:
                pArgs->pStatus[i] = DXGI_DDI_RESIDENCY_EVICTED_TO_DISK;
                hr = S_NOT_RESIDENT;
                break;
        }
    }

    return hr;
}

HRESULT APIENTRY virtio_wddm_rotate_resource_identities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *pArgs) {
    TRACE();
    if (pArgs->Resources <= 1) {
        return S_OK;
    }

    VIRTIO_WDDM_Device *device = (void *) pArgs->hDevice;
    VIRTIO_WDDM_Resource first = *(VIRTIO_WDDM_Resource *) pArgs->pResources[0];

    for (size_t i = 0; i < pArgs->Resources - 1; i++) {
        VIRTIO_WDDM_Resource *curr = (void *) pArgs->pResources[i];
        VIRTIO_WDDM_Resource *next = (void *) pArgs->pResources[i + 1];
        D3D10DDI_HRTRESOURCE res = curr->base.hRTResource;
        TRITON_VIEWLINK *views = curr->base.pViewList;
        *curr = *next;
        curr->base.hRTResource = res;
        curr->base.pViewList = views;
    }

    VIRTIO_WDDM_Resource *last = (void *) pArgs->pResources[pArgs->Resources - 1];
    D3D10DDI_HRTRESOURCE res = last->base.hRTResource;
    TRITON_VIEWLINK *views = last->base.pViewList;
    *last = first;
    last->base.hRTResource = res;
    last->base.pViewList = views;

    for (size_t i = 0; i < pArgs->Resources; i++) {
        VIRTIO_WDDM_Resource *resource = (void *) pArgs->pResources[i];
        tritonResourceRecreateViews(&device->base, &resource->base);
    }

    device->base.pUMCallbacks->pfnStateVsSrvCb(device->base.hRTCoreLayer, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
    device->base.pUMCallbacks->pfnStateGsSrvCb(device->base.hRTCoreLayer, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
    device->base.pUMCallbacks->pfnStatePsSrvCb(device->base.hRTCoreLayer, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
    if (device->base.uIfVersion >= D3D11_0_DDI_INTERFACE_VERSION) {
        device->base.pUMCallbacks->pfnStateHsSrvCb(device->base.hRTCoreLayer, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        device->base.pUMCallbacks->pfnStateDsSrvCb(device->base.hRTCoreLayer, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        device->base.pUMCallbacks->pfnStateCsSrvCb(device->base.hRTCoreLayer, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        device->base.pUMCallbacks->pfnStateCsUavCb(device->base.hRTCoreLayer, 0, D3D11_PS_CS_UAV_REGISTER_COUNT);
    }
    device->base.pUMCallbacks->pfnStateOmRenderTargetsCb(device->base.hRTCoreLayer);

    return S_OK;
}

HRESULT APIENTRY virtio_wddm_blt(DXGI_DDI_ARG_BLT *pArgs) {
    TRACE();
    VIRTIO_WDDM_Resource *src = (void *) pArgs->hSrcResource;
    DXGI_DDI_ARG_BLT1 args = {
        .hDevice = pArgs->hDevice,
        .hDstResource = pArgs->hDstResource,
        .DstSubresource = pArgs->DstSubresource,
        .DstLeft = pArgs->DstLeft,
        .DstTop = pArgs->DstTop,
        .DstRight = pArgs->DstRight,
        .DstBottom = pArgs->DstBottom,
        .hSrcResource = pArgs->hSrcResource,
        .SrcSubresource = pArgs->SrcSubresource,
        .SrcLeft = 0,
        .SrcTop = 0,
        .SrcRight = src->base.Width,
        .SrcBottom = src->base.Height,
        .Flags = pArgs->Flags,
        .Rotate = pArgs->Rotate,
    };
    return virtio_wddm_blt1(&args);
}

HRESULT APIENTRY virtio_wddm_resolve_shared_resource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *pArgs) {
    TRACE();
    return S_OK;
}

HRESULT APIENTRY virtio_wddm_blt1(DXGI_DDI_ARG_BLT1 *pArgs) {
    TRACE();

    VIRTIO_WDDM_Device *device = (void *) pArgs->hDevice;
    VIRTIO_WDDM_Resource *dst = (void *) pArgs->hDstResource;
    VIRTIO_WDDM_Resource *src = (void *) pArgs->hSrcResource;

    ASSERT(pArgs->Rotate == DXGI_DDI_MODE_ROTATION_IDENTITY);

    if (pArgs->Flags.Resolve) {
        ID3D11DeviceContext1_ResolveSubresource(
            device->base.pCtx1,
            dst->base.pResource, pArgs->DstSubresource,
            src->base.pResource, pArgs->SrcSubresource,
            dst->base.Format
        );
    }

    if (pArgs->Flags.Stretch || pArgs->Flags.Convert) {
        TODO("Stretch/shrink or convert format");
    } else {
        D3D11_BOX src_box = {
            .left = pArgs->SrcLeft,
            .top = pArgs->SrcTop,
            .front = 0,
            .right = pArgs->SrcRight,
            .bottom = pArgs->SrcBottom,
            .back = 1,
        };

        ID3D11DeviceContext1_CopySubresourceRegion(
            device->base.pCtx1,
            dst->base.pResource, pArgs->DstSubresource, pArgs->DstLeft, pArgs->DstTop, 0,
            src->base.pResource, pArgs->SrcSubresource, &src_box
        );
    }

    if (pArgs->Flags.Present) {
        double flush_ms = 0.0;
        double fence_ms = 0.0;
        BOOL fence_pending = FALSE;
        LONGLONG t_present = present_qpc_now();
        HRESULT hr = virtio_wddm_sync_with_present_context(device, &flush_ms, &fence_ms,
                                                           &fence_pending);
        if (FAILED(hr)) {
            ERROR("%s: Failed to flush: 0x%08lx", __FUNCTION__, hr);
            return hr;
        }
        device->present.blt_presents++;
        INFO("blt1 #%llu (Present flag): flush=%.3f fence=%.3f total=%.3f ms "
             "(fence=%llu lag_pending=%d)", (unsigned long long) device->present.blt_presents,
             flush_ms, fence_ms, present_qpc_ms(t_present, present_qpc_now()),
             (unsigned long long) device->base.presentFenceValue, (int) fence_pending);
    }

    return S_OK;
}

HRESULT APIENTRY virtio_wddm_offer_resources(DXGI_DDI_ARG_OFFERRESOURCES *pArgs) {
    TRACE();
    return S_OK;
}

HRESULT APIENTRY virtio_wddm_reclaim_resources(DXGI_DDI_ARG_RECLAIMRESOURCES *pArgs) {
    TRACE();
    for (size_t i = 0; i < pArgs->Resources; i++) {
        pArgs->pDiscarded[i] = false;
    }

    return S_OK;
}

#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winddk_compat.h>
#include <virtio_wddm_uapi.h>

#include <drm_hw.h>

#undef ERROR
#include "adapter.h"
#include "device.h"
#include "dxgi.h"
#include "resource.h"

#include "dxvk.h"

__thread VIRTIO_WDDM_RuntimeAllocRecord virtio_wddm_last_runtime_alloc;
__thread VIRTIO_WDDM_RuntimeAllocScope virtio_wddm_runtime_alloc_scope;

/* Perform an allocation the ICD asked for, on the runtime's device.
 *
 * The ICD picks blob_id and size (only it knows the image's memory
 * requirements, and the host requires GEM_NEW and RESOURCE_CREATE_BLOB to agree
 * exactly); we only supply pfnAllocateCb, which is the sole way to obtain the
 * kernel resource handle an application needs for GetSharedHandle.
 *
 * Called from inside vkAllocateMemory, i.e. nested in the CreateResource whose
 * hRTResource is being passed back -- same thread, and the runtime resource is
 * live for the whole call. */
int virtio_wddm_runtime_alloc(void *ctx, const VIRTIO_WDDM_RuntimeAllocRequest *req,
                             VIRTIO_WDDM_RuntimeAllocResult *out) {
    VIRTIO_WDDM_Device *device = ctx;

    if (!device || !req || !out) {
        return -EINVAL;
    }

    /* Mirrors what the vdrm WDDM backend would have put in a plain
     * CreateAllocation, so the KMD sees an identical blob request either way. */
    VIRTIO_WDDM_CreateResource res_priv = {
        .tag = VIRTIO_WDDM_CREATE_RESOURCE_TAG,
    };
    VIRTIO_WDDM_CreateAllocation alloc_priv = {
        .blob = {
            .tag   = VIRTIO_WDDM_ALLOCATE_BLOB_TAG,
            .id    = req->blob_id,
            .mem   = req->mem,
            .flags = req->flags,
            .size  = req->size,
        },
    };

    VIRTIO_WDDM_ReuseBlobAllocation reuse = {
        .allocation = alloc_priv,
        .reuse_tag = VIRTIO_WDDM_REUSE_BLOB_TAG,
        .source = req->sourceAllocation,
    };

    D3DDDI_ALLOCATIONINFO2 alloc_info = {
        .pPrivateDriverData    = req->sourceAllocation ? (void *)&reuse : (void *)&alloc_priv,
        .PrivateDriverDataSize = req->sourceAllocation ? sizeof(reuse) : sizeof(alloc_priv),
        .Flags.Primary = req->hRTResource &&
            virtio_wddm_runtime_alloc_scope.hRTResource == req->hRTResource &&
            virtio_wddm_runtime_alloc_scope.primary,
    };

    VIRTIO_WDDM_PrimaryAllocation primary = {
        .base = reuse,
        .primary_tag = VIRTIO_WDDM_PRIMARY_ALLOCATION_TAG,
        .refresh_numerator = virtio_wddm_runtime_alloc_scope.primary_desc.ModeDesc.RefreshRate.Numerator,
        .refresh_denominator = virtio_wddm_runtime_alloc_scope.primary_desc.ModeDesc.RefreshRate.Denominator,
        .vidpn_source = virtio_wddm_runtime_alloc_scope.primary_desc.VidPnSourceId,
    };
    if (alloc_info.Flags.Primary) {
        alloc_info.pPrivateDriverData = &primary;
        alloc_info.PrivateDriverDataSize = sizeof(primary);
    }

    D3DDDICB_ALLOCATE allocate = {
        .pPrivateDriverData    = &res_priv,
        .PrivateDriverDataSize = sizeof(res_priv),
        /* This is what makes the runtime mint hKMResource. NULL is legal and
         * simply yields no resource handle. */
        .hResource             = req->hRTResource,
        .NumAllocations        = 1,
        .pAllocationInfo2      = &alloc_info,
    };

    VERBOSE("%s: pfnAllocateCb begin rtdev=%p rtres=%p blob=%llu size=%llu mem=0x%x flags=0x%x",
         __FUNCTION__, device->base.hRTDevice.handle, req->hRTResource,
         (unsigned long long)req->blob_id, (unsigned long long)req->size,
         req->mem, req->flags);

    SetLastError(ERROR_SUCCESS);
    HRESULT hr = device->base.KTCallbacks.pfnAllocateCb(device->base.hRTDevice.handle, &allocate);
    DWORD last_error = GetLastError();

    VERBOSE("%s: pfnAllocateCb end hr=0x%08lx last_error=%lu kmres=0x%08x allocation=0x%08x",
         __FUNCTION__, hr, last_error, allocate.hKMResource, alloc_info.hAllocation);

    if (FAILED(hr) || alloc_info.hAllocation == 0) {
        ERROR("%s: pfnAllocateCb failed: 0x%08lx", __FUNCTION__, hr);
        return -EIO;
    }

    out->hAllocation = alloc_info.hAllocation;
    out->hKMResource = allocate.hKMResource;

    /* Hand the handles to the CreateResource further up this stack: it cannot
     * get hAllocation any other way (exporting it from the VkDeviceMemory needs
     * vkGetMemoryWin32HandleKHR, which turnip does not implement). */
    virtio_wddm_last_runtime_alloc = (VIRTIO_WDDM_RuntimeAllocRecord) {
        .hRTResource = req->hRTResource,
        .hAllocation = alloc_info.hAllocation,
        .hKMResource = allocate.hKMResource,
    };

    return 0;
}

/* Release an allocation the alloc hook produced.
 *
 * Allocation-level (HandleList) on purpose: retiring the runtime resource is
 * DestroyResource's job, and it runs after this -- vk_FreeMemory is what closes
 * the ICD's BO. Freeing by hResource here would retire the resource while the
 * UMD still holds it. */
int virtio_wddm_runtime_free(void *ctx, const VIRTIO_WDDM_RuntimeAllocResult *alloc) {
    VIRTIO_WDDM_Device *device = ctx;

    if (!device || !alloc) {
        return -EINVAL;
    }
    if (alloc->hAllocation == 0) {
        return 0;
    }

    D3DKMT_HANDLE handles[1] = { alloc->hAllocation };
    D3DDDICB_DEALLOCATE deallocate = {
        .hResource      = NULL,
        .NumAllocations = 1,
        .HandleList     = handles,
    };

    HRESULT hr = device->base.KTCallbacks.pfnDeallocateCb(device->base.hRTDevice.handle, &deallocate);
    VERBOSE("%s: pfnDeallocateCb allocation=0x%08x kmres=0x%08x hr=0x%08lx", __FUNCTION__,
         alloc->hAllocation, alloc->hKMResource, hr);

    if (virtio_wddm_last_runtime_alloc.hAllocation == alloc->hAllocation) {
        memset(&virtio_wddm_last_runtime_alloc, 0, sizeof(virtio_wddm_last_runtime_alloc));
    }

    if (FAILED(hr)) {
        ERROR("%s: pfnDeallocateCb failed: 0x%08lx", __FUNCTION__, hr);
        return -EIO;
    }

    return 0;
}

SIZE_T APIENTRY virtio_wddm_calc_device_size(D3D10DDI_HADAPTER hAdapter, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pArgs) {
    /* Keep the private block comfortably above the runtime's alignment and
     * bookkeeping granularity while isolating the CalcPrivateDeviceSize ABI.
     * The driver only uses the first sizeof(VIRTIO_WDDM_Device) bytes. */
    const SIZE_T size = 4096;
    INFO("%s: size=%zu align=%zu interface=0x%x version=0x%x flags=0x%x", __FUNCTION__,
         size, _Alignof(VIRTIO_WDDM_Device), pArgs->Interface, pArgs->Version, pArgs->Flags);
    return size;
}

static inline void free_d3d11_device(void *ptr) {
    if (*(ID3D11Device **)ptr)
        ID3D11Device_Release(*(ID3D11Device **)ptr);
}

static inline void free_d3d11_device_context(void *ptr) {
    if (*(ID3D11DeviceContext **)ptr)
        ID3D11DeviceContext_Release(*(ID3D11DeviceContext **)ptr);
}

extern const char *vk_result_to_str(VkResult result) {
#define CASE_VK_RESULT(name) case VK_##name: return #name;
    switch (result) {
        CASE_VK_RESULT(SUCCESS)
        CASE_VK_RESULT(NOT_READY)
        CASE_VK_RESULT(TIMEOUT)
        CASE_VK_RESULT(EVENT_SET)
        CASE_VK_RESULT(EVENT_RESET)
        CASE_VK_RESULT(INCOMPLETE)
        CASE_VK_RESULT(ERROR_OUT_OF_HOST_MEMORY)
        CASE_VK_RESULT(ERROR_OUT_OF_DEVICE_MEMORY)
        CASE_VK_RESULT(ERROR_INITIALIZATION_FAILED)
        CASE_VK_RESULT(ERROR_DEVICE_LOST)
        CASE_VK_RESULT(ERROR_MEMORY_MAP_FAILED)
        CASE_VK_RESULT(ERROR_LAYER_NOT_PRESENT)
        CASE_VK_RESULT(ERROR_EXTENSION_NOT_PRESENT)
        CASE_VK_RESULT(ERROR_FEATURE_NOT_PRESENT)
        CASE_VK_RESULT(ERROR_INCOMPATIBLE_DRIVER)
        CASE_VK_RESULT(ERROR_TOO_MANY_OBJECTS)
        CASE_VK_RESULT(ERROR_FORMAT_NOT_SUPPORTED)
        CASE_VK_RESULT(ERROR_FRAGMENTED_POOL)
        CASE_VK_RESULT(ERROR_UNKNOWN)
        CASE_VK_RESULT(ERROR_VALIDATION_FAILED)
        CASE_VK_RESULT(ERROR_OUT_OF_POOL_MEMORY)
        CASE_VK_RESULT(ERROR_INVALID_EXTERNAL_HANDLE)
        CASE_VK_RESULT(ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
        CASE_VK_RESULT(ERROR_FRAGMENTATION)
        CASE_VK_RESULT(PIPELINE_COMPILE_REQUIRED)
        CASE_VK_RESULT(ERROR_NOT_PERMITTED)
        CASE_VK_RESULT(ERROR_SURFACE_LOST_KHR)
        CASE_VK_RESULT(ERROR_NATIVE_WINDOW_IN_USE_KHR)
        CASE_VK_RESULT(SUBOPTIMAL_KHR)
        CASE_VK_RESULT(ERROR_OUT_OF_DATE_KHR)
        CASE_VK_RESULT(ERROR_INCOMPATIBLE_DISPLAY_KHR)
        CASE_VK_RESULT(ERROR_INVALID_SHADER_NV)
        CASE_VK_RESULT(ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR)
        CASE_VK_RESULT(ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR)
        CASE_VK_RESULT(ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR)
        CASE_VK_RESULT(ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR)
        CASE_VK_RESULT(ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR)
        CASE_VK_RESULT(ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR)
        CASE_VK_RESULT(ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT)
        CASE_VK_RESULT(ERROR_PRESENT_TIMING_QUEUE_FULL_EXT)
        CASE_VK_RESULT(ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
        CASE_VK_RESULT(THREAD_IDLE_KHR)
        CASE_VK_RESULT(THREAD_DONE_KHR)
        CASE_VK_RESULT(OPERATION_DEFERRED_KHR)
        CASE_VK_RESULT(OPERATION_NOT_DEFERRED_KHR)
        CASE_VK_RESULT(ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR)
        CASE_VK_RESULT(ERROR_COMPRESSION_EXHAUSTED_EXT)
        CASE_VK_RESULT(INCOMPATIBLE_SHADER_BINARY_EXT)
        CASE_VK_RESULT(PIPELINE_BINARY_MISSING_KHR)
        CASE_VK_RESULT(ERROR_NOT_ENOUGH_SPACE_KHR)
    }
#undef CASE_VK_RESULT
    ERROR("Unknown VkResult: %d", result);
    return "unknown";
}

static inline const char *d3d10_ddi_interface_version_name(unsigned interface_) {
#define CASE_INTERFACE_VERSION(name) case name##_DDI_INTERFACE_VERSION: return #name;
    switch (interface_) {
        CASE_INTERFACE_VERSION(D3D10_0)
        CASE_INTERFACE_VERSION(D3D10_1)
        CASE_INTERFACE_VERSION(D3D11_0)
        CASE_INTERFACE_VERSION(D3D10on9)
        CASE_INTERFACE_VERSION(D3D10_0_x)
        CASE_INTERFACE_VERSION(D3D10_0_x_vista)
        CASE_INTERFACE_VERSION(D3D10_1_x)
        CASE_INTERFACE_VERSION(D3D10_1_x_vista)
        CASE_INTERFACE_VERSION(D3D10_0_7)
        CASE_INTERFACE_VERSION(D3D10_1_7)
        CASE_INTERFACE_VERSION(D3D11_0_7)
        CASE_INTERFACE_VERSION(D3D11_0_vista)
        CASE_INTERFACE_VERSION(D3D11_1)
        CASE_INTERFACE_VERSION(D3DWDDM1_3)
        CASE_INTERFACE_VERSION(D3DWDDM2_0)
        CASE_INTERFACE_VERSION(D3DWDDM2_1)
        CASE_INTERFACE_VERSION(D3DWDDM2_2)
        CASE_INTERFACE_VERSION(D3DWDDM2_3)
        CASE_INTERFACE_VERSION(D3DWDDM2_4)
        CASE_INTERFACE_VERSION(D3DWDDM2_5)
        CASE_INTERFACE_VERSION(D3DWDDM2_6)
        CASE_INTERFACE_VERSION(D3DWDDM2_7)
        CASE_INTERFACE_VERSION(D3DWDDM2_8)
        CASE_INTERFACE_VERSION(D3DWDDM2_9)
        CASE_INTERFACE_VERSION(D3DWDDM3_0)
        CASE_INTERFACE_VERSION(D3DWDDM3_1)
        CASE_INTERFACE_VERSION(D3DWDDM3_2)
    }
#undef CASE_INTERFACE_VERSION
    ERROR("Unknown interface version: %x", interface_);
    return "unknown";
}

static const char *dxgi_ddi_interface_version_name(unsigned interface_, unsigned version) {
    if (IS_DXGI1_6_1_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.6.1";
    } else if (IS_DXGI1_6_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.6";
    } else if (IS_DXGI1_5_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.5";
    } else if (IS_DXGI1_4_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.4";
    } else if (IS_DXGI1_3_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.3";
    } /* else if (IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS(interface_, version)) {
        return "DXGI MULTIPLANE OVERLAY";
    } */ else if (IS_DXGI1_2_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.2";
    } else if (IS_DXGI1_1_BASE_FUNCTIONS(interface_, version)) {
        return "DXGI1.1";
    } else {
        return "DXGI1.0";
    }
}

static void APIENTRY virtio_wddm_destroy_device(D3D10DDI_HDEVICE hDevice)
{
    TRACE();
    VIRTIO_WDDM_Device *device = hDevice.pDrvPrivate;

    if (!device) {
        ERROR("%s: missing device private data", __FUNCTION__);
        return;
    }

    INFO("%s: destroying device %p / %p", __FUNCTION__, device->callbacks.hRTDevice, device->base.hRTDevice.handle);

    // FIXME: ensure that DXVK never tries to submit any more commands

    if (device->base.pPresentFence) {
        INFO("%s: releasing present fence %p", __FUNCTION__, device->base.pPresentFence);
        ULONG refs = ID3D11Fence_Release(device->base.pPresentFence);
        device->base.pPresentFence = NULL;
        INFO("%s: released present fence, refs=%lu", __FUNCTION__, refs);
    }

    if (device->base.pCtx1) {
        INFO("%s: flushing context1 %p", __FUNCTION__, device->base.pCtx1);
        ID3D11DeviceContext1_Flush(device->base.pCtx1);
        INFO("%s: flushed context1", __FUNCTION__);
    }

#define RELEASE_CONTEXT(version) do { \
        if (device->base.pCtx##version) { \
            INFO("%s: releasing context" #version " %p", __FUNCTION__, device->base.pCtx##version); \
            ULONG refs = ID3D11DeviceContext##version##_Release(device->base.pCtx##version); \
            device->base.pCtx##version = NULL; \
            INFO("%s: released context" #version ", refs=%lu", __FUNCTION__, refs); \
        } \
    } while (0)
    RELEASE_CONTEXT(1);
    RELEASE_CONTEXT(2);
    RELEASE_CONTEXT(3);
    RELEASE_CONTEXT(4);
#undef RELEASE_CONTEXT

#define RELEASE_DEVICE(version) do { \
        if (device->base.pDev##version) { \
            INFO("%s: releasing device" #version " %p", __FUNCTION__, device->base.pDev##version); \
            ULONG refs = ID3D11Device##version##_Release(device->base.pDev##version); \
            device->base.pDev##version = NULL; \
            INFO("%s: released device" #version ", refs=%lu", __FUNCTION__, refs); \
        } \
    } while (0)
    RELEASE_DEVICE(1);
    RELEASE_DEVICE(2);
    RELEASE_DEVICE(3);
    RELEASE_DEVICE(5);
#undef RELEASE_DEVICE

    if (device->paging.queue) {
        D3DDDI_DESTROYPAGINGQUEUE args = { .hPagingQueue = device->paging.queue };
        HRESULT hr = device->base.KTCallbacks.pfnDestroyPagingQueueCb(
            device->base.hRTDevice.handle, &args);
        INFO("r59 DestroyPagingQueue hr=0x%08lx", hr);
        device->paging.queue = 0;
    }
    if (device->present.context) {
        D3DDDICB_DESTROYCONTEXT args = { .hContext = device->present.context };
        HRESULT hr = device->base.KTCallbacks.pfnDestroyContextCb(
            device->base.hRTDevice.handle, &args);
        INFO("r59 DestroyContext hr=0x%08lx", hr);
        device->present.context = NULL;
    }

    if (device->adapter) {
        INFO("%s: releasing DXGI adapter %p", __FUNCTION__, device->adapter);
        ULONG refs = IDXGIAdapter_Release(device->adapter);
        device->adapter = NULL;
        INFO("%s: released DXGI adapter, refs=%lu", __FUNCTION__, refs);
    }

    if (device->vk_inst && device->vk_DestroyInstance) {
        INFO("%s: destroying Vulkan instance %p", __FUNCTION__, device->vk_inst);
        device->vk_DestroyInstance(device->vk_inst, NULL);
        device->vk_inst = VK_NULL_HANDLE;
        INFO("%s: destroyed Vulkan instance", __FUNCTION__);
    }

    if (device->vulkan) {
        INFO("%s: unloading Vulkan loader %p", __FUNCTION__, device->vulkan);
        FreeLibrary(device->vulkan);
        device->vulkan = NULL;
        INFO("%s: unloaded Vulkan loader", __FUNCTION__);
    }
    if (device->icd) {
        INFO("%s: unloading Vulkan ICD %p", __FUNCTION__, device->icd);
        FreeLibrary(device->icd);
        device->icd = NULL;
        INFO("%s: unloaded Vulkan ICD", __FUNCTION__);
    }

    memset(device, 0, sizeof(*device));
    INFO("%s: complete", __FUNCTION__);
}

/* Declared before temporary COM references, so those unwind first. The
 * imported Vulkan instance must outlive every DXVK object using it. */
static void cleanup_failed_device(VIRTIO_WDDM_Device **device) {
    if (*device) {
        INFO("r59 CreateDevice failure cleanup");
        virtio_wddm_destroy_device((D3D10DDI_HDEVICE) { .pDrvPrivate = *device });
    }
}

#define CLEANUP_D3D11_DEVICE __attribute__((cleanup(free_d3d11_device)))
#define CLEANUP_D3D11_CONTEXT __attribute__((cleanup(free_d3d11_device_context)))

static HRESULT create_present_context(VIRTIO_WDDM_Device *device) {
#if 1
    D3DDDICB_CREATECONTEXT context = {};
    HRESULT hr = device->base.KTCallbacks.pfnCreateContextCb(device->base.hRTDevice.handle, &context);
    if (FAILED(hr)) {
        return hr;
    }
#else
    D3DDDICB_CREATECONTEXTVIRTUAL context = {
        .NodeOrdinal = 2,
    };
    HRESULT hr = device->base.KTCallbacks.pfnCreateContextVirtualCb(device->base.hRTDevice.handle, &context);
    if (FAILED(hr)) {
        return hr;
    }
#endif

    device->present.context = context.hContext;
    return S_OK;
}

static HRESULT create_paging_queue(VIRTIO_WDDM_Device *device) {
    D3DDDICB_CREATEPAGINGQUEUE paging_queue = {
        .Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL,
        .PhysicalAdapterIndex = 0,
    };

    HRESULT hr = device->base.KTCallbacks.pfnCreatePagingQueueCb(device->base.hRTDevice.handle, &paging_queue);
    if (FAILED(hr)) {
        return hr;
    }

    device->paging.queue = paging_queue.hPagingQueue;
    device->paging.sync_object = paging_queue.hSyncObject;
    device->paging.fence_value = paging_queue.FenceValueCPUVirtualAddress;

    return S_OK;
}

static const char *get_drm_context_type_name(uint32_t drm_context_type) {
    switch (drm_context_type) {
        case VIRTGPU_DRM_CONTEXT_MSM:      return "msm";
        case VIRTGPU_DRM_CONTEXT_AMDGPU:   return "amdgpu";
        case VIRTGPU_DRM_CONTEXT_I915:     return "i915";
        case VIRTGPU_DRM_CONTEXT_ASAHI:    return "asahi";
        case VIRTGPU_DRM_CONTEXT_PANFROST: return "panfrost";
        case VIRTGPU_DRM_CONTEXT_XE:       return "xe";
    }
    ERROR("%s: Unknown drm context type: %u", __FUNCTION__, drm_context_type);
    return NULL;
}

static const wchar_t *get_drm_context_type_icd_name(uint32_t drm_context_type) {
    switch (drm_context_type) {
        case VIRTGPU_DRM_CONTEXT_MSM:      return L"vulkan_freedreno.dll";
        // TODO: port more drivers
        /*
        case VIRTGPU_DRM_CONTEXT_AMDGPU:   return L"vulkan_radeon.dll";
        case VIRTGPU_DRM_CONTEXT_I915:     return L"vulkan_intel.dll";
        case VIRTGPU_DRM_CONTEXT_ASAHI:    return L"vulkan_asahi.dll";
        case VIRTGPU_DRM_CONTEXT_PANFROST: return L"vulkan_panfrost.dll";
        */
        case VIRTGPU_DRM_CONTEXT_XE:       return L"vulkan_intel.dll";
    }
    ERROR("%s: Unsupported drm context type: %u (%s)", __FUNCTION__, drm_context_type, get_drm_context_type_name(drm_context_type));
    return NULL;
}

static HRESULT get_drm_context_type(VIRTIO_WDDM_Device *device, uint32_t *drm_context_type) {
    struct {
        VIRTIO_WDDM_Capset capset;
        uint8_t caps[sizeof(struct virgl_renderer_capset_drm)];
    } escape_priv = {
        .capset = {
            .tag = VIRTIO_WDDM_ESCAPE_CAPSET_TAG,
            .capset_id = VIRTIO_WDDM_CAPSET_ID_DRM,
            .version = 0,
        },
    };
    D3DDDICB_ESCAPE escape = {
        .hDevice = device->base.hRTDevice.handle,
        .pPrivateDriverData = &escape_priv,
        .PrivateDriverDataSize = sizeof(escape_priv),
    };

    INFO("%s: Escape DRM capset enter size=%u", __FUNCTION__,
         (unsigned)escape.PrivateDriverDataSize);
    HRESULT hr = device->base.KTCallbacks.pfnEscapeCb(device->base.pAdapter->hRTAdapter.handle, &escape);
    INFO("%s: Escape DRM capset result=0x%08lx", __FUNCTION__, (unsigned long)hr);
    if (FAILED(hr)) {
        ERROR("%s: Failed to query DRM capset info: %08lx", __FUNCTION__, hr);
        return hr;
    }

    struct virgl_renderer_capset_drm caps;
    memcpy(&caps, escape_priv.caps, sizeof(caps));

    *drm_context_type = caps.context_type;

    return S_OK;
}

static HMODULE load_icd(const wchar_t *icd_name) {
    wchar_t umd_path[MAX_PATH];
    HMODULE umd_module = NULL;

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&load_icd, &umd_module);

    if (!umd_module) return NULL;

    GetModuleFileNameW(umd_module, umd_path, MAX_PATH);
    wchar_t *last_slash = wcsrchr(umd_path, L'\\');
    if (!last_slash) {
        return NULL;
    }

    size_t remaining_space = MAX_PATH - (last_slash - umd_path) - 1;

    if (wcscpy_s(last_slash + 1, remaining_space, icd_name) != 0) {
        return FALSE;
    }

    return LoadLibraryExW(umd_path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

HRESULT APIENTRY virtio_wddm_create_device(D3D10DDI_HADAPTER hAdapter, D3D10DDIARG_CREATEDEVICE *pArgs) {
    TRACE();
    INFO("%s: enter", __FUNCTION__);
    HRESULT hr = S_OK;
    VkResult res = VK_SUCCESS;

    VIRTIO_WDDM_Adapter *adapter = hAdapter.pDrvPrivate;
    VIRTIO_WDDM_Device *device = pArgs->hDrvDevice.pDrvPrivate;
    memset(device, 0, sizeof(*device));
    __attribute__((cleanup(cleanup_failed_device)))
        VIRTIO_WDDM_Device *failed_device = device;

    device->base.pAdapter = &adapter->base;
    device->base.hRTDevice = pArgs->hRTDevice;
    device->base.uIfVersion = pArgs->Interface;
    device->base.uRtVersion = pArgs->Version;
    device->base.KTCallbacks = *pArgs->pKTCallbacks;
    device->base.hRTCoreLayer = pArgs->hRTCoreLayer;
    device->base.pUMCallbacks = pArgs->p11UMCallbacks;
    device->base.FeatureLevel = D3D_FEATURE_LEVEL_11_0;

    device->dxgi_callbacks = pArgs->DXGIBaseDDI.pDXGIBaseCallbacks;

    INFO("%s: process=%lu thread=%lu interface=0x%x version=0x%x flags=0x%x",
         __FUNCTION__, GetCurrentProcessId(), GetCurrentThreadId(),
         pArgs->Interface, pArgs->Version, pArgs->Flags);

    device->vulkan = LoadLibraryA("vulkan-1.dll");
    if (!device->vulkan) {
        ERROR("%s: Vulkan loader unavailable: %lu", __FUNCTION__, GetLastError());
        return E_FAIL;
    }
    INFO("%s: loaded vulkan-1.dll=%p", __FUNCTION__, device->vulkan);

    if (adapter->supported_capsets & VIRTIO_WDDM_CAPSET_MASK_DRM) {
        uint32_t drm_context_type = 0;
        hr = get_drm_context_type(device, &drm_context_type);
        if (SUCCEEDED(hr)) {
            INFO("%s: Supported DRM context type: %s", __FUNCTION__, get_drm_context_type_name(drm_context_type));
            const wchar_t *vdrm_icd_name = get_drm_context_type_icd_name(drm_context_type);
            if (vdrm_icd_name != NULL) {
                INFO("%s: Using vDRM Vulkan ICD: %ws", __FUNCTION__, vdrm_icd_name);
                device->icd = load_icd(vdrm_icd_name);
            }
        }
    }

    if (device->icd == NULL) {
        device->icd = load_icd(L"vulkan_virtio.dll");
    }

    /* Missing optional ICDs must fail device creation, not abort the host
     * process (which may be the desktop window manager). */
    if (!device->icd) {
        ERROR("%s: No compatible Vulkan ICD: %lu", __FUNCTION__, GetLastError());
        FreeLibrary(device->vulkan);
        device->vulkan = NULL;
        return E_FAIL;
    }

    /* Chained off `callbacks` below. Both hooks or neither: the ICD ignores a
     * half-installed allocator, and an allocation made without the matching free
     * would leak the runtime's resource association. */
    device->runtime_allocator = (VkD3DDDIRuntimeAllocator) {
        .sType = VK_STRUCTURE_TYPE_D3DDDI_RUNTIME_ALLOCATOR,
        .pNext = NULL,
        .ctx   = device,
        .alloc = (void *) virtio_wddm_runtime_alloc,
        .free  = (void *) virtio_wddm_runtime_free,
    };

    device->callbacks = (VkD3DDDICallbacks) {
        .sType = VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS,
        .pNext = &device->runtime_allocator,
        .AdapterLuid = adapter->luid,
        .hRTAdapter = adapter->base.hRTAdapter.handle,
        .hRTDevice = device->base.hRTDevice.handle,
        .pAdapterCallbacks = &adapter->base.KTCallbacks,
        .pKTCallbacks = &device->base.KTCallbacks,
        .pDXGIBaseCallbacks = pArgs->DXGIBaseDDI.pDXGIBaseCallbacks,
        .hRTCoreLayer = pArgs->hRTCoreLayer.handle,
        .p11UMCallbacks = pArgs->p11UMCallbacks,
    };

    VkDirectDriverLoadingInfoLUNARG driver_info = {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG,
        .pNext = NULL,
        .flags = 0,
        .pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddrLUNARG) GetProcAddress(device->icd, "vk_icdGetInstanceProcAddr"),
    };
    if (!driver_info.pfnGetInstanceProcAddr) {
        ERROR("%s: Vulkan ICD lacks vk_icdGetInstanceProcAddr", __FUNCTION__);
        FreeLibrary(device->icd);
        FreeLibrary(device->vulkan);
        device->icd = NULL;
        device->vulkan = NULL;
        return E_FAIL;
    }

    VkDirectDriverLoadingListLUNARG loading_list = {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
        .pNext = &device->callbacks,
        .mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG,
        .driverCount = 1,
        .pDrivers = &driver_info,
    };

    INFO("D3DDDI callbacks: %p, dev %p", &device->callbacks, device->callbacks.hRTDevice);

    PFN_vkGetInstanceProcAddr vk_GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) GetProcAddress(device->vulkan, "vkGetInstanceProcAddr");
    PFN_vkGetDeviceProcAddr vk_GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) GetProcAddress(device->vulkan, "vkGetDeviceProcAddr");
    if (!vk_GetInstanceProcAddr || !vk_GetDeviceProcAddr) {
        ERROR("%s: Vulkan loader entry points unavailable: gipa=%p gdpa=%p",
              __FUNCTION__, vk_GetInstanceProcAddr, vk_GetDeviceProcAddr);
        return E_FAIL;
    }
    INFO("%s: resolving loader instance entry points", __FUNCTION__);

#define LOAD_PROC(name) PFN_vk##name vk_##name = (PFN_vk##name) vk_GetInstanceProcAddr(NULL, "vk" #name)
    LOAD_PROC(CreateInstance);
    LOAD_PROC(EnumerateInstanceExtensionProperties);
#undef LOAD_PROC

    if (!vk_CreateInstance || !vk_EnumerateInstanceExtensionProperties)
        return E_FAIL;

    uint32_t extension_count = 0;
    INFO("%s: enumerating instance extensions (count)", __FUNCTION__);
    res = vk_EnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
    if (res != VK_SUCCESS) {
        ERROR("Failed to enumerate instance extensions: %s", vk_result_to_str(res));
        return E_FAIL;
    }

    CLEANUP_FREE VkExtensionProperties *extension_props = calloc(sizeof(*extension_props), extension_count);
    CLEANUP_FREE const char **extension_names = calloc(sizeof(*extension_names), extension_count);
    if (extension_count && (!extension_props || !extension_names))
        return E_OUTOFMEMORY;
    INFO("%s: enumerating %u instance extensions (properties)", __FUNCTION__, extension_count);
    res = vk_EnumerateInstanceExtensionProperties(NULL, &extension_count, extension_props);
    if (res != VK_SUCCESS) {
        ERROR("Failed to enumerate instance extensions: %s", vk_result_to_str(res));
        return E_FAIL;
    }

    bool have_LUNARG_direct_driver_loading = false;

    for (size_t i = 0; i < extension_count; i++) {
        extension_names[i] = extension_props[i].extensionName;
        if (!strcmp(extension_props[i].extensionName, VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME)) {
            have_LUNARG_direct_driver_loading = true;
        }
    }

    if (!have_LUNARG_direct_driver_loading) {
        ERROR("Direct driver loading is not supported, cannot continue");
        return E_FAIL;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "D3D11",
        .applicationVersion = 0,
        .pEngineName = "VirtIO D3D11 UMD",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 1),
        .apiVersion = VK_API_VERSION_1_3,
    };

    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = &loading_list,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = NULL,
        //.enabledLayerCount = 1,
        //.ppEnabledLayerNames = (const char *[]) { "VK_LAYER_KHRONOS_validation" },
        // TODO: do we need more extensions?
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = (const char *[]) { VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME },
    };

    VkInstance instance;
    INFO("%s: calling vkCreateInstance", __FUNCTION__);
    res = vk_CreateInstance(&info, NULL, &instance);
    if (res != VK_SUCCESS) {
        ERROR("Failed to create instance: %s", vk_result_to_str(res));
        return E_FAIL;
    }
    device->vk_inst = instance;
    device->vk_DestroyInstance = (PFN_vkDestroyInstance)
        vk_GetInstanceProcAddr(instance, "vkDestroyInstance");
    INFO("%s: vkCreateInstance returned %p", __FUNCTION__, instance);

    Vulkan_Instance_Info instance_info = {
        .loader_proc = vk_GetInstanceProcAddr,
        .instance = instance,
        // TODO: do we need other extensions?
        .extension_count = 1,
        .extension_names = (const char *[]) { VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME },
    };

    IDXGIFactory4 *factory = NULL;
    INFO("%s: creating imported DXVK factory", __FUNCTION__);
    hr = DXVK_CreateDXGIFactory(&instance_info, &IID_IDXGIFactory4, (void **) &factory);
    if (FAILED(hr)) {
        ERROR("Failed to create DXVK DXGI factory: 0x%x", hr);
        return hr;
    }
    INFO("%s: imported DXVK factory=%p", __FUNCTION__, factory);

    INFO("%s: enumerating DXVK adapter by LUID %lx-%lx", __FUNCTION__,
         adapter->luid.HighPart, adapter->luid.LowPart);
    hr = IDXGIFactory4_EnumAdapterByLuid(factory, adapter->luid, &IID_IDXGIAdapter, (void **) &device->adapter);
    if (FAILED(hr)) {
        ERROR("Failed to enum DXVK DXGI adapter by LUID %lx-%lx: 0x%x",
              adapter->luid.HighPart, adapter->luid.LowPart, hr);
        IDXGIFactory4_Release(factory);
        return hr;
    }
    INFO("%s: DXVK adapter=%p", __FUNCTION__, device->adapter);
    IDXGIFactory4_Release(factory);
    factory = NULL;

    D3D_FEATURE_LEVEL feature_level;
    switch (D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(pArgs->Flags)) {
        case D3D11DDI_3DPIPELINELEVEL_10_0:
            feature_level = D3D_FEATURE_LEVEL_10_0;
            break;
        case D3D11DDI_3DPIPELINELEVEL_10_1:
            feature_level = D3D_FEATURE_LEVEL_10_1;
            break;
        case D3D11DDI_3DPIPELINELEVEL_11_0:
            feature_level = D3D_FEATURE_LEVEL_11_0;
            break;
        case D3D11_1DDI_3DPIPELINELEVEL_11_1:
            feature_level = D3D_FEATURE_LEVEL_11_1;
            break;
        default:
            ERROR("%s: unknown pipeline level %u", __FUNCTION__, D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(pArgs->Flags));
            return E_FAIL;
    }

    CLEANUP_D3D11_DEVICE ID3D11Device *d3d11_device = NULL;
    CLEANUP_D3D11_CONTEXT ID3D11DeviceContext *d3d11_context = NULL;
    INFO("%s: calling internal D3D11CreateDevice feature=0x%x", __FUNCTION__, feature_level);
    hr = D3D11CreateDevice(device->adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                           &feature_level, 1, D3D11_SDK_VERSION, &d3d11_device,
                           &device->base.FeatureLevel, &d3d11_context);
    if (FAILED(hr)) {
        ERROR("Failed to create DXVK D3D11 device: 0x%x", hr);
        return hr;
    }
    INFO("%s: internal D3D11CreateDevice returned dev=%p ctx=%p feature=0x%x",
         __FUNCTION__, d3d11_device, d3d11_context, device->base.FeatureLevel);

    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_ID3D11Device1, (void **) &device->base.pDev1);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11Device1 interface for DXVK D3D11 device: 0x%x", hr);
        return hr;
    }

    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_ID3D11Device2, (void **) &device->base.pDev2);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11Device2 interface for DXVK D3D11 device: 0x%x", hr);
        return hr;
    }

    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_ID3D11Device3, (void **) &device->base.pDev3);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11Device3 interface for DXVK D3D11 device: 0x%x", hr);
        return hr;
    }

    hr = ID3D11Device_QueryInterface(d3d11_device, &IID_ID3D11Device5, (void **) &device->base.pDev5);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11Device5 interface for DXVK D3D11 device: 0x%x", hr);
        return hr;
    }

    hr = ID3D11DeviceContext_QueryInterface(d3d11_context, &IID_ID3D11DeviceContext1, (void **) &device->base.pCtx1);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11DeviceContext1 interface for DXVK D3D11 device context: 0x%x", hr);
        return hr;
    }

    hr = ID3D11DeviceContext_QueryInterface(d3d11_context, &IID_ID3D11DeviceContext2, (void **) &device->base.pCtx2);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11DeviceContext2 interface for DXVK D3D11 device context: 0x%x", hr);
        return hr;
    }

    hr = ID3D11DeviceContext_QueryInterface(d3d11_context, &IID_ID3D11DeviceContext3, (void **) &device->base.pCtx3);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11DeviceContext3 interface for DXVK D3D11 device context: 0x%x", hr);
        return hr;
    }

    hr = ID3D11DeviceContext_QueryInterface(d3d11_context, &IID_ID3D11DeviceContext4, (void **) &device->base.pCtx4);
    if (FAILED(hr)) {
        ERROR("Failed to query ID3D11DeviceContext4 interface for DXVK D3D11 device context: 0x%x", hr);
        return hr;
    }

    VkInstance imported_instance = VK_NULL_HANDLE;
    hr = DXVK_IDXGIVkInteropDevice1_GetVulkanHandles(device->base.pDev1, &imported_instance, &device->vk_phys, &device->vk);
    if (FAILED(hr)) {
        ERROR("Failed to GetVulkanHandles from DXVK D3D11 device: 0x%x", hr);
        return hr;
    }

    if (imported_instance != device->vk_inst) {
        ERROR("DXVK returned a different instance than the imported instance");
        return E_FAIL;
    }

    INFO("%s: inst %p, phys %p, dev %p", __FUNCTION__, device->vk_inst, device->vk_phys, device->vk);

#define X(name) device->vk_##name = (PFN_vk##name) vk_GetInstanceProcAddr(device->vk_inst, "vk" #name);
    VK_INSTANCE_FUNCTION_LIST
#undef X
#define X(name) device->vk_##name = (PFN_vk##name) vk_GetDeviceProcAddr(device->vk, "vk" #name);
    VK_DEVICE_FUNCTION_LIST
    VK_OPTIONAL_DEVICE_FUNCTION_LIST
#undef X
#define X(name) if (device->vk_##name == NULL) { ERROR("Failed to load vk%s from ICD", #name); return E_FAIL; }
    VK_INSTANCE_FUNCTION_LIST
    VK_DEVICE_FUNCTION_LIST
#undef X

    /* Turnip's WDDM backend does not expose Vulkan Win32 semaphore handles
     * yet.  A private timeline fence plus a bounded CPU wait preserves
     * ordering for bring-up without pretending cross-API sharing exists. */
    hr = ID3D11Device5_CreateFence(device->base.pDev5, 0, (D3D11_FENCE_FLAG)0, &IID_ID3D11Fence, (void **) &device->base.pPresentFence);
    if (FAILED(hr)) {
        ERROR("Failed to create D3D11 fence: 0x%x", hr);
        return hr;
    }
    device->base.presentFenceValue = 1;

    hr = create_present_context(device);
    if (FAILED(hr)) {
        ERROR("Failed to create present context: 0x%x", hr);
        return hr;
    }

    /* Turnip's WDDM transport creates its own KMT device and only consumes
     * the adapter LUID from VkD3DDDICallbacks.  Its ContextInit therefore
     * does not initialize this runtime device.  AllocateCb/OpenAllocation
     * and presentation use this device and require a virtio context too. */
    if (adapter->supported_capsets & VIRTIO_WDDM_CAPSET_MASK_DRM) {
        VIRTIO_WDDM_ContextInit init = {
            .tag = VIRTIO_WDDM_ESCAPE_CONTEXT_INIT_TAG,
            .capset_id = VIRTIO_WDDM_CAPSET_ID_DRM,
            .num_rings = 1,
            .debug_name = "d3d11-runtime-r54",
        };
        D3DDDICB_ESCAPE escape = {
            .hDevice = device->base.hRTDevice.handle,
            .pPrivateDriverData = &init,
            .PrivateDriverDataSize = sizeof(init),
        };
        hr = device->base.KTCallbacks.pfnEscapeCb(adapter->base.hRTAdapter.handle, &escape);
        INFO("%s: r54 runtime ContextInit rtdev=%p hr=0x%08lx",
             __FUNCTION__, device->base.hRTDevice.handle, hr);
        if (FAILED(hr))
            return hr;
    }

    hr = create_paging_queue(device);
    if (FAILED(hr)) {
        ERROR("Failed to create paging queue: 0x%x", hr);
        return hr;
    }

    INFO("%s: D3D11 DDI version = %s", __FUNCTION__, d3d10_ddi_interface_version_name(pArgs->Interface));
    INFO("%s: DXGI DDI version = %s", __FUNCTION__, dxgi_ddi_interface_version_name(pArgs->Interface, pArgs->Version));

    switch (pArgs->Interface) {
        case D3D11_0_DDI_INTERFACE_VERSION:
            tritonFillD3D11DeviceFuncs(pArgs->p11DeviceFuncs);
            pArgs->p11DeviceFuncs->pfnDestroyDevice = virtio_wddm_destroy_device;
            break;
        case D3D11_1_DDI_INTERFACE_VERSION:
            tritonFillD3D11_1DeviceFuncs(pArgs->p11_1DeviceFuncs);
            pArgs->p11_1DeviceFuncs->pfnDestroyDevice = virtio_wddm_destroy_device;
            break;
        case D3DWDDM1_3_DDI_INTERFACE_VERSION:
            tritonFillWDDM1_3DeviceFuncs(pArgs->pWDDM1_3DeviceFuncs);
            pArgs->pWDDM1_3DeviceFuncs->pfnDestroyDevice = virtio_wddm_destroy_device;
            break;
        case D3DWDDM2_0_DDI_INTERFACE_VERSION:
            tritonFillWDDM2_0DeviceFuncs(pArgs->pWDDM2_0DeviceFuncs);
            pArgs->pWDDM2_0DeviceFuncs->pfnDestroyDevice = virtio_wddm_destroy_device;
            break;
        case D3DWDDM2_1_DDI_INTERFACE_VERSION:
            tritonFillWDDM2_1DeviceFuncs(pArgs->pWDDM2_1DeviceFuncs);
            pArgs->pWDDM2_1DeviceFuncs->pfnDestroyDevice = virtio_wddm_destroy_device;
            break;
        default:
            ERROR("%s: unsupported interface: %u", __FUNCTION__, pArgs->Interface);
            return E_FAIL;
    }

    pArgs->p11DeviceFuncs->pfnCalcPrivateResourceSize = virtio_wddm_calc_resource_size;
    pArgs->p11DeviceFuncs->pfnCalcPrivateOpenedResourceSize = virtio_wddm_calc_opened_resource_size;
    pArgs->p11DeviceFuncs->pfnCreateResource = virtio_wddm_create_resource;
    pArgs->p11DeviceFuncs->pfnOpenResource = virtio_wddm_open_resource;
    pArgs->p11DeviceFuncs->pfnDestroyResource = virtio_wddm_destroy_resource;

    if (IS_DXGI1_2_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)) {
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnPresent                  = virtio_wddm_present;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnGetGammaCaps             = virtio_wddm_get_gamma_caps;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnSetDisplayMode           = virtio_wddm_set_display_mode;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnSetResourcePriority      = virtio_wddm_set_resource_priority;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnQueryResourceResidency   = virtio_wddm_query_resource_residency;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnRotateResourceIdentities = virtio_wddm_rotate_resource_identities;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnBlt                      = virtio_wddm_blt;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnResolveSharedResource    = virtio_wddm_resolve_shared_resource;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnBlt1                     = virtio_wddm_blt1;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnOfferResources           = virtio_wddm_offer_resources;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnReclaimResources         = virtio_wddm_reclaim_resources;
    } else if (IS_DXGI1_1_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)) {
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnPresent                  = virtio_wddm_present;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnGetGammaCaps             = virtio_wddm_get_gamma_caps;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnSetDisplayMode           = virtio_wddm_set_display_mode;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnSetResourcePriority      = virtio_wddm_set_resource_priority;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnQueryResourceResidency   = virtio_wddm_query_resource_residency;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnRotateResourceIdentities = virtio_wddm_rotate_resource_identities;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnBlt                      = virtio_wddm_blt;
        pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnResolveSharedResource    = virtio_wddm_resolve_shared_resource;
    }

    failed_device = NULL;
    return S_OK;
    //return DXGI_STATUS_NO_REDIRECTION;
}

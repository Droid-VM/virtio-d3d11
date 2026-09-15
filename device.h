#pragma once

#include <pthread.h>

#include <triton.h>
#include <virtio_wddm_uapi.h>
#include <vulkan/vulkan.h>
#include <vulkan_d3dddi.h>

#define VK_INSTANCE_FUNCTION_LIST \
    X(GetPhysicalDeviceMemoryProperties2) \
    X(DestroyInstance)

#define VK_DEVICE_FUNCTION_LIST \
    X(AllocateMemory) \
    X(FreeMemory) \
    X(CreateImage) \
    X(DestroyImage) \
    X(BindImageMemory2) \
    X(GetImageMemoryRequirements2) \
    X(GetImageSubresourceLayout)

#define VK_OPTIONAL_DEVICE_FUNCTION_LIST \
    X(GetMemoryWin32HandleKHR)

typedef struct {
    TRITON_DEVICE base;
    DXGI_DDI_BASE_CALLBACKS *dxgi_callbacks;
    HMODULE vulkan;
    HMODULE icd;
    VkD3DDDICallbacks callbacks;
    IDXGIAdapter *adapter;
    // ID3D11Device is stored in base

    struct {
        D3DKMT_HANDLE queue;
        D3DKMT_HANDLE sync_object;
        void *fence_value;
    } paging;

    struct {
        HANDLE context;
    } present;

    /* Kept alive for the lifetime of the instance: vkCreateInstance chains it
     * off `callbacks`, and the ICD reads the hooks out of it. */
    VkD3DDDIRuntimeAllocator runtime_allocator;

    VkInstance vk_inst;
    VkPhysicalDevice vk_phys;
    VkDevice vk;
#define X(name) PFN_vk##name vk_##name;
    VK_INSTANCE_FUNCTION_LIST
    VK_DEVICE_FUNCTION_LIST
    VK_OPTIONAL_DEVICE_FUNCTION_LIST
#undef X
} VIRTIO_WDDM_Device;

/* Result of the most recent runtime allocation on this thread.
 *
 * The ICD calls the alloc hook from inside vkAllocateMemory, which the UMD
 * called from CreateResource on this same thread, so a thread-local slot is
 * enough to hand the handles back to CreateResource -- and unlike a field on the
 * device it stays correct when the runtime creates resources concurrently.
 *
 * CreateResource must check hRTResource against the resource it is creating
 * before trusting hAllocation: a stale record from an earlier resource on the
 * same thread would otherwise be attributed to this one. hAllocation == 0 means
 * no runtime allocation happened. */
typedef struct {
    void *hRTResource;
    D3DKMT_HANDLE hAllocation;
    D3DKMT_HANDLE hKMResource;
} VIRTIO_WDDM_RuntimeAllocRecord;

extern __thread VIRTIO_WDDM_RuntimeAllocRecord virtio_wddm_last_runtime_alloc;

/* Input flags for the synchronous ICD -> runtime allocator callback. Keep
 * this separate from the result so nested, unrelated allocations cannot
 * overwrite the resource being created. */
typedef struct {
    HANDLE hRTResource;
    bool primary;
    DXGI_DDI_PRIMARY_DESC primary_desc;
} VIRTIO_WDDM_RuntimeAllocScope;

extern __thread VIRTIO_WDDM_RuntimeAllocScope virtio_wddm_runtime_alloc_scope;

/* VIRTIO_WDDM_RuntimeAllocator hooks; ctx is the VIRTIO_WDDM_Device. */
int virtio_wddm_runtime_alloc(void *ctx, const VIRTIO_WDDM_RuntimeAllocRequest *req,
                              VIRTIO_WDDM_RuntimeAllocResult *out);
int virtio_wddm_runtime_free(void *ctx, const VIRTIO_WDDM_RuntimeAllocResult *alloc);

SIZE_T APIENTRY virtio_wddm_calc_device_size(D3D10DDI_HADAPTER hAdapter, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pArgs);
HRESULT APIENTRY virtio_wddm_create_device(D3D10DDI_HADAPTER hAdapter, D3D10DDIARG_CREATEDEVICE *pArgs);

extern const char *vk_result_to_str(VkResult result);

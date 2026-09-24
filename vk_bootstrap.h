#pragma once

/* Shared by the D3D11 (dx11um_virtio) and D3D12 (dx12um_virtio) user-mode
 * drivers: validating the adapter, and loading the driver package's Vulkan
 * loader and vDRM ICD with the VkD3DDDICallbacks chain the ICD reads.
 *
 * Include windows.h, the WDDM UMD headers, vulkan.h and vulkan_d3dddi.h
 * first. */

#include <virtio_wddm_uapi.h>

typedef struct {
    LUID luid;
    VIRTIO_WDDM_CapsetMask capsets;
} VIRTIO_WDDM_AdapterCaps;

/* QueryAdapterInfo through the runtime and check that the KMD is ours and can
 * run a Vulkan ICD (3D, shmem, Venus or vDRM). */
HRESULT virtio_wddm_query_adapter(const D3DDDI_ADAPTERCALLBACKS *callbacks, HANDLE hRTAdapter,
                                  VIRTIO_WDDM_AdapterCaps *caps);

typedef struct {
    HMODULE vulkan; /* droidvm_vulkan.dll from the driver package */
    HMODULE icd;    /* the vDRM ICD matching the host context type, else vulkan_virtio.dll */
    PFN_vkGetInstanceProcAddr vk_GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr vk_GetDeviceProcAddr;
    VkDirectDriverLoadingInfoLUNARG driver_info;
    /* Chain into VkInstanceCreateInfo.pNext and enable
     * VK_LUNARG_direct_driver_loading. Its pNext is the caller's
     * VkD3DDDICallbacks, which must outlive vkCreateInstance. */
    VkDirectDriverLoadingListLUNARG loading_list;
} VIRTIO_WDDM_VkBootstrap;

/* hRTDevice/kt are the runtime device and its kernel callbacks, used for the
 * DRM capset escape that picks the ICD. On failure everything is released. */
HRESULT virtio_wddm_vk_bootstrap(VIRTIO_WDDM_VkBootstrap *vk, VIRTIO_WDDM_CapsetMask capsets,
                                 HANDLE hRTAdapter, HANDLE hRTDevice,
                                 const D3DDDI_DEVICECALLBACKS *kt, VkD3DDDICallbacks *callbacks);
void virtio_wddm_vk_bootstrap_release(VIRTIO_WDDM_VkBootstrap *vk);

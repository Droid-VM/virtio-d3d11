#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

#include <winddk_compat.h>
/* d3d10umddi.h defines the D3D10 hardware constants as non-static C objects;
 * the UMD that includes it first owns them. */
#define _D3D10_CONSTANTS
#define _D3D10_1_CONSTANTS
#include <d3d10umddi.h>
#include <d3dumddi.h>

#ifndef ERROR_GRAPHICS_DRIVER_MISMATCH
#define ERROR_GRAPHICS_DRIVER_MISMATCH _HRESULT_TYPEDEF_(0xC0262009L)
#endif

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan_d3dddi.h>
#include <virtio_wddm_uapi.h>
#include <drm_hw.h>

#include "vk_bootstrap.h"

/* Each UMD provides virtio_wddm_log (dx11um: adapter.c). */
void virtio_wddm_log(const char *file, int line, const char *label, const char *format, ...) __attribute__((format(printf, 4, 5)));
#undef ERROR
#define ERROR(...) virtio_wddm_log(__FILE__, __LINE__, "ERROR", __VA_ARGS__)
#define INFO(...) virtio_wddm_log(__FILE__, __LINE__, "INFO", __VA_ARGS__)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

static void dump_capsets(char *buf, size_t len, VIRTIO_WDDM_CapsetMask mask) {
    static const struct {
        VIRTIO_WDDM_CapsetMask value;
        const char *name;
    } mask_table[] = {
        { VIRTIO_WDDM_CAPSET_MASK_VIRGL,        "virgl"        },
        { VIRTIO_WDDM_CAPSET_MASK_VIRGL2,       "virgl2"       },
        { VIRTIO_WDDM_CAPSET_MASK_GFXSTREAM,    "gfxstream"    },
        { VIRTIO_WDDM_CAPSET_MASK_VENUS,        "venus"        },
        { VIRTIO_WDDM_CAPSET_MASK_CROSS_DOMAIN, "cross_domain" },
        { VIRTIO_WDDM_CAPSET_MASK_DRM,          "drm"          },
    };

    buf[0] = 0;

    size_t pos = 0;
    bool first = true;

    for (size_t i = 0; i < ARRAY_SIZE(mask_table); i++) {
        if (!(mask & mask_table[i].value)) continue;
        if (!first) {
            int ret = snprintf(&buf[pos], len - pos, " | ");
            if (ret < 0 || (size_t)ret >= len - pos) {
                break;
            }
            pos += ret;
        }
        first = false;

        int ret = snprintf(&buf[pos], len - pos, "%s", mask_table[i].name);
        if (ret < 0 || (size_t)ret >= len - pos)
            break;
        pos += ret;
    }

    if (first) {
        snprintf(buf, len, "NONE");
    }
}

HRESULT virtio_wddm_query_adapter(const D3DDDI_ADAPTERCALLBACKS *callbacks, HANDLE hRTAdapter,
                                  VIRTIO_WDDM_AdapterCaps *caps) {
    VIRTIO_WDDM_AdapterInfo adapter_info_priv = { 0 };
    D3DDDICB_QUERYADAPTERINFO query_adapter_info = {
        .pPrivateDriverData = &adapter_info_priv,
        .PrivateDriverDataSize = sizeof(adapter_info_priv),
    };

    HRESULT hr = callbacks->pfnQueryAdapterInfoCb(hRTAdapter, &query_adapter_info);
    if (FAILED(hr)) {
        return hr;
    };
    if (adapter_info_priv.tag != VIRTIO_WDDM_ADAPTER_INFO_TAG || !adapter_info_priv.supports_3d || !adapter_info_priv.has_shmem) {
        ERROR("Invalid driver: tag %llu, 3d %u, shmem %u", adapter_info_priv.tag, adapter_info_priv.supports_3d, adapter_info_priv.has_shmem);
        return ERROR_GRAPHICS_DRIVER_MISMATCH;
    }

    char capsets[4096];
    dump_capsets(capsets, sizeof(capsets), adapter_info_priv.capset_mask);
    INFO("%s: Supported capsets: %s", __FUNCTION__, capsets);

    if (!(adapter_info_priv.capset_mask & (VIRTIO_WDDM_CAPSET_MASK_VENUS | VIRTIO_WDDM_CAPSET_MASK_DRM))) {
        INFO("%s: Adapter does not support nor Venus nor vDRM", __FUNCTION__);
        return ERROR_GRAPHICS_DRIVER_MISMATCH;
    }

    static_assert(sizeof(caps->luid) == sizeof(adapter_info_priv.luid));
    memcpy(&caps->luid, &adapter_info_priv.luid, sizeof(caps->luid));
    caps->capsets = adapter_info_priv.capset_mask;
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

static HRESULT get_drm_context_type(HANDLE hRTAdapter, HANDLE hRTDevice, const D3DDDI_DEVICECALLBACKS *kt,
                                    uint32_t *drm_context_type) {
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
        .hDevice = hRTDevice,
        .pPrivateDriverData = &escape_priv,
        .PrivateDriverDataSize = sizeof(escape_priv),
    };

    INFO("%s: Escape DRM capset enter size=%u", __FUNCTION__,
         (unsigned)escape.PrivateDriverDataSize);
    HRESULT hr = kt->pfnEscapeCb(hRTAdapter, &escape);
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

/* Load a DLL from the directory of the UMD itself: the driver store copy of
 * the package, never whatever else is on the search path. */
static HMODULE load_icd(const wchar_t *icd_name) {
    wchar_t umd_path[MAX_PATH];
    HMODULE umd_module = NULL;
#if defined(__i386__)
    /* The signed package contains both 64-bit and WoW dependencies. */
    wchar_t wow_name[MAX_PATH];
    const wchar_t *suffix = wcsrchr(icd_name, L'.');
    if (!suffix || wcscmp(suffix, L".dll") != 0) return NULL;
    size_t stem = suffix - icd_name;
    if (stem + ARRAYSIZE(L"_x86.dll") > ARRAYSIZE(wow_name)) return NULL;
    wmemcpy(wow_name, icd_name, stem);
    wcscpy(wow_name + stem, L"_x86.dll");
    icd_name = wow_name;
#endif

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

void virtio_wddm_vk_bootstrap_release(VIRTIO_WDDM_VkBootstrap *vk) {
    if (vk->vulkan) {
        INFO("%s: unloading Vulkan loader %p", __FUNCTION__, vk->vulkan);
        FreeLibrary(vk->vulkan);
        vk->vulkan = NULL;
    }
    if (vk->icd) {
        INFO("%s: unloading Vulkan ICD %p", __FUNCTION__, vk->icd);
        FreeLibrary(vk->icd);
        vk->icd = NULL;
    }
    memset(vk, 0, sizeof(*vk));
}

static bool have_direct_driver_loading(PFN_vkGetInstanceProcAddr vk_GetInstanceProcAddr) {
    PFN_vkEnumerateInstanceExtensionProperties vk_EnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties) vk_GetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!vk_EnumerateInstanceExtensionProperties)
        return false;

    uint32_t extension_count = 0;
    INFO("%s: enumerating instance extensions (count)", __FUNCTION__);
    VkResult res = vk_EnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
    if (res != VK_SUCCESS) {
        ERROR("Failed to enumerate instance extensions: %d", res);
        return false;
    }

    VkExtensionProperties *extension_props = calloc(sizeof(*extension_props), extension_count);
    if (extension_count && !extension_props)
        return false;
    INFO("%s: enumerating %u instance extensions (properties)", __FUNCTION__, extension_count);
    res = vk_EnumerateInstanceExtensionProperties(NULL, &extension_count, extension_props);
    if (res != VK_SUCCESS) {
        ERROR("Failed to enumerate instance extensions: %d", res);
        free(extension_props);
        return false;
    }

    bool found = false;
    for (size_t i = 0; i < extension_count; i++) {
        if (!strcmp(extension_props[i].extensionName, VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME))
            found = true;
    }
    free(extension_props);
    return found;
}

HRESULT virtio_wddm_vk_bootstrap(VIRTIO_WDDM_VkBootstrap *vk, VIRTIO_WDDM_CapsetMask capsets,
                                 HANDLE hRTAdapter, HANDLE hRTDevice,
                                 const D3DDDI_DEVICECALLBACKS *kt, VkD3DDDICallbacks *callbacks) {
    memset(vk, 0, sizeof(*vk));

    /* Keep the loader in the signed package and in the same ABI as the ICD. */
    vk->vulkan = load_icd(L"droidvm_vulkan.dll");
    if (!vk->vulkan) {
        ERROR("%s: Vulkan loader unavailable: %lu", __FUNCTION__, GetLastError());
        return E_FAIL;
    }
    INFO("%s: loaded droidvm_vulkan.dll=%p", __FUNCTION__, vk->vulkan);

    if (capsets & VIRTIO_WDDM_CAPSET_MASK_DRM) {
        uint32_t drm_context_type = 0;
        HRESULT hr = get_drm_context_type(hRTAdapter, hRTDevice, kt, &drm_context_type);
        if (SUCCEEDED(hr)) {
            INFO("%s: Supported DRM context type: %s", __FUNCTION__, get_drm_context_type_name(drm_context_type));
            const wchar_t *vdrm_icd_name = get_drm_context_type_icd_name(drm_context_type);
            if (vdrm_icd_name != NULL) {
                INFO("%s: Using vDRM Vulkan ICD: %ws", __FUNCTION__, vdrm_icd_name);
                vk->icd = load_icd(vdrm_icd_name);
            }
        }
    }

    if (vk->icd == NULL) {
        vk->icd = load_icd(L"vulkan_virtio.dll");
    }

    /* Missing optional ICDs must fail device creation, not abort the host
     * process (which may be the desktop window manager). */
    if (!vk->icd) {
        ERROR("%s: No compatible Vulkan ICD: %lu", __FUNCTION__, GetLastError());
        virtio_wddm_vk_bootstrap_release(vk);
        return E_FAIL;
    }

    vk->driver_info = (VkDirectDriverLoadingInfoLUNARG) {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG,
        .pNext = NULL,
        .flags = 0,
        .pfnGetInstanceProcAddr = (PFN_vkGetInstanceProcAddrLUNARG) GetProcAddress(vk->icd, "vk_icdGetInstanceProcAddr"),
    };
    if (!vk->driver_info.pfnGetInstanceProcAddr) {
        ERROR("%s: Vulkan ICD lacks vk_icdGetInstanceProcAddr", __FUNCTION__);
        virtio_wddm_vk_bootstrap_release(vk);
        return E_FAIL;
    }

    vk->loading_list = (VkDirectDriverLoadingListLUNARG) {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
        .pNext = callbacks,
        .mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG,
        .driverCount = 1,
        .pDrivers = &vk->driver_info,
    };

    INFO("D3DDDI callbacks: %p, dev %p", callbacks, callbacks->hRTDevice);

    vk->vk_GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) GetProcAddress(vk->vulkan, "vkGetInstanceProcAddr");
    vk->vk_GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) GetProcAddress(vk->vulkan, "vkGetDeviceProcAddr");
    if (!vk->vk_GetInstanceProcAddr || !vk->vk_GetDeviceProcAddr) {
        ERROR("%s: Vulkan loader entry points unavailable: gipa=%p gdpa=%p",
              __FUNCTION__, vk->vk_GetInstanceProcAddr, vk->vk_GetDeviceProcAddr);
        virtio_wddm_vk_bootstrap_release(vk);
        return E_FAIL;
    }

    if (!have_direct_driver_loading(vk->vk_GetInstanceProcAddr)) {
        ERROR("Direct driver loading is not supported, cannot continue");
        virtio_wddm_vk_bootstrap_release(vk);
        return E_FAIL;
    }

    return S_OK;
}

#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef ERROR_GRAPHICS_DRIVER_MISMATCH
#define ERROR_GRAPHICS_DRIVER_MISMATCH _HRESULT_TYPEDEF_(0xC0262009L)
#endif

#include <winddk_compat.h>
#include <d3d10umddi.h>
#include <d3d11.h>
#include <d3dumddi.h>

#include <triton.h>
#include <virtio_wddm_uapi.h>

#undef ERROR
#include "adapter.h"
#include "device.h"

#ifdef NO_DEBUG
extern void triton_log_raw(const char *line) {
}

void virtio_wddm_log(const char *file, int line, const char *label, const char *format, ...) {
}

extern int __cdecl DXVK_umd_log_output(const char *line) {
    return 0;
}
#else
/* File logging is opt-in: set VIRTIO_WDDM_LOG_FILE to a writable path
 * ("1" = C:\dx11um_log.txt).  Off by default: every line is fflush()ed on
 * the caller's thread, and DWM + app appends to one NTFS file have stalled
 * Present for tens of ms.  OutputDebugStringA remains tied to
 * VIRTIO_WDDM_VERBOSE as before. */
extern void print_log_raw(const char *line) {
    static volatile LONG state;   /* 0 = pending, 1 = no file, 2 = file enabled */
    static FILE *out = NULL;

    LONG value = InterlockedCompareExchange(&state, 0, 0);
    if (!value) {
        char path[MAX_PATH] = {0};
        DWORD len = GetEnvironmentVariableA("VIRTIO_WDDM_LOG_FILE", path, sizeof(path));
        FILE *f = NULL;
        if (len > 0 && len < sizeof(path)) {
            if (len == 1 && path[0] == '1')
                strcpy(path, "C:\\dx11um_log.txt");
            if (!(len == 1 && path[0] == '0'))
                f = fopen(path, "a+");
        }
        if (InterlockedCompareExchange(&state, f ? 2 : 1, 0) == 0)
            out = f;
        else if (f)
            fclose(f);
        value = InterlockedCompareExchange(&state, 0, 0);
    }

    if (value == 1 && !tritonVerboseLogEnabled())
        return;

    char tagged[4608];
    int len = snprintf(tagged, sizeof(tagged), "[umd pid=%lu tid=%lu] %s",
                       GetCurrentProcessId(), GetCurrentThreadId(), line);
    if (len < 0)
        return;
    tagged[sizeof(tagged) - 1] = '\0';

    if (tritonVerboseLogEnabled())
        OutputDebugStringA(tagged);

    if (value == 2 && out != NULL) {
        fwrite(tagged, strlen(tagged), 1, out);
        fflush(out);
    }
}

void virtio_wddm_log(const char *file, int line, const char *label, const char *format, ...) {
    char buf[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    size_t len = strlen(buf);
    if (len == sizeof(buf) - 1) {
        buf[sizeof(buf)-2] = '\n';
    } else {
        buf[len] = '\n';
        buf[len+1] = '\0';
    }
    va_end(args);
    print_log_raw(buf);
}

extern int __cdecl DXVK_umd_log_output(const char *line) {
    print_log_raw(line);
    return 0;
}

extern void triton_log_raw(const char *line) {
    print_log_raw(line);
}
#endif

static HRESULT APIENTRY virtio_wddm_close_adapter(D3D10DDI_HADAPTER hAdapter) {
    TRACE();
    free(hAdapter.pDrvPrivate);
    return S_OK;
}

__attribute__((visibility("default"))) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER* pArgs) {
    TRACE();
    INFO("%s: enter interface=0x%x version=0x%x", __FUNCTION__,
         pArgs->Interface, pArgs->Version);
    VIRTIO_WDDM_AdapterCaps adapter_caps;
    HRESULT hr = virtio_wddm_query_adapter(pArgs->pAdapterCallbacks, pArgs->hRTAdapter.handle, &adapter_caps);
    if (FAILED(hr))
        return hr;

    VIRTIO_WDDM_Adapter *adapter = calloc(sizeof(*adapter), 1);
    if (!adapter) return E_OUTOFMEMORY;

    adapter->luid = adapter_caps.luid;
    adapter->supported_capsets = adapter_caps.capsets;

    adapter->base.hRTAdapter = pArgs->hRTAdapter;
    adapter->base.uIfVersion = pArgs->Interface;
    adapter->base.uRtVersion = pArgs->Version;
    adapter->base.KTCallbacks = *pArgs->pAdapterCallbacks;

    pArgs->hAdapter.pDrvPrivate = adapter;
    pArgs->pAdapterFuncs_2->pfnCalcPrivateDeviceSize = virtio_wddm_calc_device_size;
    pArgs->pAdapterFuncs_2->pfnCreateDevice          = virtio_wddm_create_device;
    pArgs->pAdapterFuncs_2->pfnCloseAdapter          = virtio_wddm_close_adapter;
    pArgs->pAdapterFuncs_2->pfnGetSupportedVersions  = tritonGetSupportedVersions;
    // TODO: we could create a temporary DXVK device from LUID and query caps from it.
    // NOTE: this device MUST NOT be used for anything else.
    pArgs->pAdapterFuncs_2->pfnGetCaps               = tritonGetCaps;

    INFO("%s: adapter callbacks installed calc=%p create=%p", __FUNCTION__,
         (void *)pArgs->pAdapterFuncs_2->pfnCalcPrivateDeviceSize,
         (void *)pArgs->pAdapterFuncs_2->pfnCreateDevice);

    return S_OK;
}

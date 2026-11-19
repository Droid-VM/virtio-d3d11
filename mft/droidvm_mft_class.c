/* droidvm_mft_class.c - COM in-proc server plumbing.
 *
 * Export surface (mft_h264_decoder.def):
 *   DllGetClassObject   - MF / CoCreateInstance activates the transform
 *   DllCanUnloadNow     - unload when no transform and no server lock is alive
 *   DllRegisterServer   - COM keys + Media Foundation registration
 *   DllUnregisterServer - removes both
 *
 * The registry side lives in droidvm_mft_reg.c: it writes the COM keys, calls
 * MFTRegister, forces the hardware MFTFlags/category/attributes and mirrors
 * the x86 view into WOW6432Node.  The CLSID/category/friendly name depend on
 * MFT_BUILD_MODE (decoder or encoder), see droidvm_mft.h.
 */
#include "droidvm_mft.h"

#include <stdio.h>
#include <string.h>

static HINSTANCE g_hinst;
static LONG g_objects;
static LONG g_server_locks;

void dvm_mft_addref_object(void)
{
    InterlockedIncrement(&g_objects);
}

void dvm_mft_release_object(void)
{
    InterlockedDecrement(&g_objects);
}

/* ------------------------------------------------------------------------- */
/* Class factory                                                              */
/* ------------------------------------------------------------------------- */

typedef struct DvmFactory {
    const IClassFactoryVtbl *vtbl;
    LONG                     ref;
} DvmFactory;

static HRESULT STDMETHODCALLTYPE fac_QueryInterface(IClassFactory *This, REFIID riid,
                                                    void **ppv);
static ULONG STDMETHODCALLTYPE fac_AddRef(IClassFactory *This);
static ULONG STDMETHODCALLTYPE fac_Release(IClassFactory *This);
static HRESULT STDMETHODCALLTYPE fac_CreateInstance(IClassFactory *This,
                                                     IUnknown *outer, REFIID riid,
                                                     void **ppv);
static HRESULT STDMETHODCALLTYPE fac_LockServer(IClassFactory *This, BOOL lock);

static const IClassFactoryVtbl g_factory_vtbl = {
    fac_QueryInterface,
    fac_AddRef,
    fac_Release,
    fac_CreateInstance,
    fac_LockServer
};

static HRESULT STDMETHODCALLTYPE fac_QueryInterface(IClassFactory *This, REFIID riid,
                                                    void **ppv)
{
    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = (void *)This;
        fac_AddRef(This);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fac_AddRef(IClassFactory *This)
{
    DvmFactory *f = (DvmFactory *)This;

    return (ULONG)InterlockedIncrement(&f->ref);
}

static ULONG STDMETHODCALLTYPE fac_Release(IClassFactory *This)
{
    DvmFactory *f = (DvmFactory *)This;
    LONG ref = InterlockedDecrement(&f->ref);

    if (ref == 0)
        HeapFree(GetProcessHeap(), 0, f);
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE fac_CreateInstance(IClassFactory *This,
                                                     IUnknown *outer, REFIID riid,
                                                     void **ppv)
{
    (void)This;
    if (outer != NULL)
        return CLASS_E_NOAGGREGATION;
    return dvm_mft_create_instance(riid, ppv);
}

static HRESULT STDMETHODCALLTYPE fac_LockServer(IClassFactory *This, BOOL lock)
{
    (void)This;
    if (lock)
        InterlockedIncrement(&g_server_locks);
    else
        InterlockedDecrement(&g_server_locks);
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* DLL entry points                                                           */
/* ------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hinst = inst;
        DisableThreadLibraryCalls(inst);
        break;
    case DLL_PROCESS_DETACH:
        dvm_log_flush_close();
        break;
    default:
        break;
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    DvmFactory *f;

    if (ppv == NULL)
        return E_POINTER;
    *ppv = NULL;
    if (!IsEqualGUID(rclsid, dvm_mft_clsid()))
        return CLASS_E_CLASSNOTAVAILABLE;

    f = (DvmFactory *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
    if (f == NULL)
        return E_OUTOFMEMORY;
    f->vtbl = &g_factory_vtbl;
    f->ref = 1;

    {
        HRESULT hr = fac_QueryInterface((IClassFactory *)f, riid, ppv);
        fac_Release((IClassFactory *)f);
        return hr;
    }
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    if (g_objects == 0 && g_server_locks == 0) {
        dvm_log_flush_close();
        return S_OK;
    }
    return S_FALSE;
}

STDAPI DllRegisterServer(void)
{
    return dvm_mft_register(g_hinst);
}

STDAPI DllUnregisterServer(void)
{
    return dvm_mft_unregister();
}

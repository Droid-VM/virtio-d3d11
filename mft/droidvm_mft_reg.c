/* droidvm_mft_reg.c - COM + Media Foundation registration for both MFT modes.
 *
 * Why this file exists
 * --------------------
 * P-BATCH-MFT registered with
 *
 *     MFTRegister(clsid, category, name, 0, ...)
 *
 * and also put MFT_PROCESS_LOCAL_Attribute=1 into the transform attributes.
 * On the test guest the transform key appeared, CoCreateInstance worked, but
 * MFTEnumEx() never returned it.  A stock Windows 11 install shows what a
 * hardware MFT looks like:
 *
 *     HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\{clsid}
 *         (Default)    = friendly name
 *         InputTypes   REG_BINARY
 *         OutputTypes  REG_BINARY
 *         Attributes   REG_BINARY   ("AFMI" blob)
 *         MFTFlags     REG_DWORD    0x00000004  <- MFT_ENUM_FLAG_HARDWARE
 *     HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\Categories\
 *         {d6c02d4b-...decoder...}\{clsid}
 *
 * AMD's hardware playback decoder has MFTFlags=4; pure software async MFTs
 * have MFTFlags=2; legacy sync MFTs have no MFTFlags value at all.  The
 * hardware Attributes blob carries
 *   MFT_ENUM_HARDWARE_VENDOR_ID_Attribute ("VEN_AMD" / "VEN_QCOM" / ...) and
 *   MFT_ENUM_HARDWARE_URL_Attribute.
 * (Evidence: 05-参考文档/作者原始文档/evidence/gpudriverx2-video-20261001/
 *  qcdx8480.inf.txt lines 287-379 - Qualcomm registers three hardware encoder
 *  MFTs with MFTFlags=4 and the two enumeration attributes.)
 *
 * So this module:
 *   1. removes the stale transform/category keys,
 *   2. writes the COM in-proc server keys,
 *   3. calls MFTRegister with MFT_ENUM_FLAG_HARDWARE (falls back to the old
 *      flags=0 call if this Windows build rejects the flag, then patches the
 *      registry itself),
 *   4. forces MFTFlags=0x4 and the Categories link,
 *   5. mirrors the whole registration into WOW6432Node when the sibling
 *      mft_h264_{decoder,encoder}_x86.dll is present and really is i386,
 *   6. reads everything back and fails the registration if a gate is missing.
 *
 * The x86 view is a *different registry view*, not a forwarding stub: an x86
 * player reads HKLM\SOFTWARE\Classes\WOW6432Node\MediaFoundation\Transforms.
 * ARM64EC processes (x64 emulation) read the native view, so one CLSID cannot
 * point at both a pure ARM64 DLL and an ARM64EC DLL; that is what an ARM64X
 * binary solves.  See 04-测试记录/13-MFT.md section 4.
 */
#include "droidvm_mft.h"

#include <mftransform.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if DVM_MFT_MODE == 0
/* {9F3C2A11-7D64-4E3B-9A21-6C0D5B48E7A2} */
const CLSID CLSID_DroidVmH264DecoderMft = {
    0x9F3C2A11, 0x7D64, 0x4E3B,
    { 0x9A, 0x21, 0x6C, 0x0D, 0x5B, 0x48, 0xE7, 0xA2 }
};
#define DVM_MFT_X86_SIBLING L"mft_h264_decoder_x86.dll"
#elif DVM_MFT_MODE == 2
/* {9F3C2A13-7D64-4E3B-9A21-6C0D5B48E7A4} */
const CLSID CLSID_DroidVmHevcEncoderMft = {
    0x9F3C2A13, 0x7D64, 0x4E3B,
    { 0x9A, 0x21, 0x6C, 0x0D, 0x5B, 0x48, 0xE7, 0xA4 }
};
#define DVM_MFT_X86_SIBLING L"mft_hevc_encoder_x86.dll"
#else
/* {9F3C2A12-7D64-4E3B-9A21-6C0D5B48E7A3} */
const CLSID CLSID_DroidVmH264EncoderMft = {
    0x9F3C2A12, 0x7D64, 0x4E3B,
    { 0x9A, 0x21, 0x6C, 0x0D, 0x5B, 0x48, 0xE7, 0xA3 }
};
#define DVM_MFT_X86_SIBLING L"mft_h264_encoder_x86.dll"
#endif

/* MFT_CATEGORY_VIDEO_DECODER / _ENCODER as raw GUIDs, so the unregister path
 * can remove the link from both categories even if this DLL was rebuilt with
 * the other MFT_BUILD_MODE. */
static const GUID k_cat_video_decoder = {
    0xD6C02D4B, 0x6833, 0x45B4,
    { 0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91 }
};
static const GUID k_cat_video_encoder = {
    0xF79EAC7D, 0xE545, 0x4387,
    { 0xBD, 0xEE, 0xD6, 0x47, 0xD7, 0xBD, 0xE4, 0x2A }
};

#define DVM_MAX_PATH_CHARS 1024

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

const CLSID *dvm_mft_clsid(void)
{
    return &DVM_MFT_ACTIVE_CLSID;
}

static void guid_braced(REFGUID g, WCHAR *out, int cch)
{
    if (out == NULL || cch <= 0)
        return;
    out[0] = 0;
    (void)StringFromGUID2((REFGUID)g, out, cch);
    out[cch - 1] = 0;
}

static void guid_key(REFGUID g, WCHAR *out, size_t cch)
{
    (void)_snwprintf(out, cch,
                     L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     (unsigned)g->Data1, (unsigned)g->Data2,
                     (unsigned)g->Data3,
                     (unsigned)g->Data4[0], (unsigned)g->Data4[1],
                     (unsigned)g->Data4[2], (unsigned)g->Data4[3],
                     (unsigned)g->Data4[4], (unsigned)g->Data4[5],
                     (unsigned)g->Data4[6], (unsigned)g->Data4[7]);
    if (cch != 0)
        out[cch - 1] = 0;
}

static LONG reg_create(HKEY root, LPCWSTR subkey, REGSAM sam, HKEY *out)
{
    DWORD disp = 0;

    return RegCreateKeyExW(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                           sam, NULL, out, &disp);
}

static LONG reg_set_sz(HKEY root, LPCWSTR subkey, LPCWSTR value, LPCWSTR data)
{
    HKEY key = NULL;
    LONG rc;
    DWORD bytes;

    rc = reg_create(root, subkey, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS)
        return rc;
    bytes = (DWORD)((wcslen(data) + 1) * sizeof(WCHAR));
    rc = RegSetValueExW(key, value, 0, REG_SZ, (const BYTE *)data, bytes);
    RegCloseKey(key);
    return rc;
}

static LONG reg_set_dword(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD data)
{
    HKEY key = NULL;
    LONG rc;

    rc = reg_create(root, subkey, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS)
        return rc;
    rc = RegSetValueExW(key, value, 0, REG_DWORD, (const BYTE *)&data,
                        (DWORD)sizeof(data));
    RegCloseKey(key);
    return rc;
}

static LONG reg_get_dword(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD *data)
{
    HKEY key = NULL;
    LONG rc;
    DWORD type = 0;
    DWORD size = (DWORD)sizeof(*data);

    rc = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS)
        return rc;
    rc = RegQueryValueExW(key, value, NULL, &type, (BYTE *)data, &size);
    RegCloseKey(key);
    if (rc == ERROR_SUCCESS && (type != REG_DWORD || size != sizeof(*data)))
        return ERROR_INVALID_DATA;
    return rc;
}

static LONG reg_get_binary(HKEY root, LPCWSTR subkey, LPCWSTR value,
                           BYTE **data, DWORD *size)
{
    HKEY key = NULL;
    LONG rc;
    DWORD type = 0;
    DWORD bytes = 0;

    *data = NULL;
    *size = 0;
    rc = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS)
        return rc;
    rc = RegQueryValueExW(key, value, NULL, &type, NULL, &bytes);
    if (rc != ERROR_SUCCESS || bytes == 0) {
        RegCloseKey(key);
        return (rc == ERROR_SUCCESS) ? ERROR_INVALID_DATA : rc;
    }
    *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (*data == NULL) {
        RegCloseKey(key);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    rc = RegQueryValueExW(key, value, NULL, &type, *data, &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_BINARY) {
        HeapFree(GetProcessHeap(), 0, *data);
        *data = NULL;
        return (rc == ERROR_SUCCESS) ? ERROR_INVALID_DATA : rc;
    }
    *size = bytes;
    return ERROR_SUCCESS;
}

static int reg_key_exists(HKEY root, LPCWSTR subkey)
{
    HKEY key = NULL;
    LONG rc = RegOpenKeyExW(root, subkey, 0, KEY_READ, &key);

    if (rc != ERROR_SUCCESS)
        return 0;
    RegCloseKey(key);
    return 1;
}

static int blob_has(const BYTE *blob, DWORD len, const void *needle, size_t nlen)
{
    DWORD i;

    if (blob == NULL || needle == NULL || nlen == 0 || len < nlen)
        return 0;
    for (i = 0; i + nlen <= len; i++) {
        if (memcmp(blob + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static int blob_has_wstring(const BYTE *blob, DWORD len, LPCWSTR s)
{
    return blob_has(blob, len, s, (wcslen(s) + 1) * sizeof(WCHAR));
}

/* ------------------------------------------------------------------------- */
/* PE machine check for the x86 sibling                                       */
/* ------------------------------------------------------------------------- */

#if defined(_WIN64)
static WORD pe_machine_of(const WCHAR *path)
{
    HANDLE file;
    IMAGE_DOS_HEADER dos;
    IMAGE_FILE_HEADER fh;
    DWORD signature = 0;
    DWORD got = 0;
    LARGE_INTEGER off;

    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(file, &dos, (DWORD)sizeof(dos), &got, NULL) ||
        got != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(file);
        return 0;
    }
    off.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file, off, NULL, FILE_BEGIN) ||
        !ReadFile(file, &signature, (DWORD)sizeof(signature), &got, NULL) ||
        got != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
        !ReadFile(file, &fh, (DWORD)sizeof(fh), &got, NULL) ||
        got != sizeof(fh)) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return fh.Machine;
}

static void sibling_path(const WCHAR *module, LPCWSTR name, WCHAR *out,
                         size_t cch)
{
    const WCHAR *slash = wcsrchr(module, L'\\');
    size_t dir_len = (slash != NULL) ? (size_t)(slash - module + 1) : 0;

    if (dir_len + wcslen(name) + 1 > cch)
        dir_len = 0;
    if (dir_len != 0)
        memcpy(out, module, dir_len * sizeof(WCHAR));
    out[dir_len] = 0;
    wcsncat(out, name, cch - wcslen(out) - 1);
}
#endif /* _WIN64 */

/* ------------------------------------------------------------------------- */
/* Registry paths                                                             */
/* ------------------------------------------------------------------------- */

static void transform_path(WCHAR *out, size_t cch, BOOL wow64,
                           const WCHAR *clsid_key)
{
    (void)_snwprintf(out, cch,
                     wow64
                         ? L"SOFTWARE\\Classes\\WOW6432Node\\MediaFoundation\\Transforms\\%ls"
                         : L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\%ls",
                     clsid_key);
    out[cch - 1] = 0;
}

static void category_path(WCHAR *out, size_t cch, BOOL wow64,
                          const WCHAR *cat_key, const WCHAR *clsid_key)
{
    (void)_snwprintf(out, cch,
                     wow64
                         ? L"SOFTWARE\\Classes\\WOW6432Node\\MediaFoundation\\Transforms\\Categories\\%ls\\%ls"
                         : L"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\%ls\\%ls",
                     cat_key, clsid_key);
    out[cch - 1] = 0;
}

static void clsid_path(WCHAR *out, size_t cch, BOOL wow64,
                       const WCHAR *clsid_braced, const WCHAR *suffix)
{
    (void)_snwprintf(out, cch,
                     wow64
                         ? L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\%ls%ls"
                         : L"SOFTWARE\\Classes\\CLSID\\%ls%ls",
                     clsid_braced, suffix != NULL ? suffix : L"");
    out[cch - 1] = 0;
}

/* ------------------------------------------------------------------------- */
/* COM keys                                                                   */
/* ------------------------------------------------------------------------- */

static LONG register_com(BOOL wow64, const WCHAR *clsid_braced,
                         const WCHAR *module_path)
{
    WCHAR key[512];
    LONG rc;

    clsid_path(key, 512, wow64, clsid_braced, NULL);
    rc = reg_set_sz(HKEY_LOCAL_MACHINE, key, NULL, DVM_MFT_FRIENDLY_NAME);
    if (rc != ERROR_SUCCESS)
        return rc;
    clsid_path(key, 512, wow64, clsid_braced, L"\\InprocServer32");
    rc = reg_set_sz(HKEY_LOCAL_MACHINE, key, NULL, module_path);
    if (rc != ERROR_SUCCESS)
        return rc;
    return reg_set_sz(HKEY_LOCAL_MACHINE, key, L"ThreadingModel", L"Both");
}

/* ------------------------------------------------------------------------- */
/* WOW6432Node mirror                                                         */
/* ------------------------------------------------------------------------- */

#if defined(_WIN64)
static LONG copy_transform_values(BOOL wow64, const WCHAR *src_sub,
                                  const WCHAR *dst_sub)
{
    HKEY src = NULL, dst = NULL;
    LONG rc;
    DWORD index;

    (void)wow64;
    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, src_sub, 0, KEY_READ, &src);
    if (rc != ERROR_SUCCESS)
        return rc;
    rc = reg_create(HKEY_LOCAL_MACHINE, dst_sub, KEY_SET_VALUE, &dst);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(src);
        return rc;
    }

    for (index = 0;; index++) {
        WCHAR name[256];
        DWORD name_len = (DWORD)(sizeof(name) / sizeof(name[0]));
        DWORD type = 0;
        DWORD size = 0;

        rc = RegEnumValueW(src, index, name, &name_len, NULL, &type, NULL, &size);
        if (rc == ERROR_NO_MORE_ITEMS) {
            rc = ERROR_SUCCESS;
            break;
        }
        if (rc != ERROR_SUCCESS)
            break;
        if (size != 0) {
            BYTE *value = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
            DWORD got = size;

            if (value == NULL) {
                rc = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
            rc = RegQueryValueExW(src, name, NULL, &type, value, &got);
            if (rc == ERROR_SUCCESS)
                rc = RegSetValueExW(dst, name, 0, type, value, got);
            HeapFree(GetProcessHeap(), 0, value);
            if (rc != ERROR_SUCCESS)
                break;
        } else {
            rc = RegSetValueExW(dst, name, 0, type, NULL, 0);
            if (rc != ERROR_SUCCESS)
                break;
        }
    }

    RegCloseKey(src);
    RegCloseKey(dst);
    return rc;
}
#endif /* _WIN64 */

/* Register the x86 sibling in the 32-bit registry view so a 32-bit player can
 * enumerate and activate its own copy.  No-op on a 32-bit build (that build's
 * HKLM\SOFTWARE\Classes already *is* WOW6432Node) and when the sibling is not
 * shipped. */
static void mirror_x86(const WCHAR *module_path, const WCHAR *transform,
                       const WCHAR *cat_key, const WCHAR *clsid_key,
                       const WCHAR *clsid_braced)
{
#if defined(_WIN64)
    WCHAR x86_path[DVM_MAX_PATH_CHARS];
    WCHAR wow_transform[512];
    WCHAR wow_category[640];
    WORD machine;
    LONG rc;

    sibling_path(module_path, DVM_MFT_X86_SIBLING, x86_path,
                 sizeof(x86_path) / sizeof(x86_path[0]));
    machine = pe_machine_of(x86_path);
    if (machine != IMAGE_FILE_MACHINE_I386) {
        dvm_logf("WOW6432Node: no usable x86 sibling (%ls, machine=0x%04x); "
                 "run the x86 DLL's regsvr32 separately",
                 x86_path, (unsigned)machine);
        return;
    }

    transform_path(wow_transform, 512, TRUE, clsid_key);
    category_path(wow_category, 640, TRUE, cat_key, clsid_key);

    rc = register_com(TRUE, clsid_braced, x86_path);
    if (rc == ERROR_SUCCESS)
        rc = copy_transform_values(TRUE, transform, wow_transform);
    if (rc == ERROR_SUCCESS)
        rc = reg_set_dword(HKEY_LOCAL_MACHINE, wow_transform, L"MFTFlags",
                           DVM_MFT_ENUM_FLAGS);
    if (rc == ERROR_SUCCESS) {
        HKEY cat = NULL;

        rc = reg_create(HKEY_LOCAL_MACHINE, wow_category, KEY_WRITE, &cat);
        if (cat != NULL)
            RegCloseKey(cat);
    }
    dvm_logf("WOW6432Node[%s]: x86=%ls rc=%ld", DVM_MFT_MODENAME, x86_path,
             (long)rc);
#else
    (void)module_path;
    (void)transform;
    (void)cat_key;
    (void)clsid_key;
    (void)clsid_braced;
#endif
}

/* ------------------------------------------------------------------------- */
/* Read-back verification                                                     */
/* ------------------------------------------------------------------------- */

static int verify_view(BOOL wow64, const WCHAR *transform,
                       const WCHAR *category, const char *what)
{
    DWORD flags = 0;
    BYTE *blob = NULL;
    DWORD blob_len = 0;
    GUID vendor = MFT_ENUM_HARDWARE_VENDOR_ID_Attribute;
    GUID url = MFT_ENUM_HARDWARE_URL_Attribute;
    int ok = 1;

    (void)wow64;

    if (reg_get_dword(HKEY_LOCAL_MACHINE, transform, L"MFTFlags", &flags) !=
            ERROR_SUCCESS ||
        flags != DVM_MFT_ENUM_FLAGS) {
        dvm_logf("REGCHECK[%s/%s]: MFTFlags=%lu (want 0x%lx)", what,
                 DVM_MFT_MODENAME, (unsigned long)flags,
                 (unsigned long)DVM_MFT_ENUM_FLAGS);
        ok = 0;
    }
    if (reg_get_binary(HKEY_LOCAL_MACHINE, transform, L"Attributes", &blob,
                       &blob_len) != ERROR_SUCCESS) {
        dvm_logf("REGCHECK[%s/%s]: no Attributes blob", what, DVM_MFT_MODENAME);
        ok = 0;
    } else {
        if (blob_len < 4 || memcmp(blob, "AFMI", 4) != 0) {
            dvm_logf("REGCHECK[%s/%s]: Attributes is not an AFMI blob (%lu B)",
                     what, DVM_MFT_MODENAME, (unsigned long)blob_len);
            ok = 0;
        }
        if (!blob_has(blob, blob_len, &vendor, sizeof(vendor))) {
            dvm_logf("REGCHECK[%s/%s]: MFT_ENUM_HARDWARE_VENDOR_ID missing",
                     what, DVM_MFT_MODENAME);
            ok = 0;
        }
        if (!blob_has(blob, blob_len, &url, sizeof(url))) {
            dvm_logf("REGCHECK[%s/%s]: MFT_ENUM_HARDWARE_URL missing", what,
                     DVM_MFT_MODENAME);
            ok = 0;
        }
        if (!blob_has_wstring(blob, blob_len, DVM_MFT_HW_VENDOR_ID)) {
            dvm_logf("REGCHECK[%s/%s]: vendor string %ls missing", what,
                     DVM_MFT_MODENAME, DVM_MFT_HW_VENDOR_ID);
            ok = 0;
        }
        HeapFree(GetProcessHeap(), 0, blob);
    }
    if (!reg_key_exists(HKEY_LOCAL_MACHINE, category)) {
        dvm_logf("REGCHECK[%s/%s]: category link missing", what,
                 DVM_MFT_MODENAME);
        ok = 0;
    }
    dvm_logf("REGCHECK[%s/%s]: %s", what, DVM_MFT_MODENAME,
             ok ? "PASS" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Register / unregister                                                      */
/* ------------------------------------------------------------------------- */

HRESULT dvm_mft_register(HINSTANCE inst)
{
    WCHAR module_path[DVM_MAX_PATH_CHARS];
    WCHAR clsid_braced[64];
    WCHAR clsid_key[64];
    WCHAR cat_key[64];
    WCHAR transform[512];
    WCHAR category[640];
    const CLSID *clsid = dvm_mft_clsid();
    IMFAttributes *attrs = NULL;
    MFT_REGISTER_TYPE_INFO in_types[2];
    MFT_REGISTER_TYPE_INFO out_types[1];
    UINT32 in_count = 0, out_count = 0;
    HRESULT hr;
    LONG rc;
    int verified;

    {
        DWORD len = GetModuleFileNameW(inst, module_path,
                                       (DWORD)(sizeof(module_path) /
                                               sizeof(WCHAR)));
        if (len == 0 || len >= sizeof(module_path) / sizeof(WCHAR))
            return HRESULT_FROM_WIN32(GetLastError());
    }

    guid_braced(clsid, clsid_braced, 64);
    guid_key(clsid, clsid_key, 64);
    guid_key(&DVM_MFT_CATEGORY, cat_key, 64);
    transform_path(transform, 512, FALSE, clsid_key);
    category_path(category, 640, FALSE, cat_key, clsid_key);

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
        return hr;

    /* Remove whatever an older build (or the other mode) left behind.  The
     * transform key is the one that carries the wrong MFTFlags/process-local
     * attributes, so it must go, not just be overwritten. */
    (void)MFTUnregister((CLSID)*clsid);
    (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, transform);
    {
        WCHAR other_cat[640];
        WCHAR other_key[64];

        guid_key(&k_cat_video_decoder, other_key, 64);
        category_path(other_cat, 640, FALSE, other_key, clsid_key);
        (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, other_cat);
        guid_key(&k_cat_video_encoder, other_key, 64);
        category_path(other_cat, 640, FALSE, other_key, clsid_key);
        (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, other_cat);
    }

    rc = register_com(FALSE, clsid_braced, module_path);
    if (rc != ERROR_SUCCESS) {
        dvm_logf("DllRegisterServer[%s]: COM keys failed rc=%ld",
                 DVM_MFT_MODENAME, (long)rc);
        MFShutdown();
        return HRESULT_FROM_WIN32(rc);
    }

    memset(in_types, 0, sizeof(in_types));
    memset(out_types, 0, sizeof(out_types));
#if DVM_MFT_MODE == 0
    in_types[0].guidMajorType = MFMediaType_Video;
    in_types[0].guidSubtype = MFVideoFormat_H264;
    in_types[1].guidMajorType = MFMediaType_Video;
    in_types[1].guidSubtype = MFVideoFormat_H264_ES;
    in_count = 2;
    out_types[0].guidMajorType = MFMediaType_Video;
    out_types[0].guidSubtype = MFVideoFormat_NV12;
    out_count = 1;
#else
    in_types[0].guidMajorType = MFMediaType_Video;
    in_types[0].guidSubtype = MFVideoFormat_NV12;
    in_count = 1;
    out_types[0].guidMajorType = MFMediaType_Video;
    out_types[0].guidSubtype = DVM_MFT_CODED_SUBTYPE;
    out_count = 1;
#endif

    hr = MFCreateAttributes(&attrs, 8);
    if (FAILED(hr)) {
        MFShutdown();
        return hr;
    }
    (void)IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC, 1);
    (void)IMFAttributes_SetUINT32(attrs, &MFT_CODEC_MERIT_Attribute,
                                  DVM_MFT_MERIT);
    (void)IMFAttributes_SetString(attrs, &MFT_FRIENDLY_NAME_Attribute,
                                  DVM_MFT_FRIENDLY_NAME);
    (void)IMFAttributes_SetGUID(attrs, &MFT_TRANSFORM_CLSID_Attribute,
                                clsid);
    (void)IMFAttributes_SetString(attrs, &MFT_ENUM_HARDWARE_VENDOR_ID_Attribute,
                                  DVM_MFT_HW_VENDOR_ID);
    (void)IMFAttributes_SetString(attrs, &MFT_ENUM_HARDWARE_URL_Attribute,
                                  DVM_MFT_HW_URL);
    /* Deliberately NOT set:
     *   MFT_PROCESS_LOCAL_Attribute  - that attribute belongs to
     *       MFTRegisterLocal (per-process MFTs) and hides the transform from a
     *       normal MFTEnumEx call.  P-BATCH set it to 1 by mistake.
     *   MF_SA_D3D11_AWARE            - only meaningful once the MFT accepts
     *       DXGI surfaces; this decoder hands out system-memory NV12. */

    hr = MFTRegister((CLSID)*clsid, (GUID)DVM_MFT_CATEGORY,
                     (LPWSTR)DVM_MFT_FRIENDLY_NAME, DVM_MFT_ENUM_FLAGS,
                     in_count, in_types, out_count, out_types, attrs);
    if (hr == E_INVALIDARG) {
        /* Some builds validate the flags argument against the V1/V2 software
         * values only.  Register with the historic flags=0 call and patch
         * MFTFlags ourselves below; MFTEnumEx reads the registry. */
        dvm_logf("DllRegisterServer[%s]: MFTRegister(HARDWARE) -> "
                 "E_INVALIDARG, retrying flags=0", DVM_MFT_MODENAME);
        (void)MFTUnregister((CLSID)*clsid);
        (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, transform);
        hr = MFTRegister((CLSID)*clsid, (GUID)DVM_MFT_CATEGORY,
                         (LPWSTR)DVM_MFT_FRIENDLY_NAME, 0,
                         in_count, in_types, out_count, out_types, attrs);
    }
    IMFAttributes_Release(attrs);

    if (FAILED(hr)) {
        dvm_logf("DllRegisterServer[%s]: MFTRegister failed hr=0x%08lX",
                 DVM_MFT_MODENAME, (unsigned long)hr);
        (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, transform);
        MFShutdown();
        return hr;
    }

    /* The two gates MFTRegister does not reliably write for us. */
    rc = reg_set_dword(HKEY_LOCAL_MACHINE, transform, L"MFTFlags",
                       DVM_MFT_ENUM_FLAGS);
    if (rc == ERROR_SUCCESS) {
        HKEY cat = NULL;

        rc = reg_create(HKEY_LOCAL_MACHINE, category, KEY_WRITE, &cat);
        if (cat != NULL)
            RegCloseKey(cat);
    }
    if (rc != ERROR_SUCCESS) {
        dvm_logf("DllRegisterServer[%s]: MFTFlags/category failed rc=%ld",
                 DVM_MFT_MODENAME, (long)rc);
        MFShutdown();
        return HRESULT_FROM_WIN32(rc);
    }

    mirror_x86(module_path, transform, cat_key, clsid_key, clsid_braced);

    verified = verify_view(FALSE, transform, category, "native");
#if defined(_WIN64)
    if (verified) {
        /* Verify the mirrored view only if it was actually written. */
        WCHAR wow_transform[512];
        WCHAR wow_category[640];

        transform_path(wow_transform, 512, TRUE, clsid_key);
        category_path(wow_category, 640, TRUE, cat_key, clsid_key);
        if (reg_key_exists(HKEY_LOCAL_MACHINE, wow_transform)) {
            if (!verify_view(TRUE, wow_transform, wow_category, "wow64"))
                verified = 0;
        }
    }
#endif

    dvm_logf("DllRegisterServer[%s]: %s (clsid=%ls, MFTFlags=0x%lx, "
             "in=%lu out=%lu, dll=%ls)",
             DVM_MFT_MODENAME, verified ? "ok" : "FAILED",
             clsid_braced, (unsigned long)DVM_MFT_ENUM_FLAGS,
             (unsigned long)in_count, (unsigned long)out_count, module_path);

    MFShutdown();
    return verified ? S_OK : E_FAIL;
}

HRESULT dvm_mft_unregister(void)
{
    const CLSID *clsid = dvm_mft_clsid();
    WCHAR clsid_braced[64];
    WCHAR clsid_key[64];
    WCHAR key[640];
    WCHAR cat_key[64];
    const GUID *cats[2];
    int i;
    BOOL wow;

    guid_braced(clsid, clsid_braced, 64);
    guid_key(clsid, clsid_key, 64);
    cats[0] = &k_cat_video_decoder;
    cats[1] = &k_cat_video_encoder;

    (void)MFStartup(MF_VERSION, MFSTARTUP_LITE);
    (void)MFTUnregister((CLSID)*clsid);

    for (wow = FALSE; wow <= TRUE; wow++) {
#if !defined(_WIN64)
        if (wow)
            break;
#endif
        transform_path(key, 640, wow, clsid_key);
        (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
        for (i = 0; i < 2; i++) {
            guid_key(cats[i], cat_key, 64);
            category_path(key, 640, wow, cat_key, clsid_key);
            (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
        }
        clsid_path(key, 640, wow, clsid_braced, NULL);
        (void)RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
    }
    MFShutdown();
    dvm_logf("DllUnregisterServer[%s]: done (clsid=%ls)", DVM_MFT_MODENAME,
             clsid_braced);
    return S_OK;
}

#ifndef __VIRTIO_WDDM_UAPI_H__
#define __VIRTIO_WDDM_UAPI_H__

#include <stdint.h>
#include <d3dkmthk.h>

/* Optional guest-allocated BO capability. Old KMDs reject this escape. */
#define VIRTIO_WDDM_ESCAPE_GUEST_ALLOC_CAPS_TAG 5782977369871697238ull
typedef struct VIRTIO_WDDM_GuestAllocCaps {
   uint64_t tag;
   uint32_t supported;
   uint32_t alignment;
} VIRTIO_WDDM_GuestAllocCaps;
#define VIRTIO_WDDM_BLOB_FLAG_CREATE_GUEST_HANDLE 8u

/* ------------------------------------------------------------------------
 * Runtime-allocated blob hook (D3D11 shared resources)
 *
 * A blob the vdrm transport creates itself (raw D3DKMTCreateAllocation2 on the
 * transport's own device) can never become a D3D11 shared resource: the kernel
 * resource handle an application needs for IDXGIResource::GetSharedHandle is
 * minted by the Direct3D runtime inside pfnAllocateCb, on the runtime's device.
 * So for shared textures the allocation has to come from the UMD's callback,
 * while the blob's identity (blob_id) and size must still come from the ICD --
 * it is the only side that knows the image's memory requirements, and the host
 * requires GEM_NEW and RESOURCE_CREATE_BLOB to agree exactly on size.
 *
 * The hook inverts the usual direction so both constraints hold at once: the
 * ICD decides what to allocate, the UMD performs the allocation. Only plain C
 * types cross the boundary -- the d3dumddi.h callback types stay in the UMD,
 * which is the only side that can include that header.
 * ------------------------------------------------------------------------ */

typedef struct VIRTIO_WDDM_RuntimeAllocRequest {
    /* Chosen by the ICD; must match the later RESOURCE_CREATE_BLOB exactly. */
    uint64_t blob_id;
    uint64_t size;
    uint32_t mem;               /* VIRTIO_WDDM_BlobMem */
    uint32_t flags;             /* VIRTIO_WDDM_BlobFlag */
    /* D3D11 runtime resource this allocation belongs to, i.e. the hRTResource
     * the UMD received in CreateResource. Passing it back is what lets the
     * runtime associate the allocation with the resource and return a kernel
     * resource handle. NULL means "no D3D resource": the UMD must then not set
     * hResource, and hKMResource comes back 0. */
    void *hRTResource;
    D3DKMT_HANDLE sourceAllocation; /* Existing blob on the ICD device. */
} VIRTIO_WDDM_RuntimeAllocRequest;

typedef struct VIRTIO_WDDM_RuntimeAllocResult {
    D3DKMT_HANDLE hAllocation;
    D3DKMT_HANDLE hKMResource;  /* 0 when hRTResource was NULL */
} VIRTIO_WDDM_RuntimeAllocResult;

/* Both return 0 on success and a negative errno on failure. The free hook takes
 * the same handles the alloc hook produced; it must tolerate hKMResource == 0. */
typedef int (*VIRTIO_WDDM_RuntimeAllocFn)(void *ctx,
                                          const VIRTIO_WDDM_RuntimeAllocRequest *req,
                                          VIRTIO_WDDM_RuntimeAllocResult *out);
typedef int (*VIRTIO_WDDM_RuntimeFreeFn)(void *ctx,
                                         const VIRTIO_WDDM_RuntimeAllocResult *alloc);

typedef struct VIRTIO_WDDM_RuntimeAllocator {
    void *ctx;
    VIRTIO_WDDM_RuntimeAllocFn alloc;
    VIRTIO_WDDM_RuntimeFreeFn free;
} VIRTIO_WDDM_RuntimeAllocator;

#define VIRTIO_WDDM_PCI_VENDOR_ID 6900

#define VIRTIO_WDDM_PCI_DEVICE_ID 26985

#define VIRTIO_WDDM_ADAPTER_INFO_TAG 5066067239879590230ull

#define VIRTIO_WDDM_ESCAPE_CAPSET_TAG 6003370060433016150ull

#define VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG 5066067248504063318ull

#define VIRTIO_WDDM_ESCAPE_RESOURCE_BUSY_TAG 6004778599252706646ull

#define VIRTIO_WDDM_ESCAPE_BLOB_INFO_SET_TAG 4778121577144468822ull

#define VIRTIO_WDDM_ESCAPE_BLOB_MAP_TAG 4778391004737914198ull

#define VIRTIO_WDDM_ESCAPE_CONTEXT_INIT_TAG 5285066810588349782ull

#define VIRTIO_WDDM_ESCAPE_BLIT_INIT_TAG 6073469419966907734ull

#define VIRTIO_WDDM_ESCAPE_EXEC_BUF_TAG 4847377628439725398ull

#define VIRTIO_WDDM_CREATE_RESOURCE_TAG 5932161353946775894ull

#define VIRTIO_WDDM_ALLOCATE_3D_TAG 5496436857343066454ull

#define VIRTIO_WDDM_ALLOCATE_BLOB_TAG 4778121577295003990ull

#define VIRTIO_WDDM_SUBMIT_COMMAND_VIRTUAL_TAG 6216178336949162838ull

#define VIRTIO_WDDM_MAX_SUBMIT_COMMAND_VIRTUAL_SIZE 8192

enum VIRTIO_WDDM_CapsetId
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
    VIRTIO_WDDM_CAPSET_ID_VIRGL = 1,
    VIRTIO_WDDM_CAPSET_ID_VIRGL2 = 2,
    VIRTIO_WDDM_CAPSET_ID_GFXSTREAM = 3,
    VIRTIO_WDDM_CAPSET_ID_VENUS = 4,
    VIRTIO_WDDM_CAPSET_ID_CROSS_DOMAIN = 5,
    VIRTIO_WDDM_CAPSET_ID_DRM = 6,
};
#if __STDC_VERSION__ >= 202311L
typedef enum VIRTIO_WDDM_CapsetId VIRTIO_WDDM_CapsetId;
#else
typedef uint32_t VIRTIO_WDDM_CapsetId;
#endif // __STDC_VERSION__ >= 202311L

enum VIRTIO_WDDM_CommandId
#if __STDC_VERSION__ >= 202311L
  : uint16_t
#endif // __STDC_VERSION__ >= 202311L
 {
    VIRTIO_WDDM_COMMAND_ID_NOP = 0,
    VIRTIO_WDDM_COMMAND_ID_SUBMIT = 1,
    VIRTIO_WDDM_COMMAND_ID_TRANSFER_TO_HOST = 2,
    VIRTIO_WDDM_COMMAND_ID_TRANSFER_FROM_HOST = 3,
    VIRTIO_WDDM_COMMAND_ID_FENCE = 4,
    VIRTIO_WDDM_COMMAND_ID_MAP_APERTURE = 61440,
    VIRTIO_WDDM_COMMAND_ID_UNMAP_APERTURE = 61441,
};
#if __STDC_VERSION__ >= 202311L
typedef enum VIRTIO_WDDM_CommandId VIRTIO_WDDM_CommandId;
#else
typedef uint16_t VIRTIO_WDDM_CommandId;
#endif // __STDC_VERSION__ >= 202311L

typedef uint64_t VIRTIO_WDDM_CapsetMask;
#define VIRTIO_WDDM_CAPSET_MASK_VIRGL (uint64_t)(1ull << 1)
#define VIRTIO_WDDM_CAPSET_MASK_VIRGL2 (uint64_t)(1ull << 2)
#define VIRTIO_WDDM_CAPSET_MASK_GFXSTREAM (uint64_t)(1ull << 3)
#define VIRTIO_WDDM_CAPSET_MASK_VENUS (uint64_t)(1ull << 4)
#define VIRTIO_WDDM_CAPSET_MASK_CROSS_DOMAIN (uint64_t)(1ull << 5)
#define VIRTIO_WDDM_CAPSET_MASK_DRM (uint64_t)(1ull << 6)

typedef struct __attribute__((packed)) {
    uint64_t tag;
    uint64_t luid;
    VIRTIO_WDDM_CapsetMask capset_mask;
    bool supports_3d;
    bool has_shmem;
} VIRTIO_WDDM_AdapterInfo;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint64_t size;
} VIRTIO_WDDM_Allocate3d;

typedef uint32_t VIRTIO_WDDM_BlobMem;
#define VIRTIO_WDDM_BLOB_MEM_GUEST (uint32_t)(1u << 0)
#define VIRTIO_WDDM_BLOB_MEM_HOST3D (uint32_t)(1u << 1)
#define VIRTIO_WDDM_BLOB_MEM_HOST3D_GUEST (uint32_t)((1u << 0) | (1u << 1))

typedef uint32_t VIRTIO_WDDM_BlobFlag;
#define VIRTIO_WDDM_BLOB_FLAG_NONE (uint32_t)0
#define VIRTIO_WDDM_BLOB_FLAG_MAPPABLE (uint32_t)(1u << 0)
#define VIRTIO_WDDM_BLOB_FLAG_SHAREABLE (uint32_t)(1u << 1)
#define VIRTIO_WDDM_BLOB_FLAG_CROSS_DEVICE (uint32_t)(1u << 2)

typedef struct __attribute__((packed)) {
    uint64_t tag;
    uint64_t id;
    VIRTIO_WDDM_BlobMem mem;
    VIRTIO_WDDM_BlobFlag flags;
    uint64_t size;
} VIRTIO_WDDM_AllocateBlob;

typedef union __attribute__((packed)) {
    uint64_t tag;
    VIRTIO_WDDM_Allocate3d _3d;
    VIRTIO_WDDM_AllocateBlob blob;
} VIRTIO_WDDM_CreateAllocation;

#define VIRTIO_WDDM_REUSE_BLOB_TAG UINT64_C(0x3145535545524242)
typedef struct __attribute__((packed)) {
    VIRTIO_WDDM_CreateAllocation allocation;
    uint64_t reuse_tag;
    D3DKMT_HANDLE source;
} VIRTIO_WDDM_ReuseBlobAllocation;

#define VIRTIO_WDDM_PRIMARY_ALLOCATION_TAG UINT64_C(0x315952414d495250)
typedef struct __attribute__((packed)) {
    VIRTIO_WDDM_ReuseBlobAllocation base;
    uint64_t primary_tag;
    uint32_t refresh_numerator;
    uint32_t refresh_denominator;
    uint32_t vidpn_source;
} VIRTIO_WDDM_PrimaryAllocation;


typedef struct __attribute__((packed)) {
    uint64_t tag;
    uint8_t cmd[0];
} VIRTIO_WDDM_CreateResource;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    VIRTIO_WDDM_CapsetId capset_id;
    uint32_t version;
    uint8_t capset[0];
} VIRTIO_WDDM_Capset;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    VIRTIO_WDDM_CapsetId capset_id;
    uint32_t num_rings;
    uint8_t debug_name[64];
} VIRTIO_WDDM_ContextInit;

typedef struct __attribute__((packed)) {
    VIRTIO_WDDM_Allocate3d _3d;
    uint64_t modifier;
    uint64_t offsets[4];
    uint32_t strides[4];
    uint32_t sizes[4];
    uint32_t num_planes;
} VIRTIO_WDDM_Allocate3dFull;

typedef struct __attribute__((packed)) {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t bind;
    uint64_t modifier;
    uint32_t strides[4];
    uint32_t offsets[4];
} VIRTIO_WDDM_BlobInfo;

typedef struct __attribute__((packed)) {
    VIRTIO_WDDM_AllocateBlob blob;
    VIRTIO_WDDM_BlobInfo info;
    bool info_valid;
    bool created;
} VIRTIO_WDDM_AllocateBlobFull;

typedef union __attribute__((packed)) {
    uint64_t tag;
    VIRTIO_WDDM_Allocate3dFull _3d;
    VIRTIO_WDDM_AllocateBlobFull blob;
} VIRTIO_WDDM_AllocationInfo;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    D3DKMT_HANDLE handle;
    uint32_t id;
    VIRTIO_WDDM_AllocationInfo info;
} VIRTIO_WDDM_ResourceInfo;

typedef union {
    HANDLE handle;
    uint64_t wow64;
} VIRTIO_WDDM_HANDLE64;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    VIRTIO_WDDM_HANDLE64 event;
    D3DKMT_HANDLE handle;
    bool wait;
    bool is_busy;
} VIRTIO_WDDM_ResourceBusy;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    D3DKMT_HANDLE handle;
    uint32_t _padding;
    VIRTIO_WDDM_BlobInfo blob_info;
} VIRTIO_WDDM_BlobInfoSet;

typedef uint32_t VIRTIO_WDDM_BlobMapFlags;
#define VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP (uint32_t)(1u << 0)

typedef union {
    uint8_t *ptr;
    uint64_t wow64;
} VIRTIO_WDDM_Pointer64;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    D3DKMT_HANDLE handle;
    VIRTIO_WDDM_BlobMapFlags flags;
    VIRTIO_WDDM_Pointer64 ptr;
} VIRTIO_WDDM_BlobMap;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    uint64_t fence_id;
    uint8_t cmd[0];
} VIRTIO_WDDM_ExecBuffer;

typedef union __attribute__((packed)) {
    uint64_t tag;
    VIRTIO_WDDM_Capset caps_req;
    VIRTIO_WDDM_ContextInit ctx_init;
    VIRTIO_WDDM_ResourceInfo res_info;
    VIRTIO_WDDM_ResourceBusy res_busy;
    VIRTIO_WDDM_BlobInfoSet blob_set;
    VIRTIO_WDDM_BlobMap blob_map;
    VIRTIO_WDDM_ExecBuffer exec_buf;
} VIRTIO_WDDM_Escape;

typedef uint8_t VIRTIO_WDDM_CommandFlag;
#define VIRTIO_WDDM_COMMAND_FLAG_RING_IDX (uint8_t)(1 << 0)
#define VIRTIO_WDDM_COMMAND_FLAG_SHADOW_VIRGL (uint8_t)(1 << 1)

typedef struct __attribute__((packed)) {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} VIRTIO_WDDM_Box3D;

typedef struct __attribute__((packed)) {
    uint32_t res_id;
    uint32_t stride;
    uint64_t offset;
    uint32_t level;
    uint32_t layer_stride;
    VIRTIO_WDDM_Box3D box;
} VIRTIO_WDDM_CommandTransfer;

typedef union __attribute__((packed)) {
    uint8_t submit[0];
    VIRTIO_WDDM_CommandTransfer transfer[0];
} VIRTIO_WDDM_Commands;

typedef struct __attribute__((packed)) {
    VIRTIO_WDDM_CommandId id;
    VIRTIO_WDDM_CommandFlag flags;
    uint8_t ring;
    uint32_t size;
    VIRTIO_WDDM_Commands body[0];
} VIRTIO_WDDM_CommandHeader;

typedef struct __attribute__((packed)) {
    uint64_t tag;
    uint8_t cmd[0];
} VIRTIO_WDDM_SubmitCommand;

#endif  /* __VIRTIO_WDDM_UAPI_H__ */

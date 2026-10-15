/* SPDX-License-Identifier: MIT
 * Private image pNext ABI. Keep the UMD and Turnip copies identical.
 * Old ICDs ignore create hints; imports must never fall back to CPU-visible memory.
 */
#ifndef VULKAN_HOST_SCANOUT_H
#define VULKAN_HOST_SCANOUT_H
#include <stdint.h>
#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#define VK_STRUCTURE_TYPE_D3DDDI_HOST_SCANOUT ((VkStructureType)4281808699u)
typedef struct VkD3DDDIHostScanout {
   VkStructureType sType;
   const void *pNext;
   uint32_t version;
   uint32_t import_backing;
   uint64_t backing_size;
} VkD3DDDIHostScanout;

static inline bool
d3dddi_host_scanout_image_eligible(const VkImageCreateInfo *info, uint64_t modifier)
{
   return info->imageType == VK_IMAGE_TYPE_2D &&
          info->format == VK_FORMAT_B8G8R8A8_UNORM &&
          info->extent.depth == 1 && info->mipLevels == 1 &&
          info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT &&
          (info->tiling == VK_IMAGE_TILING_LINEAR ||
           (info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && modifier == 0)) &&
          !(info->flags & ~(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)) &&
          !(info->usage & (VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT));
}

static inline uint32_t
d3dddi_host_scanout_type_bits(uint32_t type_count, int32_t scanout_type, bool selected)
{
   uint32_t bits = (1u << type_count) - 1;
   if (scanout_type >= 0) {
      bits &= ~(1u << scanout_type);
      if (selected)
         bits = 1u << scanout_type;
   }
   return bits;
}
#endif

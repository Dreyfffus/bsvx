#pragma once
#ifdef BSVX_VULKAN
#include <vulkan/vulkan.h>
#include "btx_header.h"
#define BTX_VULKAN_IMPL
#endif

#ifdef BTX_VULKAN_IMPL

namespace bsvx::btx {

    inline VkImageType to_vk_image_type(TextureKind kind);

    inline VkImageViewType to_vk_image_view_type(TextureKind kind);

    inline VkImageCreateInfo make_image_create_info(const TextureDesc& tex);

    inline VkImageViewCreateInfo make_image_view_create_info(VkImage image, const TextureDesc& tex, VkImageAspectFlags aspect_mask);

    inline VkBufferImageCopy2 make_buffer_image_copy_2d_array(const SubresourceDesc& sub, VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT);

    // Returns all copy regions for one texture, assuming Texture2DArray.
    inline std::vector<VkBufferImageCopy2> build_copy_regions_for_texture(const Archive& archive, uint32_t texture_id, VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT);

}

#endif

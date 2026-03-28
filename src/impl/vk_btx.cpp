#ifdef BTX_VULKAN_IMPL
#include "vk_btx.h"
namespace bsvx::btx {
    VkImageType to_vk_image_type(TextureKind kind)
    {
        switch (kind) {
        case TextureKind::TEXTURE_2D:        return VK_IMAGE_TYPE_2D;
        case TextureKind::TEXTURE_3D:        return VK_IMAGE_TYPE_3D;
        case TextureKind::TEXEL_BUFFER:      return VK_IMAGE_TYPE_2D; // not used here
        default:                return VK_IMAGE_TYPE_2D;
        }
    }

    VkImageViewType to_vk_image_view_type(TextureKind kind)
    {
        switch (kind) {
        case TextureKind::TEXTURE_2D:        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureKind::TEXTURE_3D:        return VK_IMAGE_VIEW_TYPE_3D;
        default:                return VK_IMAGE_VIEW_TYPE_2D;
        }
    }

    VkImageCreateInfo make_image_create_info(const TextureDesc& tex)
    {
        if (tex.kind == TextureKind::TEXEL_BUFFER) throw std::runtime_error("[btx]: texel buffers are not VkImage objects");

        VkImageCreateInfo ci{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = to_vk_image_type(tex.kind);
        ci.format = static_cast<VkFormat>(tex.vk_format);
        ci.extent.width = tex.width;
        ci.extent.height = tex.height;
        ci.extent.depth = (tex.kind == TextureKind::TEXTURE_3D) ? tex.depth : 1;
        ci.mipLevels = tex.mip_levels;
        ci.arrayLayers = (tex.kind == TextureKind::TEXTURE_3D) ? 1u : tex.array_layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = tex.usage_flags;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return ci;
    }

    VkImageViewCreateInfo make_image_view_create_info(VkImage image, const TextureDesc& tex, VkImageAspectFlags aspect_mask)
    {
        VkImageViewCreateInfo ci{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ci.image = image;
        ci.viewType = to_vk_image_view_type(tex.kind);
        ci.format = static_cast<VkFormat>(tex.vk_format);
        ci.subresourceRange.aspectMask = aspect_mask;
        ci.subresourceRange.baseMipLevel = 0;
        ci.subresourceRange.levelCount = tex.mip_levels;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount = (tex.kind == TextureKind::TEXTURE_3D) ? 1u : tex.array_layers;
        return ci;
    }

    VkBufferImageCopy2 make_buffer_image_copy_2d_array(const SubresourceDesc& sub, VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT)
    {
        VkBufferImageCopy2 copy{ .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
        copy.bufferOffset = sub.blob_offset;
        copy.bufferRowLength = sub.packed_row_length;
        copy.bufferImageHeight = sub.packed_image_height;

        copy.imageSubresource.aspectMask = aspect_mask;
        copy.imageSubresource.mipLevel = sub.mip_level;
        copy.imageSubresource.baseArrayLayer = sub.layer_or_slice;
        copy.imageSubresource.layerCount = 1;

        copy.imageOffset = { 0, 0, 0 };
        copy.imageExtent = { sub.extent_x, sub.extent_y, 1 };
        return copy;
    }

    // Returns all copy regions for one texture, assuming Texture2DArray.
    std::vector<VkBufferImageCopy2> build_copy_regions_for_texture(const Archive& archive, uint32_t texture_id, VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT)
    {
        if (texture_id >= archive.textures.size()) throw std::runtime_error("[btx]: invalid texture_id");

        const TextureDesc& tex = archive.textures[texture_id];
        if (tex.kind != TextureKind::TEXTURE_2D) throw std::runtime_error("[btx]: first version Vulkan copy builder only supports Texture2DArray");

        std::vector<VkBufferImageCopy2> copies;
        for (const SubresourceDesc& sub : archive.subresources) {
            if (sub.texture_id == texture_id) {
                copies.push_back(make_buffer_image_copy_2d_array(sub, aspect_mask));
            }
        }

        std::sort(copies.begin(), copies.end(),
            [](const VkBufferImageCopy2& a, const VkBufferImageCopy2& b) {
                if (a.imageSubresource.mipLevel != b.imageSubresource.mipLevel)
                    return a.imageSubresource.mipLevel < b.imageSubresource.mipLevel;
                return a.imageSubresource.baseArrayLayer < b.imageSubresource.baseArrayLayer;
            });

        return copies;
    }
}

#endif
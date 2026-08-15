#pragma once
#include "btx_header.h"
#include <definitions.h>
#include <optional>
#include <util.h>

namespace bsvx::btx {

// Raw VkFormat values, so the container stays Vulkan-native without depending on Vulkan headers.
// Only the formats listed here can be written or sized; everything else is rejected rather than
// stored as bytes nothing can interpret.
enum : uint32_t {
  VK_FORMAT_R8_UNORM_ = 9,
  VK_FORMAT_R8G8_UNORM_ = 16,
  VK_FORMAT_R8G8B8A8_UNORM_ = 37,
  VK_FORMAT_R8G8B8A8_SRGB_ = 43,
  VK_FORMAT_B8G8R8A8_UNORM_ = 44,
  VK_FORMAT_B8G8R8A8_SRGB_ = 50,
  VK_FORMAT_R16_SFLOAT_ = 76,
  VK_FORMAT_R16G16B16A16_SFLOAT_ = 97,
  VK_FORMAT_R32_SFLOAT_ = 100,
  VK_FORMAT_R32G32B32A32_SFLOAT_ = 109,
  VK_FORMAT_BC1_RGB_UNORM_BLOCK_ = 131,
  VK_FORMAT_BC1_RGB_SRGB_BLOCK_ = 132,
  VK_FORMAT_BC1_RGBA_UNORM_BLOCK_ = 133,
  VK_FORMAT_BC1_RGBA_SRGB_BLOCK_ = 134,
  VK_FORMAT_BC3_UNORM_BLOCK_ = 137,
  VK_FORMAT_BC3_SRGB_BLOCK_ = 138,
  VK_FORMAT_BC4_UNORM_BLOCK_ = 139,
  VK_FORMAT_BC5_UNORM_BLOCK_ = 141,
  VK_FORMAT_BC7_UNORM_BLOCK_ = 145,
  VK_FORMAT_BC7_SRGB_BLOCK_ = 146,
};

// 0 for block-compressed formats, which have no per-texel size -- use subresource_size instead.
uint32_t bytes_per_texel(uint32_t vk_format);

bool is_block_compressed(uint32_t vk_format);
// Edge length of a compression block (4 for every BCn format), or 1 for uncompressed.
uint32_t format_block_extent(uint32_t vk_format);
// Bytes one block occupies; for an uncompressed format this equals bytes_per_texel.
uint32_t format_block_size(uint32_t vk_format);
bool format_is_supported(uint32_t vk_format);

// Bytes one mip/layer of the given extent occupies. Block formats round the extent up to a whole
// number of blocks, which is why a per-texel size is not enough on its own.
uint64_t subresource_size(uint32_t vk_format, uint32_t width, uint32_t height);

std::pair<uint16_t, uint16_t> mip_extent_2d(uint16_t base_w, uint16_t base_h, uint16_t mip);

class Archive final {
public:
  std::vector<SamplerDesc> samplers;
  std::vector<TextureDesc> textures;
  std::vector<SubresourceDesc> subresources;
  std::vector<MaterialDesc> materials;
  std::vector<std::byte> blob;

  BSVX_NODISCARD uint32_t add_sampler(SamplerDesc desc) noexcept;
  BSVX_NODISCARD uint32_t add_texture(TextureDesc desc) noexcept;
  BSVX_NODISCARD uint32_t add_material(MaterialDesc desc) noexcept;
  BSVX_NODISCARD uint32_t append_subresource(uint32_t texture_id, uint16_t mip_level, uint16_t layer, uint16_t width, uint16_t height,
                                             std::span<const std::byte> texels, uint16_t packed_row_length = 0, uint16_t packed_image_height = 0);
  BSVX_NODISCARD uint32_t append_rgba8_layer(uint32_t texture_id, uint16_t mip_level, uint16_t layer, std::span<const std::byte> texels);
  // Box-filters every mip above 0 for one texture, for each array layer that already has a level-0
  // subresource. Existing higher mips for that texture are replaced.
  void generate_mips(uint32_t texture_id);
  // atomic writes through a temp file and renames onto the target.
  bool save_to_file(const std::string &path, bool atomic = true, bool backup = false) const;
  static std::optional<Archive> load_from_file(const std::string &path);
  // Same checks as load_from_file, for bytes that never came from the native filesystem.
  static Archive load_from_memory(std::span<const std::byte> bytes);
  std::vector<std::byte> serialize_to_bytes() const;
  std::vector<ValidationError> validate() const;

private:
  void serialize(std::ostream &os) const;
  static Archive deserialize(std::istream &is);
};
} // namespace bsvx::btx

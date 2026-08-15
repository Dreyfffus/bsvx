#pragma once
#include "btx_header.h"
#include <definitions.h>
#include <optional>
#include <util.h>

namespace bsvx::btx {

uint32_t bytes_per_texel(uint32_t vk_format);

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
  bool save_to_file(const std::string &path) const;
  static std::optional<Archive> load_from_file(const std::string &path);
  // Same checks as load_from_file, for bytes that never came from the native filesystem.
  static Archive load_from_memory(std::span<const std::byte> bytes);
  std::vector<ValidationError> validate() const;

private:
  void serialize(std::ostream &os) const;
  static Archive deserialize(std::istream &is);
};
} // namespace bsvx::btx

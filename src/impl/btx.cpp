#include "btx.h"
#include <algorithm>
#include <sstream>

namespace bsvx::btx {

uint32_t bytes_per_texel(uint32_t vk_format) {
  switch (vk_format) {
  case VK_FORMAT_R8_UNORM_:
    return 1;
  case VK_FORMAT_R8G8_UNORM_:
  case VK_FORMAT_R16_SFLOAT_:
    return 2;
  case VK_FORMAT_R8G8B8A8_UNORM_:
  case VK_FORMAT_R8G8B8A8_SRGB_:
  case VK_FORMAT_B8G8R8A8_UNORM_:
  case VK_FORMAT_B8G8R8A8_SRGB_:
  case VK_FORMAT_R32_SFLOAT_:
    return 4;
  case VK_FORMAT_R16G16B16A16_SFLOAT_:
    return 8;
  case VK_FORMAT_R32G32B32A32_SFLOAT_:
    return 16;
  default:
    // Block-compressed formats land here too: they have no meaningful per-texel size.
    return 0;
  }
}

bool is_block_compressed(uint32_t vk_format) {
  switch (vk_format) {
  case VK_FORMAT_BC1_RGB_UNORM_BLOCK_:
  case VK_FORMAT_BC1_RGB_SRGB_BLOCK_:
  case VK_FORMAT_BC1_RGBA_UNORM_BLOCK_:
  case VK_FORMAT_BC1_RGBA_SRGB_BLOCK_:
  case VK_FORMAT_BC3_UNORM_BLOCK_:
  case VK_FORMAT_BC3_SRGB_BLOCK_:
  case VK_FORMAT_BC4_UNORM_BLOCK_:
  case VK_FORMAT_BC5_UNORM_BLOCK_:
  case VK_FORMAT_BC7_UNORM_BLOCK_:
  case VK_FORMAT_BC7_SRGB_BLOCK_:
    return true;
  default:
    return false;
  }
}

uint32_t format_block_extent(uint32_t vk_format) { return is_block_compressed(vk_format) ? 4u : 1u; }

uint32_t format_block_size(uint32_t vk_format) {
  switch (vk_format) {
  // 64 bits per 4x4 block.
  case VK_FORMAT_BC1_RGB_UNORM_BLOCK_:
  case VK_FORMAT_BC1_RGB_SRGB_BLOCK_:
  case VK_FORMAT_BC1_RGBA_UNORM_BLOCK_:
  case VK_FORMAT_BC1_RGBA_SRGB_BLOCK_:
  case VK_FORMAT_BC4_UNORM_BLOCK_:
    return 8;
  // 128 bits per 4x4 block.
  case VK_FORMAT_BC3_UNORM_BLOCK_:
  case VK_FORMAT_BC3_SRGB_BLOCK_:
  case VK_FORMAT_BC5_UNORM_BLOCK_:
  case VK_FORMAT_BC7_UNORM_BLOCK_:
  case VK_FORMAT_BC7_SRGB_BLOCK_:
    return 16;
  default:
    return bytes_per_texel(vk_format);
  }
}

bool format_is_supported(uint32_t vk_format) { return bytes_per_texel(vk_format) != 0u || is_block_compressed(vk_format); }

uint64_t subresource_size(uint32_t vk_format, uint32_t width, uint32_t height) {
  const uint32_t block = format_block_extent(vk_format);
  const uint32_t block_size = format_block_size(vk_format);
  if (block_size == 0u)
    return 0u;

  // A 5x5 BC7 image is 2x2 blocks, not 1.25x1.25 -- round up, never down.
  const uint64_t blocks_x = (static_cast<uint64_t>(width) + block - 1u) / block;
  const uint64_t blocks_y = (static_cast<uint64_t>(height) + block - 1u) / block;
  return blocks_x * blocks_y * block_size;
}

std::pair<uint16_t, uint16_t> mip_extent_2d(uint16_t base_w, uint16_t base_h, uint16_t mip) {
  const uint16_t w = static_cast<uint16_t>(std::max(1u, static_cast<unsigned>(base_w) >> mip));
  const uint16_t h = static_cast<uint16_t>(std::max(1u, static_cast<unsigned>(base_h) >> mip));
  return {w, h};
}

BSVX_NODISCARD uint32_t Archive::add_sampler(SamplerDesc desc) noexcept {
  desc.sampler_id = static_cast<uint32_t>(samplers.size());
  samplers.push_back(desc);
  return desc.sampler_id;
}

BSVX_NODISCARD uint32_t Archive::add_texture(TextureDesc desc) noexcept {
  desc.texture_id = static_cast<uint32_t>(textures.size());
  textures.push_back(desc);
  return desc.texture_id;
}

BSVX_NODISCARD uint32_t Archive::add_material(MaterialDesc desc) noexcept {
  desc.material_id = static_cast<uint32_t>(materials.size());
  materials.push_back(desc);
  return desc.material_id;
}

BSVX_NODISCARD uint32_t Archive::append_subresource(uint32_t texture_id, uint16_t mip_level, uint16_t layer, uint16_t width, uint16_t height,
                                                    std::span<const std::byte> texels, uint16_t packed_row_length, uint16_t packed_image_height) {
  if (texture_id >= textures.size())
    throw std::runtime_error("[btx]: invalid texture_id");
  const TextureDesc &tex = textures[texture_id];
  if (tex.kind != TextureKind::TEXTURE_2D)
    throw std::runtime_error("[btx]: first version append_subresource only supports 2D Textures");
  if (mip_level >= tex.mip_levels)
    throw std::runtime_error("[btx]: mip out of range");
  if (layer >= tex.array_layers)
    throw std::runtime_error("[btx]: layer out of range");

  if (!format_is_supported(tex.vk_format))
    throw std::runtime_error("[btx]: unsupported vk_format");

  uint64_t expected = 0;
  if (is_block_compressed(tex.vk_format)) {
    // bufferRowLength / bufferImageHeight are texel counts even for block formats, so they still
    // describe the padded extent -- the size just comes out of the block arithmetic.
    const uint32_t row_width = packed_row_length != 0 ? packed_row_length : width;
    const uint32_t image_height = packed_image_height != 0 ? packed_image_height : height;
    expected = subresource_size(tex.vk_format, row_width, image_height);
  } else {
    const uint64_t bbp = bytes_per_texel(tex.vk_format);
    const uint64_t row_width = packed_row_length != 0 ? packed_row_length : width;
    const uint64_t image_height = packed_image_height != 0 ? packed_image_height : height;
    expected = row_width * image_height * bbp;
  }

  if (texels.size() != expected) {
    throw std::runtime_error("[btx]: texel payload size does not match format / extents (expected " + std::to_string(expected) + " bytes, got " +
                             std::to_string(texels.size()) + ")");
  }

  const uint64_t blob_offset = static_cast<uint64_t>(blob.size());
  blob.insert(blob.end(), texels.begin(), texels.end());

  SubresourceDesc sub{};
  sub.texture_id = texture_id;
  sub.mip_level = mip_level;
  sub.layer_or_slice = layer;
  sub.extent_x = width;
  sub.extent_y = height;
  sub.extent_z = 1;
  sub.packed_row_length = packed_row_length;
  sub.packed_image_height = packed_image_height;
  sub.blob_offset = blob_offset;
  sub.blob_size = static_cast<uint64_t>(texels.size());
  sub.checksum = fnv1a64(texels);

  subresources.push_back(sub);
  return static_cast<uint32_t>(subresources.size() - 1);
}

BSVX_NODISCARD uint32_t Archive::append_rgba8_layer(uint32_t texture_id, uint16_t mip_level, uint16_t layer, std::span<const std::byte> texels) {
  if (texture_id >= textures.size())
    throw std::runtime_error("[btx]: invalid texture_id");
  const TextureDesc &tex = textures[texture_id];
  auto [w, h] = mip_extent_2d(tex.width, tex.height, mip_level);

  return append_subresource(texture_id, mip_level, layer, w, h, texels, 0, 0);
}

std::vector<ValidationError> Archive::validate() const {
  std::vector<ValidationError> errors;
  for (const TextureDesc &tex : textures) {
    if (tex.width == 0 || tex.height == 0)
      errors.push_back({"texture has zero width or height"});
    if (tex.mip_levels == 0)
      errors.push_back({"texture has 0 mip_levels"});
    if (tex.kind == TextureKind::TEXTURE_2D && tex.array_layers == 0)
      errors.push_back({"2D texture has 0 array layers"});
    if (tex.kind == TextureKind::TEXTURE_3D && tex.array_layers != 1)
      errors.push_back({"3D textures must have 1 array layer in this format"});
    if (!format_is_supported(tex.vk_format) && tex.kind != TextureKind::TEXEL_BUFFER)
      errors.push_back({"unsupported vk_format in current implementation"});
  }

  for (const SubresourceDesc &sub : subresources) {
    if (sub.texture_id >= textures.size()) {
      errors.push_back({"subresource references invalid texture_id"});
      continue;
    }

    const TextureDesc &tex = textures[sub.texture_id];

    if (sub.mip_level >= tex.mip_levels)
      errors.push_back({"subresource mip out of range"});
    if (tex.kind == TextureKind::TEXTURE_2D && sub.layer_or_slice >= tex.array_layers)
      errors.push_back({"subresource layer out of range"});
    if (sub.blob_offset + sub.blob_size > blob.size())
      errors.push_back({"subresource blob range is out of bounds"});
    if (sub.extent_x == 0 || sub.extent_y == 0 || sub.extent_z == 0)
      errors.push_back({"subresource has zero extent"});
  }

  for (const MaterialDesc &mat : materials) {
    auto check_tex = [&](uint32_t id, const char *name) {
      if (id != 0 && id >= textures.size())
        errors.push_back({std::string("material references invalid texture_id: ") + name});
    };

    check_tex(mat.albedo_texture_id, "albedo");
    check_tex(mat.normal_texture_id, "normal");
    check_tex(mat.orm_texture_id, "orm");
    check_tex(mat.emissive_texture_id, "emissive");
  }

  return errors;
}

void Archive::serialize(std::ostream &os) const {
  const std::vector<std::byte> bytes = serialize_to_bytes();
  if (!bytes.empty()) {
    os.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!os)
      throw std::runtime_error("[btx]: failed writing archive");
  }
}

std::vector<std::byte> Archive::serialize_to_bytes() const {
  const auto errs = validate();
  if (!errs.empty())
    throw std::runtime_error("[btx]: serialize called on invalid archive");

  // Build a deterministic subresource ordering grouped by texture, then mip, then layer.
  std::vector<uint32_t> sorted_indices(subresources.size());
  for (uint32_t i = 0; i < sorted_indices.size(); ++i) {
    sorted_indices[i] = i;
  }

  std::sort(sorted_indices.begin(), sorted_indices.end(), [&](uint32_t a, uint32_t b) {
    const auto &lhs = subresources[a];
    const auto &rhs = subresources[b];
    if (lhs.texture_id != rhs.texture_id)
      return lhs.texture_id < rhs.texture_id;
    if (lhs.mip_level != rhs.mip_level)
      return lhs.mip_level < rhs.mip_level;
    return lhs.layer_or_slice < rhs.layer_or_slice;
  });

  std::vector<DiskTextureDesc> disk_textures(textures.size());
  for (size_t i = 0; i < textures.size(); ++i) {
    const TextureDesc &src = textures[i];
    auto &dst = disk_textures[i];
    dst.texture_id = src.texture_id;
    dst.kind = static_cast<uint8_t>(src.kind);
    dst.vk_format = src.vk_format;
    dst.usage_flags = src.usage_flags;
    dst.width = src.width;
    dst.height = src.height;
    dst.depth = src.depth;
    dst.array_layers = src.array_layers;
    dst.mip_levels = src.mip_levels;
    dst.sampler_id = src.sampler_id;
    dst.name_hash = src.name_hash;
    dst.source_hash = src.source_hash;
    dst.first_subresource = 0;
    dst.subresource_count = 0;
  }

  std::vector<SubresourceDesc> ordered_subresources;
  ordered_subresources.reserve(subresources.size());

  for (uint32_t idx : sorted_indices) {
    ordered_subresources.push_back(subresources[idx]);
  }

  for (uint32_t i = 0; i < ordered_subresources.size(); ++i) {
    const uint32_t tex_id = ordered_subresources[i].texture_id;
    auto &dtex = disk_textures[tex_id];

    if (dtex.subresource_count == 0) {
      dtex.first_subresource = i;
    }
    dtex.subresource_count++;
  }

  DiskHeader header{};
  header.sampler_count = static_cast<uint32_t>(samplers.size());
  header.texture_count = static_cast<uint32_t>(disk_textures.size());
  header.subresource_count = static_cast<uint32_t>(ordered_subresources.size());
  header.material_count = static_cast<uint32_t>(materials.size());

  uint64_t cursor = sizeof(DiskHeader);
  cursor = align64(cursor, 16);
  header.sampler_table_offset = cursor;
  cursor += samplers.size() * sizeof(SamplerDesc);

  cursor = align64(cursor, 16);
  header.texture_table_offset = cursor;
  cursor += disk_textures.size() * sizeof(DiskTextureDesc);

  cursor = align64(cursor, 16);
  header.subresource_table_offset = cursor;
  cursor += ordered_subresources.size() * sizeof(SubresourceDesc);

  cursor = align64(cursor, 16);
  header.material_table_offset = cursor;
  cursor += materials.size() * sizeof(MaterialDesc);

  cursor = align64(cursor, 16);
  header.blob_offset = cursor;
  header.blob_size = blob.size();
  header.crc64 = fnv1a64(std::span(blob.data(), blob.size()));
  cursor += blob.size();

  // Built as one zero-filled image rather than written through a seeking stream: the alignment
  // padding then has a defined value, and the same code path can serve an in-memory save.
  std::vector<std::byte> bytes(static_cast<size_t>(cursor), std::byte{0});
  std::memcpy(bytes.data(), &header, sizeof(header));

  const auto put_table = [&](uint64_t offset, const auto &table) {
    if (!table.empty())
      std::memcpy(bytes.data() + offset, table.data(), table.size() * sizeof(typename std::decay_t<decltype(table)>::value_type));
  };

  put_table(header.sampler_table_offset, samplers);
  put_table(header.texture_table_offset, disk_textures);
  put_table(header.subresource_table_offset, ordered_subresources);
  put_table(header.material_table_offset, materials);

  if (!blob.empty())
    std::memcpy(bytes.data() + header.blob_offset, blob.data(), blob.size());

  return bytes;
}

Archive Archive::deserialize(std::istream &is) {
  DiskHeader header{};
  read_pod(is, header);

  if (header.magic != BTX_MAGIC) {
    throw std::runtime_error("[btx]: black magic, unsupported file type");
  }
  if (header.version != BTX_VERSION) {
    throw std::runtime_error("[btx]: unsupported version");
  }
  if (header.endian != 0) {
    throw std::runtime_error("[btx]: only little-endian archives are supported");
  }

  Archive out;

  seek_abs(is, header.sampler_table_offset);
  read_raw(is, out.samplers, header.sampler_count);

  seek_abs(is, header.texture_table_offset);
  std::vector<DiskTextureDesc> disk_textures;
  read_raw(is, disk_textures, header.texture_count);

  out.textures.resize(disk_textures.size());
  for (size_t i = 0; i < disk_textures.size(); ++i) {
    const auto &src = disk_textures[i];
    auto &dst = out.textures[i];

    dst.texture_id = src.texture_id;
    dst.kind = static_cast<TextureKind>(src.kind);
    dst.vk_format = src.vk_format;
    dst.usage_flags = src.usage_flags;
    dst.width = src.width;
    dst.height = src.height;
    dst.depth = src.depth;
    dst.array_layers = src.array_layers;
    dst.mip_levels = src.mip_levels;
    dst.sampler_id = src.sampler_id;
    dst.name_hash = src.name_hash;
    dst.source_hash = src.source_hash;
  }

  seek_abs(is, header.subresource_table_offset);
  read_raw(is, out.subresources, header.subresource_count);

  seek_abs(is, header.material_table_offset);
  read_raw(is, out.materials, header.material_count);

  seek_abs(is, header.blob_offset);
  out.blob.resize(static_cast<size_t>(header.blob_size));
  if (!out.blob.empty()) {
    is.read(reinterpret_cast<char *>(out.blob.data()), static_cast<std::streamsize>(out.blob.size()));
    if (!is)
      throw std::runtime_error("[btx]: failed reading blob");
  }

  if (fnv1a64(std::span(out.blob.data(), out.blob.size())) != header.crc64)
    throw std::runtime_error("[btx]: blob CRC/hash mismatch");
  const auto errs = out.validate();
  if (!errs.empty())
    throw std::runtime_error("[btx]: archive is structurally invalid after load");

  return out;
}

bool Archive::save_to_file(const std::string &path, bool atomic, bool backup) const {
  // Paths crossing this API are UTF-8; std::filesystem must not decode them in the active code page.
  const std::filesystem::path target = path_from_utf8(path);
  const std::vector<std::byte> bytes = serialize_to_bytes();
  try {
    if (atomic)
      write_file_atomic(target, std::span<const std::byte>(bytes.data(), bytes.size()), backup);
    else
      write_file_direct(target, std::span<const std::byte>(bytes.data(), bytes.size()));
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

void Archive::generate_mips(uint32_t texture_id) {
  if (texture_id >= textures.size())
    throw std::runtime_error("[btx]: invalid texture_id");

  const TextureDesc tex = textures[texture_id];
  if (tex.kind != TextureKind::TEXTURE_2D)
    throw std::runtime_error("[btx]: generate_mips only supports 2D textures");
  // Validate before the "nothing to do" shortcut: asking to generate mips for a format that can
  // never have them is a mistake worth reporting whatever mip_levels happens to be.
  //
  // Downsampling a BCn image means decoding, filtering and re-encoding it -- a compressor, which
  // this library is not. Compressed mips have to be supplied by whatever produced the blocks.
  const uint32_t bpp = bytes_per_texel(tex.vk_format);
  if (bpp == 0)
    throw std::runtime_error(is_block_compressed(tex.vk_format) ? "[btx]: cannot generate mips for a block-compressed format; supply them yourself"
                                                                : "[btx]: unsupported format for mip generation");
  // The box filter below averages bytes, which is only meaningful for 8-bit channels.
  if (tex.vk_format == VK_FORMAT_R16_SFLOAT_ || tex.vk_format == VK_FORMAT_R16G16B16A16_SFLOAT_ || tex.vk_format == VK_FORMAT_R32_SFLOAT_ ||
      tex.vk_format == VK_FORMAT_R32G32B32A32_SFLOAT_)
    throw std::runtime_error("[btx]: mip generation only supports 8-bit-per-channel formats");

  if (tex.mip_levels <= 1)
    return;

  // Anything above level 0 is regenerated, so drop what is there rather than ending up with two
  // subresources claiming the same (texture, mip, layer).
  std::erase_if(subresources, [&](const SubresourceDesc &sub) { return sub.texture_id == texture_id && sub.mip_level > 0; });

  for (uint16_t layer = 0; layer < tex.array_layers; ++layer) {
    const auto base = std::find_if(subresources.begin(), subresources.end(), [&](const SubresourceDesc &sub) {
      return sub.texture_id == texture_id && sub.mip_level == 0 && sub.layer_or_slice == layer;
    });
    if (base == subresources.end())
      continue;

    auto [prev_w, prev_h] = mip_extent_2d(tex.width, tex.height, 0);
    std::vector<std::byte> prev(blob.begin() + static_cast<ptrdiff_t>(base->blob_offset),
                                blob.begin() + static_cast<ptrdiff_t>(base->blob_offset + base->blob_size));

    for (uint16_t mip = 1; mip < tex.mip_levels; ++mip) {
      auto [w, h] = mip_extent_2d(tex.width, tex.height, mip);
      std::vector<std::byte> next(static_cast<size_t>(w) * h * bpp);

      // 2x2 box filter, clamped at the edges so odd extents keep their last row/column.
      for (uint16_t y = 0; y < h; ++y) {
        for (uint16_t x = 0; x < w; ++x) {
          const uint32_t x0 = std::min<uint32_t>(static_cast<uint32_t>(x) * 2u, prev_w - 1u);
          const uint32_t y0 = std::min<uint32_t>(static_cast<uint32_t>(y) * 2u, prev_h - 1u);
          const uint32_t x1 = std::min<uint32_t>(x0 + 1u, prev_w - 1u);
          const uint32_t y1 = std::min<uint32_t>(y0 + 1u, prev_h - 1u);

          for (uint32_t c = 0; c < bpp; ++c) {
            const auto at = [&](uint32_t px, uint32_t py) {
              return static_cast<uint32_t>(prev[(static_cast<size_t>(py) * prev_w + px) * bpp + c]);
            };
            const uint32_t sum = at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1);
            next[(static_cast<size_t>(y) * w + x) * bpp + c] = static_cast<std::byte>((sum + 2u) / 4u);
          }
        }
      }

      const uint32_t index = append_subresource(texture_id, mip, layer, w, h, std::span<const std::byte>(next.data(), next.size()), 0, 0);
      (void)index;

      prev = std::move(next);
      prev_w = w;
      prev_h = h;
    }
  }
}

std::optional<Archive> Archive::load_from_file(const std::string &path) {
  std::ifstream is(path_from_utf8(path), std::ios::binary);
  if (!is)
    return std::nullopt;
  return deserialize(is);
}

Archive Archive::load_from_memory(std::span<const std::byte> bytes) {
  std::istringstream is(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), std::ios::binary);
  return deserialize(is);
}

} // namespace bsvx::btx

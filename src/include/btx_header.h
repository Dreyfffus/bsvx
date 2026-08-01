#pragma once
#include "definitions.h"
#include "util.h"


namespace bsvx::btx {
	inline constexpr uint64_t BTX_MAGIC = 0x5845544C49534142ULL; // BASILTEX
	inline constexpr uint16_t BTX_VERSION = 1u;

	struct SamplerDesc final {

		uint32_t sampler_id = 0;
		SamplerFilter min_filter = SamplerFilter::NEAREST;
		SamplerFilter mag_filter = SamplerFilter::NEAREST;
		SamplerFilter mip_filter = SamplerFilter::NEAREST;

		SamplerAddressMode address_u = SamplerAddressMode::REPEAT;
		SamplerAddressMode address_v = SamplerAddressMode::REPEAT;
		SamplerAddressMode address_w = SamplerAddressMode::REPEAT;

		uint8_t anisotropy_enable = 0;
		uint16_t max_anisotropy_x100 = 100; //1.00
		uint8_t compare_enable = 0;
		uint8_t compare_op = 0; // raw Vulkan compare-op

		int16_t  mip_lod_bias_x1000 = 0;
		uint16_t min_lod_x1000 = 0;
		uint16_t max_lod_x1000 = 0;

		BorderColor border_color = BorderColor::FLOAT_TRANSPARENT_BLACK;
		std::array<uint8_t, 5> reserved{};

	};

	struct TextureDesc final {
		uint32_t texture_id = 0;
		TextureKind kind = TextureKind::TEXTURE_2D;

		// Store exact Vulkan-facing format and usage flags as raw integers.
		// This keeps the tool format Vulkan-native even if the tool itself
		// is built without Vulkan headers.
		uint32_t vk_format = 0;
		uint32_t usage_flags = 0;

		uint16_t width = 1;
		uint16_t height = 1;
		uint16_t depth = 1;  // only meaningful for Texture3D
		uint16_t array_layers = 1;  // only meaningful for Texture2DArray
		uint16_t mip_levels = 1;
		uint16_t sampler_id = 0;

		uint64_t name_hash = 0;
		uint64_t source_hash = 0;
	};

	struct SubresourceDesc final {
		uint32_t texture_id = 0;
		uint16_t mip_level = 0;

		// For Texture2DArray: array layer
		// For Texture3D: reserved in this first version
		uint16_t layer_or_slice = 0;

		uint16_t extent_x = 1;
		uint16_t extent_y = 1;
		uint16_t extent_z = 1;

		// Vulkan copy helpers map these directly to bufferRowLength / bufferImageHeight.
		// Zero means tightly packed.
		uint16_t packed_row_length = 0;
		uint16_t packed_image_height = 0;

		uint64_t blob_offset = 0;
		uint64_t blob_size = 0;
		uint64_t checksum = 0;
	};

	struct MaterialDesc final {
		uint32_t material_id = 0;
		uint32_t flags = 0;

		uint32_t albedo_texture_id = 0;
		uint32_t normal_texture_id = 0;
		uint32_t orm_texture_id = 0;
		uint32_t emissive_texture_id = 0;

		uint16_t albedo_layer_px = 0;
		uint16_t albedo_layer_nx = 0;
		uint16_t albedo_layer_py = 0;
		uint16_t albedo_layer_ny = 0;
		uint16_t albedo_layer_pz = 0;
		uint16_t albedo_layer_nz = 0;

		uint16_t normal_layer_px = 0;
		uint16_t normal_layer_nx = 0;
		uint16_t normal_layer_py = 0;
		uint16_t normal_layer_ny = 0;
		uint16_t normal_layer_pz = 0;
		uint16_t normal_layer_nz = 0;

		uint32_t tint_rgba8 = 0xFFFFFFFFu;
		uint32_t reserved = 0;
	};

	struct DiskHeader final {

		uint64_t magic = BTX_MAGIC;
		uint16_t version = BTX_VERSION;
		uint8_t endian = 0;
		uint8_t flags = 0;

		uint32_t sampler_count = 0;
		uint32_t texture_count = 0;
		uint32_t subresource_count = 0;
		uint32_t material_count = 0;


		uint64_t sampler_table_offset = 0;
		uint64_t texture_table_offset = 0;
		uint64_t subresource_table_offset = 0;
		uint64_t material_table_offset = 0;
		uint64_t blob_offset = 0;
		uint64_t blob_size = 0;
		uint64_t crc64 = 0;

	};

	struct DiskTextureDesc final {

		uint32_t texture_id = 0;
		uint8_t  kind = 0;
		uint8_t  reserved0 = 0;
		uint16_t reserved1 = 0;

		uint32_t vk_format = 0;
		uint32_t usage_flags = 0;

		uint16_t width = 1;
		uint16_t height = 1;
		uint16_t depth = 1;
		uint16_t array_layers = 1;
		uint16_t mip_levels = 1;
		uint16_t sampler_id = 0;

		uint32_t first_subresource = 0;
		uint32_t subresource_count = 0;

		uint64_t name_hash = 0;
		uint64_t source_hash = 0;
	};

	static_assert(TriviallySerializable<DiskHeader>);
	static_assert(TriviallySerializable<DiskTextureDesc>);
	static_assert(TriviallySerializable<SamplerDesc>);
	static_assert(TriviallySerializable<SubresourceDesc>);
	static_assert(TriviallySerializable<MaterialDesc>);

}
#pragma once
#include "util.h"
#include "definitions.h"

namespace bsvx::bvx {

	struct EncodedVoxelPayload final {
		VoxelCodec codec = VoxelCodec::CHUNK_INVALID;
		std::vector<std::byte> bytes;
	};

	uint32_t linear_index(uint16_t x, uint16_t y, uint16_t z, uint16_t sx, uint16_t sy, uint16_t sz);
	void append_u32(std::vector<std::byte>& out, uint32_t v);
	void append_u16(std::vector<std::byte>& out, uint32_t v);
	void append_u8 (std::vector<std::byte>& out, uint32_t v);
	void pack_indices_lsb(std::span<const uint32_t> indices, uint8_t bits, std::vector<std::byte>& out);
	std::vector<uint32_t> unpack_indices_lsb(std::span<const std::byte> bytes, size_t count, uint8_t bits);

	std::vector<std::byte> encode_voxels_empty();
	std::vector<std::byte> encode_voxels_uniform(uint32_t voxel_key);
	std::vector<std::byte> encode_voxels_raw_dense(std::span<const uint32_t> dense);
	std::vector<std::byte> encode_voxels_sparse_list(std::span<const uint32_t> dense);
	std::vector<std::byte> encode_voxels_palette_bitpack(std::span<const uint32_t> dense);
	std::vector<std::byte> encode_voxels_y_column_intervals(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz);
	std::vector<uint32_t>  decode_voxels_empty(size_t count);
	std::vector<uint32_t>  decode_voxels_uniform(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_raw_dense(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_sparse_list(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_palette_bitpack(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_y_column_intervals(std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz);
	std::vector<uint32_t>  decode_voxel_payload(VoxelCodec codec, std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz);
	EncodedVoxelPayload    choose_best_voxel(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz);
};
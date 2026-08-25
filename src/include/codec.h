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
	void unpack_indices_lsb_into(std::span<const std::byte> bytes, std::span<uint32_t> out, uint8_t bits);
	std::vector<uint32_t> unpack_indices_lsb(std::span<const std::byte> bytes, size_t count, uint8_t bits);

	std::vector<std::byte> encode_voxels_empty();
	std::vector<std::byte> encode_voxels_uniform(uint32_t voxel_key);
	std::vector<std::byte> encode_voxels_raw_dense(std::span<const uint32_t> dense);
	std::vector<std::byte> encode_voxels_sparse_list(std::span<const uint32_t> dense);
	// SPARSE_LIST with both of its fields cut to the width they actually need: a chunk-linear index
	// fits the chunk (12 bits for 16^3, not 32), and a voxel key becomes an index into the chunk's
	// own palette. Costs roughly two bytes a voxel where SPARSE_LIST costs eight, which is what
	// makes surface-like content -- a shell, a heightfield skin -- competitive with a flat voxel
	// list instead of several times its size.
	std::vector<std::byte> encode_voxels_sparse_packed(std::span<const uint32_t> dense);
	std::vector<std::byte> encode_voxels_palette_bitpack(std::span<const uint32_t> dense);
	std::vector<std::byte> encode_voxels_y_column_intervals(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz);
	// Where a decoded chunk is written.
	//
	// A chunk is laid out x-fastest, so a row of sx voxels is always contiguous; row_stride and
	// plane_stride say how far apart consecutive rows and planes sit in the *destination*. For a
	// standalone chunk buffer those are sx and sx*sy, which makes writing into a chunk-sized buffer
	// a special case of writing into a slice of a region-wide array rather than a separate path.
	//
	// That is the whole point of this type. Decoding a region into one dense array used to go
	// decode-into-scratch, then copy the scratch out one 64-byte row at a time -- three times the
	// memory traffic of writing the destination once, and a million strided copies for a 256^3
	// region. Handing the codec the strides instead lets it write where the data belongs.
	struct VoxelDest final {
		uint32_t* base = nullptr;
		uint16_t sx = 0, sy = 0, sz = 0;
		size_t row_stride = 0;
		size_t plane_stride = 0;
		// The destination is known to be all-zero already. Codecs that only describe occupied
		// voxels may then skip clearing it, which is what makes a sparse chunk cost its voxels
		// rather than its volume.
		bool already_air = false;

		size_t voxel_count() const { return static_cast<size_t>(sx) * sy * sz; }
		uint32_t* row(uint16_t y, uint16_t z) const { return base + y * row_stride + z * plane_stride; }
		uint32_t* at(uint16_t x, uint16_t y, uint16_t z) const { return row(y, z) + x; }
		// For codecs that carry a chunk-linear index rather than coordinates.
		uint32_t* at_linear(uint32_t index) const
		{
			const uint32_t x = index % sx;
			const uint32_t rest = index / sx;
			return at(static_cast<uint16_t>(x), static_cast<uint16_t>(rest % sy), static_cast<uint16_t>(rest / sy));
		}
	};

	// A destination covering a plain contiguous chunk buffer.
	VoxelDest contiguous_dest(std::span<uint32_t> out, uint16_t sx, uint16_t sy, uint16_t sz);

	// True for the codecs that encode only the voxels that are there, and so need their destination
	// cleared before they write it. The rest define every voxel and can ignore whatever was in the
	// buffer. This is what lets a caller decide, before decoding anything, whether clearing the
	// whole output up front is worth it.
	bool codec_describes_only_occupied(VoxelCodec codec);

	void decode_voxel_payload_into(VoxelCodec codec, std::span<const std::byte> payload, const VoxelDest& dest);

	// Decode straight into a caller-owned contiguous buffer. Every decoder defines *all* of `out`
	// -- the ones that only touch occupied voxels zero the rest -- so these are drop-in for the
	// returning forms below, which is what lets those be thin wrappers rather than a second
	// implementation.
	//
	// `out.size()` must be exactly the chunk's voxel count; a mismatch throws rather than writing a
	// partial chunk.
	void decode_voxels_empty_into(std::span<uint32_t> out);
	void decode_voxels_uniform_into(std::span<const std::byte> payload, std::span<uint32_t> out);
	void decode_voxels_raw_dense_into(std::span<const std::byte> payload, std::span<uint32_t> out);
	void decode_voxels_sparse_list_into(std::span<const std::byte> payload, std::span<uint32_t> out);
	void decode_voxels_sparse_packed_into(std::span<const std::byte> payload, std::span<uint32_t> out);
	void decode_voxels_palette_bitpack_into(std::span<const std::byte> payload, std::span<uint32_t> out);
	void decode_voxels_y_column_intervals_into(std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz, std::span<uint32_t> out);
	void decode_voxel_payload_into(VoxelCodec codec, std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz, std::span<uint32_t> out);

	std::vector<uint32_t>  decode_voxels_empty(size_t count);
	std::vector<uint32_t>  decode_voxels_uniform(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_raw_dense(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_sparse_list(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_sparse_packed(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_palette_bitpack(std::span<const std::byte> payload, size_t count);
	std::vector<uint32_t>  decode_voxels_y_column_intervals(std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz);
	std::vector<uint32_t>  decode_voxel_payload(VoxelCodec codec, std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz);
	EncodedVoxelPayload    choose_best_voxel(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz);
};
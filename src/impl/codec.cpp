#include "codec.h"
#include <unordered_map>

namespace bsvx::bvx {
	uint32_t linear_index(uint16_t x, uint16_t y, uint16_t z, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		return
			static_cast<uint32_t>(x) + static_cast<uint32_t>(sx) 
			* ( static_cast<uint32_t>(y) + static_cast<uint32_t>(sy) * static_cast<uint32_t>(z));
	}

	void append_u32(std::vector<std::byte>& out, uint32_t v)
	{
		append_pod(out, v);
	}

	void append_u16(std::vector<std::byte>& out, uint32_t v)
	{
		append_pod(out, static_cast<uint16_t>(v));
	}

	void append_u8(std::vector<std::byte>& out, uint32_t v)
	{
		append_pod(out, static_cast<uint8_t>(v));
	}

	void pack_indices_lsb(std::span<const uint32_t> indices, uint8_t bits, std::vector<std::byte>& out)
	{
		if (bits == 0 || bits > 32) {
			throw std::runtime_error("[bvx] invalid bit width");
		}

		uint64_t scratch = 0;
		uint32_t scratch_bits = 0;
		const uint64_t mask = (bits == 32) ? ~0ull : ((1ull << bits) - 1ull);

		for (uint32_t v : indices) {
			scratch |= (static_cast<uint64_t>(v) & mask) << scratch_bits;
			scratch_bits += bits;

			while (scratch_bits >= 8) {
				out.push_back(static_cast<std::byte>(scratch & 0xFFu));
				scratch >>= 8;
				scratch_bits -= 8;
			}
		}

		if (scratch_bits > 0) {
			out.push_back(static_cast<std::byte>(scratch & 0xFFu));
		}
	}

	std::vector<uint32_t> unpack_indices_lsb(std::span<const std::byte> bytes, size_t count, uint8_t bits)
	{
		std::vector<uint32_t> out(count, 0u);
		if (bits == 0 || bits > 32) throw std::runtime_error("[bvx]: invalid bit width");

		uint64_t scratch = 0u;
		uint32_t scratch_bits = 0u;
		size_t byte_cursor = 0;
		const uint64_t mask = (bits == 32) ? ~0ull : ((1ull << bits) - 1ull);

		for (size_t i = 0; i < count; i++) {
			while (scratch_bits < bits) {
				if (byte_cursor >= bytes.size()) throw std::runtime_error("[bvx]: bitpacked payload truncated");
				scratch |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[byte_cursor++])) << scratch_bits;
				scratch_bits += 8;
			}

			out[i] = static_cast<uint32_t>(scratch & mask);
			scratch >>= bits;
			scratch_bits -= bits;
		}

		return out;
	}

	std::vector<std::byte> encode_voxels_empty()
	{
		return {};
	}

	std::vector<std::byte> encode_voxels_uniform(uint32_t voxel_key)
	{
		std::vector<std::byte> out;
		append_u32(out, voxel_key);
		return out;
	}

	std::vector<std::byte> encode_voxels_raw_dense(std::span<const uint32_t> dense)
	{
		std::vector<std::byte> out;
		append_raw(out, std::span(dense.data(), dense.size()));
		return out;
	}

	std::vector<std::byte> encode_voxels_sparse_list(std::span<const uint32_t> dense)
	{
		std::vector<std::byte> out;
		uint32_t count = 0;
		for (uint32_t v : dense) if (v != 0) ++count;

		append_u32(out, count);
		for (uint32_t i = 0; i < dense.size(); i++) {
			if (dense[i] == 0) continue;
			append_u32(out, i);
			append_u32(out, dense[i]);
		}
		return out;
	}

	std::vector<std::byte> encode_voxels_palette_bitpack(std::span<const uint32_t> dense)
	{
		std::unordered_map<uint32_t, uint32_t> index_of;
		std::vector<uint32_t> pallete;
		std::vector<uint32_t> indices;
		indices.reserve(dense.size());

		for (uint32_t v : dense) {
			auto it = index_of.find(v);
			if (it == index_of.end()) {
				const uint32_t idx = static_cast<uint32_t>(pallete.size());
				index_of.emplace(v, idx);
				pallete.push_back(v);
				indices.push_back(idx);
			}
			else {
				indices.push_back(it->second);
			}
		}

		if (pallete.size() > std::numeric_limits<uint16_t>::max()) throw std::runtime_error("[bvx]: pallete too large for u16 count");

		const uint8_t bits = bit_width_u32(static_cast<uint32_t>(pallete.size()));
		std::vector<std::byte> out;
		append_u16(out, static_cast<uint16_t>(pallete.size()));
		append_u8(out, bits);
		append_u8(out, 0);
		append_raw(out, std::span<const uint32_t>(pallete.data(), pallete.size()));
		pack_indices_lsb(std::span(indices.data(), indices.size()), bits, out);
		return out;
	}

	std::vector<std::byte> encode_voxels_y_column_intervals(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		struct Run { uint16_t y_start; uint16_t y_length; uint32_t voxel_key; };
		std::vector<std::byte> out;

		std::vector<std::tuple<uint16_t, uint16_t, std::vector<Run>>> columns;

		for (uint16_t z = 0; z < sz; ++z) {
			for (uint16_t x = 0; x < sx; ++x) {
				std::vector<Run> runs;
				uint16_t y = 0;
				while (y < sy) {
					const uint32_t idx = linear_index(x, y, z, sx, sy, sz);
					const uint32_t v = dense[idx];
					if (v == 0) { ++y; continue; }
					const uint16_t start = y;
					++y;
					while (y < sy && dense[linear_index(x, y, z, sx, sy, sz)] == v) ++y;
					runs.push_back({ start, static_cast<uint16_t>(y - start), v });
				}
				if (!runs.empty()) columns.emplace_back(x, z, std::move(runs));
			}
		}

		append_u32(out, static_cast<uint32_t>(columns.size()));
		for (const auto& [x, z, runs] : columns) {
			append_u16(out, x);
			append_u16(out, z);
			append_u16(out, static_cast<uint16_t>(runs.size()));
			for (const Run& r : runs) {
				append_u16(out, r.y_start);
				append_u16(out, r.y_length);
				append_u32(out, r.voxel_key);
			}
		}
		return out;
	}

	std::vector<uint32_t> decode_voxels_empty(size_t count)
	{
		return std::vector<uint32_t>(count, 0u);
	}

	std::vector<uint32_t> decode_voxels_uniform(std::span<const std::byte> payload, size_t count)
	{
		size_t cursor = 0;
		const uint32_t v = read_pod<uint32_t>(payload, cursor);
		return std::vector<uint32_t>(count, v);
	}

	std::vector<uint32_t> decode_voxels_raw_dense(std::span<const std::byte> payload, size_t count)
	{
		if (payload.size() != count * sizeof(uint32_t)) {
			throw std::runtime_error("[bvx]: raw dense payload has wrong size");
		}
		std::vector<uint32_t> out(count);
		std::memcpy(out.data(), payload.data(), payload.size());
		return out;
	}

	std::vector<uint32_t> decode_voxels_sparse_list(std::span<const std::byte> payload, size_t voxel_count)
	{
		size_t cursor = 0;
		const uint32_t count = read_pod<uint32_t>(payload, cursor);
		std::vector<uint32_t> out(voxel_count, 0u);

		for (uint32_t i = 0; i < count; ++i) {
			const uint32_t idx = read_pod<uint32_t>(payload, cursor);
			const uint32_t key = read_pod<uint32_t>(payload, cursor);
			if (idx >= voxel_count) {
				throw std::runtime_error("[bvx]: sparse list index out of range");
			}
			out[idx] = key;
		}
		return out;
	}

	std::vector<uint32_t> decode_voxels_palette_bitpack(std::span<const std::byte> payload, size_t count)
	{
		size_t cursor = 0;
		const uint16_t palette_count = read_pod<uint16_t>(payload, cursor);
		const uint8_t bits = read_pod<uint8_t>(payload, cursor);
		(void)read_pod<uint8_t>(payload, cursor);

		std::vector<uint32_t> palette(palette_count);
		for (uint16_t i = 0; i < palette_count; ++i) {
			palette[i] = read_pod<uint32_t>(payload, cursor);
		}

		const auto packed = payload.subspan(cursor);
		std::vector<uint32_t> indices = unpack_indices_lsb(packed, count, bits);
		std::vector<uint32_t> out(count, 0u);

		for (size_t i = 0; i < count; ++i) {
			if (indices[i] >= palette.size()) {
				throw std::runtime_error("[bvx]: palette index out of range");
			}
			out[i] = palette[indices[i]];
		}
		return out;
	}

	std::vector<uint32_t> decode_voxels_y_column_intervals(std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		const size_t voxel_count = static_cast<size_t>(sx) * sy * sz;
		std::vector<uint32_t> out(voxel_count, 0u);

		size_t cursor = 0;
		const uint32_t column_count = read_pod<uint32_t>(payload, cursor);

		for (uint32_t c = 0; c < column_count; ++c) {
			const uint16_t x = read_pod<uint16_t>(payload, cursor);
			const uint16_t z = read_pod<uint16_t>(payload, cursor);
			const uint16_t run_count = read_pod<uint16_t>(payload, cursor);

			if (x >= sx || z >= sz) {
				throw std::runtime_error("[bvx]: y-column column coordinate out of range");
			}

			for (uint16_t r = 0; r < run_count; ++r) {
				const uint16_t y_start = read_pod<uint16_t>(payload, cursor);
				const uint16_t y_length = read_pod<uint16_t>(payload, cursor);
				const uint32_t key = read_pod<uint32_t>(payload, cursor);

				if (y_start + y_length > sy) {
					throw std::runtime_error("[bvx]: y-column run out of range");
				}

				for (uint16_t y = y_start; y < static_cast<uint16_t>(y_start + y_length); ++y) {
					out[linear_index(x, y, z, sx, sy, sz)] = key;
				}
			}
		}

		return out;
	}

	std::vector<uint32_t> decode_voxel_payload(VoxelCodec codec, std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		const size_t voxel_count = static_cast<size_t>(sx) * sy * sz;
		switch (codec) {
		
		case VoxelCodec::CHUNK_EMPTY:			return decode_voxels_empty(voxel_count);
		case VoxelCodec::CHUNK_UNIFORM:			return decode_voxels_uniform(payload, voxel_count);
		case VoxelCodec::PALLETE_BITPACK:		return decode_voxels_palette_bitpack(payload, voxel_count);
		case VoxelCodec::SPARSE_LIST:			return decode_voxels_sparse_list(payload, voxel_count);
		case VoxelCodec::Y_COLUMN_INTERVALS:	return decode_voxels_y_column_intervals(payload, sx, sy, sz);
		case VoxelCodec::RAW_DENSE:				return decode_voxels_raw_dense(payload, voxel_count);
		default: throw std::runtime_error("[bvx]: unsupported voxel codec");

		}
	}
	EncodedVoxelPayload choose_best_voxel(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		EncodedVoxelPayload best{};
		best.codec = VoxelCodec::RAW_DENSE;
		best.bytes = encode_voxels_raw_dense(dense);

		const auto try_candidate = [&](VoxelCodec codec, std::vector<std::byte> bytes) {
			if (best.codec == VoxelCodec::CHUNK_INVALID || bytes.size() < best.bytes.size()) {
				best.codec = codec;
				best.bytes = std::move(bytes);
			}
			};

		bool all_zero = true;
		bool uniform = true;
		const uint32_t first = dense.empty() ? 0u : dense[0];
		for (uint32_t v : dense) {
			if (v != 0u) all_zero = false;
			if (v != first) uniform = false;
		}

		if (all_zero) {
			return { VoxelCodec::CHUNK_EMPTY, encode_voxels_empty() };
		}
		if (uniform) {
			return { VoxelCodec::CHUNK_UNIFORM, encode_voxels_uniform(first) };
		}

		try_candidate(VoxelCodec::PALLETE_BITPACK, encode_voxels_palette_bitpack(dense));
		try_candidate(VoxelCodec::SPARSE_LIST, encode_voxels_sparse_list(dense));
		try_candidate(VoxelCodec::Y_COLUMN_INTERVALS, encode_voxels_y_column_intervals(dense, sx, sy, sz));
		return best;
	}
}
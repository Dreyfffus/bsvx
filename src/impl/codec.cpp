#include "codec.h"
#include <algorithm>
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

	void unpack_indices_lsb_into(std::span<const std::byte> bytes, std::span<uint32_t> out, uint8_t bits)
	{
		if (bits == 0 || bits > 32) throw std::runtime_error("[bvx]: invalid bit width");

		uint64_t scratch = 0u;
		uint32_t scratch_bits = 0u;
		size_t byte_cursor = 0;
		const uint64_t mask = (bits == 32) ? ~0ull : ((1ull << bits) - 1ull);

		for (size_t i = 0; i < out.size(); i++) {
			while (scratch_bits < bits) {
				if (byte_cursor >= bytes.size()) throw std::runtime_error("[bvx]: bitpacked payload truncated");
				scratch |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[byte_cursor++])) << scratch_bits;
				scratch_bits += 8;
			}

			out[i] = static_cast<uint32_t>(scratch & mask);
			scratch >>= bits;
			scratch_bits -= bits;
		}
	}

	std::vector<uint32_t> unpack_indices_lsb(std::span<const std::byte> bytes, size_t count, uint8_t bits)
	{
		std::vector<uint32_t> out(count, 0u);
		unpack_indices_lsb_into(bytes, out, bits);
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

	// --- decode into a destination view ----------------------------------------------------------
	//
	// These are the real implementations. Everything below them -- the contiguous-span forms and the
	// vector-returning forms -- funnels into these, so each codec is decoded in exactly one place.
	//
	// Each is responsible for every voxel of `dest`, including the air its codec does not mention,
	// except when dest.already_air says the caller has guaranteed the destination is clear.

	namespace {

		// Sequential LSB-first bit reader. Pulled out of unpack_indices_lsb so the palette codec can
		// walk one row of the destination at a time while the bit stream keeps running across rows.
		class LsbBitReader final {
		public:
			LsbBitReader(std::span<const std::byte> bytes, uint8_t bits)
				: bytes_(bytes), bits_(bits), mask_((bits == 32) ? ~0ull : ((1ull << bits) - 1ull))
			{
				if (bits == 0 || bits > 32) throw std::runtime_error("[bvx]: invalid bit width");
			}

			uint32_t next()
			{
				while (scratch_bits_ < bits_) {
					if (cursor_ >= bytes_.size()) throw std::runtime_error("[bvx]: bitpacked payload truncated");
					scratch_ |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_[cursor_++])) << scratch_bits_;
					scratch_bits_ += 8;
				}
				const uint32_t value = static_cast<uint32_t>(scratch_ & mask_);
				scratch_ >>= bits_;
				scratch_bits_ -= bits_;
				return value;
			}

		private:
			std::span<const std::byte> bytes_;
			uint64_t scratch_ = 0;
			uint32_t scratch_bits_ = 0;
			size_t cursor_ = 0;
			uint8_t bits_;
			uint64_t mask_;
		};

		void fill_dest(const VoxelDest& dest, uint32_t value)
		{
			for (uint16_t z = 0; z < dest.sz; ++z) {
				for (uint16_t y = 0; y < dest.sy; ++y) {
					uint32_t* row = dest.row(y, z);
					std::fill(row, row + dest.sx, value);
				}
			}
		}

		void clear_unless_air(const VoxelDest& dest)
		{
			if (!dest.already_air) fill_dest(dest, 0u);
		}

	}

	bool codec_describes_only_occupied(VoxelCodec codec)
	{
		switch (codec) {
		case VoxelCodec::CHUNK_EMPTY:
		case VoxelCodec::SPARSE_LIST:
		case VoxelCodec::Y_COLUMN_INTERVALS:
			return true;
		default:
			return false;
		}
	}

	VoxelDest contiguous_dest(std::span<uint32_t> out, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		const size_t expected = static_cast<size_t>(sx) * sy * sz;
		if (out.size() != expected) throw std::runtime_error("[bvx]: decode buffer is the wrong size");

		VoxelDest dest;
		dest.base = out.data();
		dest.sx = sx; dest.sy = sy; dest.sz = sz;
		dest.row_stride = sx;
		dest.plane_stride = static_cast<size_t>(sx) * sy;
		return dest;
	}

	void decode_voxel_payload_into(VoxelCodec codec, std::span<const std::byte> payload, const VoxelDest& dest)
	{
		size_t cursor = 0;

		switch (codec) {

		case VoxelCodec::CHUNK_EMPTY: {
			clear_unless_air(dest);
			return;
		}

		case VoxelCodec::CHUNK_UNIFORM: {
			// Always written, air or not: a uniform chunk of key 0 is still every voxel.
			fill_dest(dest, read_pod<uint32_t>(payload, cursor));
			return;
		}

		case VoxelCodec::RAW_DENSE: {
			if (payload.size() != dest.voxel_count() * sizeof(uint32_t)) {
				throw std::runtime_error("[bvx]: raw dense payload has wrong size");
			}
			const auto* src = reinterpret_cast<const uint32_t*>(payload.data());
			for (uint16_t z = 0; z < dest.sz; ++z) {
				for (uint16_t y = 0; y < dest.sy; ++y) {
					std::memcpy(dest.row(y, z), src, static_cast<size_t>(dest.sx) * sizeof(uint32_t));
					src += dest.sx;
				}
			}
			return;
		}

		case VoxelCodec::SPARSE_LIST: {
			const uint32_t count = read_pod<uint32_t>(payload, cursor);
			clear_unless_air(dest);
			const size_t voxel_count = dest.voxel_count();

			for (uint32_t i = 0; i < count; ++i) {
				const uint32_t idx = read_pod<uint32_t>(payload, cursor);
				const uint32_t key = read_pod<uint32_t>(payload, cursor);
				if (idx >= voxel_count) throw std::runtime_error("[bvx]: sparse list index out of range");
				*dest.at_linear(idx) = key;
			}
			return;
		}

		case VoxelCodec::PALLETE_BITPACK: {
			const uint16_t palette_count = read_pod<uint16_t>(payload, cursor);
			const uint8_t bits = read_pod<uint8_t>(payload, cursor);
			(void)read_pod<uint8_t>(payload, cursor);

			std::vector<uint32_t> palette(palette_count);
			for (uint16_t i = 0; i < palette_count; ++i) palette[i] = read_pod<uint32_t>(payload, cursor);

			// One pass: unpack an index and resolve it through the palette straight into place. The
			// old path materialised a whole second index array first, which for a 16^3 chunk is
			// another 16 KB allocated, written and read for nothing.
			LsbBitReader reader(payload.subspan(cursor), bits);
			for (uint16_t z = 0; z < dest.sz; ++z) {
				for (uint16_t y = 0; y < dest.sy; ++y) {
					uint32_t* row = dest.row(y, z);
					for (uint16_t x = 0; x < dest.sx; ++x) {
						const uint32_t index = reader.next();
						if (index >= palette.size()) throw std::runtime_error("[bvx]: palette index out of range");
						row[x] = palette[index];
					}
				}
			}
			return;
		}

		case VoxelCodec::Y_COLUMN_INTERVALS: {
			clear_unless_air(dest);
			const uint32_t column_count = read_pod<uint32_t>(payload, cursor);

			for (uint32_t c = 0; c < column_count; ++c) {
				const uint16_t x = read_pod<uint16_t>(payload, cursor);
				const uint16_t z = read_pod<uint16_t>(payload, cursor);
				const uint16_t run_count = read_pod<uint16_t>(payload, cursor);

				if (x >= dest.sx || z >= dest.sz) {
					throw std::runtime_error("[bvx]: y-column column coordinate out of range");
				}

				for (uint16_t r = 0; r < run_count; ++r) {
					const uint16_t y_start = read_pod<uint16_t>(payload, cursor);
					const uint16_t y_length = read_pod<uint16_t>(payload, cursor);
					const uint32_t key = read_pod<uint32_t>(payload, cursor);

					if (y_start + y_length > dest.sy) {
						throw std::runtime_error("[bvx]: y-column run out of range");
					}

					// A run walks y, so it steps by one row in the destination.
					uint32_t* cell = dest.at(x, y_start, z);
					for (uint16_t n = 0; n < y_length; ++n, cell += dest.row_stride) *cell = key;
				}
			}
			return;
		}

		default: throw std::runtime_error("[bvx]: unsupported voxel codec");

		}
	}

	// --- contiguous-buffer forms -----------------------------------------------------------------

	void decode_voxel_payload_into(VoxelCodec codec, std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz, std::span<uint32_t> out)
	{
		decode_voxel_payload_into(codec, payload, contiguous_dest(out, sx, sy, sz));
	}

	void decode_voxels_empty_into(std::span<uint32_t> out)
	{
		std::fill(out.begin(), out.end(), 0u);
	}

	void decode_voxels_uniform_into(std::span<const std::byte> payload, std::span<uint32_t> out)
	{
		size_t cursor = 0;
		std::fill(out.begin(), out.end(), read_pod<uint32_t>(payload, cursor));
	}

	void decode_voxels_raw_dense_into(std::span<const std::byte> payload, std::span<uint32_t> out)
	{
		if (payload.size() != out.size() * sizeof(uint32_t)) {
			throw std::runtime_error("[bvx]: raw dense payload has wrong size");
		}
		std::memcpy(out.data(), payload.data(), payload.size());
	}

	// Kept separate from the VoxelDest path only because this signature carries a voxel count
	// rather than chunk dimensions, and a linear index needs the dimensions to be decomposed.
	void decode_voxels_sparse_list_into(std::span<const std::byte> payload, std::span<uint32_t> out)
	{
		size_t cursor = 0;
		const uint32_t count = read_pod<uint32_t>(payload, cursor);
		std::fill(out.begin(), out.end(), 0u);

		for (uint32_t i = 0; i < count; ++i) {
			const uint32_t idx = read_pod<uint32_t>(payload, cursor);
			const uint32_t key = read_pod<uint32_t>(payload, cursor);
			if (idx >= out.size()) throw std::runtime_error("[bvx]: sparse list index out of range");
			out[idx] = key;
		}
	}

	void decode_voxels_palette_bitpack_into(std::span<const std::byte> payload, std::span<uint32_t> out)
	{
		size_t cursor = 0;
		const uint16_t palette_count = read_pod<uint16_t>(payload, cursor);
		const uint8_t bits = read_pod<uint8_t>(payload, cursor);
		(void)read_pod<uint8_t>(payload, cursor);

		std::vector<uint32_t> palette(palette_count);
		for (uint16_t i = 0; i < palette_count; ++i) palette[i] = read_pod<uint32_t>(payload, cursor);

		LsbBitReader reader(payload.subspan(cursor), bits);
		for (uint32_t& value : out) {
			const uint32_t index = reader.next();
			if (index >= palette.size()) throw std::runtime_error("[bvx]: palette index out of range");
			value = palette[index];
		}
	}

	void decode_voxels_y_column_intervals_into(std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz, std::span<uint32_t> out)
	{
		decode_voxel_payload_into(VoxelCodec::Y_COLUMN_INTERVALS, payload, contiguous_dest(out, sx, sy, sz));
	}

	// --- returning forms, for callers that want to own the buffer --------------------------------

	std::vector<uint32_t> decode_voxels_empty(size_t count)
	{
		return std::vector<uint32_t>(count, 0u);
	}

	std::vector<uint32_t> decode_voxels_uniform(std::span<const std::byte> payload, size_t count)
	{
		std::vector<uint32_t> out(count);
		decode_voxels_uniform_into(payload, out);
		return out;
	}

	std::vector<uint32_t> decode_voxels_raw_dense(std::span<const std::byte> payload, size_t count)
	{
		std::vector<uint32_t> out(count);
		decode_voxels_raw_dense_into(payload, out);
		return out;
	}

	std::vector<uint32_t> decode_voxels_sparse_list(std::span<const std::byte> payload, size_t voxel_count)
	{
		std::vector<uint32_t> out(voxel_count);
		decode_voxels_sparse_list_into(payload, out);
		return out;
	}

	std::vector<uint32_t> decode_voxels_palette_bitpack(std::span<const std::byte> payload, size_t count)
	{
		std::vector<uint32_t> out(count);
		decode_voxels_palette_bitpack_into(payload, out);
		return out;
	}

	std::vector<uint32_t> decode_voxels_y_column_intervals(std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		std::vector<uint32_t> out(static_cast<size_t>(sx) * sy * sz);
		decode_voxels_y_column_intervals_into(payload, sx, sy, sz, out);
		return out;
	}

	std::vector<uint32_t> decode_voxel_payload(VoxelCodec codec, std::span<const std::byte> payload, uint16_t sx, uint16_t sy, uint16_t sz)
	{
		std::vector<uint32_t> out(static_cast<size_t>(sx) * sy * sz);
		decode_voxel_payload_into(codec, payload, sx, sy, sz, out);
		return out;
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
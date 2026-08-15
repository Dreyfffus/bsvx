#include "bvx_header.h"
#include "codec.h"
#include <algorithm>

namespace bsvx::bvx {

	FaceState compute_face_state(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz, int face_axis, int face_side) {
		uint32_t occupied = 0;
		uint32_t total = 0;

		for (uint16_t z = 0; z < sz; z++) {

			for (uint16_t y = 0; y < sy; y++) {

				for (uint16_t x = 0; x < sx; x++) {
					bool on = false;
					switch (face_axis) {
					case 0: on = (face_side > 0) ? (x == sx - 1) : (x == 0); break;
					case 1: on = (face_side > 0) ? (y == sy - 1) : (y == 0); break;
					case 2: on = (face_side > 0) ? (z == sz - 1) : (z == 0); break;
					default: throw std::runtime_error("[bvx]: Invalid face_axis convention"); break;
					}
					if (!on) continue;

					++total;
					if (dense[linear_index(x, y, z, sx, sy, sz)] != 0) ++occupied;
				}
			}
		}

		if (total == 0u) return FaceState::EMPTY;
		if (occupied == 0u) return FaceState::EMPTY;
		if (occupied == total) return FaceState::FULL;
		return FaceState::MIXED;
	}

	ChunkSummary build_chunk_summary(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz, const std::unordered_map<uint32_t, RegistryEntry>* registry_lookup) {
		ChunkSummary sum{};
		if (dense.size() != static_cast<size_t>(sx) * sy * sz) throw std::runtime_error("[bvx]: build_chunk_summary dense size mismatch");

		std::unordered_map<uint32_t, uint32_t> hist;
		bool has_any = false;
		uint16_t min_x = sx, min_y = sy, min_z = sz;
		uint16_t max_x = 0, max_y = 0, max_z = 0;
		
		for (uint16_t z = 0; z < sz; ++z) {
			for (uint16_t y = 0; y < sy; ++y) {
				for (uint16_t x = 0; x < sx; ++x) {
				
					const uint32_t v = dense[linear_index(x, y, z, sx, sy, sz)];
					if (v == 0) continue;

					has_any = true;
					++sum.non_air_count;
					++hist[v];

					min_x = std::min(min_x, x);
					min_y = std::min(min_y, y);
					min_z = std::min(min_z, z);
					max_x = std::max(max_x, x);
					max_y = std::max(max_y, y);
					max_z = std::max(max_z, z);
					

					const uint8_t gx	= static_cast<uint8_t>((static_cast<uint32_t>(x) * 4u) / sx);
					const uint8_t gy	= static_cast<uint8_t>((static_cast<uint32_t>(y) * 4u) / sy);
					const uint8_t gz	= static_cast<uint8_t>((static_cast<uint32_t>(z) * 4u) / sz);
					const uint8_t bit	= static_cast<uint8_t>(gx + 4u * (gy + 4u * gz));
					sum.macro_occ_4x4x4 |= (uint64_t{ 1 } << bit);
					
					if (registry_lookup) {
						auto it = registry_lookup->find(v);
						if (it != registry_lookup->end()) {

							if ((it->second.flags & to_underlying(RegistryFlags::OPAQUE)) != 0u) ++sum.opaque_count;
							if ((it->second.flags & to_underlying(RegistryFlags::EMISSIVE)) != 0u) ++sum.emissive_count;
							if ((it->second.flags & to_underlying(RegistryFlags::SPECIAL)) != 0u) ++sum.special_count;

						} else {
							++sum.opaque_count;
						}
					} else {
						++sum.opaque_count;
					}
				}
			}
		}

		if (has_any) {
			sum.aabb_min_x = static_cast<uint8_t>(min_x);
			sum.aabb_min_y = static_cast<uint8_t>(min_y);
			sum.aabb_min_z = static_cast<uint8_t>(min_z);
			sum.aabb_max_x = static_cast<uint8_t>(max_x);
			sum.aabb_max_y = static_cast<uint8_t>(max_y);
			sum.aabb_max_z = static_cast<uint8_t>(max_z);
		}


		sum.face_state_px = static_cast<uint8_t>(compute_face_state(dense, sx, sy, sz, 0, +1));
		sum.face_state_nx = static_cast<uint8_t>(compute_face_state(dense, sx, sy, sz, 0, -1));
		sum.face_state_py = static_cast<uint8_t>(compute_face_state(dense, sx, sy, sz, 1, +1));
		sum.face_state_ny = static_cast<uint8_t>(compute_face_state(dense, sx, sy, sz, 1, -1));
		sum.face_state_pz = static_cast<uint8_t>(compute_face_state(dense, sx, sy, sz, 2, +1));
		sum.face_state_nz = static_cast<uint8_t>(compute_face_state(dense, sx, sy, sz, 2, -1));


		std::vector<std::pair<uint32_t, uint32_t>> ranked(hist.begin(), hist.end());
		std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
			if (a.second != b.second) return a.second > b.second;
			return a.first < b.first;
		});

		const auto set_top = [&](size_t idx, uint32_t id, uint16_t count) {
			switch (idx) {
			case 0: sum.top_id_0 = id; sum.top_count_0 = count; break;
			case 1: sum.top_id_1 = id; sum.top_count_1 = count; break;
			case 2: sum.top_id_2 = id; sum.top_count_2 = count; break;
			case 3: sum.top_id_3 = id; sum.top_count_3 = count; break;
			default: break;
			}
		};

		for (size_t i = 0; i < std::min<size_t>(4, ranked.size()); i++)
			set_top(i, ranked[i].first, static_cast<uint16_t>(std::min<uint32_t>(ranked[i].second, std::numeric_limits<uint16_t>::max())));

		return sum;
	}

	std::vector<std::byte> build_world_desc_blob(const WorldDesc& out) {
		DiskWorldDescHeader hdr{};
		hdr.version = WORLD_DESC_VERSION;
		hdr.chunk_size_x = out.geometry.chunk_size_x;
		hdr.chunk_size_y = out.geometry.chunk_size_y;
		hdr.chunk_size_z = out.geometry.chunk_size_z;
		hdr.region_chunks_x = out.geometry.region_size_x;
		hdr.region_chunks_y = out.geometry.region_size_y;
		hdr.region_chunks_z = out.geometry.region_size_z;
		hdr.voxel_schema = static_cast<uint16_t>(out.voxel_schema);
		hdr.axis_convention = static_cast<uint16_t>(out.axis_convention);
		hdr.bounds_mode = static_cast<uint16_t>(out.bounds_mode);
		hdr.world_min_region_x = out.world_min_region_x;
		hdr.world_min_region_y = out.world_min_region_y;
		hdr.world_min_region_z = out.world_min_region_z;
		hdr.world_max_region_x = out.world_max_region_x;
		hdr.world_max_region_y = out.world_max_region_y;
		hdr.world_max_region_z = out.world_max_region_z;
		hdr.texture_ref_count = static_cast<uint32_t>(out.texture_refs.size());
		hdr.registry_entry_count = static_cast<uint32_t>(out.registry_entries.size());
		hdr.asset_name_hash = out.asset_name_hash;
		hdr.registry_hash = out.registry_hash;
		hdr.manifest_hash = out.manifest_hash;

		std::vector<std::byte> blob;
		append_pod(blob, hdr);
		append_raw(blob, std::span(out.texture_refs.data(), out.texture_refs.size()));
		append_raw(blob, std::span(out.registry_entries.data(), out.registry_entries.size()));

		// --- v2 extension ------------------------------------------------------------------------
		// Offset 0 of the string table is always an empty string, so a name_offset of 0 means "no
		// name" without needing a sentinel.
		std::vector<std::byte> strings;
		strings.push_back(std::byte{ 0 });

		std::vector<uint32_t> name_offsets;
		name_offsets.reserve(out.registry_entries.size());
		for (const RegistryEntry& entry : out.registry_entries) {
			const std::string name = out.registry_name(entry.voxel_key);
			if (name.empty()) {
				name_offsets.push_back(0u);
				continue;
			}
			name_offsets.push_back(static_cast<uint32_t>(strings.size()));
			const auto bytes = std::as_bytes(std::span(name.data(), name.size()));
			strings.insert(strings.end(), bytes.begin(), bytes.end());
			strings.push_back(std::byte{ 0 });
		}

		std::vector<uint32_t> colors;
		colors.reserve(out.registry_entries.size());
		bool any_color = false;
		for (const RegistryEntry& entry : out.registry_entries) {
			const uint32_t color = out.registry_color(entry.voxel_key);
			colors.push_back(color);
			any_color = any_color || color != 0u;
		}
		if (!any_color) colors.clear();

		DiskWorldDescExt ext{};
		ext.ext_size = static_cast<uint32_t>(sizeof(DiskWorldDescExt));
		ext.registry_name_count = static_cast<uint32_t>(name_offsets.size());
		ext.registry_color_count = static_cast<uint32_t>(colors.size());
		ext.voxel_size_x = out.units.voxel_size_x;
		ext.voxel_size_y = out.units.voxel_size_y;
		ext.voxel_size_z = out.units.voxel_size_z;
		ext.origin_x = out.units.origin_x;
		ext.origin_y = out.units.origin_y;
		ext.origin_z = out.units.origin_z;
		ext.string_table_size = static_cast<uint32_t>(strings.size());

		append_pod(blob, ext);
		append_raw(blob, std::span<const uint32_t>(name_offsets.data(), name_offsets.size()));
		append_raw(blob, std::span<const uint32_t>(colors.data(), colors.size()));
		append_bytes(blob, std::span<const std::byte>(strings.data(), strings.size()));
		return blob;
	}

	WorldDesc parse_world_desc_blob(std::span<const std::byte> blob)
	{
		size_t cursor = 0;
		const DiskWorldDescHeader hdr = read_pod<DiskWorldDescHeader>(blob, cursor);

		WorldDesc out{};
		out.geometry.chunk_size_x = hdr.chunk_size_x;
		out.geometry.chunk_size_y = hdr.chunk_size_y;
		out.geometry.chunk_size_z = hdr.chunk_size_z;
		out.geometry.region_size_x = hdr.region_chunks_x;
		out.geometry.region_size_y = hdr.region_chunks_y;
		out.geometry.region_size_z = hdr.region_chunks_z;
		out.voxel_schema = static_cast<VoxelSchema>(hdr.voxel_schema);
		out.axis_convention = static_cast<AxisConvention>(hdr.axis_convention);
		out.bounds_mode = static_cast<BoundsMode>(hdr.bounds_mode);
		out.world_min_region_x = hdr.world_min_region_x;
		out.world_min_region_y = hdr.world_min_region_y;
		out.world_min_region_z = hdr.world_min_region_z;
		out.world_max_region_x = hdr.world_max_region_x;
		out.world_max_region_y = hdr.world_max_region_y;
		out.world_max_region_z = hdr.world_max_region_z;
		out.asset_name_hash = hdr.asset_name_hash;
		out.registry_hash = hdr.registry_hash;
		out.manifest_hash = hdr.manifest_hash;

		const size_t tex_bytes = static_cast<size_t>(hdr.texture_ref_count) * sizeof(BtxRef);
		const size_t reg_bytes = static_cast<size_t>(hdr.registry_entry_count) * sizeof(RegistryEntry);

		if (cursor + tex_bytes + reg_bytes > blob.size()) throw std::runtime_error("[btx]: world desc blob truncated");

		out.texture_refs.resize(hdr.texture_ref_count);
		if (!out.texture_refs.empty()) {
			std::memcpy(out.texture_refs.data(), blob.data() + cursor, tex_bytes);
			cursor += tex_bytes;
		}

		out.registry_entries.resize(hdr.registry_entry_count);
		if (!out.registry_entries.empty()) {
			std::memcpy(out.registry_entries.data(), blob.data() + cursor, reg_bytes);
			cursor += reg_bytes;
		}

		// A v1 blob simply ends here. Anything newer than v2 is read for the fields v2 knows and the
		// rest is skipped via ext_size, so a forward-written file still loads.
		if (hdr.version < 2u || cursor + sizeof(DiskWorldDescExt) > blob.size()) return out;

		// Read ext_size first, then copy only as much as both sides agree exists: a blob written by
		// an older v2 build has a shorter extension, and reading sizeof() bytes would spill into the
		// tables behind it.
		uint32_t ext_size = 0;
		std::memcpy(&ext_size, blob.data() + cursor, sizeof(ext_size));
		if (ext_size < sizeof(uint32_t) * 2u) throw std::runtime_error("[bvx]: world desc extension truncated");
		if (cursor + ext_size > blob.size()) throw std::runtime_error("[bvx]: world desc extension out of bounds");

		DiskWorldDescExt ext{};
		std::memcpy(&ext, blob.data() + cursor, std::min<size_t>(ext_size, sizeof(ext)));
		cursor += ext_size;

		out.units.voxel_size_x = ext.voxel_size_x;
		out.units.voxel_size_y = ext.voxel_size_y;
		out.units.voxel_size_z = ext.voxel_size_z;
		out.units.origin_x = ext.origin_x;
		out.units.origin_y = ext.origin_y;
		out.units.origin_z = ext.origin_z;

		const size_t offsets_bytes = static_cast<size_t>(ext.registry_name_count) * sizeof(uint32_t);
		const size_t colors_bytes = static_cast<size_t>(ext.registry_color_count) * sizeof(uint32_t);
		if (cursor + offsets_bytes + colors_bytes + ext.string_table_size > blob.size()) throw std::runtime_error("[bvx]: world desc name table out of bounds");

		std::vector<uint32_t> name_offsets(ext.registry_name_count);
		if (!name_offsets.empty()) {
			std::memcpy(name_offsets.data(), blob.data() + cursor, offsets_bytes);
		}
		cursor += offsets_bytes;

		std::vector<uint32_t> colors(ext.registry_color_count);
		if (!colors.empty()) {
			std::memcpy(colors.data(), blob.data() + cursor, colors_bytes);
		}
		cursor += colors_bytes;

		for (size_t i = 0; i < colors.size() && i < out.registry_entries.size(); ++i) {
			if (colors[i] != 0u) out.registry_colors.emplace(out.registry_entries[i].voxel_key, colors[i]);
		}

		const char* strings = reinterpret_cast<const char*>(blob.data() + cursor);
		const size_t strings_size = ext.string_table_size;

		for (size_t i = 0; i < name_offsets.size() && i < out.registry_entries.size(); ++i) {
			const uint32_t offset = name_offsets[i];
			if (offset == 0u || offset >= strings_size) continue;

			size_t len = 0;
			while (offset + len < strings_size && strings[offset + len] != '\0') ++len;
			if (len != 0) out.registry_names.emplace(out.registry_entries[i].voxel_key, std::string(strings + offset, len));
		}

		return out;
	}

	std::vector<std::byte> build_metadata_blob(const MetadataMap& metadata)
	{
		std::vector<std::byte> blob;
		if (metadata.empty()) return blob;

		append_pod(blob, static_cast<uint32_t>(metadata.size()));
		for (const auto& [key, value] : metadata) {
			append_pod(blob, static_cast<uint32_t>(key.size()));
			append_pod(blob, static_cast<uint32_t>(value.size()));
		}
		for (const auto& [key, value] : metadata) {
			const auto key_bytes = std::as_bytes(std::span(key.data(), key.size()));
			blob.insert(blob.end(), key_bytes.begin(), key_bytes.end());
			blob.insert(blob.end(), value.begin(), value.end());
		}
		return blob;
	}

	MetadataMap parse_metadata_blob(std::span<const std::byte> blob)
	{
		MetadataMap out;
		if (blob.empty()) return out;

		size_t cursor = 0;
		const uint32_t count = read_pod<uint32_t>(blob, cursor);

		std::vector<std::pair<uint32_t, uint32_t>> sizes;
		sizes.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			const uint32_t key_size = read_pod<uint32_t>(blob, cursor);
			const uint32_t value_size = read_pod<uint32_t>(blob, cursor);
			sizes.emplace_back(key_size, value_size);
		}

		for (const auto& [key_size, value_size] : sizes) {
			if (cursor + key_size + value_size > blob.size()) throw std::runtime_error("[bvx]: metadata blob truncated");

			std::string key(reinterpret_cast<const char*>(blob.data() + cursor), key_size);
			cursor += key_size;

			std::vector<std::byte> value(blob.begin() + static_cast<ptrdiff_t>(cursor), blob.begin() + static_cast<ptrdiff_t>(cursor + value_size));
			cursor += value_size;

			out.emplace(std::move(key), std::move(value));
		}
		return out;
	}

}

#include "bvx_header.h"
#include "codec.h"

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
					case 1: on = (face_side > 0) ? (x == sx - 1) : (x == 0); break;
					case 2: on = (face_side > 0) ? (x == sx - 1) : (x == 0); break;
					default: throw std::runtime_error("[bvx]: Invalid face_axis convention"); break;
					}
					if (!on) continue;
					if (dense[linear_index(x, y, z, sx, sy, sz)] != 0) ++occupied;
				}
			}
		}

		if (!occupied) return FaceState::EMPTY;
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

		return out;
	}

}

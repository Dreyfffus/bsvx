#pragma once
#include "definitions.h"
#include "util.h"
#include <unordered_map>

namespace bsvx::bvx {
	inline constexpr uint64_t BVX_MAGIC = 0x584F564C49534142ULL; // BASILVOX
	inline constexpr uint16_t BVX_VERSION = 1u;
	inline constexpr uint64_t INVALID_OFFSET = ~uint64_t{ 0 };

	struct DiskHeader final {

		uint64_t magic = BVX_MAGIC;
		uint16_t version = BVX_VERSION;
		uint16_t flags = 0;

		int32_t region_x = 0;
		int32_t region_y = 0;
		int32_t region_z = 0;

		uint32_t chunk_count = 0;
		uint16_t section_count = 0;
		uint16_t reserved = 0;

		uint64_t chunk_map_offset = 0;
		uint64_t summary_table_offset = 0;
		uint64_t section_dir_offset = 0;
		uint64_t file_size = 0;

		uint64_t manifest_hash = 0;
		uint64_t registry_hash = 0;
		uint64_t crc64 = 0;

	};

	struct ChunkMapEntry final {

		uint16_t local_chunk_x = 0;
		uint16_t local_chunk_y = 0;
		uint16_t local_chunk_z = 0;
		uint16_t flags = 0;
		uint32_t summary_index = 0;
		uint32_t user0 = 0;

	};

	struct ChunkSummary final {

		uint32_t non_air_count = 0;
		uint32_t opaque_count = 0;
		uint16_t emissive_count = 0;
		uint16_t special_count = 0;

		uint8_t aabb_min_x = 0;
		uint8_t aabb_min_y = 0;
		uint8_t aabb_min_z = 0;
		uint8_t aabb_max_x = 0;
		uint8_t aabb_max_y = 0;
		uint8_t aabb_max_z = 0;

		uint8_t face_state_px = 0;
		uint8_t face_state_nx = 0;
		uint8_t face_state_py = 0;
		uint8_t face_state_ny = 0;
		uint8_t face_state_pz = 0;
		uint8_t face_state_nz = 0;

		uint64_t macro_occ_4x4x4 = 0;

		uint32_t top_id_0 = 0;
		uint32_t top_id_1 = 0;
		uint32_t top_id_2 = 0;
		uint32_t top_id_3 = 0;

		uint16_t top_count_0 = 0;
		uint16_t top_count_1 = 0;
		uint16_t top_count_2 = 0;
		uint16_t top_count_3 = 0;

	};

	struct SectionRecord final {

		uint32_t section_type = 0;
		uint16_t default_codec = 0;
		uint16_t entry_stride = 0;
		uint32_t entry_count = 0;
		uint64_t entry_table_offset = 0;
		uint64_t blob_offset = 0;
		uint64_t blob_size = 0;

	};

	struct OffsetSizeEntry final {

		uint64_t offset = INVALID_OFFSET;
		uint32_t size = 0;
		uint16_t flags = 0;
		uint16_t codec = 0;

	};

	struct DiskWorldDescHeader final {

		uint16_t version = 1;
		uint16_t chunk_size_x = 16;
		uint16_t chunk_size_y = 16;
		uint16_t chunk_size_z = 16;

		uint16_t region_chunks_x = 1;
		uint16_t region_chunks_y = 1;
		uint16_t region_chunks_z = 1;
		uint16_t voxel_schema = static_cast<uint16_t>(VoxelSchema::DENSE_U32_VOXEL_KEY);

		uint16_t axis_convention = static_cast<uint16_t>(AxisConvention::X_RIGHT_Y_UP_Z_FORWARD);
		uint16_t bounds_mode = static_cast<uint16_t>(BoundsMode::UNBOUNDED);
		uint16_t reserved0 = 0;
		uint16_t reserved1 = 0;

		int32_t world_min_region_x = 0;
		int32_t world_min_region_y = 0;
		int32_t world_min_region_z = 0;
		int32_t world_max_region_x = 0;
		int32_t world_max_region_y = 0;
		int32_t world_max_region_z = 0;

		uint32_t texture_ref_count = 0;
		uint32_t registry_entry_count = 0;

		uint64_t asset_name_hash = 0;
		uint64_t registry_hash = 0;
		uint64_t manifest_hash = 0;

	};

	struct BtxRef final {

		char relative_path[128]{};
		uint64_t path_hash = 0;
		uint64_t content_hash = 0;

	};

	struct RegistryEntry final {

		uint32_t voxel_key = 0;
		uint32_t material_id = 0;
		uint32_t reserved = 0;
		uint32_t flags = 0;
		uint64_t name_hash = 0;

	};

	static_assert(TriviallySerializable<DiskHeader>);
	static_assert(TriviallySerializable<ChunkMapEntry>);
	static_assert(TriviallySerializable<ChunkSummary>);
	static_assert(TriviallySerializable<SectionRecord>);
	static_assert(TriviallySerializable<OffsetSizeEntry>);
	static_assert(TriviallySerializable<DiskWorldDescHeader>);
	static_assert(TriviallySerializable<BtxRef>);
	static_assert(TriviallySerializable<RegistryEntry>);

	struct GeometryDesc final {
		uint16_t chunk_size_x = 16;
		uint16_t chunk_size_y = 16;
		uint16_t chunk_size_z = 16;

		uint16_t region_size_x = 16;
		uint16_t region_size_y = 16;
		uint16_t region_size_z = 16;
	};

	struct WorldDesc final {
		GeometryDesc geometry{};
		VoxelSchema voxel_schema = VoxelSchema::DENSE_U32_VOXEL_KEY;
		AxisConvention axis_convention = AxisConvention::X_RIGHT_Y_UP_Z_FORWARD;
		BoundsMode bounds_mode = BoundsMode::UNBOUNDED;

		int32_t world_min_region_x = 0;
		int32_t world_min_region_y = 0;
		int32_t world_min_region_z = 0;
		int32_t world_max_region_x = 0;
		int32_t world_max_region_y = 0;
		int32_t world_max_region_z = 0;

		uint64_t asset_name_hash = 0;
		uint64_t registry_hash = 0;
		uint64_t manifest_hash = 0;

		std::vector<BtxRef> texture_refs;
		std::vector<RegistryEntry> registry_entries;
	};

	struct PayloadSection final {
		SectionType type = SectionType::VOXELS;
		bool chunk_associated = true;
		uint16_t default_codec = 0;
		std::vector<OffsetSizeEntry> entries;
		std::vector<std::byte> blob;
	};

	FaceState compute_face_state(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz, int face_axis, int face_side);
	ChunkSummary build_chunk_summary(std::span<const uint32_t> dense, uint16_t sx, uint16_t sy, uint16_t sz, const std::unordered_map<uint32_t, RegistryEntry>* registry_lookup);
	std::vector<std::byte> build_world_desc_blob(const WorldDesc& out);
	WorldDesc parse_world_desc_blob(std::span<const std::byte> blob);
}

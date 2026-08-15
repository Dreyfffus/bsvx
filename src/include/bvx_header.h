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
		// Occupies what used to be alignment padding, which every previous writer left zeroed --
		// hence SectionFlags::FLAGS_PRESENT to distinguish "no flags" from "an older file".
		uint32_t section_flags = 0;
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

	// Everything a WORLD_DESC blob carried in v1, in exactly the v1 layout. A v2 blob appends
	// DiskWorldDescExt (registry names, units) after the registry entry table; see
	// build_world_desc_blob.
	inline constexpr uint16_t WORLD_DESC_VERSION = 2u;

	struct DiskWorldDescHeader final {

		uint16_t version = WORLD_DESC_VERSION;
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

	// Trails the v1 payload of a WORLD_DESC blob. ext_size is the size of this struct as the writer
	// knew it, so a later version can grow it and an older reader still finds the tables behind it.
	struct DiskWorldDescExt final {

		uint32_t ext_size = 0;
		uint32_t registry_name_count = 0;   // parallel to the registry entry table

		double voxel_size_x = 1.0;
		double voxel_size_y = 1.0;
		double voxel_size_z = 1.0;

		double origin_x = 0.0;
		double origin_y = 0.0;
		double origin_z = 0.0;

		uint32_t string_table_size = 0;
		// Parallel to the registry entry table, like the name offsets; 0 means "no colour". A
		// reader that predates this field sees a smaller ext_size and skips it.
		uint32_t registry_color_count = 0;

	};

	struct BtxRef final {

		char relative_path[128]{};
		uint64_t path_hash = 0;
		uint64_t content_hash = 0;

	};

	// Longest relative path a BtxRef can hold without truncating. Authoring paths are checked
	// against this rather than silently cut, which used to produce a texture that never resolved.
	inline constexpr size_t BTX_REF_PATH_CAPACITY = sizeof(BtxRef::relative_path) - 1u;

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
	static_assert(TriviallySerializable<DiskWorldDescExt>);
	static_assert(TriviallySerializable<BtxRef>);
	static_assert(TriviallySerializable<RegistryEntry>);

	// section_flags has to land in the old padding word, or v1 files stop parsing.
	static_assert(sizeof(SectionRecord) == 40, "SectionRecord must stay 40 bytes wide");
	static_assert(offsetof(SectionRecord, entry_table_offset) == 16, "SectionRecord layout changed");

	struct GeometryDesc final {
		uint16_t chunk_size_x = 16;
		uint16_t chunk_size_y = 16;
		uint16_t chunk_size_z = 16;

		uint16_t region_size_x = 16;
		uint16_t region_size_y = 16;
		uint16_t region_size_z = 16;
	};

	// Metres per voxel edge and the world-space position of voxel (0,0,0) in region (0,0,0). The
	// format carried neither before v2, so a DCC tool had to keep the scale factor outside the file
	// and every re-import guessed it.
	struct UnitsDesc final {
		double voxel_size_x = 1.0;
		double voxel_size_y = 1.0;
		double voxel_size_z = 1.0;
		double origin_x = 0.0;
		double origin_y = 0.0;
		double origin_z = 0.0;

		constexpr bool is_default() const noexcept
		{
			return voxel_size_x == 1.0 && voxel_size_y == 1.0 && voxel_size_z == 1.0 &&
				origin_x == 0.0 && origin_y == 0.0 && origin_z == 0.0;
		}
	};

	// Free-form key/value store. Values are opaque bytes: a host stores whatever it needs to make a
	// re-import lossless, and every other tool is required to carry unknown keys through unchanged.
	using MetadataMap = std::map<std::string, std::vector<std::byte>>;

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

		UnitsDesc units{};

		std::vector<BtxRef> texture_refs;
		std::vector<RegistryEntry> registry_entries;

		// voxel_key -> human-readable name. Kept beside the entry table rather than inside
		// RegistryEntry so the registry hash (which is taken over the raw entry bytes) does not
		// move, and so a name of any length can be stored.
		std::map<uint32_t, std::string> registry_names;

		// voxel_key -> 0xRRGGBBAA display colour. Authoritative only when no material resolves;
		// it exists so a palette means something before a world has any .btx at all. 0 = unset.
		std::map<uint32_t, uint32_t> registry_colors;

		std::string registry_name(uint32_t voxel_key) const
		{
			const auto it = registry_names.find(voxel_key);
			return it == registry_names.end() ? std::string{} : it->second;
		}

		uint32_t registry_color(uint32_t voxel_key) const
		{
			const auto it = registry_colors.find(voxel_key);
			return it == registry_colors.end() ? 0u : it->second;
		}
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
	std::vector<std::byte> build_metadata_blob(const MetadataMap& metadata);
	MetadataMap parse_metadata_blob(std::span<const std::byte> blob);
}

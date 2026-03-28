#pragma once
#include "definitions.h"
#include "util.h"
#include "bvx_header.h"
#include <unordered_map>

namespace bsvx::bvx {
	class Archive final {
	public:

		Archive() = default;
		Archive(const Archive&) = default;
		Archive(Archive&&) noexcept = default;
		Archive& operator=(const Archive&) = default;
		Archive& operator=(Archive&&) noexcept = default;
		~Archive() = default;

		int32_t region_x = 0;
		int32_t region_y = 0;
		int32_t region_z = 0;
		uint64_t manifest_hash = 0;
		uint64_t registry_hash = 0;

		std::optional<WorldDesc> standalone;
		std::vector<ChunkMapEntry> chunk_map;
		std::vector<ChunkSummary> chunk_summaries;
		std::vector<PayloadSection> sections;

		constexpr bool is_standalone() const noexcept { return standalone.has_value(); }

		static uint64_t make_chunk_key(uint16_t x, uint16_t y, uint16_t z);
		static size_t chunk_voxel_count(const GeometryDesc& g) noexcept;
		static BtxRef make_btx_ref(std::string_view relative_path, uint64_t content_hash = 0);
		static RegistryEntry make_registry_entry(uint32_t voxel_key, uint32_t material_id, RegistryFlags flags, std::string_view name = {});

		void set_world_desc(WorldDesc desc);
		uint32_t find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const;
		std::optional<uint32_t> try_find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const;
		uint32_t add_or_get_chunk(uint16_t x, uint16_t y, uint16_t z);
		PayloadSection& get_or_create_chunk_section(SectionType type, uint16_t default_codec = 0);
		const PayloadSection* find_section(SectionType type) const;
		GeometryDesc resolve_geometry(const GeometryDesc* override_geometry = nullptr) const;
		std::unordered_map<uint32_t, RegistryEntry> build_registry_lookup() const;
		void set_chunk_voxels_dense(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, std::span<const uint32_t> dense, const GeometryDesc* geometry_override = nullptr, VoxelCodec req_codec = VoxelCodec::AUTO);
		void set_chunk_payload(SectionType type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, std::span<const std::byte> payload, uint16_t entry_flags = 0);
		std::vector<uint32_t> decode_chunk_voxels(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const GeometryDesc* geometry_override = nullptr) const;

		bool save_to_file(const std::string& path) const;
		static std::optional<Archive> load_from_file(const std::string& path);

	private:

		static Archive deserialize(std::span<const std::byte> bytes);
		static Archive deserialize(std::istream& is);
		std::vector<std::byte> serialize_to_bytes() const;
		void serialize(std::ostream& os) const;
	};

}
#pragma once
#include "definitions.h"
#include "util.h"
#include "bvx_header.h"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace bsvx::bvx {

	using RegistryLookup = std::unordered_map<uint32_t, RegistryEntry>;

	// Builds the voxel_key -> RegistryEntry map used to classify voxels when a chunk summary is
	// rebuilt. Regions that belong to a manifest world carry no registry of their own, so the caller
	// has to hand theirs down (see Archive::set_chunk_voxels_dense).
	RegistryLookup build_registry_lookup(const WorldDesc& desc);

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
		// Only needed after chunk_map has been edited directly; every mutator in this class keeps
		// the index in sync on its own.
		void rebuild_chunk_index();
		PayloadSection& get_or_create_chunk_section(SectionType type, uint16_t default_codec = 0);
		const PayloadSection* find_section(SectionType type) const;
		GeometryDesc resolve_geometry(const GeometryDesc* override_geometry = nullptr) const;
		RegistryLookup build_registry_lookup() const;
		void set_chunk_voxels_dense(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, std::span<const uint32_t> dense, const GeometryDesc* geometry_override = nullptr, VoxelCodec req_codec = VoxelCodec::AUTO, const RegistryLookup* registry_override = nullptr);
		void set_chunk_payload(SectionType type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, std::span<const std::byte> payload, uint16_t entry_flags = 0);
		// Returns a view into the section blob; it is invalidated by any mutation of this archive.
		std::optional<std::span<const std::byte>> get_chunk_payload(SectionType type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t* out_codec = nullptr, uint16_t* out_entry_flags = nullptr) const;
		std::vector<uint32_t> decode_chunk_voxels(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const GeometryDesc* geometry_override = nullptr) const;

		// Every set_chunk_* appends to a section blob and repoints the entry, so repeated edits grow
		// the archive without bound. compact() rewrites each blob to hold only live, deduplicated
		// ranges and returns the number of bytes reclaimed. reclaimable_bytes() reports the same
		// number without touching anything.
		size_t compact();
		size_t reclaimable_bytes() const;

		bool save_to_file(const std::string& path) const;
		static std::optional<Archive> load_from_file(const std::string& path);
		static Archive load_from_memory(std::span<const std::byte> bytes);

		static Archive deserialize(std::span<const std::byte> bytes);
		std::vector<std::byte> serialize_to_bytes() const;

	private:

		static Archive deserialize(std::istream& is);
		void serialize(std::ostream& os) const;

		std::unordered_map<uint64_t, uint32_t> chunk_index_;
	};

	// ---------------------------------------------------------------------------------------------
	// Streaming / partial loading
	// ---------------------------------------------------------------------------------------------

	// Random-access byte source backing a RegionReader. Implementations must be safe to call
	// concurrently from several threads.
	class ByteSource {
	public:
		virtual ~ByteSource() = default;
		virtual uint64_t size() const = 0;
		virtual void read(uint64_t offset, std::span<std::byte> dst) const = 0;
	};

	class MemoryByteSource final : public ByteSource {
	public:
		// copy == false borrows the caller's buffer, which must outlive this object.
		MemoryByteSource(std::span<const std::byte> bytes, bool copy);
		uint64_t size() const override;
		void read(uint64_t offset, std::span<std::byte> dst) const override;
	private:
		std::vector<std::byte> owned_;
		std::span<const std::byte> view_;
	};

	// Positional reads, so several worker threads can pull different chunks out of one open file
	// without serializing on a shared stream position (POSIX pread where available).
	class FileByteSource final : public ByteSource {
	public:
		explicit FileByteSource(const std::string& path);
		~FileByteSource() override;
		uint64_t size() const override;
		void read(uint64_t offset, std::span<std::byte> dst) const override;
	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
		uint64_t size_ = 0;
	};

	// Reads a .bvx header, chunk map, summaries, section directory and entry tables up front, and
	// leaves every payload blob on the source until it is asked for. That is what makes chunk
	// granularity streaming possible: metadata is a few tens of KB, the voxel blobs are not.
	class RegionReader final {
	public:
		RegionReader() = default;
		RegionReader(const RegionReader&) = delete;
		RegionReader& operator=(const RegionReader&) = delete;
		RegionReader(RegionReader&&) noexcept = default;
		RegionReader& operator=(RegionReader&&) noexcept = default;

		static RegionReader open(std::shared_ptr<ByteSource> source);
		static RegionReader open_file(const std::string& path);
		static RegionReader open_memory(std::span<const std::byte> bytes, bool copy);

		const DiskHeader& header() const noexcept { return header_; }
		bool is_standalone() const noexcept { return standalone_.has_value(); }
		const WorldDesc* world_desc() const noexcept { return standalone_ ? &*standalone_ : nullptr; }
		const std::vector<ChunkMapEntry>& chunk_map() const noexcept { return chunk_map_; }
		const std::vector<ChunkSummary>& chunk_summaries() const noexcept { return chunk_summaries_; }
		const std::vector<SectionRecord>& section_directory() const noexcept { return directory_; }

		// Regions that are part of a manifest world carry no geometry; supply the world's before
		// decoding anything from them.
		void set_geometry_override(const GeometryDesc& geometry) { geometry_override_ = geometry; }
		GeometryDesc resolve_geometry() const;

		std::optional<uint32_t> try_find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const;
		const OffsetSizeEntry* find_chunk_entry(SectionType type, uint32_t chunk_index) const;

		// Fetches exactly the bytes of one chunk's payload from the source.
		std::optional<std::vector<std::byte>> read_chunk_payload(SectionType type, uint32_t chunk_index, uint16_t* out_codec = nullptr, uint16_t* out_entry_flags = nullptr) const;
		std::vector<uint32_t> decode_chunk_voxels(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z) const;

		// Reads the whole file and checks the header hash. Not done at open() -- the point of this
		// class is to avoid touching every byte.
		bool verify_integrity() const;
		// Materializes a fully resident Archive, reading every blob.
		Archive load_full() const;

		uint64_t resident_bytes() const noexcept;

	private:
		std::shared_ptr<ByteSource> source_;
		DiskHeader header_{};
		std::vector<ChunkMapEntry> chunk_map_;
		std::vector<ChunkSummary> chunk_summaries_;
		std::vector<SectionRecord> directory_;
		std::vector<std::vector<OffsetSizeEntry>> entries_;
		std::optional<WorldDesc> standalone_;
		std::optional<GeometryDesc> geometry_override_;
		std::unordered_map<uint64_t, uint32_t> chunk_index_;
	};

}

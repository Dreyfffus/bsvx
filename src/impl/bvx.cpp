#include "bvx.h"
#include "codec.h"
#include <algorithm>
#include <map>
#include <string_view>
#include <unordered_map>

// Positional reads keep concurrent chunk fetches off a shared file position. Anything without
// pread (Windows) falls back to a mutex-guarded ifstream below.
#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#define BSVX_HAS_PREAD 1
#endif

namespace bsvx::bvx {

	RegistryLookup build_registry_lookup(const WorldDesc& desc)
	{
		RegistryLookup lookup;
		lookup.reserve(desc.registry_entries.size());
		for (const RegistryEntry& reg : desc.registry_entries) lookup.emplace(reg.voxel_key, reg);
		return lookup;
	}

	void Archive::set_world_desc(WorldDesc desc)
	{
		standalone = std::move(desc);
		manifest_hash = standalone->manifest_hash;
		registry_hash = standalone->registry_hash;
	}
	uint64_t Archive::make_chunk_key(uint16_t x, uint16_t y, uint16_t z)
	{
		return uint64_t{ x } | (uint64_t{ y } << 16u) | (uint64_t{ z } << 32u);
	}
	void Archive::rebuild_chunk_index()
	{
		chunk_index_.clear();
		chunk_index_.reserve(chunk_map.size());
		for (uint32_t i = 0; i < chunk_map.size(); i++) {
			const auto& c = chunk_map[i];
			chunk_index_.emplace(make_chunk_key(c.local_chunk_x, c.local_chunk_y, c.local_chunk_z), i);
		}
	}
	uint32_t Archive::find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const
	{
		if (auto found = try_find_chunk_index(x, y, z)) return *found;
		throw std::runtime_error("[bvx]: chunk not found");
	}
	std::optional<uint32_t> Archive::try_find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const
	{
		// The index is kept in sync by every mutator, so a const lookup never writes to it and
		// concurrent decodes of the same archive stay safe.
		if (chunk_index_.size() == chunk_map.size()) {
			const auto it = chunk_index_.find(make_chunk_key(x, y, z));
			if (it == chunk_index_.end()) return std::nullopt;
			return it->second;
		}

		// chunk_map was edited behind our back; fall back to a scan rather than lie about it.
		const uint64_t key = make_chunk_key(x, y, z);
		for (uint32_t i = 0; i < chunk_map.size(); i++) {
			const auto& c = chunk_map[i];
			if (make_chunk_key(c.local_chunk_x, c.local_chunk_y, c.local_chunk_z) == key) return i;
		}
		return std::nullopt;
	}
	uint32_t Archive::add_or_get_chunk(uint16_t x, uint16_t y, uint16_t z)
	{
		if (auto existing = try_find_chunk_index(x, y, z)) return *existing;

		ChunkMapEntry entry{};
		entry.local_chunk_x = x;
		entry.local_chunk_y = y;
		entry.local_chunk_z = z;
		entry.flags = to_underlying(ChunkFlags::PRESENT);
		entry.summary_index = static_cast<uint32_t>(chunk_summaries.size());

		const uint32_t index = static_cast<uint32_t>(chunk_map.size());
		chunk_map.push_back(entry);
		chunk_summaries.push_back({});
		chunk_index_.emplace(make_chunk_key(x, y, z), index);

		for (PayloadSection& sec : sections) if (sec.chunk_associated) sec.entries.push_back({});

		return index;
	}
	PayloadSection& Archive::get_or_create_chunk_section(SectionType type, uint16_t default_codec)
	{
		for (PayloadSection& sec : sections) if (sec.type == type) return sec;


		PayloadSection sec{};
		sec.type = type;
		sec.chunk_associated = true;
		sec.default_codec = default_codec;
		sec.entries.resize(chunk_map.size());
		sections.push_back(std::move(sec));
		return sections.back();
	}
	const PayloadSection* Archive::find_section(SectionType type) const
	{
		for (const PayloadSection& sec : sections) if (sec.type == type) return &sec;
		return nullptr;
	}
	GeometryDesc Archive::resolve_geometry(const GeometryDesc* override_geometry) const
	{
		if (standalone) return standalone->geometry;
		if (override_geometry) return *override_geometry;
		throw std::runtime_error("[btx]: could not find geometry in non-authored region file. This indicates that the region file is part of a larger map and cannot be loaded as a standalone asset");
	}
	size_t Archive::chunk_voxel_count(const GeometryDesc& g) noexcept
	{
		return static_cast<size_t>(g.chunk_size_x) * g.chunk_size_y * g.chunk_size_z;
	}
	RegistryLookup Archive::build_registry_lookup() const
	{
		if (!standalone) return {};
		return bvx::build_registry_lookup(*standalone);
	}
	void Archive::set_chunk_voxels_dense(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, std::span<const uint32_t> dense, const GeometryDesc* geometry_override, VoxelCodec req_codec, const RegistryLookup* registry_override)
	{
		const GeometryDesc g = resolve_geometry(geometry_override);
		if (dense.size() != chunk_voxel_count(g)) throw std::runtime_error("[bvx]: dense chunk voxel count mismatch");

		const uint32_t chunk_index = add_or_get_chunk(chunk_x, chunk_y, chunk_z);
		PayloadSection& voxels = get_or_create_chunk_section(SectionType::VOXELS, static_cast<uint16_t>(VoxelCodec::PALLETE_BITPACK));

		EncodedVoxelPayload payload{};
		if (req_codec == VoxelCodec::AUTO) {
			payload = choose_best_voxel(dense, g.chunk_size_x, g.chunk_size_y, g.chunk_size_z);
		} else {
			payload.codec = req_codec;
			switch (req_codec) {
			case VoxelCodec::CHUNK_EMPTY:           payload.bytes = encode_voxels_empty(); break;
			case VoxelCodec::CHUNK_UNIFORM:         payload.bytes = encode_voxels_uniform(dense.empty() ? 0u : dense[0]); break;
			case VoxelCodec::PALLETE_BITPACK:		payload.bytes = encode_voxels_palette_bitpack(dense); break;
			case VoxelCodec::SPARSE_LIST:			payload.bytes = encode_voxels_sparse_list(dense); break;
			case VoxelCodec::Y_COLUMN_INTERVALS:	payload.bytes = encode_voxels_y_column_intervals(dense, g.chunk_size_x, g.chunk_size_y, g.chunk_size_z); break;
			case VoxelCodec::RAW_DENSE:				payload.bytes = encode_voxels_raw_dense(dense); break;
			default: throw std::runtime_error("[bvx]: unsupported requested codec");
			}
		}

		const uint64_t offset = static_cast<uint64_t>(voxels.blob.size());
		append_bytes(voxels.blob, payload.bytes);
		voxels.entries[chunk_index] = {
			.offset = offset,
			.size = static_cast<uint32_t>(payload.bytes.size()),
			.flags = 0,
			.codec = static_cast<uint16_t>(payload.codec)
		};

		// A region that belongs to a manifest world has no registry of its own. Without the caller's
		// lookup every non-air voxel would be counted as opaque and the emissive/special counts
		// would be silently zeroed on write.
		const RegistryLookup owned_lookup = registry_override ? RegistryLookup{} : build_registry_lookup();
		const RegistryLookup* registry_ptr = registry_override ? registry_override : &owned_lookup;
		if (registry_ptr->empty()) registry_ptr = nullptr;
		chunk_summaries[chunk_map[chunk_index].summary_index] = build_chunk_summary(dense, g.chunk_size_x, g.chunk_size_y, g.chunk_size_z, registry_ptr);

		auto& cmap = chunk_map[chunk_index];
		cmap.flags |= to_underlying(ChunkFlags::HAS_VOXELS);
		if (chunk_summaries[cmap.summary_index].non_air_count == 0) cmap.flags |= to_underlying(ChunkFlags::CHUNK_EMPTY);
		else cmap.flags &= ~to_underlying(ChunkFlags::CHUNK_EMPTY);
	}
	void Archive::set_chunk_payload(SectionType type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, std::span<const std::byte> payload, uint16_t entry_flags)
	{
		if (type == SectionType::WORLD_DESC) throw std::runtime_error("[bvx]: use set_world_desc for standalone metadata");

		const uint32_t idx = add_or_get_chunk(chunk_x, chunk_y, chunk_z);
		PayloadSection& sec = get_or_create_chunk_section(type, codec);
		const uint64_t offset = static_cast<uint64_t>(sec.blob.size());
		append_bytes(sec.blob, payload);
		sec.entries[idx] = {
			.offset = offset,
			.size = static_cast<uint32_t>(payload.size()),
			.flags = entry_flags,
			.codec = codec
		};

		auto& cmap = chunk_map[idx];
		switch (type) {
		case SectionType::SURFACE:			cmap.flags |= to_underlying(ChunkFlags::HAS_SURFACE_BAKE); break;
		case SectionType::COLLISION:		cmap.flags |= to_underlying(ChunkFlags::HAS_COLLISION_BAKE); break;
		case SectionType::DISTANCE_FIELD:	cmap.flags |= to_underlying(ChunkFlags::HAS_DISTANCE_FIELD); break;
		case SectionType::LIGHT:			cmap.flags |= to_underlying(ChunkFlags::HAS_LIGHT_BAKE); break;
		default: break;
		}
	}
	std::optional<std::span<const std::byte>> Archive::get_chunk_payload(SectionType type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t* out_codec, uint16_t* out_entry_flags) const
	{
		const auto chunk_index = try_find_chunk_index(chunk_x, chunk_y, chunk_z);
		if (!chunk_index) throw std::runtime_error("[bvx]: get_chunk_payload chunk not found");

		const PayloadSection* sec = find_section(type);
		if (!sec || *chunk_index >= sec->entries.size()) return std::nullopt;

		const OffsetSizeEntry& entry = sec->entries[*chunk_index];
		if (entry.offset == INVALID_OFFSET) return std::nullopt;
		if (entry.offset > sec->blob.size() || entry.size > sec->blob.size() - entry.offset) throw std::runtime_error("[bvx]: payload entry out of bounds");

		if (out_codec) *out_codec = entry.codec;
		if (out_entry_flags) *out_entry_flags = entry.flags;
		return std::span<const std::byte>(sec->blob.data() + entry.offset, entry.size);
	}
	std::vector<uint32_t> Archive::decode_chunk_voxels(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const GeometryDesc* geometry_override) const
	{
		const GeometryDesc g = resolve_geometry(geometry_override);
		const size_t voxel_count = chunk_voxel_count(g);
		const auto chunk_index_opt = try_find_chunk_index(chunk_x, chunk_y, chunk_z);
		if (!chunk_index_opt) throw std::runtime_error("[bvx]: decode_chunk_voxels chunk not found");
		const uint32_t chunk_index = *chunk_index_opt;
		const PayloadSection* voxels = find_section(SectionType::VOXELS);
		if (!voxels || chunk_index >= voxels->entries.size()) {
			return std::vector<uint32_t>(voxel_count, 0u);
		}

		const OffsetSizeEntry& entry = voxels->entries[chunk_index];
		if (entry.offset == INVALID_OFFSET) return std::vector<uint32_t>(voxel_count, 0u);
		if (entry.offset + entry.size > voxels->blob.size()) throw std::runtime_error("[bvx]: voxel payload entry out of bounds");

		const auto payload = std::span(voxels->blob.data() + entry.offset, entry.size);
		return decode_voxel_payload(static_cast<VoxelCodec>(entry.codec), payload, g.chunk_size_x, g.chunk_size_y, g.chunk_size_z);

	}
	uint64_t Archive::chunk_content_hash(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const GeometryDesc* geometry_override) const
	{
		const auto dense = decode_chunk_voxels(chunk_x, chunk_y, chunk_z, geometry_override);
		return fnv1a64(std::as_bytes(std::span(dense.data(), dense.size())));
	}
	bool Archive::remove_chunk(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z)
	{
		const auto found = try_find_chunk_index(chunk_x, chunk_y, chunk_z);
		if (!found) return false;
		const uint32_t index = *found;

		// The summary table is addressed through ChunkMapEntry::summary_index rather than
		// positionally, so every index above the erased one has to come down with it.
		const uint32_t summary_index = chunk_map[index].summary_index;
		if (summary_index < chunk_summaries.size()) {
			chunk_summaries.erase(chunk_summaries.begin() + static_cast<ptrdiff_t>(summary_index));
			for (ChunkMapEntry& entry : chunk_map) {
				if (entry.summary_index > summary_index) --entry.summary_index;
			}
		}

		chunk_map.erase(chunk_map.begin() + static_cast<ptrdiff_t>(index));

		for (PayloadSection& sec : sections) {
			if (!sec.chunk_associated) continue;
			if (index < sec.entries.size()) sec.entries.erase(sec.entries.begin() + static_cast<ptrdiff_t>(index));
		}

		rebuild_chunk_index();
		return true;
	}
	bool Archive::clear_chunk(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z)
	{
		const auto found = try_find_chunk_index(chunk_x, chunk_y, chunk_z);
		if (!found) return false;
		const uint32_t index = *found;

		for (PayloadSection& sec : sections) {
			if (!sec.chunk_associated || index >= sec.entries.size()) continue;
			sec.entries[index] = OffsetSizeEntry{};
		}

		chunk_summaries[chunk_map[index].summary_index] = ChunkSummary{};
		chunk_map[index].flags = to_underlying(bit_or(ChunkFlags::PRESENT, ChunkFlags::CHUNK_EMPTY));
		return true;
	}
	bool Archive::remove_chunk_payload(SectionType type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z)
	{
		const auto found = try_find_chunk_index(chunk_x, chunk_y, chunk_z);
		if (!found) return false;
		const uint32_t index = *found;

		for (PayloadSection& sec : sections) {
			if (sec.type != type || !sec.chunk_associated || index >= sec.entries.size()) continue;
			if (sec.entries[index].offset == INVALID_OFFSET) return false;

			sec.entries[index] = OffsetSizeEntry{};

			auto& cmap = chunk_map[index];
			switch (type) {
			case SectionType::VOXELS:			cmap.flags &= ~to_underlying(ChunkFlags::HAS_VOXELS); break;
			case SectionType::SURFACE:			cmap.flags &= ~to_underlying(ChunkFlags::HAS_SURFACE_BAKE); break;
			case SectionType::COLLISION:		cmap.flags &= ~to_underlying(ChunkFlags::HAS_COLLISION_BAKE); break;
			case SectionType::DISTANCE_FIELD:	cmap.flags &= ~to_underlying(ChunkFlags::HAS_DISTANCE_FIELD); break;
			case SectionType::LIGHT:			cmap.flags &= ~to_underlying(ChunkFlags::HAS_LIGHT_BAKE); break;
			default: break;
			}
			if (type == SectionType::VOXELS) chunk_summaries[cmap.summary_index] = ChunkSummary{};
			return true;
		}
		return false;
	}
	size_t Archive::reclaimable_bytes() const
	{
		size_t reclaimable = 0;
		for (const PayloadSection& sec : sections) {
			std::map<std::pair<uint64_t, uint32_t>, bool> live;
			size_t live_bytes = 0;
			for (const OffsetSizeEntry& e : sec.entries) {
				if (e.offset == INVALID_OFFSET) continue;
				if (e.offset > sec.blob.size() || e.size > sec.blob.size() - e.offset) continue;
				if (live.emplace(std::pair{ e.offset, e.size }, true).second) live_bytes += e.size;
			}
			reclaimable += sec.blob.size() - std::min(live_bytes, sec.blob.size());
		}
		return reclaimable;
	}
	size_t Archive::compact()
	{
		size_t reclaimed = 0;
		for (PayloadSection& sec : sections) {
			std::vector<std::byte> rebuilt;
			rebuilt.reserve(sec.blob.size());

			// Entries pointing at the same range (a chunk written twice with identical bytes, or
			// aliased payloads) collapse onto one copy.
			std::map<std::pair<uint64_t, uint32_t>, uint64_t> moved;
			for (OffsetSizeEntry& e : sec.entries) {
				if (e.offset == INVALID_OFFSET) continue;
				if (e.offset > sec.blob.size() || e.size > sec.blob.size() - e.offset) throw std::runtime_error("[bvx]: compact: payload entry out of bounds");

				const auto key = std::pair{ e.offset, e.size };
				if (const auto it = moved.find(key); it != moved.end()) {
					e.offset = it->second;
					continue;
				}

				const uint64_t new_offset = static_cast<uint64_t>(rebuilt.size());
				rebuilt.insert(rebuilt.end(), sec.blob.begin() + static_cast<ptrdiff_t>(e.offset), sec.blob.begin() + static_cast<ptrdiff_t>(e.offset + e.size));
				moved.emplace(key, new_offset);
				e.offset = new_offset;
			}

			reclaimed += sec.blob.size() - rebuilt.size();
			rebuilt.shrink_to_fit();
			sec.blob = std::move(rebuilt);
		}
		return reclaimed;
	}
	std::vector<std::byte> Archive::serialize_to_bytes() const
	{
		std::vector<PayloadSection> temp_sections = sections;
		if (standalone) {
			PayloadSection world_sec{};
			world_sec.type = SectionType::WORLD_DESC;
			world_sec.chunk_associated = false;
			world_sec.default_codec = 0;
			world_sec.blob = build_world_desc_blob(*standalone);
			world_sec.entries.resize(1);
			world_sec.entries[0] = { .offset = 0, .size = static_cast<uint32_t>(world_sec.blob.size()), .flags = 0, .codec = 0 };
			temp_sections.push_back(std::move(world_sec));
		}
		if (!metadata.empty()) {
			PayloadSection meta_sec{};
			meta_sec.type = SectionType::METADATA;
			meta_sec.chunk_associated = false;
			meta_sec.default_codec = 0;
			meta_sec.blob = build_metadata_blob(metadata);
			meta_sec.entries.resize(1);
			meta_sec.entries[0] = { .offset = 0, .size = static_cast<uint32_t>(meta_sec.blob.size()), .flags = 0, .codec = 0 };
			temp_sections.push_back(std::move(meta_sec));
		}

		std::sort(temp_sections.begin(), temp_sections.end(), [](const PayloadSection& a, const PayloadSection& b) {
			return static_cast<uint32_t>(a.type) < static_cast<uint32_t>(b.type);
			});

		std::vector<SectionRecord> dir(temp_sections.size());

		DiskHeader header{};
		header.flags = is_standalone() ? to_underlying(FileFlags::STANDALONE) : 0;
		header.region_x = region_x;
		header.region_y = region_y;
		header.region_z = region_z;
		header.chunk_count = static_cast<uint32_t>(chunk_map.size());
		header.section_count = static_cast<uint16_t>(temp_sections.size());
		header.manifest_hash = manifest_hash;
		header.registry_hash = registry_hash;

		uint64_t cursor = sizeof(DiskHeader);
		cursor = align64(cursor, 16);
		header.chunk_map_offset = cursor;
		cursor += chunk_map.size() * sizeof(ChunkMapEntry);

		cursor = align64(cursor, 16);
		header.summary_table_offset = cursor;
		cursor += chunk_summaries.size() * sizeof(ChunkSummary);

		cursor = align64(cursor, 16);
		header.section_dir_offset = cursor;
		cursor += dir.size() * sizeof(SectionRecord);

		for (size_t i = 0; i < temp_sections.size(); ++i) {
			dir[i].section_type = static_cast<uint32_t>(temp_sections[i].type);
			dir[i].default_codec = temp_sections[i].default_codec;
			dir[i].entry_stride = sizeof(OffsetSizeEntry);
			dir[i].entry_count = static_cast<uint32_t>(temp_sections[i].entries.size());
			dir[i].section_flags = to_underlying(SectionFlags::FLAGS_PRESENT);
			if (temp_sections[i].chunk_associated) dir[i].section_flags |= to_underlying(SectionFlags::CHUNK_ASSOCIATED);

			cursor = align64(cursor, 16);
			dir[i].entry_table_offset = cursor;
			cursor += temp_sections[i].entries.size() * sizeof(OffsetSizeEntry);

			cursor = align64(cursor, 16);
			dir[i].blob_offset = cursor;
			dir[i].blob_size = temp_sections[i].blob.size();
			cursor += temp_sections[i].blob.size();
		}

		header.file_size = cursor;

		std::vector<std::byte> bytes;
		bytes.resize(static_cast<size_t>(header.file_size), std::byte{ 0 });
		std::memcpy(bytes.data(), &header, sizeof(header));

		if (!chunk_map.empty()) {
			std::memcpy(bytes.data() + header.chunk_map_offset, chunk_map.data(), chunk_map.size() * sizeof(ChunkMapEntry));
		}
		if (!chunk_summaries.empty()) {
			std::memcpy(bytes.data() + header.summary_table_offset, chunk_summaries.data(), chunk_summaries.size() * sizeof(ChunkSummary));
		}
		if (!dir.empty()) {
			std::memcpy(bytes.data() + header.section_dir_offset, dir.data(), dir.size() * sizeof(SectionRecord));
		}

		for (size_t i = 0; i < temp_sections.size(); ++i) {
			const auto& sec = temp_sections[i];
			if (!sec.entries.empty()) {
				std::memcpy(bytes.data() + dir[i].entry_table_offset, sec.entries.data(), sec.entries.size() * sizeof(OffsetSizeEntry));
			}
			if (!sec.blob.empty()) {
				std::memcpy(bytes.data() + dir[i].blob_offset, sec.blob.data(), sec.blob.size());
			}
		}

		DiskHeader* header_ptr = reinterpret_cast<DiskHeader*>(bytes.data());
		header_ptr->crc64 = 0;
		header_ptr->crc64 = fnv1a64(std::span<const std::byte>(bytes.data(), bytes.size()));
		return bytes;
	}
	void Archive::serialize(std::ostream& os) const
	{
		const std::vector<std::byte> bytes = serialize_to_bytes();
		os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!os) throw std::runtime_error("[bvx]: write failed");
	}
	bool Archive::save_to_file(const std::string& path, bool atomic, bool backup) const
	{
		// Paths crossing this API are UTF-8, so they must not be reinterpreted in the platform's
		// narrow encoding on the way to std::filesystem.
		const std::filesystem::path target = path_from_utf8(path);
		const std::vector<std::byte> bytes = serialize_to_bytes();
		try {
			if (atomic) write_file_atomic(target, std::span<const std::byte>(bytes.data(), bytes.size()), backup);
			else write_file_direct(target, std::span<const std::byte>(bytes.data(), bytes.size()));
			return true;
		}
		catch (const std::exception&) {
			return false;
		}
	}
	Archive Archive::deserialize(std::span<const std::byte> bytes)
	{
		if (bytes.size() < sizeof(DiskHeader)) {
			throw std::runtime_error("[bvx]: file too small");
		}

		DiskHeader header{};
		std::memcpy(&header, bytes.data(), sizeof(header));
		if (header.magic != BVX_MAGIC) throw std::runtime_error("[bvx]: black magic, unsupported file type");
		if (header.version != BVX_VERSION) throw std::runtime_error("[bvx]: unsupported version");
		if (header.file_size != bytes.size()) throw std::runtime_error("[bvx]: file size mismatch");

		std::vector<std::byte> crc_bytes(bytes.begin(), bytes.end());
		reinterpret_cast<DiskHeader*>(crc_bytes.data())->crc64 = 0;
		const uint64_t actual_crc = fnv1a64(std::span<const std::byte>(crc_bytes.data(), crc_bytes.size()));
		if (actual_crc != header.crc64) throw std::runtime_error("[bvx]: crc mismatch");

		auto require_range = [&](uint64_t offset, uint64_t size, const char* what) {
			if (offset > bytes.size() || size > bytes.size() - offset) throw std::runtime_error(std::string("[bvx]: out-of-bounds ") + what);
			};

		Archive out{};
		out.region_x = header.region_x;
		out.region_y = header.region_y;
		out.region_z = header.region_z;
		out.manifest_hash = header.manifest_hash;
		out.registry_hash = header.registry_hash;

		require_range(header.chunk_map_offset, uint64_t(header.chunk_count) * sizeof(ChunkMapEntry), "chunk map");
		out.chunk_map.resize(header.chunk_count);
		if (!out.chunk_map.empty()) {
			std::memcpy(out.chunk_map.data(), bytes.data() + header.chunk_map_offset, out.chunk_map.size() * sizeof(ChunkMapEntry));
		}

		require_range(header.summary_table_offset, uint64_t(header.chunk_count) * sizeof(ChunkSummary), "summary table");
		out.chunk_summaries.resize(header.chunk_count);
		if (!out.chunk_summaries.empty()) {
			std::memcpy(out.chunk_summaries.data(), bytes.data() + header.summary_table_offset, out.chunk_summaries.size() * sizeof(ChunkSummary));
		}

		require_range(header.section_dir_offset, uint64_t(header.section_count) * sizeof(SectionRecord), "section directory");
		std::vector<SectionRecord> dir(header.section_count);
		if (!dir.empty()) {
			std::memcpy(dir.data(), bytes.data() + header.section_dir_offset, dir.size() * sizeof(SectionRecord));
		}

		for (const SectionRecord& rec : dir) {
			require_range(rec.entry_table_offset, uint64_t(rec.entry_count) * rec.entry_stride, "section entry table");
			require_range(rec.blob_offset, rec.blob_size, "section blob");
			if (rec.entry_stride != sizeof(OffsetSizeEntry)) throw std::runtime_error("[bvx]: unsupported section entry stride");

			std::vector<OffsetSizeEntry> entries(rec.entry_count);
			if (!entries.empty()) {
				std::memcpy(entries.data(), bytes.data() + rec.entry_table_offset, entries.size() * sizeof(OffsetSizeEntry));
			}

			std::vector<std::byte> blob(static_cast<size_t>(rec.blob_size));
			if (!blob.empty()) {
				std::memcpy(blob.data(), bytes.data() + rec.blob_offset, blob.size());
			}

			const SectionType type = static_cast<SectionType>(rec.section_type);
			if (type == SectionType::WORLD_DESC) {
				if (entries.size() != 1 || entries[0].offset != 0 || entries[0].size != blob.size()) throw std::runtime_error("[bvx]: malformed world desc section");
				out.standalone = parse_world_desc_blob(blob);
				continue;
			}
			if (type == SectionType::METADATA) {
				if (entries.size() != 1 || entries[0].offset != 0 || entries[0].size != blob.size()) throw std::runtime_error("[bvx]: malformed metadata section");
				out.metadata = parse_metadata_blob(blob);
				continue;
			}

			PayloadSection sec{};
			sec.type = type;
			sec.default_codec = rec.default_codec;
			// Pre-v4 writers left section_flags zeroed, so the entry-count heuristic is still the
			// fallback -- but only when the writer genuinely did not record the answer.
			sec.chunk_associated = has_bits(static_cast<SectionFlags>(rec.section_flags), SectionFlags::FLAGS_PRESENT)
				? has_bits(static_cast<SectionFlags>(rec.section_flags), SectionFlags::CHUNK_ASSOCIATED)
				: (entries.size() == out.chunk_map.size());
			sec.entries = std::move(entries);
			sec.blob = std::move(blob);
			out.sections.push_back(std::move(sec));
		}

		if ((header.flags & to_underlying(FileFlags::STANDALONE)) != 0u && !out.standalone) throw std::runtime_error("[bvx]: standalone flag set but WORLD_DESC section missing");

		out.rebuild_chunk_index();
		return out;
	}
	Archive Archive::deserialize(std::istream& is)
	{
		is.seekg(0, std::ios::end);
		const auto end = is.tellg();
		is.seekg(0, std::ios::beg);
		if (end < 0) throw std::runtime_error("[bvx]: failed to query file size");
		std::vector<std::byte> bytes(static_cast<size_t>(end));
		if (!bytes.empty()) {
			is.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!is) throw std::runtime_error("[bvx]: failed to read file");
		}
		return deserialize(std::span<const std::byte>(bytes.data(), bytes.size()));
	}
	std::optional<Archive> Archive::load_from_file(const std::string& path)
	{
		std::ifstream is(path_from_utf8(path), std::ios::binary);
		if (!is) return std::nullopt;
		return std::optional<Archive>{deserialize(is)};
	}
	Archive Archive::load_from_memory(std::span<const std::byte> bytes)
	{
		return deserialize(bytes);
	}

	BtxRef Archive::make_btx_ref(std::string_view relative_path, uint64_t content_hash)
	{
		BtxRef ref{};
		const size_t n = std::min(relative_path.size(), sizeof(ref.relative_path) - 1);
		std::memcpy(ref.relative_path, relative_path.data(), n);
		ref.relative_path[n] = '\0';
		ref.path_hash = fnv1a64(relative_path);
		ref.content_hash = content_hash;
		return ref;
	}

	RegistryEntry Archive::make_registry_entry(uint32_t voxel_key, uint32_t material_id, RegistryFlags flags, std::string_view name)
	{
		RegistryEntry e{};
		e.voxel_key = voxel_key;
		e.material_id = material_id;
		e.flags = to_underlying(flags);
		e.name_hash = name.empty() ? 0 : fnv1a64(name);
		return e;
	}

	// -------------------------------------------------------------------------------------------
	// Byte sources
	// -------------------------------------------------------------------------------------------

	MemoryByteSource::MemoryByteSource(std::span<const std::byte> bytes, bool copy)
	{
		if (copy) {
			owned_.assign(bytes.begin(), bytes.end());
			view_ = std::span<const std::byte>(owned_.data(), owned_.size());
		}
		else {
			view_ = bytes;
		}
	}
	uint64_t MemoryByteSource::size() const
	{
		return static_cast<uint64_t>(view_.size());
	}
	void MemoryByteSource::read(uint64_t offset, std::span<std::byte> dst) const
	{
		if (offset > view_.size() || dst.size() > view_.size() - offset) throw std::runtime_error("[bvx]: memory source read out of bounds");
		if (!dst.empty()) std::memcpy(dst.data(), view_.data() + offset, dst.size());
	}

#if defined(BSVX_HAS_PREAD)
	struct FileByteSource::Impl {
		int fd = -1;
		~Impl() { if (fd >= 0) ::close(fd); }
	};

	FileByteSource::FileByteSource(const std::string& path)
		: impl_(std::make_unique<Impl>())
	{
		// POSIX takes the UTF-8 bytes verbatim; no conversion needed or wanted here.
		impl_->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (impl_->fd < 0) throw std::runtime_error("[bvx]: could not open region file: " + path);

		const off_t end = ::lseek(impl_->fd, 0, SEEK_END);
		if (end < 0) throw std::runtime_error("[bvx]: failed to query region file size: " + path);
		size_ = static_cast<uint64_t>(end);
	}
	FileByteSource::~FileByteSource() = default;
	void FileByteSource::read(uint64_t offset, std::span<std::byte> dst) const
	{
		if (offset > size_ || dst.size() > size_ - offset) throw std::runtime_error("[bvx]: file source read out of bounds");

		// pread carries its own offset, so concurrent reads need no lock at all.
		size_t done = 0;
		while (done < dst.size()) {
			const ssize_t n = ::pread(impl_->fd, dst.data() + done, dst.size() - done, static_cast<off_t>(offset + done));
			if (n < 0) {
				if (errno == EINTR) continue;
				throw std::runtime_error("[bvx]: region file read failed");
			}
			if (n == 0) throw std::runtime_error("[bvx]: unexpected end of region file");
			done += static_cast<size_t>(n);
		}
	}
#else
	struct FileByteSource::Impl {
		std::mutex mutex;
		std::ifstream stream;
		explicit Impl(const std::string& path) : stream(path_from_utf8(path), std::ios::binary) {}
	};

	FileByteSource::FileByteSource(const std::string& path)
		: impl_(std::make_unique<Impl>(path))
	{
		if (!impl_->stream) throw std::runtime_error("[bvx]: could not open region file: " + path);
		impl_->stream.seekg(0, std::ios::end);
		const auto end = impl_->stream.tellg();
		if (end < 0) throw std::runtime_error("[bvx]: failed to query region file size: " + path);
		size_ = static_cast<uint64_t>(end);
	}
	FileByteSource::~FileByteSource() = default;
	void FileByteSource::read(uint64_t offset, std::span<std::byte> dst) const
	{
		if (offset > size_ || dst.size() > size_ - offset) throw std::runtime_error("[bvx]: file source read out of bounds");
		if (dst.empty()) return;

		// One ifstream shared by every reader thread, so the seek+read pair has to be atomic.
		const std::lock_guard<std::mutex> guard(impl_->mutex);
		impl_->stream.clear();
		impl_->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		impl_->stream.read(reinterpret_cast<char*>(dst.data()), static_cast<std::streamsize>(dst.size()));
		if (!impl_->stream) throw std::runtime_error("[bvx]: region file read failed");
	}
#endif
	uint64_t FileByteSource::size() const
	{
		return size_;
	}

	// -------------------------------------------------------------------------------------------
	// RegionReader
	// -------------------------------------------------------------------------------------------

	namespace {
		template <TriviallySerializable T>
		std::vector<T> read_table(const ByteSource& src, uint64_t offset, size_t count, const char* what)
		{
			const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(T);
			if (offset > src.size() || bytes > src.size() - offset) throw std::runtime_error(std::string("[bvx]: out-of-bounds ") + what);
			std::vector<T> out(count);
			if (count != 0) src.read(offset, std::as_writable_bytes(std::span<T>(out.data(), out.size())));
			return out;
		}
	}

	RegionReader RegionReader::open(std::shared_ptr<ByteSource> source)
	{
		if (!source) throw std::invalid_argument("[bvx]: RegionReader requires a byte source");

		RegionReader out{};
		out.source_ = std::move(source);
		const ByteSource& src = *out.source_;

		if (src.size() < sizeof(DiskHeader)) throw std::runtime_error("[bvx]: file too small");
		src.read(0, std::as_writable_bytes(std::span(&out.header_, 1)));

		const DiskHeader& header = out.header_;
		if (header.magic != BVX_MAGIC) throw std::runtime_error("[bvx]: black magic, unsupported file type");
		if (header.version != BVX_VERSION) throw std::runtime_error("[bvx]: unsupported version");
		if (header.file_size != src.size()) throw std::runtime_error("[bvx]: file size mismatch");

		out.chunk_map_ = read_table<ChunkMapEntry>(src, header.chunk_map_offset, header.chunk_count, "chunk map");
		out.chunk_summaries_ = read_table<ChunkSummary>(src, header.summary_table_offset, header.chunk_count, "summary table");
		out.directory_ = read_table<SectionRecord>(src, header.section_dir_offset, header.section_count, "section directory");

		out.entries_.resize(out.directory_.size());
		for (size_t i = 0; i < out.directory_.size(); ++i) {
			const SectionRecord& rec = out.directory_[i];
			if (rec.entry_stride != sizeof(OffsetSizeEntry)) throw std::runtime_error("[bvx]: unsupported section entry stride");
			if (rec.blob_offset > src.size() || rec.blob_size > src.size() - rec.blob_offset) throw std::runtime_error("[bvx]: out-of-bounds section blob");

			out.entries_[i] = read_table<OffsetSizeEntry>(src, rec.entry_table_offset, rec.entry_count, "section entry table");

			// The world desc and the metadata are description, not payload -- the first carries the
			// geometry needed to decode anything else, the second is what a host inspects before
			// deciding whether to page the region in at all. Both are read up front; the voxel
			// blobs are not.
			const SectionType type = static_cast<SectionType>(rec.section_type);
			if (type == SectionType::WORLD_DESC || type == SectionType::METADATA) {
				if (out.entries_[i].size() != 1 || out.entries_[i][0].offset != 0 || out.entries_[i][0].size != rec.blob_size) throw std::runtime_error("[bvx]: malformed description section");
				std::vector<std::byte> blob(static_cast<size_t>(rec.blob_size));
				if (!blob.empty()) src.read(rec.blob_offset, std::span<std::byte>(blob.data(), blob.size()));

				if (type == SectionType::WORLD_DESC) out.standalone_ = parse_world_desc_blob(blob);
				else out.metadata_ = parse_metadata_blob(blob);
			}
		}

		if ((header.flags & to_underlying(FileFlags::STANDALONE)) != 0u && !out.standalone_) throw std::runtime_error("[bvx]: standalone flag set but WORLD_DESC section missing");

		out.chunk_index_.reserve(out.chunk_map_.size());
		for (uint32_t i = 0; i < out.chunk_map_.size(); ++i) {
			const ChunkMapEntry& c = out.chunk_map_[i];
			out.chunk_index_.emplace(Archive::make_chunk_key(c.local_chunk_x, c.local_chunk_y, c.local_chunk_z), i);
		}

		return out;
	}
	RegionReader RegionReader::open_file(const std::string& path)
	{
		return open(std::make_shared<FileByteSource>(path));
	}
	RegionReader RegionReader::open_memory(std::span<const std::byte> bytes, bool copy)
	{
		return open(std::make_shared<MemoryByteSource>(bytes, copy));
	}
	GeometryDesc RegionReader::resolve_geometry() const
	{
		if (standalone_) return standalone_->geometry;
		if (geometry_override_) return *geometry_override_;
		throw std::runtime_error("[bvx]: region carries no geometry; call set_geometry_override with the world's geometry first");
	}
	std::optional<uint32_t> RegionReader::try_find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const
	{
		const auto it = chunk_index_.find(Archive::make_chunk_key(x, y, z));
		if (it == chunk_index_.end()) return std::nullopt;
		return it->second;
	}
	const OffsetSizeEntry* RegionReader::find_chunk_entry(SectionType type, uint32_t chunk_index) const
	{
		for (size_t i = 0; i < directory_.size(); ++i) {
			if (static_cast<SectionType>(directory_[i].section_type) != type) continue;
			if (chunk_index >= entries_[i].size()) return nullptr;
			const OffsetSizeEntry& entry = entries_[i][chunk_index];
			return entry.offset == INVALID_OFFSET ? nullptr : &entry;
		}
		return nullptr;
	}
	std::optional<std::vector<std::byte>> RegionReader::read_chunk_payload(SectionType type, uint32_t chunk_index, uint16_t* out_codec, uint16_t* out_entry_flags) const
	{
		for (size_t i = 0; i < directory_.size(); ++i) {
			if (static_cast<SectionType>(directory_[i].section_type) != type) continue;
			if (chunk_index >= entries_[i].size()) return std::nullopt;

			const OffsetSizeEntry& entry = entries_[i][chunk_index];
			if (entry.offset == INVALID_OFFSET) return std::nullopt;

			const SectionRecord& rec = directory_[i];
			if (entry.offset > rec.blob_size || entry.size > rec.blob_size - entry.offset) throw std::runtime_error("[bvx]: payload entry out of bounds");

			std::vector<std::byte> payload(entry.size);
			if (!payload.empty()) source_->read(rec.blob_offset + entry.offset, std::span<std::byte>(payload.data(), payload.size()));

			if (out_codec) *out_codec = entry.codec;
			if (out_entry_flags) *out_entry_flags = entry.flags;
			return payload;
		}
		return std::nullopt;
	}
	std::vector<uint32_t> RegionReader::decode_chunk_voxels(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z) const
	{
		const GeometryDesc g = resolve_geometry();
		const size_t voxel_count = Archive::chunk_voxel_count(g);

		const auto chunk_index = try_find_chunk_index(chunk_x, chunk_y, chunk_z);
		if (!chunk_index) throw std::runtime_error("[bvx]: decode_chunk_voxels chunk not found");

		uint16_t codec = 0;
		const auto payload = read_chunk_payload(SectionType::VOXELS, *chunk_index, &codec);
		if (!payload) return std::vector<uint32_t>(voxel_count, 0u);

		return decode_voxel_payload(static_cast<VoxelCodec>(codec), std::span<const std::byte>(payload->data(), payload->size()), g.chunk_size_x, g.chunk_size_y, g.chunk_size_z);
	}
	bool RegionReader::verify_integrity() const
	{
		std::vector<std::byte> bytes(static_cast<size_t>(source_->size()));
		if (bytes.empty()) return false;
		source_->read(0, std::span<std::byte>(bytes.data(), bytes.size()));

		const uint64_t expected = reinterpret_cast<const DiskHeader*>(bytes.data())->crc64;
		reinterpret_cast<DiskHeader*>(bytes.data())->crc64 = 0;
		return fnv1a64(std::span<const std::byte>(bytes.data(), bytes.size())) == expected;
	}
	Archive RegionReader::load_full() const
	{
		std::vector<std::byte> bytes(static_cast<size_t>(source_->size()));
		if (!bytes.empty()) source_->read(0, std::span<std::byte>(bytes.data(), bytes.size()));
		return Archive::deserialize(std::span<const std::byte>(bytes.data(), bytes.size()));
	}
	uint64_t RegionReader::resident_bytes() const noexcept
	{
		uint64_t total = sizeof(DiskHeader);
		total += chunk_map_.size() * sizeof(ChunkMapEntry);
		total += chunk_summaries_.size() * sizeof(ChunkSummary);
		total += directory_.size() * sizeof(SectionRecord);
		for (const auto& table : entries_) total += table.size() * sizeof(OffsetSizeEntry);
		if (standalone_) {
			total += standalone_->texture_refs.size() * sizeof(BtxRef);
			total += standalone_->registry_entries.size() * sizeof(RegistryEntry);
		}
		for (const auto& [key, value] : metadata_) total += key.size() + value.size();
		return total;
	}
}

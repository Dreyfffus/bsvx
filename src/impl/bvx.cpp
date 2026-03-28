#include "bvx.h"
#include "codec.h"
#include <string_view>
#include <unordered_map>

namespace bsvx::bvx {

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
	uint32_t Archive::find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const
	{
		const uint64_t key = make_chunk_key(x, y, z);
		for (uint32_t i = 0; i < chunk_map.size(); i++) {
			const auto& c = chunk_map[i];
			if (make_chunk_key(c.local_chunk_x, c.local_chunk_y, c.local_chunk_z) == key) return i;
		}
		throw std::runtime_error("[bvx]: chunk not found");
	}
	std::optional<uint32_t> Archive::try_find_chunk_index(uint16_t x, uint16_t y, uint16_t z) const
	{
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

		chunk_map.push_back(entry);
		chunk_summaries.push_back({});

		for (PayloadSection& sec : sections) if (sec.chunk_associated) sec.entries.push_back({});

		return static_cast<uint32_t>(chunk_map.size() - 1);
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
	std::unordered_map<uint32_t, RegistryEntry> Archive::build_registry_lookup() const
	{
		std::unordered_map<uint32_t, RegistryEntry> lookup;
		if (!standalone) return lookup;
		lookup.reserve(standalone->registry_entries.size());
		for (const RegistryEntry& reg : standalone->registry_entries) lookup.emplace(reg.voxel_key, reg);
		return lookup;

	}
	void Archive::set_chunk_voxels_dense(uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, std::span<const uint32_t> dense, const GeometryDesc* geometry_override, VoxelCodec req_codec)
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

		const auto registry_lookup = build_registry_lookup();
		const auto* registry_ptr = registry_lookup.empty() ? nullptr : &registry_lookup;
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
	bool Archive::save_to_file(const std::string& path) const 
	{
		std::ofstream os(path, std::ios::binary);
		if (!os) return false;
		serialize(os);
		return static_cast<bool>(os);
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

			PayloadSection sec{};
			sec.type = type;
			sec.default_codec = rec.default_codec;
			sec.chunk_associated = (entries.size() == out.chunk_map.size());
			sec.entries = std::move(entries);
			sec.blob = std::move(blob);
			out.sections.push_back(std::move(sec));
		}

		if ((header.flags & to_underlying(FileFlags::STANDALONE)) != 0u && !out.standalone) throw std::runtime_error("[bvx]: standalone flag set but WORLD_DESC section missing");

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
		std::ifstream is(path, std::ios::binary);
		if (!is) return std::nullopt;
		return std::optional<Archive>{deserialize(is)};
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
}

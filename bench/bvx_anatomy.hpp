#pragma once
// Where the bytes of a .bvx actually go. Reads the on-disk structures directly through the
// library's own layout headers, so a size result can be attributed to the header, the chunk map,
// the summary table or the voxel payloads rather than just reported as a total.
//
// This walks the file the way the format documents it: DiskHeader at offset 0 names the chunk
// map, summary table and section directory; each SectionRecord names an entry table of
// OffsetSizeEntry (which carries the per-chunk codec) and a blob.

#include "bvx_header.h"

#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace anatomy {

	using namespace bsvx;
	using namespace bsvx::bvx;

	struct Report final {
		uint64_t file_size = 0;
		uint32_t chunk_count = 0;
		uint64_t header_bytes = sizeof(DiskHeader);
		uint64_t chunk_map_bytes = 0;
		uint64_t summary_bytes = 0;
		uint64_t section_dir_bytes = 0;
		uint64_t entry_table_bytes = 0;
		uint64_t voxel_blob_bytes = 0;
		uint64_t other_blob_bytes = 0;     // world desc, metadata, baked payloads
		std::map<uint16_t, uint32_t> codec_chunks;   // codec -> chunks using it
		std::map<uint16_t, uint64_t> codec_bytes;    // codec -> payload bytes
		// Voxel payload size per chunk, indexed by chunk ordinal -- the VOXELS entry table runs
		// parallel to the chunk map, so this lines up with the order chunks were written.
		std::vector<uint32_t> payload_per_chunk;

		uint64_t fixed_overhead() const
		{
			return header_bytes + chunk_map_bytes + summary_bytes + section_dir_bytes + entry_table_bytes;
		}
	};

	inline const char* codec_name(uint16_t codec)
	{
		switch (static_cast<VoxelCodec>(codec)) {
		case VoxelCodec::CHUNK_EMPTY:        return "empty";
		case VoxelCodec::CHUNK_UNIFORM:      return "uniform";
		case VoxelCodec::PALLETE_BITPACK:    return "palette";
		case VoxelCodec::SPARSE_LIST:        return "sparse";
		case VoxelCodec::Y_COLUMN_INTERVALS: return "y-runs";
		case VoxelCodec::RAW_DENSE:          return "raw";
		case VoxelCodec::SPARSE_PACKED:      return "packed";
		default:                             return "?";
		}
	}

	inline Report inspect(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in) throw std::runtime_error("[anatomy]: cannot open " + path);
		const std::streamsize size = in.tellg();
		in.seekg(0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		in.read(reinterpret_cast<char*>(bytes.data()), size);

		const auto at = [&](uint64_t offset, void* dst, size_t n) {
			if (offset + n > bytes.size()) throw std::runtime_error("[anatomy]: read past end of " + path);
			std::memcpy(dst, bytes.data() + offset, n);
			};

		Report report;
		report.file_size = static_cast<uint64_t>(size);

		DiskHeader header{};
		at(0, &header, sizeof(header));
		if (header.magic != BVX_MAGIC) throw std::runtime_error("[anatomy]: not a .bvx: " + path);

		report.chunk_count = header.chunk_count;
		report.chunk_map_bytes = static_cast<uint64_t>(header.chunk_count) * sizeof(ChunkMapEntry);
		report.section_dir_bytes = static_cast<uint64_t>(header.section_count) * sizeof(SectionRecord);

		// The summary table is sized by the highest summary_index any chunk points at, not by the
		// chunk count -- an edited region can leave stale summaries behind.
		uint32_t summary_slots = 0;
		for (uint32_t i = 0; i < header.chunk_count; ++i) {
			ChunkMapEntry entry{};
			at(header.chunk_map_offset + static_cast<uint64_t>(i) * sizeof(ChunkMapEntry), &entry, sizeof(entry));
			summary_slots = std::max(summary_slots, entry.summary_index + 1u);
		}
		report.summary_bytes = static_cast<uint64_t>(summary_slots) * sizeof(ChunkSummary);

		for (uint16_t s = 0; s < header.section_count; ++s) {
			SectionRecord section{};
			at(header.section_dir_offset + static_cast<uint64_t>(s) * sizeof(SectionRecord), &section, sizeof(section));

			report.entry_table_bytes += static_cast<uint64_t>(section.entry_count) * sizeof(OffsetSizeEntry);

			const bool is_voxels = section.section_type == static_cast<uint32_t>(SectionType::VOXELS);
			if (!is_voxels) {
				report.other_blob_bytes += section.blob_size;
				continue;
			}

			report.voxel_blob_bytes += section.blob_size;
			for (uint32_t e = 0; e < section.entry_count; ++e) {
				OffsetSizeEntry entry{};
				at(section.entry_table_offset + static_cast<uint64_t>(e) * sizeof(OffsetSizeEntry), &entry, sizeof(entry));
				if (entry.offset == INVALID_OFFSET) continue;
				report.codec_chunks[entry.codec] += 1u;
				report.codec_bytes[entry.codec] += entry.size;
				if (report.payload_per_chunk.size() <= e) report.payload_per_chunk.resize(e + 1u, 0u);
				report.payload_per_chunk[e] = entry.size;
			}
		}

		return report;
	}

}

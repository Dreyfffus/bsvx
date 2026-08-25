#pragma once
// A minimal, deliberately fast MagicaVoxel .vox reader/writer, here only so the benchmark has
// something honest to compare against. It is not part of the library and knows nothing about bsvx.
//
// .vox layout (version 150):
//   "VOX " int32 version
//   chunk MAIN { content 0, children:
//       [PACK] int32 model_count
//       per model: SIZE { int32 x, y, z }  XYZI { int32 n, n * (u8 x, u8 y, u8 z, u8 color) }
//       [RGBA] 256 * (u8 r, g, b, a)   -- file entry i is colour index i+1; index 0 is empty
//   }
// Every chunk header is id[4], int32 content_bytes, int32 children_bytes, so an unknown chunk can
// be skipped without understanding it -- which is what the reader does with the scene-graph
// chunks (nTRN/nGRP/nSHP/LAYR/MATL) a modern MagicaVoxel file carries.

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vox {

	struct Voxel final { uint8_t x, y, z, color; };

	struct Model final {
		int32_t size_x = 0, size_y = 0, size_z = 0;
		std::vector<Voxel> voxels;
	};

	struct Scene final {
		std::vector<Model> models;
		std::array<uint8_t, 1024> palette{};   // 256 RGBA entries, file order
	};

	// The default MagicaVoxel palette is irrelevant to size and time; any 256 entries cost 1024
	// bytes. Benchmarks write their own.
	inline void append_u32(std::vector<std::byte>& out, uint32_t v)
	{
		std::byte bytes[4];
		std::memcpy(bytes, &v, 4);
		out.insert(out.end(), bytes, bytes + 4);
	}

	inline void append_id(std::vector<std::byte>& out, const char (&id)[5])
	{
		for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(id[i]));
	}

	inline void append_chunk(std::vector<std::byte>& out, const char (&id)[5], const std::vector<std::byte>& content)
	{
		append_id(out, id);
		append_u32(out, static_cast<uint32_t>(content.size()));
		append_u32(out, 0u);
		out.insert(out.end(), content.begin(), content.end());
	}

	inline std::vector<std::byte> encode(const Scene& scene)
	{
		std::vector<std::byte> children;

		if (scene.models.size() > 1) {
			std::vector<std::byte> pack;
			append_u32(pack, static_cast<uint32_t>(scene.models.size()));
			append_chunk(children, "PACK", pack);
		}

		for (const Model& model : scene.models) {
			std::vector<std::byte> size;
			append_u32(size, static_cast<uint32_t>(model.size_x));
			append_u32(size, static_cast<uint32_t>(model.size_y));
			append_u32(size, static_cast<uint32_t>(model.size_z));
			append_chunk(children, "SIZE", size);

			std::vector<std::byte> xyzi;
			xyzi.reserve(4 + 4 * model.voxels.size());
			append_u32(xyzi, static_cast<uint32_t>(model.voxels.size()));
			for (const Voxel& v : model.voxels) {
				xyzi.push_back(static_cast<std::byte>(v.x));
				xyzi.push_back(static_cast<std::byte>(v.y));
				xyzi.push_back(static_cast<std::byte>(v.z));
				xyzi.push_back(static_cast<std::byte>(v.color));
			}
			append_chunk(children, "XYZI", xyzi);
		}

		std::vector<std::byte> rgba;
		rgba.reserve(1024);
		for (uint8_t byte : scene.palette) rgba.push_back(static_cast<std::byte>(byte));
		append_chunk(children, "RGBA", rgba);

		std::vector<std::byte> out;
		out.reserve(children.size() + 32);
		append_id(out, "VOX ");
		append_u32(out, 150u);
		append_id(out, "MAIN");
		append_u32(out, 0u);
		append_u32(out, static_cast<uint32_t>(children.size()));
		out.insert(out.end(), children.begin(), children.end());
		return out;
	}

	// Parses bytes already in memory, so a caller can time file I/O and parsing separately.
	inline Scene decode(const std::byte* data, size_t size)
	{
		const auto read_u32 = [&](size_t offset) -> uint32_t {
			if (offset + 4 > size) throw std::runtime_error("[vox]: truncated");
			uint32_t v = 0;
			std::memcpy(&v, data + offset, 4);
			return v;
			};

		if (size < 8 || std::memcmp(data, "VOX ", 4) != 0) throw std::runtime_error("[vox]: bad magic");

		Scene scene;
		size_t cursor = 8;   // magic + version

		while (cursor + 12 <= size) {
			char id[5] = {};
			std::memcpy(id, data + cursor, 4);
			const uint32_t content = read_u32(cursor + 4);
			cursor += 12;
			if (cursor + content > size) throw std::runtime_error("[vox]: chunk overruns file");

			// MAIN and the scene-graph chunks carry their payload in *children*, which follow the
			// content bytes -- so skipping content alone walks into them, and anything not
			// recognised below is skipped whole.
			if (std::memcmp(id, "SIZE", 4) == 0) {
				Model model;
				model.size_x = static_cast<int32_t>(read_u32(cursor + 0));
				model.size_y = static_cast<int32_t>(read_u32(cursor + 4));
				model.size_z = static_cast<int32_t>(read_u32(cursor + 8));
				scene.models.push_back(std::move(model));
			}
			else if (std::memcmp(id, "XYZI", 4) == 0) {
				if (scene.models.empty()) throw std::runtime_error("[vox]: XYZI before SIZE");
				const uint32_t count = read_u32(cursor);
				Model& model = scene.models.back();
				model.voxels.resize(count);
				// One bulk copy: Voxel is four bytes in exactly the file's order.
				static_assert(sizeof(Voxel) == 4);
				std::memcpy(model.voxels.data(), data + cursor + 4, static_cast<size_t>(count) * 4);
			}
			else if (std::memcmp(id, "RGBA", 4) == 0) {
				std::memcpy(scene.palette.data(), data + cursor, 1024);
			}

			cursor += content;                // unknown chunks are skipped whole
		}

		return scene;
	}

	inline std::vector<std::byte> read_file(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in) throw std::runtime_error("[vox]: cannot open " + path);
		const std::streamsize size = in.tellg();
		in.seekg(0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		in.read(reinterpret_cast<char*>(bytes.data()), size);
		return bytes;
	}

	inline void write_file(const std::string& path, const std::vector<std::byte>& bytes)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) throw std::runtime_error("[vox]: cannot write " + path);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

}

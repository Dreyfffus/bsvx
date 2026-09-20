#pragma once
// A minimal, deliberately fast MagicaVoxel .vox reader/writer. It feeds the command-line converter
// and gives the benchmark something honest to compare against. It is not part of the library and
// knows nothing about bsvx.
//
// .vox layout (version 150):
//   "VOX " int32 version
//   chunk MAIN { content 0, children:
//       [PACK] int32 model_count
//       per model: SIZE { int32 x, y, z }  XYZI { int32 n, n * (u8 x, u8 y, u8 z, u8 color) }
//       [RGBA] 256 * (u8 r, g, b, a)   -- file entry i is colour index i+1; index 0 is empty
//   }
// Every chunk header is id[4], int32 content_bytes, int32 children_bytes, so an unknown chunk can
// be skipped without understanding it -- which is what the reader does with MATL, rOBJ, rCAM and
// anything else it has no use for.
//
// The scene graph (MagicaVoxel 0.99+) is read and flattened into Scene::instances:
//   nTRN { node_id, DICT{_name,_hidden}, child_id, -1, layer_id, frame_count, frame_count*DICT{_r,_t,_f} }
//   nGRP { node_id, DICT, child_count, child_count * child_id }
//   nSHP { node_id, DICT, model_count, model_count * (model_id, DICT{_f}) }
//   LAYR { layer_id, DICT{_name,_hidden}, -1 }
// A node id is the index into a table, and node 0 is the root. Only the first keyframe of an nTRN
// is used. `_r` packs a signed axis permutation into one byte -- bits 0-1 and 2-3 index the
// non-zero column of rows 0 and 1, bits 4-6 the signs of rows 0-2 -- and `_t` is "x y z".
//
// Placement: an instance's transform is applied about the model's pivot, floor(size / 2), and it
// acts on the *corners* of a voxel cell, not its index. That is what makes a mirrored axis land a
// cell at -c-1 rather than -c, and it is why Instance::world_cell maps both corners and keeps the
// smaller. A file with no scene graph (pre-0.99, or the PACK form) yields one identity instance
// per model.
//
// Coordinates are MagicaVoxel's own: X right, Y away, Z up.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vox {

	struct Voxel final { uint8_t x, y, z, color; };

	struct Model final {
		int32_t size_x = 0, size_y = 0, size_z = 0;
		std::vector<Voxel> voxels;
	};

	// world = rotation * (local - pivot) + translation, in integer voxel units. The rotation is a
	// signed permutation, so every entry is -1, 0 or 1.
	struct Transform final {
		int32_t rotation[3][3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
		int32_t translation[3] = { 0, 0, 0 };

		static Transform identity() { return {}; }

		// (this) after (inner): apply inner first, then this.
		Transform compose(const Transform& inner) const
		{
			Transform out;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					int32_t sum = 0;
					for (int k = 0; k < 3; ++k) sum += rotation[i][k] * inner.rotation[k][j];
					out.rotation[i][j] = sum;
				}
				out.translation[i] = translation[i];
				for (int k = 0; k < 3; ++k) out.translation[i] += rotation[i][k] * inner.translation[k];
			}
			return out;
		}

		void apply(const int32_t (&p)[3], int32_t (&out)[3]) const
		{
			for (int i = 0; i < 3; ++i) {
				out[i] = translation[i];
				for (int k = 0; k < 3; ++k) out[i] += rotation[i][k] * p[k];
			}
		}
	};

	struct Instance final {
		uint32_t model = 0;
		Transform transform;        // model-local (pivot-relative) -> scene, all groups folded in
		std::string name;           // the innermost nTRN's _name, or empty
		bool hidden = false;        // the instance, any group above it, or its layer is hidden

		// Where a voxel at model-local cell (x, y, z) lands in the scene.
		void world_cell(const Model& m, int32_t x, int32_t y, int32_t z, int32_t (&out)[3]) const
		{
			const int32_t pivot[3] = { m.size_x / 2, m.size_y / 2, m.size_z / 2 };
			const int32_t lo[3] = { x - pivot[0], y - pivot[1], z - pivot[2] };
			const int32_t hi[3] = { lo[0] + 1, lo[1] + 1, lo[2] + 1 };
			int32_t a[3], b[3];
			transform.apply(lo, a);
			transform.apply(hi, b);
			for (int i = 0; i < 3; ++i) out[i] = a[i] < b[i] ? a[i] : b[i];
		}
	};

	struct Scene final {
		std::vector<Model> models;
		std::array<uint8_t, 1024> palette{};   // 256 RGBA entries, file order
		std::vector<Instance> instances;       // scene order; later instances paint over earlier
		bool has_scene_graph = false;
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

	namespace detail {

		struct Node final {
			enum class Kind { NONE, TRANSFORM, GROUP, SHAPE } kind = Kind::NONE;
			// TRANSFORM
			int32_t child = -1;
			int32_t layer = -1;
			Transform transform;
			std::string name;
			bool hidden = false;
			// GROUP and SHAPE
			std::vector<int32_t> children;   // node ids, or model ids for a shape
		};

		struct Layer final { bool hidden = false; };

		// A cursor over a chunk's content with the bounds checks the format needs.
		struct Reader final {
			const std::byte* data; size_t size; size_t at = 0;

			uint32_t u32()
			{
				if (at + 4 > size) throw std::runtime_error("[vox]: truncated chunk");
				uint32_t v = 0; std::memcpy(&v, data + at, 4); at += 4; return v;
			}
			int32_t i32() { return static_cast<int32_t>(u32()); }
			std::string str()
			{
				const uint32_t n = u32();
				if (at + n > size) throw std::runtime_error("[vox]: truncated string");
				std::string s(reinterpret_cast<const char*>(data + at), n); at += n; return s;
			}
			std::vector<std::pair<std::string, std::string>> dict()
			{
				std::vector<std::pair<std::string, std::string>> out;
				const uint32_t n = u32();
				for (uint32_t i = 0; i < n; ++i) { std::string k = str(); std::string v = str(); out.emplace_back(std::move(k), std::move(v)); }
				return out;
			}
		};

		inline const std::string* find(const std::vector<std::pair<std::string, std::string>>& dict, const char* key)
		{
			for (const auto& [k, v] : dict) if (k == key) return &v;
			return nullptr;
		}

		inline Transform transform_from(const std::string* r, const std::string* t)
		{
			Transform out;
			if (r) {
				const int bits = std::atoi(r->c_str());
				const int row0 = bits & 3, row1 = (bits >> 2) & 3;
				if (row0 > 2 || row1 > 2 || row0 == row1) throw std::runtime_error("[vox]: invalid _r rotation " + *r);
				const int row2 = 3 - row0 - row1;
				std::memset(out.rotation, 0, sizeof(out.rotation));
				out.rotation[0][row0] = (bits & (1 << 4)) ? -1 : 1;
				out.rotation[1][row1] = (bits & (1 << 5)) ? -1 : 1;
				out.rotation[2][row2] = (bits & (1 << 6)) ? -1 : 1;
			}
			if (t) {
				if (std::sscanf(t->c_str(), "%d %d %d", &out.translation[0], &out.translation[1], &out.translation[2]) != 3)
					throw std::runtime_error("[vox]: invalid _t translation '" + *t + "'");
			}
			return out;
		}

		// Depth-first from the root, folding every nTRN on the path into one transform per shape.
		inline void flatten(const std::vector<Node>& nodes, const std::vector<Layer>& layers, int32_t id,
			const Transform& parent, bool hidden, const std::string& name, size_t model_count, int depth, std::vector<Instance>& out)
		{
			if (id < 0 || static_cast<size_t>(id) >= nodes.size()) throw std::runtime_error("[vox]: scene graph references node " + std::to_string(id) + " which does not exist");
			if (depth > 256) throw std::runtime_error("[vox]: scene graph is cyclic or absurdly deep");
			const Node& node = nodes[static_cast<size_t>(id)];
			switch (node.kind) {
			case Node::Kind::TRANSFORM: {
				bool h = hidden || node.hidden;
				if (node.layer >= 0 && static_cast<size_t>(node.layer) < layers.size() && layers[static_cast<size_t>(node.layer)].hidden) h = true;
				flatten(nodes, layers, node.child, parent.compose(node.transform), h, node.name.empty() ? name : node.name, model_count, depth + 1, out);
				break;
			}
			case Node::Kind::GROUP:
				for (int32_t child : node.children) flatten(nodes, layers, child, parent, hidden, name, model_count, depth + 1, out);
				break;
			case Node::Kind::SHAPE:
				for (int32_t model : node.children) {
					if (model < 0 || static_cast<size_t>(model) >= model_count) throw std::runtime_error("[vox]: nSHP references model " + std::to_string(model) + " which does not exist");
					Instance instance;
					instance.model = static_cast<uint32_t>(model);
					instance.transform = parent;
					instance.name = name;
					instance.hidden = hidden;
					out.push_back(std::move(instance));
				}
				break;
			case Node::Kind::NONE:
				throw std::runtime_error("[vox]: scene graph references node " + std::to_string(id) + " which was never defined");
			}
		}

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
		std::vector<detail::Node> nodes;
		std::vector<detail::Layer> layers;
		const auto node_at = [&](int32_t id) -> detail::Node& {
			if (id < 0 || id > 1'000'000) throw std::runtime_error("[vox]: unreasonable node id " + std::to_string(id));
			if (static_cast<size_t>(id) >= nodes.size()) nodes.resize(static_cast<size_t>(id) + 1);
			return nodes[static_cast<size_t>(id)];
			};
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
				if (content < 1024) throw std::runtime_error("[vox]: short RGBA chunk");
				std::memcpy(scene.palette.data(), data + cursor, 1024);
			}
			else if (std::memcmp(id, "nTRN", 4) == 0) {
				detail::Reader r{ data + cursor, content };
				const int32_t node_id = r.i32();
				const auto attrs = r.dict();
				detail::Node& node = node_at(node_id);
				node.kind = detail::Node::Kind::TRANSFORM;
				node.child = r.i32();
				r.i32();                                   // reserved, -1
				node.layer = r.i32();
				if (const std::string* n = detail::find(attrs, "_name")) node.name = *n;
				if (const std::string* h = detail::find(attrs, "_hidden")) node.hidden = *h == "1";
				const uint32_t frames = r.u32();
				for (uint32_t f = 0; f < frames; ++f) {
					const auto frame = r.dict();
					if (f == 0) node.transform = detail::transform_from(detail::find(frame, "_r"), detail::find(frame, "_t"));
				}
				scene.has_scene_graph = true;
			}
			else if (std::memcmp(id, "nGRP", 4) == 0) {
				detail::Reader r{ data + cursor, content };
				const int32_t node_id = r.i32();
				r.dict();
				detail::Node& node = node_at(node_id);
				node.kind = detail::Node::Kind::GROUP;
				const uint32_t count = r.u32();
				for (uint32_t i = 0; i < count; ++i) node.children.push_back(r.i32());
				scene.has_scene_graph = true;
			}
			else if (std::memcmp(id, "nSHP", 4) == 0) {
				detail::Reader r{ data + cursor, content };
				const int32_t node_id = r.i32();
				r.dict();
				detail::Node& node = node_at(node_id);
				node.kind = detail::Node::Kind::SHAPE;
				const uint32_t count = r.u32();
				for (uint32_t i = 0; i < count; ++i) { node.children.push_back(r.i32()); r.dict(); }
				scene.has_scene_graph = true;
			}
			else if (std::memcmp(id, "LAYR", 4) == 0) {
				detail::Reader r{ data + cursor, content };
				const int32_t layer_id = r.i32();
				const auto attrs = r.dict();
				if (layer_id >= 0 && layer_id < 100'000) {
					if (static_cast<size_t>(layer_id) >= layers.size()) layers.resize(static_cast<size_t>(layer_id) + 1);
					if (const std::string* h = detail::find(attrs, "_hidden")) layers[static_cast<size_t>(layer_id)].hidden = *h == "1";
				}
			}

			cursor += content;                // unknown chunks are skipped whole
		}

		if (scene.has_scene_graph) {
			detail::flatten(nodes, layers, 0, Transform::identity(), false, std::string{}, scene.models.size(), 0, scene.instances);
		}
		else {
			for (uint32_t i = 0; i < scene.models.size(); ++i) {
				Instance instance;
				instance.model = i;
				scene.instances.push_back(std::move(instance));
			}
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

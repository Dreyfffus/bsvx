// bsvx -- the command-line tool.
//
//   bsvx --convert <source.vox> <dest.bvx> [--chunk N] [--model N] [--include-hidden]
//
// Converts a MagicaVoxel file into a standalone .bvx region through the public C ABI, exactly as
// any other importer would: one world, one region, chunks written with the automatic codec, the
// .vox palette carried as registry colours so the file renders meaningfully before it has a .btx.
//
// The whole scene is converted: every model the scene graph places, at the position and rotation
// its nTRN chain gives it, with hidden instances and hidden layers left out unless asked for. A
// file without a scene graph is its models at the origin. --model N converts one model alone, in
// its own local frame, ignoring the scene graph.
//
// MagicaVoxel is Z-up with Y away from the viewer -- X_RIGHT_Z_UP_Y_FORWARD in the library's
// terms. A world is authored in the canonical Y-up frame (bsvx_world_set_desc refuses any other
// tag; conventions are reached by conversion, never by relabelling), so every cell goes through
// bsvx_convert_cell on the way in, the same as the Blender add-on does on export. The scene's box
// is then translated so its minimum corner sits at the origin: a standalone .bvx is region (0,0,0)
// and a scene, or a flipped axis, would otherwise put voxels in negative space.

#include "bsvx_dll.h"
#include "vox.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

	constexpr uint16_t DEFAULT_CHUNK = 16;

	void print_usage(std::FILE* to)
	{
		std::fprintf(to,
			"usage: bsvx --convert <source.vox> <dest.bvx> [options]\n"
			"       bsvx -C <source.vox> <dest.bvx> [options]\n"
			"\n"
			"Converts a MagicaVoxel file into a standalone .bvx region.\n"
			"\n"
			"options:\n"
			"  --chunk N          voxels per chunk edge (default %u)\n"
			"  --model N          convert only model N, in its own frame, ignoring the scene graph\n"
			"  --include-hidden   also convert instances and layers MagicaVoxel has hidden\n"
			"  -h, --help         this text\n"
			"  --version          library ABI and build information\n"
			"\n"
			"The scene is written in bsvx's canonical Y-up frame (MagicaVoxel is Z-up), with its\n"
			"minimum corner at the origin. Every placed model is included at the position and\n"
			"rotation its scene graph gives it; where instances overlap, the later one in scene\n"
			"order wins. Colours in use become registry entries carrying the .vox palette colour.\n",
			static_cast<unsigned>(DEFAULT_CHUNK));
	}

	[[noreturn]] void fail(const std::string& message, bsvx_context* ctx = nullptr)
	{
		std::string text = "bsvx: " + message;
		if (ctx) {
			const char* detail = bsvx_context_last_error(ctx);
			if (detail && *detail) text += ": " + std::string(detail);
		}
		std::fprintf(stderr, "%s\n", text.c_str());
		std::exit(1);
	}

	void require(bsvx_result rc, const char* what, bsvx_context* ctx)
	{
		if (rc != BSVX_RESULT_OK) fail(std::string(what) + " failed (" + bsvx_result_string(rc) + ")", ctx);
	}

	void print_warnings(bsvx_context* ctx)
	{
		const size_t count = bsvx_context_warning_count(ctx);
		for (size_t i = 0; i < count; ++i) std::fprintf(stderr, "bsvx: warning: %s\n", bsvx_context_warning(ctx, i));
	}

	// Strict enough that "--chunk 16x" is an error rather than 16.
	long parse_long(const char* text, const char* flag)
	{
		char* end = nullptr;
		const long value = std::strtol(text, &end, 10);
		if (!text[0] || (end && *end)) fail(std::string("expected a number after ") + flag + ", got '" + text + "'");
		return value;
	}

	struct ConvertOptions final {
		std::string source;
		std::string dest;
		uint16_t chunk = DEFAULT_CHUNK;
		bool single_model = false;
		size_t model = 0;
		bool include_hidden = false;
	};

	std::string human(uint64_t bytes)
	{
		char out[64];
		if (bytes < 1024ull) std::snprintf(out, sizeof(out), "%llu B", static_cast<unsigned long long>(bytes));
		else if (bytes < 1024ull * 1024) std::snprintf(out, sizeof(out), "%.1f KB", bytes / 1024.0);
		else std::snprintf(out, sizeof(out), "%.1f MB", bytes / (1024.0 * 1024.0));
		return out;
	}

	// The voxels of the scene in the canonical frame, before the origin shift: one dense buffer per
	// chunk that has anything in it, keyed by chunk coordinate. A scene can be far larger than any
	// one model (models cap at 256 an axis; a scene does not), so a dense grid of its box is not an
	// option, and a chunk map is what the region wants written anyway.
	struct ChunkKey final {
		int64_t x, y, z;
		bool operator<(const ChunkKey& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
	};

	struct Scatter final {
		std::map<ChunkKey, std::vector<uint32_t>> chunks;
		int64_t lo[3] = { std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max() };
		int64_t hi[3] = { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min() };
		size_t written = 0;
		std::vector<uint8_t> used = std::vector<uint8_t>(256, 0);
	};

	// Every voxel of every instance, in the canonical frame. Two passes over the same loop: the
	// first only finds the bounding box, the second writes voxels shifted so that box starts at the
	// origin. It is cheaper than one pass into chunk buffers that then have to be re-scattered.
	template <typename Visit>
	void each_placed_voxel(const vox::Scene& scene, const std::vector<vox::Instance>& instances, bool local_frame, bsvx_context* ctx, Visit&& visit)
	{
		constexpr uint32_t FROM = BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, TO = BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD;
		for (const vox::Instance& instance : instances) {
			const vox::Model& model = scene.models[instance.model];
			for (const vox::Voxel& v : model.voxels) {
				// Index 0 is "no voxel" in .vox as in bsvx; a coordinate outside SIZE is a malformed
				// file, and skipping it is the only thing to do short of refusing the whole model.
				if (v.color == 0 || v.x >= model.size_x || v.y >= model.size_y || v.z >= model.size_z) { visit(nullptr, v.color); continue; }

				int32_t placed[3] = { v.x, v.y, v.z };
				if (!local_frame) instance.world_cell(model, v.x, v.y, v.z, placed);

				int64_t c[3];
				require(bsvx_convert_cell(FROM, TO, placed[0], placed[1], placed[2], &c[0], &c[1], &c[2]), "bsvx_convert_cell", ctx);
				visit(c, v.color);
			}
		}
	}

	Scatter scatter_scene(const vox::Scene& scene, const std::vector<vox::Instance>& instances, bool local_frame, uint16_t chunk, bsvx_context* ctx, size_t& skipped)
	{
		Scatter out;
		skipped = 0;

		each_placed_voxel(scene, instances, local_frame, ctx, [&](const int64_t* c, uint8_t) {
			if (!c) { ++skipped; return; }
			for (int i = 0; i < 3; ++i) { out.lo[i] = std::min(out.lo[i], c[i]); out.hi[i] = std::max(out.hi[i], c[i]); }
			});
		if (out.lo[0] > out.hi[0]) return out;   // nothing placed

		const size_t chunk_voxels = static_cast<size_t>(chunk) * chunk * chunk;
		each_placed_voxel(scene, instances, local_frame, ctx, [&](const int64_t* c, uint8_t colour) {
			if (!c) return;
			const int64_t x = c[0] - out.lo[0], y = c[1] - out.lo[1], z = c[2] - out.lo[2];
			const ChunkKey key{ x / chunk, y / chunk, z / chunk };
			auto it = out.chunks.find(key);
			if (it == out.chunks.end()) it = out.chunks.emplace(key, std::vector<uint32_t>(chunk_voxels, 0u)).first;
			const size_t lx = static_cast<size_t>(x - key.x * chunk), ly = static_cast<size_t>(y - key.y * chunk), lz = static_cast<size_t>(z - key.z * chunk);
			it->second[lx + chunk * (ly + static_cast<size_t>(chunk) * lz)] = colour;
			out.used[colour] = 1;
			++out.written;
			});
		return out;
	}

	int convert(const ConvertOptions& options)
	{
		const std::vector<std::byte> bytes = vox::read_file(options.source);
		const vox::Scene scene = vox::decode(bytes.data(), bytes.size());

		if (scene.models.empty()) fail(options.source + ": no model in file");
		for (const vox::Model& m : scene.models) {
			if (m.size_x <= 0 || m.size_y <= 0 || m.size_z <= 0) fail(options.source + ": a model has a zero extent");
		}

		// What gets placed: the scene, or one model in its own frame.
		std::vector<vox::Instance> instances;
		size_t hidden_skipped = 0;
		if (options.single_model) {
			if (options.model >= scene.models.size()) {
				fail(options.source + ": --model " + std::to_string(options.model) + " but the file holds " +
					std::to_string(scene.models.size()) + (scene.models.size() == 1 ? " model" : " models"));
			}
			vox::Instance one;
			one.model = static_cast<uint32_t>(options.model);
			instances.push_back(one);
		}
		else {
			for (const vox::Instance& instance : scene.instances) {
				if (instance.hidden && !options.include_hidden) { ++hidden_skipped; continue; }
				instances.push_back(instance);
			}
			if (!scene.has_scene_graph && scene.models.size() > 1) {
				std::fprintf(stderr, "bsvx: %s has %zu models and no scene graph; all are placed at the origin (--model N converts one)\n",
					options.source.c_str(), scene.models.size());
			}
		}
		if (instances.empty()) fail(options.source + (hidden_skipped ? ": every instance is hidden (--include-hidden converts them anyway)" : ": nothing to convert"));

		bsvx_context* ctx = bsvx_context_create();
		if (!ctx) fail("could not create a bsvx context");

		size_t skipped = 0;
		const Scatter scatter = scatter_scene(scene, instances, options.single_model, options.chunk, ctx, skipped);
		if (skipped) std::fprintf(stderr, "bsvx: warning: %zu voxels outside their model's SIZE or with colour 0 were skipped\n", skipped);
		if (scatter.written == 0) fail(options.source + ": no voxels to write");

		// The region is the scene's bounding box, which now starts at the origin.
		int64_t chunks_across[3];
		for (int i = 0; i < 3; ++i) {
			chunks_across[i] = (scatter.hi[i] - scatter.lo[i]) / options.chunk + 1;
			if (chunks_across[i] > std::numeric_limits<uint16_t>::max()) fail("the scene spans more chunks on one axis than a region can hold; use a larger --chunk");
		}

		bsvx_geometry_desc geometry{};
		geometry.chunk_size_x = geometry.chunk_size_y = geometry.chunk_size_z = options.chunk;
		geometry.region_size_x = static_cast<uint16_t>(chunks_across[0]);
		geometry.region_size_y = static_cast<uint16_t>(chunks_across[1]);
		geometry.region_size_z = static_cast<uint16_t>(chunks_across[2]);

		bsvx_world* world = nullptr;
		require(bsvx_world_create(ctx, &geometry, &world), "bsvx_world_create", ctx);

		// One registry entry per colour the scene actually uses, carrying the palette colour. The
		// rest of the 255-entry palette is not the scene's, and an importer that builds one material
		// per registry entry does not want 250 empty ones.
		uint32_t colours = 0;
		for (uint32_t key = 1; key < 256; ++key) {
			if (!scatter.used[key]) continue;
			bsvx_registry_entry entry{ key, 0u, 1u /* opaque */, 0u };
			require(bsvx_world_set_registry_entry(world, &entry), "bsvx_world_set_registry_entry", ctx);

			// RGBA chunk entry i is colour index i+1. The library stores 0xRRGGBBAA and reads 0 as
			// "unset", so a fully transparent black becomes the documented 0x00000001 instead.
			const uint8_t* rgba = scene.palette.data() + 4 * (key - 1);
			uint32_t packed = (uint32_t(rgba[0]) << 24) | (uint32_t(rgba[1]) << 16) | (uint32_t(rgba[2]) << 8) | uint32_t(rgba[3]);
			if (packed == 0u) packed = 0x00000001u;
			require(bsvx_world_set_registry_color(world, key, packed), "bsvx_world_set_registry_color", ctx);
			++colours;
		}

		size_t region = 0;
		require(bsvx_world_add_region(world, 0, 0, 0, &region), "bsvx_world_add_region", ctx);

		// std::map iterates in key order, so the file's chunk order is deterministic.
		for (const auto& [key, voxels] : scatter.chunks) {
			require(bsvx_region_set_chunk_u32_ex(ctx, world, region,
				static_cast<uint16_t>(key.x), static_cast<uint16_t>(key.y), static_cast<uint16_t>(key.z),
				voxels.data(), voxels.size(), 0xFFFFu), "bsvx_region_set_chunk_u32_ex", ctx);
		}

		require(bsvx_world_save_region_ex(ctx, world, options.dest.c_str()), "bsvx_world_save_region_ex", ctx);
		print_warnings(ctx);

		std::error_code ec;
		const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(options.dest, ec));

		std::printf("%s -> %s: %zu %s, %lldx%lldx%lld, %zu voxels, %zu chunks of %u^3, %u colours%s, %s\n",
			options.source.c_str(), options.dest.c_str(),
			instances.size(), instances.size() == 1 ? "instance" : "instances",
			static_cast<long long>(scatter.hi[0] - scatter.lo[0] + 1), static_cast<long long>(scatter.hi[1] - scatter.lo[1] + 1), static_cast<long long>(scatter.hi[2] - scatter.lo[2] + 1),
			scatter.written, scatter.chunks.size(), static_cast<unsigned>(options.chunk), colours,
			hidden_skipped ? (", " + std::to_string(hidden_skipped) + " hidden skipped").c_str() : "",
			ec ? "size unknown" : human(size).c_str());

		bsvx_world_destroy(world);
		bsvx_context_destroy(ctx);
		return 0;
	}

}

int main(int argc, char** argv)
{
	if (argc < 2) { print_usage(stderr); return 2; }

	const std::string command = argv[1];
	if (command == "-h" || command == "--help") { print_usage(stdout); return 0; }
	if (command == "--version") {
		std::printf("%s\n", bsvx_build_info());   // already carries the ABI version
		return 0;
	}

	if (command == "--convert" || command == "-C") {
		ConvertOptions options;
		std::vector<std::string> positional;
		for (int i = 2; i < argc; ++i) {
			const std::string arg = argv[i];
			const auto value = [&](const char* flag) -> const char* {
				if (i + 1 >= argc) fail(std::string("missing value after ") + flag);
				return argv[++i];
				};
			if (arg == "--chunk") {
				const long n = parse_long(value("--chunk"), "--chunk");
				if (n < 1 || n > 255) fail("--chunk must be between 1 and 255");
				options.chunk = static_cast<uint16_t>(n);
			}
			else if (arg == "--model") {
				const long n = parse_long(value("--model"), "--model");
				if (n < 0) fail("--model must not be negative");
				options.single_model = true;
				options.model = static_cast<size_t>(n);
			}
			else if (arg == "--include-hidden") options.include_hidden = true;
			else if (!arg.empty() && arg[0] == '-') fail("unknown option '" + arg + "'\n\n" + "run bsvx --help for usage");
			else positional.push_back(arg);
		}
		if (positional.size() != 2) {
			std::fprintf(stderr, "bsvx: --convert takes a source .vox and a destination .bvx\n\n");
			print_usage(stderr);
			return 2;
		}
		options.source = positional[0];
		options.dest = positional[1];

		try {
			return convert(options);
		}
		catch (const std::exception& e) {
			fail(e.what());
		}
	}

	std::fprintf(stderr, "bsvx: unknown command '%s'\n\n", command.c_str());
	print_usage(stderr);
	return 2;
}

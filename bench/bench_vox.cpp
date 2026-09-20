// bsvx (.bvx) against MagicaVoxel (.vox): file size and load time on the same voxel data.
//
// Every scene is generated once as a dense uint32 grid, then written through both formats and
// read back through both. Both readers are timed doing the same job -- producing the dense grid
// again -- because that is the only comparison where the two formats are being asked for the same
// thing. Where bsvx can answer without doing that job (chunk summaries, partial decode), that is
// reported separately rather than folded into the headline number.
//
// The .vox side is bench/vox.hpp: a straight single-pass parser over a fully-buffered file, with
// one bulk memcpy for the voxel list. There is no slower way to read .vox worth reporting and no
// meaningfully faster one -- the format is a flat array of 4-byte records.

#include "bsvx_dll.h"
#include "bvx_anatomy.hpp"
#include "vox.hpp"   // tools/vox.hpp

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <numeric>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// scaffolding
// ---------------------------------------------------------------------------------------------

namespace {

	[[noreturn]] void fail(const std::string& what, bsvx_context* ctx = nullptr)
	{
		std::string message = "bench: " + what;
		if (ctx) {
			const char* detail = bsvx_context_last_error(ctx);
			if (detail && *detail) message += ": " + std::string(detail);
		}
		std::fprintf(stderr, "%s\n", message.c_str());
		std::exit(1);
	}

	void require(bsvx_result rc, const char* what, bsvx_context* ctx)
	{
		if (rc != BSVX_RESULT_OK) fail(std::string(what) + " returned " + bsvx_result_string(rc), ctx);
	}

	using Clock = std::chrono::steady_clock;

	// Median of `runs`, so one scheduling hiccup does not become the reported figure.
	double time_ms(int runs, const std::function<void()>& body)
	{
		std::vector<double> samples;
		samples.reserve(static_cast<size_t>(runs));
		for (int i = 0; i < runs; ++i) {
			const auto start = Clock::now();
			body();
			const auto stop = Clock::now();
			samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
		}
		std::sort(samples.begin(), samples.end());
		return samples[samples.size() / 2];
	}

	// Drops a file's clean pages from the page cache. No root needed, and it is the only way to
	// measure what an asset load actually costs the first time a world is opened -- which is the
	// case a shipping game cares about and a warm-cache benchmark never shows.
	void evict(const fs::path& path)
	{
		const int fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0) return;
		::fsync(fd);
		::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		::close(fd);
	}

	void evict_all(const std::vector<fs::path>& paths)
	{
		for (const fs::path& path : paths) evict(path);
	}

	// Median again, but with per-iteration setup (cache eviction) left out of the measurement.
	double time_ms_setup(int runs, const std::function<void()>& setup, const std::function<void()>& body)
	{
		std::vector<double> samples;
		samples.reserve(static_cast<size_t>(runs));
		for (int i = 0; i < runs; ++i) {
			setup();
			const auto start = Clock::now();
			body();
			const auto stop = Clock::now();
			samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
		}
		std::sort(samples.begin(), samples.end());
		return samples[samples.size() / 2];
	}

	uint64_t bytes_of(const fs::path& path)
	{
		return static_cast<uint64_t>(fs::file_size(path));
	}

	uint64_t dir_size(const fs::path& root)
	{
		uint64_t total = 0;
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (entry.is_regular_file()) total += static_cast<uint64_t>(entry.file_size());
		}
		return total;
	}

	// Size after gzip -9, because most asset pipelines ship compressed and a format that merely
	// defers its redundancy to the packer should not get credit for it.
	uint64_t gzip_size(const fs::path& path)
	{
		const std::string command = "gzip -9 -c '" + path.string() + "' | wc -c";
		FILE* pipe = popen(command.c_str(), "r");
		if (!pipe) return 0;
		char buffer[64] = {};
		if (!std::fgets(buffer, sizeof(buffer), pipe)) { pclose(pipe); return 0; }
		pclose(pipe);
		return std::strtoull(buffer, nullptr, 10);
	}

	uint64_t gzip_dir_size(const fs::path& root)
	{
		uint64_t total = 0;
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (entry.is_regular_file()) total += gzip_size(entry.path());
		}
		return total;
	}

	struct SplitMix64 final {
		uint64_t state;
		uint64_t next()
		{
			uint64_t z = (state += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}
		double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
	};

}

// ---------------------------------------------------------------------------------------------
// scenes
// ---------------------------------------------------------------------------------------------

namespace {

	// A dense grid in bsvx order: x + sx * (y + sy * z), y up. Keys are 1..255 so the same data
	// fits a .vox palette; 0 is air in both formats.
	struct Grid final {
		std::string name;
		std::string note;
		uint16_t sx = 0, sy = 0, sz = 0;
		std::vector<uint32_t> voxels;

		size_t index(uint32_t x, uint32_t y, uint32_t z) const { return x + sx * (static_cast<size_t>(y) + sy * z); }
		size_t count() const { return static_cast<size_t>(sx) * sy * sz; }
		size_t non_air() const
		{
			size_t n = 0;
			for (uint32_t v : voxels) if (v != 0u) ++n;
			return n;
		}
	};

	// Smooth value noise from a coarse lattice; enough to give a heightfield real structure
	// without pulling in a noise library.
	struct ValueNoise final {
		SplitMix64 rng;
		std::vector<double> lattice;
		uint32_t side;

		explicit ValueNoise(uint64_t seed, uint32_t lattice_side) : rng{ seed }, side(lattice_side)
		{
			lattice.resize(static_cast<size_t>(side) * side);
			for (double& v : lattice) v = rng.unit();
		}

		double sample(double u, double v) const
		{
			const double fx = u * (side - 1), fy = v * (side - 1);
			const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
			const uint32_t x1 = std::min(x0 + 1u, side - 1u), y1 = std::min(y0 + 1u, side - 1u);
			const double tx = fx - x0, ty = fy - y0;
			const double sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty);
			const double a = lattice[y0 * side + x0], b = lattice[y0 * side + x1];
			const double c = lattice[y1 * side + x0], d = lattice[y1 * side + x1];
			return (a + (b - a) * sx) * (1.0 - sy) + (c + (d - c) * sx) * sy;
		}
	};

	Grid make_terrain(const char* name, uint16_t sx, uint16_t sy, uint16_t sz, bool caves)
	{
		Grid grid;
		grid.name = name;
		grid.note = caves ? "heightfield terrain, 3D-noise caves, 5 keys" : "heightfield terrain, solid below surface, 5 keys";
		grid.sx = sx; grid.sy = sy; grid.sz = sz;
		grid.voxels.assign(grid.count(), 0u);

		const ValueNoise surface(0xB5D1CE ^ (caves ? 7u : 0u), 12);
		const ValueNoise cave_a(0x51DE, 20), cave_b(0xC0FFEE, 14);

		for (uint16_t z = 0; z < sz; ++z) {
			for (uint16_t x = 0; x < sx; ++x) {
				const double n = surface.sample(static_cast<double>(x) / sx, static_cast<double>(z) / sz);
				const uint32_t height = static_cast<uint32_t>(4.0 + n * (sy - 10));
				for (uint32_t y = 0; y <= height && y < sy; ++y) {
					uint32_t key = 1u;                                  // stone
					if (y == height) key = 3u;                          // grass
					else if (y + 3 >= height) key = 2u;                 // dirt
					else if (y < 2) key = 4u;                           // bedrock
					if (caves) {
						const double a = cave_a.sample(static_cast<double>(x) / sx, static_cast<double>(z) / sz);
						const double b = cave_b.sample(static_cast<double>(y) / sy, static_cast<double>(z) / sz);
						if (y + 4 < height && std::abs(a - b) < 0.06) continue;
					}
					grid.voxels[grid.index(x, y, z)] = key;
				}
			}
		}
		return grid;
	}

	Grid make_shell(const char* name, uint16_t side, uint32_t key_count)
	{
		Grid grid;
		grid.name = name;
		grid.note = "hollow sphere shell, ~1 voxel thick, " + std::to_string(key_count) + " keys";
		grid.sx = grid.sy = grid.sz = side;
		grid.voxels.assign(grid.count(), 0u);

		const double centre = (side - 1) * 0.5;
		const double radius = centre - 1.0;
		SplitMix64 rng{ 0xA11CE };

		for (uint16_t z = 0; z < side; ++z) {
			for (uint16_t y = 0; y < side; ++y) {
				for (uint16_t x = 0; x < side; ++x) {
					const double dx = x - centre, dy = y - centre, dz = z - centre;
					const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (d <= radius && d >= radius - 1.2) {
						grid.voxels[grid.index(x, y, z)] = 1u + static_cast<uint32_t>(rng.next() % key_count);
					}
				}
			}
		}
		return grid;
	}

	Grid make_prop(const char* name, uint16_t side, uint32_t key_count)
	{
		Grid grid;
		grid.name = name;
		grid.note = "solid ellipsoid with colour patches, " + std::to_string(key_count) + " keys";
		grid.sx = grid.sy = grid.sz = side;
		grid.voxels.assign(grid.count(), 0u);

		const double centre = (side - 1) * 0.5;
		const ValueNoise patches(0xDECAF, 8);

		for (uint16_t z = 0; z < side; ++z) {
			for (uint16_t y = 0; y < side; ++y) {
				for (uint16_t x = 0; x < side; ++x) {
					const double dx = (x - centre) / (centre * 0.9);
					const double dy = (y - centre) / centre;
					const double dz = (z - centre) / (centre * 0.7);
					if (dx * dx + dy * dy + dz * dz > 1.0) continue;
					const double n = patches.sample(static_cast<double>(x) / side, static_cast<double>(z) / side);
					grid.voxels[grid.index(x, y, z)] = 1u + static_cast<uint32_t>(n * (key_count - 1));
				}
			}
		}
		return grid;
	}

	Grid make_scatter(const char* name, uint16_t side, double fill, uint32_t key_count)
	{
		Grid grid;
		grid.name = name;
		grid.note = "uniform random scatter, " + std::to_string(fill * 100.0).substr(0, 4) + "% fill";
		grid.sx = grid.sy = grid.sz = side;
		grid.voxels.assign(grid.count(), 0u);

		SplitMix64 rng{ 0x5CA77E2 };
		for (size_t i = 0; i < grid.voxels.size(); ++i) {
			if (rng.unit() < fill) grid.voxels[i] = 1u + static_cast<uint32_t>(rng.next() % key_count);
		}
		return grid;
	}

	Grid make_solid(const char* name, uint16_t side)
	{
		Grid grid;
		grid.name = name;
		grid.note = "completely filled, 1 key";
		grid.sx = grid.sy = grid.sz = side;
		grid.voxels.assign(grid.count(), 1u);
		return grid;
	}

	Grid make_white_noise(const char* name, uint16_t side, uint32_t key_count)
	{
		Grid grid;
		grid.name = name;
		grid.note = "every voxel independently random, " + std::to_string(key_count) + " keys";
		grid.sx = grid.sy = grid.sz = side;
		grid.voxels.assign(grid.count(), 0u);

		SplitMix64 rng{ 0x1337 };
		for (uint32_t& v : grid.voxels) v = 1u + static_cast<uint32_t>(rng.next() % key_count);
		return grid;
	}

	// What a bit-packed sparse codec costs, chunk by chunk, priced from the grid rather than from
	// the file.
	//
	// SPARSE_LIST spends a full uint32 on the linear index and another on the voxel key: 8 bytes a
	// voxel, twice what .vox's (x, y, z, colour) record costs. Neither field needs that width inside
	// a chunk -- a 16x16x16 chunk indexes in 12 bits, and a chunk with eight distinct keys indexes
	// its own palette in 3. This was written to size that gap before SPARSE_PACKED existed; now that
	// it does, the model and the shipped encoder are two independent answers to the same question,
	// and the table reads as a check that they agree.
	//
	// Layout priced: u16 palette count, u8 index bits, u8 key bits, u32 voxel count, the palette
	// as uint32, then (index_bits + key_bits) per non-air voxel, bit-packed.
	uint32_t packed_sparse_bytes(const Grid& grid, uint16_t chunk_side, uint16_t cx, uint16_t cy, uint16_t cz)
	{
		const size_t chunk_voxels = static_cast<size_t>(chunk_side) * chunk_side * chunk_side;
		std::vector<uint32_t> keys;
		size_t non_air = 0;

		for (uint16_t lz = 0; lz < chunk_side; ++lz) {
			const uint32_t wz = cz * chunk_side + lz;
			if (wz >= grid.sz) break;
			for (uint16_t ly = 0; ly < chunk_side; ++ly) {
				const uint32_t wy = cy * chunk_side + ly;
				if (wy >= grid.sy) break;
				for (uint16_t lx = 0; lx < chunk_side; ++lx) {
					const uint32_t wx = cx * chunk_side + lx;
					if (wx >= grid.sx) break;
					const uint32_t key = grid.voxels[grid.index(wx, wy, wz)];
					if (key == 0u) continue;
					++non_air;
					if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
				}
			}
		}
		if (non_air == 0) return 0;

		const auto bits_for = [](size_t values) -> uint32_t {
			uint32_t bits = 0;
			while ((1ull << bits) < values) ++bits;
			return bits == 0 ? 1u : bits;
			};

		const uint32_t index_bits = bits_for(chunk_voxels);
		const uint32_t key_bits = bits_for(keys.size());
		const uint64_t payload_bits = static_cast<uint64_t>(non_air) * (index_bits + key_bits);
		return static_cast<uint32_t>(8 + keys.size() * 4 + (payload_bits + 7) / 8);
	}

	// A .vox file loaded from disk, mapped into the same dense grid the generators produce, so a
	// real MagicaVoxel model can be run through the identical pipeline.
	Grid grid_from_vox(const fs::path& path)
	{
		const auto bytes = vox::read_file(path.string());
		const vox::Scene scene = vox::decode(bytes.data(), bytes.size());
		if (scene.models.empty()) fail("no models in " + path.string());
		const vox::Model& model = scene.models.front();

		Grid grid;
		grid.name = path.stem().string();
		grid.note = "loaded from " + path.filename().string() + ", model 0 of " + std::to_string(scene.models.size());
		grid.sx = static_cast<uint16_t>(model.size_x);
		grid.sy = static_cast<uint16_t>(model.size_y);
		grid.sz = static_cast<uint16_t>(model.size_z);
		grid.voxels.assign(grid.count(), 0u);
		for (const vox::Voxel& v : model.voxels) grid.voxels[grid.index(v.x, v.y, v.z)] = v.color;
		return grid;
	}

}

// ---------------------------------------------------------------------------------------------
// format sides
// ---------------------------------------------------------------------------------------------

namespace {

	// .vox coordinates are u8, so no model may exceed 256 on an axis. Nothing in the format
	// carries a model's placement except the scene-graph chunks, so a world larger than that is
	// not one file's worth of data in .vox at all.
	constexpr uint16_t VOX_AXIS_LIMIT = 256;

	vox::Scene vox_from_grid(const Grid& grid)
	{
		vox::Scene scene;
		vox::Model model;
		model.size_x = grid.sx;
		model.size_y = grid.sy;
		model.size_z = grid.sz;
		model.voxels.reserve(grid.non_air());

		for (uint16_t z = 0; z < grid.sz; ++z) {
			for (uint16_t y = 0; y < grid.sy; ++y) {
				for (uint16_t x = 0; x < grid.sx; ++x) {
					const uint32_t key = grid.voxels[grid.index(x, y, z)];
					if (key == 0u) continue;
					model.voxels.push_back({ static_cast<uint8_t>(x), static_cast<uint8_t>(y),
											 static_cast<uint8_t>(z), static_cast<uint8_t>(key) });
				}
			}
		}

		SplitMix64 rng{ 0xC010401 };
		for (size_t i = 0; i < scene.palette.size(); ++i) scene.palette[i] = static_cast<uint8_t>(rng.next());
		scene.models.push_back(std::move(model));
		return scene;
	}

	// The bsvx side: one world, one region, chunks written through the C ABI exactly as an
	// importer would. Region size is chosen so the whole grid lands in a single region, which
	// keeps the file count at one and the comparison against a single .vox file honest.
	struct BsvxBuild final {
		fs::path world_dir;
		fs::path standalone;
		uint16_t chunk_side = 16;
		double encode_ms = 0.0;
	};

	BsvxBuild build_bsvx(bsvx_context* ctx, const Grid& grid, const fs::path& out_dir, uint16_t chunk_side, uint32_t key_count)
	{
		BsvxBuild build;
		build.chunk_side = chunk_side;
		build.world_dir = out_dir / (grid.name + "_c" + std::to_string(chunk_side) + "_world");
		build.standalone = out_dir / (grid.name + "_c" + std::to_string(chunk_side) + ".bvx");

		const auto chunks_for = [&](uint16_t extent) -> uint16_t {
			return static_cast<uint16_t>((extent + chunk_side - 1) / chunk_side);
			};

		bsvx_geometry_desc geometry{};
		geometry.chunk_size_x = geometry.chunk_size_y = geometry.chunk_size_z = chunk_side;
		geometry.region_size_x = chunks_for(grid.sx);
		geometry.region_size_y = chunks_for(grid.sy);
		geometry.region_size_z = chunks_for(grid.sz);

		bsvx_world* world = nullptr;
		require(bsvx_world_create(ctx, &geometry, &world), "bsvx_world_create", ctx);

		// Summaries are built through the registry, so it has to exist before any voxel is written.
		for (uint32_t key = 1; key <= key_count; ++key) {
			bsvx_registry_entry entry{ key, 0u, 1u /* opaque */, 0u };
			require(bsvx_world_set_registry_entry(world, &entry), "bsvx_world_set_registry_entry", ctx);
		}

		size_t region = 0;
		require(bsvx_world_add_region(world, 0, 0, 0, &region), "bsvx_world_add_region", ctx);

		const size_t chunk_voxels = static_cast<size_t>(chunk_side) * chunk_side * chunk_side;
		std::vector<uint32_t> chunk(chunk_voxels);

		const auto start = Clock::now();
		for (uint16_t cz = 0; cz < geometry.region_size_z; ++cz) {
			for (uint16_t cy = 0; cy < geometry.region_size_y; ++cy) {
				for (uint16_t cx = 0; cx < geometry.region_size_x; ++cx) {
					std::fill(chunk.begin(), chunk.end(), 0u);
					bool any = false;
					for (uint16_t lz = 0; lz < chunk_side; ++lz) {
						const uint32_t wz = cz * chunk_side + lz;
						if (wz >= grid.sz) break;
						for (uint16_t ly = 0; ly < chunk_side; ++ly) {
							const uint32_t wy = cy * chunk_side + ly;
							if (wy >= grid.sy) break;
							for (uint16_t lx = 0; lx < chunk_side; ++lx) {
								const uint32_t wx = cx * chunk_side + lx;
								if (wx >= grid.sx) break;
								const uint32_t key = grid.voxels[grid.index(wx, wy, wz)];
								if (key == 0u) continue;
								chunk[lx + chunk_side * (ly + static_cast<size_t>(chunk_side) * lz)] = key;
								any = true;
							}
						}
					}
					// An absent chunk costs nothing; writing an empty one costs a chunk-map entry
					// and a summary, so an importer should skip it and this one does too.
					if (!any) continue;
					require(bsvx_region_set_chunk_u32_ex(ctx, world, region, cx, cy, cz, chunk.data(), chunk.size(), 0xFFFFu),
						"bsvx_region_set_chunk_u32_ex", ctx);
				}
			}
		}
		build.encode_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

		fs::create_directories(build.world_dir);
		require(bsvx_world_save_ex2(ctx, world, build.world_dir.string().c_str(), BSVX_SAVE_DEFAULT, nullptr), "bsvx_world_save_ex2", ctx);
		require(bsvx_world_save_region_ex(ctx, world, build.standalone.string().c_str()), "bsvx_world_save_region_ex", ctx);

		bsvx_world_destroy(world);
		return build;
	}

	// Decode a standalone .bvx back into the dense grid, which is the same job the .vox reader is
	// timed doing. Empty chunks are absent from the file, so they cost nothing to skip.
	std::vector<uint32_t> decode_bvx_to_grid(bsvx_context* ctx, const fs::path& path, const Grid& grid, uint16_t chunk_side)
	{
		bsvx_world* world = nullptr;
		require(bsvx_world_load_region(ctx, path.string().c_str(), &world), "bsvx_world_load_region", ctx);

		std::vector<uint32_t> out(grid.count(), 0u);
		const size_t chunk_voxels = static_cast<size_t>(chunk_side) * chunk_side * chunk_side;
		std::vector<uint32_t> chunk(chunk_voxels);

		const size_t chunk_count = bsvx_region_chunk_count(world, 0);
		for (size_t ordinal = 0; ordinal < chunk_count; ++ordinal) {
			bsvx_chunk_info info{};
			require(bsvx_region_get_chunk_info(world, 0, ordinal, &info), "bsvx_region_get_chunk_info", ctx);
			if (info.summary.non_air_count == 0u) continue;

			size_t written = 0;
			require(bsvx_region_decode_chunk_u32_ex(ctx, world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
				chunk.data(), chunk.size(), &written), "bsvx_region_decode_chunk_u32_ex", ctx);

			for (uint16_t lz = 0; lz < chunk_side; ++lz) {
				const uint32_t wz = info.local_chunk_z * chunk_side + lz;
				if (wz >= grid.sz) break;
				for (uint16_t ly = 0; ly < chunk_side; ++ly) {
					const uint32_t wy = info.local_chunk_y * chunk_side + ly;
					if (wy >= grid.sy) break;
					for (uint16_t lx = 0; lx < chunk_side; ++lx) {
						const uint32_t wx = info.local_chunk_x * chunk_side + lx;
						if (wx >= grid.sx) break;
						out[grid.index(wx, wy, wz)] = chunk[lx + chunk_side * (ly + static_cast<size_t>(chunk_side) * lz)];
					}
				}
			}
		}

		bsvx_world_destroy(world);
		return out;
	}

	// The same job through the bulk accessor: one crossing, the library lays the region out as a
	// single dense array. This is the fair headline number -- it is what the format offers a
	// consumer that genuinely wants everything, with no per-chunk FFI or scatter loop of mine in
	// the measurement.
	std::vector<uint32_t> decode_bvx_bulk(bsvx_context* ctx, const fs::path& path, const Grid& grid)
	{
		bsvx_world* world = nullptr;
		require(bsvx_world_load_region(ctx, path.string().c_str(), &world), "bsvx_world_load_region", ctx);

		// A (NULL, 0) probe reports the requirement through BUFFER_TOO_SMALL, not OK.
		size_t needed = 0;
		const bsvx_result probe = bsvx_region_decode_all_u32(ctx, world, 0, BSVX_LAYOUT_REGION_LINEAR, nullptr, 0, &needed);
		if (probe != BSVX_RESULT_BUFFER_TOO_SMALL && probe != BSVX_RESULT_OK) {
			require(probe, "bsvx_region_decode_all_u32 (size probe)", ctx);
		}

		std::vector<uint32_t> out(needed, 0u);
		size_t written = 0;
		require(bsvx_region_decode_all_u32(ctx, world, 0, BSVX_LAYOUT_REGION_LINEAR, out.data(), out.size(), &written),
			"bsvx_region_decode_all_u32", ctx);

		bsvx_world_destroy(world);
		// Every benchmark grid is a whole number of chunks on each axis, so the region-linear
		// extent is the grid extent and the two arrays are directly comparable.
		if (out.size() != grid.count()) fail(grid.name + ": region-linear extent is not the grid extent");
		return out;
	}

	std::vector<uint32_t> decode_vox_to_grid(const fs::path& path, const Grid& grid)
	{
		const auto bytes = vox::read_file(path.string());
		const vox::Scene scene = vox::decode(bytes.data(), bytes.size());
		std::vector<uint32_t> out(grid.count(), 0u);
		for (const vox::Voxel& v : scene.models.front().voxels) out[grid.index(v.x, v.y, v.z)] = v.color;
		return out;
	}

}

// ---------------------------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------------------------

namespace {

	struct Row final {
		std::string scene, note;
		size_t voxels = 0, non_air = 0;
		uint64_t vox_bytes = 0, vox_gz = 0;
		uint64_t bvx_bytes = 0, bvx_gz = 0, world_bytes = 0, world_gz = 0;
		double vox_load_ms = 0, bvx_load_ms = 0, bvx_stream_ms = 0;
		double vox_cold_ms = 0, bvx_cold_ms = 0;
		double vox_open_ms = 0, bvx_open_ms = 0;
		double bvx_encode_ms = 0;
		anatomy::Report anatomy{};
		uint64_t packed_sparse_payload = 0;   // hypothetical: best of the real codec and a packed sparse one
	};

	std::string human(uint64_t bytes)
	{
		char buffer[32];
		if (bytes >= 1024ull * 1024ull) std::snprintf(buffer, sizeof(buffer), "%.2f MB", bytes / (1024.0 * 1024.0));
		else if (bytes >= 1024ull) std::snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0);
		else std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
		return buffer;
	}

	Row run_scene(bsvx_context* ctx, const Grid& grid, const fs::path& out_dir, uint16_t chunk_side, uint32_t key_count, int runs)
	{
		if (grid.sx > VOX_AXIS_LIMIT || grid.sy > VOX_AXIS_LIMIT || grid.sz > VOX_AXIS_LIMIT) {
			fail(grid.name + ": " + std::to_string(grid.sx) + "x" + std::to_string(grid.sy) + "x" + std::to_string(grid.sz)
				+ " exceeds the 256-per-axis ceiling a single .vox model can address");
		}

		Row row;
		row.scene = grid.name;
		row.note = grid.note;
		row.voxels = grid.count();
		row.non_air = grid.non_air();

		const fs::path vox_path = out_dir / (grid.name + ".vox");
		vox::write_file(vox_path.string(), vox::encode(vox_from_grid(grid)));

		const BsvxBuild build = build_bsvx(ctx, grid, out_dir, chunk_side, key_count);
		row.bvx_encode_ms = build.encode_ms;

		row.vox_bytes = bytes_of(vox_path);
		row.vox_gz = gzip_size(vox_path);
		row.bvx_bytes = bytes_of(build.standalone);
		row.bvx_gz = gzip_size(build.standalone);
		row.world_bytes = dir_size(build.world_dir);
		row.world_gz = gzip_dir_size(build.world_dir);
		row.anatomy = anatomy::inspect(build.standalone.string());

		// Walk the chunks in the same order build_bsvx wrote them, which is the order the chunk
		// map and the VOXELS entry table are in, and price each against the hypothetical codec.
		{
			size_t ordinal = 0;
			const uint16_t rx = static_cast<uint16_t>((grid.sx + chunk_side - 1) / chunk_side);
			const uint16_t ry = static_cast<uint16_t>((grid.sy + chunk_side - 1) / chunk_side);
			const uint16_t rz = static_cast<uint16_t>((grid.sz + chunk_side - 1) / chunk_side);
			for (uint16_t cz = 0; cz < rz; ++cz) {
				for (uint16_t cy = 0; cy < ry; ++cy) {
					for (uint16_t cx = 0; cx < rx; ++cx) {
						const uint32_t hypothetical = packed_sparse_bytes(grid, chunk_side, cx, cy, cz);
						if (hypothetical == 0u) continue;    // empty chunks were never written
						const uint32_t actual = ordinal < row.anatomy.payload_per_chunk.size()
							? row.anatomy.payload_per_chunk[ordinal] : 0u;
						row.packed_sparse_payload += std::min(actual, hypothetical);
						++ordinal;
					}
				}
			}
		}

		// Correctness gate: a benchmark comparing two formats that disagree about the data is
		// measuring nothing. Both paths must reproduce the grid exactly.
		const auto from_vox = decode_vox_to_grid(vox_path, grid);
		const auto from_bvx = decode_bvx_to_grid(ctx, build.standalone, grid, chunk_side);
		const auto from_bulk = decode_bvx_bulk(ctx, build.standalone, grid);
		if (from_vox != grid.voxels) fail(grid.name + ": .vox round-trip mismatch");
		if (from_bvx != grid.voxels) fail(grid.name + ": .bvx per-chunk round-trip mismatch");
		if (from_bulk != grid.voxels) fail(grid.name + ": .bvx bulk round-trip mismatch");

		row.vox_load_ms = time_ms(runs, [&] { (void)decode_vox_to_grid(vox_path, grid); });
		row.bvx_load_ms = time_ms(runs, [&] { (void)decode_bvx_bulk(ctx, build.standalone, grid); });
		row.bvx_stream_ms = time_ms(runs, [&] { (void)decode_bvx_to_grid(ctx, build.standalone, grid, chunk_side); });

		// The same decode with the file evicted from the page cache first, so the measurement
		// includes the read the warm numbers hide. This is where an 8x smaller file shows up.
		row.vox_cold_ms = time_ms_setup(runs, [&] { evict(vox_path); }, [&] { (void)decode_vox_to_grid(vox_path, grid); });
		row.bvx_cold_ms = time_ms_setup(runs, [&] { evict(build.standalone); }, [&] { (void)decode_bvx_bulk(ctx, build.standalone, grid); });

		// "Open" is what each format costs before any voxel is available: for .bvx, the streaming
		// reader with header, chunk map and summaries resident and no payload touched; for .vox,
		// the whole file, because there is nothing else it can do.
		row.bvx_open_ms = time_ms(runs, [&] {
			bsvx_region_reader* reader = nullptr;
			require(bsvx_region_reader_open(ctx, build.standalone.string().c_str(), &reader), "bsvx_region_reader_open", ctx);
			uint64_t total = 0;
			const size_t chunks = bsvx_region_reader_chunk_count(reader);
			for (size_t i = 0; i < chunks; ++i) {
				bsvx_chunk_info info{};
				bsvx_region_reader_get_chunk_info(reader, i, &info);
				total += info.summary.non_air_count;
			}
			if (total != row.non_air) fail(grid.name + ": summary non-air count disagrees with the grid");
			bsvx_region_reader_close(reader);
			});

		row.vox_open_ms = time_ms(runs, [&] {
			const auto bytes = vox::read_file(vox_path.string());
			const vox::Scene scene = vox::decode(bytes.data(), bytes.size());
			if (scene.models.front().voxels.size() != row.non_air) fail(grid.name + ": .vox voxel count disagrees with the grid");
			});

		return row;
	}

	void print_table(const std::vector<Row>& rows)
	{
		std::printf("\n");
		std::printf("%-18s %11s %11s | %10s %10s %6s | %10s %10s %6s | %7s %7s\n",
			"scene", "voxels", "non-air", ".vox", ".bvx", "ratio", ".vox.gz", ".bvx.gz", "ratio", "vox B/v", "bvx B/v");
		std::printf("%s\n", std::string(120, '-').c_str());
		for (const Row& r : rows) {
			const double per = r.non_air ? static_cast<double>(r.non_air) : 1.0;
			std::printf("%-18s %11zu %11zu | %10s %10s %5.2fx | %10s %10s %5.2fx | %7.2f %7.2f\n",
				r.scene.c_str(), r.voxels, r.non_air,
				human(r.vox_bytes).c_str(), human(r.bvx_bytes).c_str(),
				r.bvx_bytes ? static_cast<double>(r.vox_bytes) / r.bvx_bytes : 0.0,
				human(r.vox_gz).c_str(), human(r.bvx_gz).c_str(),
				r.bvx_gz ? static_cast<double>(r.vox_gz) / r.bvx_gz : 0.0,
				r.vox_bytes / per, r.bvx_bytes / per);
		}

		std::printf("\n");
		std::printf("%-18s | %11s %11s %6s | %11s %11s %6s | %11s %11s %7s\n",
			"scene", ".vox warm", ".bvx warm", "ratio", ".vox cold", ".bvx cold", "ratio", ".vox open", ".bvx open", "ratio");
		std::printf("%s\n", std::string(122, '-').c_str());
		for (const Row& r : rows) {
			std::printf("%-18s | %8.3f ms %8.3f ms %5.2fx | %8.3f ms %8.3f ms %5.2fx | %8.3f ms %8.3f ms %6.1fx\n",
				r.scene.c_str(),
				r.vox_load_ms, r.bvx_load_ms, r.bvx_load_ms > 0 ? r.vox_load_ms / r.bvx_load_ms : 0.0,
				r.vox_cold_ms, r.bvx_cold_ms, r.bvx_cold_ms > 0 ? r.vox_cold_ms / r.bvx_cold_ms : 0.0,
				r.vox_open_ms, r.bvx_open_ms, r.bvx_open_ms > 0 ? r.vox_open_ms / r.bvx_open_ms : 0.0);
		}

		std::printf("\nratio > 1 favours .bvx. \"warm\"/\"cold\" are both formats producing the same dense grid,\n");
		std::printf("cold meaning the file was evicted from the page cache first. .bvx goes through\n");
		std::printf("bsvx_region_decode_all_u32, .vox through a single-pass parse and scatter.\n");
		std::printf("\"open\" is .bvx's streaming reader (header + chunk map + summaries, no payloads)\n");
		std::printf("against .vox, which has to read and parse the whole file to answer anything.\n");
		std::printf("B/v is bytes of file per non-air voxel.\n");
	}

	// A total tells you which format won; this tells you why, and which of the four things a .bvx
	// spends bytes on is the one that matters for a given kind of content.
	void print_anatomy(const std::vector<Row>& rows)
	{
		std::printf("\n.bvx byte anatomy (standalone region)\n");
		std::printf("%-18s %8s | %10s %10s %10s %10s | %10s %6s | %s\n",
			"scene", "chunks", "header+dir", "chunk map", "summaries", "entries", "payload", "fixed%", "codecs (chunks / payload bytes)");
		std::printf("%s\n", std::string(140, '-').c_str());
		for (const Row& r : rows) {
			const anatomy::Report& a = r.anatomy;
			std::string codecs;
			for (const auto& [codec, chunks] : a.codec_chunks) {
				if (!codecs.empty()) codecs += ", ";
				codecs += std::string(anatomy::codec_name(codec)) + " " + std::to_string(chunks)
					+ "/" + human(a.codec_bytes.at(codec));
			}
			std::printf("%-18s %8u | %10s %10s %10s %10s | %10s %5.1f%% | %s\n",
				r.scene.c_str(), a.chunk_count,
				human(a.header_bytes + a.section_dir_bytes + a.other_blob_bytes).c_str(),
				human(a.chunk_map_bytes).c_str(), human(a.summary_bytes).c_str(), human(a.entry_table_bytes).c_str(),
				human(a.voxel_blob_bytes).c_str(),
				a.file_size ? 100.0 * a.fixed_overhead() / a.file_size : 0.0,
				codecs.c_str());
		}
		std::printf("\nfixed%% is header + chunk map + summary table + section/entry tables as a share of the\n");
		std::printf("file: the part that scales with chunk count rather than with voxel count.\n");

		std::printf("\ncross-check: this model of a bit-packed sparse codec against SPARSE_PACKED as shipped\n");
		std::printf("%-18s | %10s %10s %8s | %10s %10s %8s\n",
			"scene", "payload", "would be", "saved", ".bvx now", "would be", "vs .vox");
		std::printf("%s\n", std::string(84, '-').c_str());
		for (const Row& r : rows) {
			const uint64_t now = r.anatomy.voxel_blob_bytes;
			const uint64_t then = r.packed_sparse_payload;
			const uint64_t file_then = r.bvx_bytes - now + then;
			std::printf("%-18s | %10s %10s %7.1f%% | %10s %10s %7.2fx\n",
				r.scene.c_str(), human(now).c_str(), human(then).c_str(),
				now ? 100.0 * (static_cast<double>(now) - then) / now : 0.0,
				human(r.bvx_bytes).c_str(), human(file_then).c_str(),
				file_then ? static_cast<double>(r.vox_bytes) / file_then : 0.0);
		}
	}

	void print_csv(const std::vector<Row>& rows)
	{
		std::printf("\nCSV\n");
		std::printf("scene,voxels,non_air,vox_bytes,bvx_bytes,world_bytes,vox_gz,bvx_gz,world_gz,"
			"vox_warm_ms,bvx_warm_ms,bvx_perchunk_ms,vox_cold_ms,bvx_cold_ms,vox_open_ms,bvx_open_ms,bvx_write_ms\n");
		for (const Row& r : rows) {
			std::printf("%s,%zu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
				r.scene.c_str(), r.voxels, r.non_air,
				(unsigned long long)r.vox_bytes, (unsigned long long)r.bvx_bytes, (unsigned long long)r.world_bytes,
				(unsigned long long)r.vox_gz, (unsigned long long)r.bvx_gz, (unsigned long long)r.world_gz,
				r.vox_load_ms, r.bvx_load_ms, r.bvx_stream_ms, r.vox_cold_ms, r.bvx_cold_ms,
				r.vox_open_ms, r.bvx_open_ms, r.bvx_encode_ms);
		}
	}

}

int main(int argc, char** argv)
{
	fs::path out_dir = fs::temp_directory_path() / "bsvx_bench";
	int runs = 5;
	uint16_t chunk_side = 16;
	std::vector<fs::path> extra_vox;
	bool sweep = false;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const auto next = [&]() -> std::string {
			if (i + 1 >= argc) fail("missing value for " + arg);
			return argv[++i];
			};
		if (arg == "--out") out_dir = next();
		else if (arg == "--runs") runs = std::stoi(next());
		else if (arg == "--chunk") chunk_side = static_cast<uint16_t>(std::stoi(next()));
		else if (arg == "--vox") extra_vox.push_back(next());
		else if (arg == "--sweep") sweep = true;
		else if (arg == "--help") {
			std::printf("usage: bench_vox [--out DIR] [--runs N] [--chunk N] [--vox FILE]... [--sweep]\n");
			return 0;
		}
		else fail("unknown argument " + arg);
	}

	fs::create_directories(out_dir);
	std::printf("bsvx %s\n", bsvx_build_info());
	std::printf("output %s, %d timed runs (median), chunk %ux%ux%u, warm page cache\n",
		out_dir.string().c_str(), runs, chunk_side, chunk_side, chunk_side);

	bsvx_context* ctx = bsvx_context_create();
	if (!ctx) fail("bsvx_context_create returned null");

	struct Case final { Grid grid; uint32_t keys; };
	std::vector<Case> cases;
	cases.push_back({ make_terrain("terrain_256", 256, 64, 256, false), 5 });
	cases.push_back({ make_terrain("terrain_caves", 256, 64, 256, true), 5 });
	cases.push_back({ make_shell("shell_128", 128, 8), 8 });
	cases.push_back({ make_prop("prop_64", 64, 32), 32 });
	cases.push_back({ make_scatter("scatter_256", 256, 0.001, 64), 64 });
	cases.push_back({ make_solid("solid_256", 256), 1 });
	cases.push_back({ make_white_noise("noise_64", 64, 255), 255 });

	for (const fs::path& path : extra_vox) cases.push_back({ grid_from_vox(path), 255 });

	std::vector<Row> rows;
	for (const Case& item : cases) {
		std::fprintf(stderr, "  %s (%ux%ux%u) ...\n", item.grid.name.c_str(), item.grid.sx, item.grid.sy, item.grid.sz);
		rows.push_back(run_scene(ctx, item.grid, out_dir, chunk_side, item.keys, runs));
	}

	print_table(rows);
	print_anatomy(rows);
	for (const Row& r : rows) std::printf("  %-18s %s\n", r.scene.c_str(), r.note.c_str());
	print_csv(rows);

	// Chunk size is the one bsvx knob with a real size/time tradeoff, and it is set by the
	// importer rather than by the format, so it is worth seeing rather than assuming.
	if (sweep) {
		std::printf("\nchunk size sweep\n");
		std::printf("%-18s %6s | %10s %10s | %12s %12s\n", "scene", "chunk", ".bvx", ".bvx.gz", "decode", "open");
		std::printf("%s\n", std::string(70, '-').c_str());
		for (uint16_t side : { 4, 8, 16, 32, 64 }) {
			for (const Case& item : cases) {
				if (item.grid.name != "terrain_256" && item.grid.name != "shell_128") continue;
				const Row row = run_scene(ctx, item.grid, out_dir, side, item.keys, runs);
				std::printf("%-18s %6u | %10s %10s | %9.3f ms %9.3f ms\n",
					row.scene.c_str(), side, human(row.bvx_bytes).c_str(), human(row.bvx_gz).c_str(),
					row.bvx_load_ms, row.bvx_open_ms);
			}
		}
	}

	bsvx_context_destroy(ctx);
	return 0;
}

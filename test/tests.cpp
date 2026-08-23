#include <bsvx_dll.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

    struct TestFailure final : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

#define REQUIRE(expr) do { if (!(expr)) throw TestFailure(std::string("REQUIRE failed: ") + #expr + " at " + __FILE__ + ":" + std::to_string(__LINE__)); } while (0)
#define REQUIRE_EQ(a, b) do { const auto& _lhs = (a); const auto& _rhs = (b); if (!((_lhs) == (_rhs))) throw TestFailure(std::string("REQUIRE_EQ failed: ") + #a + " == " + #b + " at " + __FILE__ + ":" + std::to_string(__LINE__)); } while (0)

    class Context final {
    public:
        Context()
            : ctx_(bsvx_context_create())
        {
            if (!ctx_) throw TestFailure("bsvx_context_create() returned null");
        }

        ~Context()
        {
            bsvx_context_destroy(ctx_);
        }

        bsvx_context* get() const noexcept { return ctx_; }
        std::string last_error() const { return bsvx_context_last_error(ctx_); }

    private:
        bsvx_context* ctx_ = nullptr;
    };

    class World final {
    public:
        World() = default;
        explicit World(bsvx_world* w) : world_(w) {}

        World(const World&) = delete;
        World& operator=(const World&) = delete;

        World(World&& other) noexcept : world_(other.world_) { other.world_ = nullptr; }
        World& operator=(World&& other) noexcept
        {
            if (this != &other) {
                reset();
                world_ = other.world_;
                other.world_ = nullptr;
            }
            return *this;
        }

        ~World() { reset(); }

        void reset()
        {
            if (world_) {
                bsvx_world_destroy(world_);
                world_ = nullptr;
            }
        }

        bsvx_world* get() const noexcept { return world_; }
        explicit operator bool() const noexcept { return world_ != nullptr; }

    private:
        bsvx_world* world_ = nullptr;
    };

    class Reader final {
    public:
        Reader() = default;
        explicit Reader(bsvx_region_reader* r) : reader_(r) {}

        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        Reader(Reader&& other) noexcept : reader_(other.reader_) { other.reader_ = nullptr; }
        Reader& operator=(Reader&& other) noexcept
        {
            if (this != &other) {
                reset();
                reader_ = other.reader_;
                other.reader_ = nullptr;
            }
            return *this;
        }

        ~Reader() { reset(); }

        void reset()
        {
            if (reader_) {
                bsvx_region_reader_close(reader_);
                reader_ = nullptr;
            }
        }

        bsvx_region_reader* get() const noexcept { return reader_; }
        explicit operator bool() const noexcept { return reader_ != nullptr; }

    private:
        bsvx_region_reader* reader_ = nullptr;
    };

    // The tests speak UTF-8 to the C API, so they need the same conversions the library uses.
    // path::string() must not be used for this: on MSVC it narrows through the ACTIVE CODE PAGE and
    // throws std::system_error for anything it cannot represent, so a non-ASCII temp directory would
    // take the whole suite down.
    std::string u8_to_string(const char8_t* text)
    {
        const std::u8string owned(text);
        return std::string(reinterpret_cast<const char*>(owned.data()), owned.size());
    }

    std::string path_to_utf8_string(const fs::path& path)
    {
        const std::u8string owned = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(owned.data()), owned.size());
    }

    std::vector<unsigned char> read_file_bytes(const fs::path& path)
    {
        std::ifstream is(path, std::ios::binary);
        if (!is) throw TestFailure("could not open " + path_to_utf8_string(path));
        is.seekg(0, std::ios::end);
        const auto size = static_cast<size_t>(is.tellg());
        is.seekg(0, std::ios::beg);
        std::vector<unsigned char> bytes(size);
        if (size != 0) is.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!is) throw TestFailure("could not read " + path_to_utf8_string(path));
        return bytes;
    }

    [[noreturn]] void throw_last_error(const Context& ctx, const char* what)
    {
        throw TestFailure(std::string(what) + ": " + ctx.last_error());
    }

    World load_world(Context& ctx, const fs::path& path)
    {
        bsvx_world* raw = nullptr;
        const auto rc = bsvx_world_load(ctx.get(), path_to_utf8_string(path).c_str(), &raw);
        if (rc != BSVX_RESULT_OK) throw_last_error(ctx, "bsvx_world_load failed");
        REQUIRE(raw != nullptr);
        return World(raw);
    }

    World load_region(Context& ctx, const fs::path& path)
    {
        bsvx_world* raw = nullptr;
        const auto rc = bsvx_world_load_region(ctx.get(), path_to_utf8_string(path).c_str(), &raw);
        if (rc != BSVX_RESULT_OK) throw_last_error(ctx, "bsvx_world_load_region failed");
        REQUIRE(raw != nullptr);
        return World(raw);
    }

    bsvx_geometry_desc world_geometry(const World& world)
    {
        bsvx_geometry_desc g{};
        REQUIRE_EQ(bsvx_world_geometry(world.get(), &g), BSVX_RESULT_OK);
        return g;
    }

    std::vector<uint32_t> decode_chunk(const World& world,
        size_t region_index,
        uint16_t cx,
        uint16_t cy,
        uint16_t cz)
    {
        const size_t required = bsvx_region_required_voxel_count(world.get(), region_index);
        REQUIRE(required != 0u);

        std::vector<uint32_t> voxels(required);
        size_t written = 0;
        const auto rc = bsvx_region_decode_chunk_u32(
            world.get(), region_index, cx, cy, cz,
            voxels.data(), voxels.size(), &written);

        REQUIRE_EQ(rc, BSVX_RESULT_OK);
        REQUIRE_EQ(written, required);
        return voxels;
    }

    fs::path make_temp_dir(std::string_view name)
    {
        const auto base = fs::temp_directory_path() / "bsvx_api_tests" / std::string(name);
        fs::remove_all(base);
        fs::create_directories(base);
        return base;
    }

    fs::path repo_root_from_cwd()
    {
        fs::path p = fs::current_path();
        for (int i = 0; i < 8; ++i) {
            if (fs::exists(p / "test")) return p;
            if (!p.has_parent_path()) break;
            p = p.parent_path();
        }
        throw TestFailure("Could not locate repository root containing test/");
    }

    fs::path fixture_world_root()
    {
        return repo_root_from_cwd() / "test" / "data" / "world_basic";
    }

    fs::path fixture_manifest()
    {
        return fixture_world_root() / "manifest.toml";
    }

    fs::path fixture_region()
    {
        return repo_root_from_cwd() / "test" / "data" / "standalone_region" / "standalone_region.bvx";
    }

    void require_fixture_exists(const fs::path& p)
    {
        REQUIRE(fs::exists(p));
    }

    void test_load_standalone_region()
    {
        Context ctx;
        const fs::path region = fixture_region();
        require_fixture_exists(region);

        World world = load_region(ctx, region);
        REQUIRE(world);
        REQUIRE_EQ(bsvx_world_region_count(world.get()), 1u);
        REQUIRE(bsvx_world_texture_count(world.get()) >= 1u);

        const auto g = world_geometry(world);
        REQUIRE(g.chunk_size_x > 0);
        REQUIRE(g.chunk_size_y > 0);
        REQUIRE(g.chunk_size_z > 0);

        int32_t rx = 0, ry = 0, rz = 0;
        REQUIRE_EQ(bsvx_world_get_region_coord(world.get(), 0, &rx, &ry, &rz), BSVX_RESULT_OK);

        const size_t chunk_count = bsvx_region_chunk_count(world.get(), 0);
        REQUIRE(chunk_count >= 1u);

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);

        const auto decoded = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE_EQ(decoded.size(), static_cast<size_t>(g.chunk_size_x) * g.chunk_size_y * g.chunk_size_z);

        bool any_nonzero = false;
        for (uint32_t v : decoded) {
            if (v != 0u) {
                any_nonzero = true;
                break;
            }
        }
        REQUIRE(any_nonzero);
    }

    void test_load_world_and_roundtrip_save()
    {
        Context ctx;
        const fs::path manifest = fixture_manifest();
        require_fixture_exists(manifest);

        World world = load_world(ctx, manifest);
        REQUIRE(world);
        REQUIRE_EQ(bsvx_world_region_count(world.get()), 2u);
        REQUIRE(bsvx_world_texture_count(world.get()) >= 1u);

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);
        const auto before = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);

        const fs::path out_root = make_temp_dir("save_world_roundtrip");
        REQUIRE_EQ(bsvx_world_save(world.get(), path_to_utf8_string(out_root).c_str()), BSVX_RESULT_OK);
        REQUIRE(fs::exists(out_root / "manifest.toml"));

        World loaded_again = load_world(ctx, out_root / "manifest.toml");
        REQUIRE_EQ(bsvx_world_region_count(loaded_again.get()), 2u);

        bsvx_chunk_info info2{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(loaded_again.get(), 0, 0, &info2), BSVX_RESULT_OK);
        const auto after = decode_chunk(loaded_again, 0, info2.local_chunk_x, info2.local_chunk_y, info2.local_chunk_z);
        REQUIRE(before == after);
    }

    void test_mutate_chunk_and_save()
    {
        Context ctx;
        const fs::path manifest = fixture_manifest();
        require_fixture_exists(manifest);

        World world = load_world(ctx, manifest);
        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);

        auto dense = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(!dense.empty());

        dense[0] = (dense[0] == 0u) ? 1u : 0u;
        dense[dense.size() / 2] = 1u;

        REQUIRE_EQ(
            bsvx_region_set_chunk_u32(world.get(),
                0,
                info.local_chunk_x,
                info.local_chunk_y,
                info.local_chunk_z,
                dense.data(),
                dense.size(),
                0xFFFFu),
            BSVX_RESULT_OK);

        const std::array<unsigned char, 4> baked = { 0x11, 0x22, 0x33, 0x44 };
        REQUIRE_EQ(
            bsvx_region_set_chunk_payload(world.get(),
                0,
                3u, // SURFACE
                info.local_chunk_x,
                info.local_chunk_y,
                info.local_chunk_z,
                7u,
                baked.data(),
                baked.size(),
                0u),
            BSVX_RESULT_OK);

        const fs::path out_root = make_temp_dir("mutate_and_save");
        REQUIRE_EQ(bsvx_world_save(world.get(), path_to_utf8_string(out_root).c_str()), BSVX_RESULT_OK);

        World loaded_again = load_world(ctx, out_root / "manifest.toml");
        const auto roundtripped = decode_chunk(loaded_again, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(roundtripped == dense);
    }

    // -----------------------------------------------------------------------------------------
    // ABI v2: memory loading, payload read-back, streaming reader, error reporting, compaction
    // -----------------------------------------------------------------------------------------

    void test_abi_version()
    {
        REQUIRE(bsvx_abi_version() >= 3u);
    }

    void test_load_region_from_memory()
    {
        Context ctx;
        const fs::path region = fixture_region();
        require_fixture_exists(region);

        // Reference: the same region loaded off disk.
        World from_disk = load_region(ctx, region);
        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(from_disk.get(), 0, 0, &info), BSVX_RESULT_OK);
        const auto expected = decode_chunk(from_disk, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);

        const auto bytes = read_file_bytes(region);
        bsvx_world* raw = nullptr;
        const auto rc = bsvx_world_load_region_memory(ctx.get(), bytes.data(), bytes.size(), &raw);
        if (rc != BSVX_RESULT_OK) throw_last_error(ctx, "bsvx_world_load_region_memory failed");
        World from_memory(raw);

        REQUIRE_EQ(bsvx_world_region_count(from_memory.get()), 1u);
        // No texture_root was given, so nothing on disk was touched.
        REQUIRE_EQ(bsvx_world_texture_count(from_memory.get()), 0u);
        REQUIRE_EQ(bsvx_region_chunk_count(from_memory.get(), 0), bsvx_region_chunk_count(from_disk.get(), 0));

        const auto actual = decode_chunk(from_memory, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(actual == expected);

        // Truncated buffers must be rejected, not read past.
        bsvx_world* bad = nullptr;
        REQUIRE(bsvx_world_load_region_memory(ctx.get(), bytes.data(), bytes.size() / 2, &bad) != BSVX_RESULT_OK);
        REQUIRE(bad == nullptr);
        REQUIRE(!ctx.last_error().empty());

        // The _ex form resolves .btx files relative to a real directory.
        bsvx_world* with_tex = nullptr;
        REQUIRE_EQ(
            bsvx_world_load_region_memory_ex(ctx.get(), bytes.data(), bytes.size(),
                path_to_utf8_string(region.parent_path()).c_str(), &with_tex),
            BSVX_RESULT_OK);
        World textured(with_tex);
        REQUIRE(bsvx_world_texture_count(textured.get()) >= 1u);
    }

    void test_find_chunk()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        const size_t chunks = bsvx_region_chunk_count(world.get(), 0);
        REQUIRE(chunks >= 1u);

        for (size_t i = 0; i < chunks; ++i) {
            bsvx_chunk_info info{};
            REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, i, &info), BSVX_RESULT_OK);

            size_t ordinal = ~size_t{ 0 };
            REQUIRE_EQ(
                bsvx_region_find_chunk(world.get(), 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z, &ordinal),
                BSVX_RESULT_OK);
            REQUIRE_EQ(ordinal, i);
        }

        size_t missing = 0;
        REQUIRE_EQ(bsvx_region_find_chunk(world.get(), 0, 999, 999, 999, &missing), BSVX_RESULT_NOT_FOUND);
    }

    // A set_chunk on a manifest-world region used to rebuild the summary without the world's
    // registry, counting every non-air voxel as opaque and zeroing emissive/special.
    void test_summary_survives_write()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        bsvx_chunk_info before{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &before), BSVX_RESULT_OK);
        REQUIRE(before.summary.non_air_count > 0u);
        REQUIRE(before.summary.opaque_count < before.summary.non_air_count); // fixture has a special voxel
        REQUIRE(before.summary.special_count > 0u);

        // Rewrite the chunk with exactly the voxels it already holds.
        const auto dense = decode_chunk(world, 0, before.local_chunk_x, before.local_chunk_y, before.local_chunk_z);
        REQUIRE_EQ(
            bsvx_region_set_chunk_u32(world.get(), 0, before.local_chunk_x, before.local_chunk_y, before.local_chunk_z,
                dense.data(), dense.size(), 0xFFFFu),
            BSVX_RESULT_OK);

        bsvx_chunk_info after{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &after), BSVX_RESULT_OK);
        REQUIRE_EQ(after.summary.non_air_count, before.summary.non_air_count);
        REQUIRE_EQ(after.summary.opaque_count, before.summary.opaque_count);
        REQUIRE_EQ(after.summary.emissive_count, before.summary.emissive_count);
        REQUIRE_EQ(after.summary.special_count, before.summary.special_count);
        REQUIRE_EQ(after.summary.macro_occ_4x4x4, before.summary.macro_occ_4x4x4);

        // And it has to survive the save/reload round-trip too.
        const fs::path out_root = make_temp_dir("summary_fidelity");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str()), BSVX_RESULT_OK);

        World reloaded = load_world(ctx, out_root / "manifest.toml");
        bsvx_chunk_info round_tripped{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(reloaded.get(), 0, 0, &round_tripped), BSVX_RESULT_OK);
        REQUIRE_EQ(round_tripped.summary.opaque_count, before.summary.opaque_count);
        REQUIRE_EQ(round_tripped.summary.special_count, before.summary.special_count);
    }

    void test_payload_readback()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);

        const std::array<unsigned char, 6> baked = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x7F };
        REQUIRE_EQ(
            bsvx_region_set_chunk_payload(world.get(), 0, 3u /*SURFACE*/,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                42u, baked.data(), baked.size(), 5u),
            BSVX_RESULT_OK);

        size_t size = 0;
        uint16_t codec = 0;
        uint16_t flags = 0;
        REQUIRE_EQ(
            bsvx_region_get_chunk_payload_info(world.get(), 0, 3u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z, &size, &codec, &flags),
            BSVX_RESULT_OK);
        REQUIRE_EQ(size, baked.size());
        REQUIRE_EQ(codec, 42u);
        REQUIRE_EQ(flags, 5u);

        // Undersized buffer reports the required size and writes nothing.
        std::array<unsigned char, 2> tiny{};
        size_t needed = 0;
        REQUIRE_EQ(
            bsvx_region_get_chunk_payload(world.get(), 0, 3u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                tiny.data(), tiny.size(), &needed, nullptr),
            BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(needed, baked.size());

        std::vector<unsigned char> out(needed);
        uint16_t out_codec = 0;
        REQUIRE_EQ(
            bsvx_region_get_chunk_payload(world.get(), 0, 3u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                out.data(), out.size(), &needed, &out_codec),
            BSVX_RESULT_OK);
        REQUIRE_EQ(out_codec, 42u);
        REQUIRE(std::memcmp(out.data(), baked.data(), baked.size()) == 0);

        // A section the chunk has nothing in.
        REQUIRE_EQ(
            bsvx_region_get_chunk_payload_info(world.get(), 0, 6u /*LIGHT*/,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z, &size, nullptr, nullptr),
            BSVX_RESULT_NOT_FOUND);

        // Survives a save/reload.
        const fs::path out_root = make_temp_dir("payload_readback");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str()), BSVX_RESULT_OK);

        World reloaded = load_world(ctx, out_root / "manifest.toml");
        std::vector<unsigned char> again(baked.size());
        size_t again_size = 0;
        REQUIRE_EQ(
            bsvx_region_get_chunk_payload_ex(ctx.get(), reloaded.get(), 0, 3u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                again.data(), again.size(), &again_size, &out_codec),
            BSVX_RESULT_OK);
        REQUIRE_EQ(again_size, baked.size());
        REQUIRE_EQ(out_codec, 42u);
        REQUIRE(again == out);
    }

    void test_error_reporting_ex()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        std::vector<uint32_t> scratch(bsvx_region_required_voxel_count(world.get(), 0));
        size_t written = 0;

        // The legacy entry point still returns a bare code...
        REQUIRE_EQ(
            bsvx_region_decode_chunk_u32(world.get(), 0, 99, 99, 99, scratch.data(), scratch.size(), &written),
            BSVX_RESULT_RUNTIME_ERROR);

        // ...while the _ex form explains itself.
        REQUIRE_EQ(
            bsvx_region_decode_chunk_u32_ex(ctx.get(), world.get(), 0, 99, 99, 99, scratch.data(), scratch.size(), &written),
            BSVX_RESULT_RUNTIME_ERROR);
        REQUIRE(!ctx.last_error().empty());

        // Saving somewhere impossible must report why. The destination has to be one no platform
        // can create: "/proc/..." only fails on Linux -- on Windows a leading slash means "root of
        // the current drive", so it resolves to a perfectly creatable D:\proc\... A path leading
        // *through* a regular file cannot be created as a directory anywhere.
        const fs::path blocked_root = make_temp_dir("error_reporting_blocked");
        const fs::path blocker = blocked_root / "not_a_directory";
        {
            std::ofstream os(blocker, std::ios::binary);
            os << "x";
        }
        REQUIRE(fs::is_regular_file(blocker));

        const fs::path impossible = blocker / "world";
        REQUIRE(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(impossible).c_str()) != BSVX_RESULT_OK);
        REQUIRE(!ctx.last_error().empty());

        // A successful call clears the previous message.
        const fs::path out_root = make_temp_dir("error_reporting");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str()), BSVX_RESULT_OK);
        REQUIRE(ctx.last_error().empty());
    }

    void test_compaction()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);
        auto dense = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);

        REQUIRE_EQ(bsvx_region_reclaimable_bytes(world.get(), 0), 0u);

        // Repeated edits append and repoint; the dead bytes accumulate.
        for (uint32_t i = 0; i < 8; ++i) {
            dense[i % dense.size()] = (i % 3u) + 1u;
            REQUIRE_EQ(
                bsvx_region_set_chunk_u32(world.get(), 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                    dense.data(), dense.size(), 0xFFFFu),
                BSVX_RESULT_OK);
        }

        const size_t reclaimable = bsvx_region_reclaimable_bytes(world.get(), 0);
        REQUIRE(reclaimable > 0u);

        const fs::path fat_root = make_temp_dir("compaction_fat");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(fat_root).c_str()), BSVX_RESULT_OK);
        const auto fat_size = fs::file_size(fat_root / "regions" / "r_0_0_0.bvx");

        size_t reclaimed = 0;
        REQUIRE_EQ(bsvx_world_compact(world.get(), &reclaimed), BSVX_RESULT_OK);
        REQUIRE_EQ(reclaimed, reclaimable);
        REQUIRE_EQ(bsvx_region_reclaimable_bytes(world.get(), 0), 0u);

        // Compaction is data-preserving.
        const auto after = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(after == dense);

        const fs::path lean_root = make_temp_dir("compaction_lean");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(lean_root).c_str()), BSVX_RESULT_OK);
        const auto lean_size = fs::file_size(lean_root / "regions" / "r_0_0_0.bvx");
        REQUIRE(lean_size < fat_size);

        World reloaded = load_world(ctx, lean_root / "manifest.toml");
        const auto round_tripped = decode_chunk(reloaded, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(round_tripped == dense);
    }

    void test_region_reader_standalone()
    {
        Context ctx;
        const fs::path region = fixture_region();
        require_fixture_exists(region);

        World eager = load_region(ctx, region);

        bsvx_region_reader* raw = nullptr;
        if (bsvx_region_reader_open(ctx.get(), path_to_utf8_string(region).c_str(), &raw) != BSVX_RESULT_OK) {
            throw_last_error(ctx, "bsvx_region_reader_open failed");
        }
        Reader reader(raw);

        REQUIRE(bsvx_region_reader_is_standalone(reader.get()) == 1);
        REQUIRE_EQ(bsvx_region_reader_verify(ctx.get(), reader.get()), BSVX_RESULT_OK);

        // A standalone region carries its own geometry.
        bsvx_geometry_desc rg{};
        REQUIRE_EQ(bsvx_region_reader_geometry(reader.get(), &rg), BSVX_RESULT_OK);
        const auto eg = world_geometry(eager);
        REQUIRE_EQ(rg.chunk_size_x, eg.chunk_size_x);
        REQUIRE_EQ(rg.chunk_size_y, eg.chunk_size_y);
        REQUIRE_EQ(rg.chunk_size_z, eg.chunk_size_z);

        REQUIRE_EQ(bsvx_region_reader_chunk_count(reader.get()), bsvx_region_chunk_count(eager.get(), 0));
        REQUIRE_EQ(bsvx_region_reader_required_voxel_count(reader.get()), bsvx_region_required_voxel_count(eager.get(), 0));
        REQUIRE_EQ(bsvx_region_reader_registry_entry_count(reader.get()), bsvx_world_registry_entry_count(eager.get()));

        int32_t rx = 0, ry = 0, rz = 0, ex = 0, ey = 0, ez = 0;
        REQUIRE_EQ(bsvx_region_reader_coord(reader.get(), &rx, &ry, &rz), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_get_region_coord(eager.get(), 0, &ex, &ey, &ez), BSVX_RESULT_OK);
        REQUIRE(rx == ex && ry == ey && rz == ez);

        // Metadata is resident; the payload blobs are not.
        const auto file_size = static_cast<size_t>(fs::file_size(region));
        REQUIRE(bsvx_region_reader_resident_bytes(reader.get()) < file_size);

        // Every chunk decodes to the same voxels as the fully-resident archive.
        const size_t chunks = bsvx_region_reader_chunk_count(reader.get());
        REQUIRE(chunks >= 1u);
        for (size_t i = 0; i < chunks; ++i) {
            bsvx_chunk_info lazy_info{};
            bsvx_chunk_info eager_info{};
            REQUIRE_EQ(bsvx_region_reader_get_chunk_info(reader.get(), i, &lazy_info), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_region_get_chunk_info(eager.get(), 0, i, &eager_info), BSVX_RESULT_OK);
            REQUIRE(std::memcmp(&lazy_info, &eager_info, sizeof(bsvx_chunk_info)) == 0);

            size_t ordinal = ~size_t{ 0 };
            REQUIRE_EQ(
                bsvx_region_reader_find_chunk(reader.get(), lazy_info.local_chunk_x, lazy_info.local_chunk_y, lazy_info.local_chunk_z, &ordinal),
                BSVX_RESULT_OK);
            REQUIRE_EQ(ordinal, i);

            std::vector<uint32_t> lazy(bsvx_region_reader_required_voxel_count(reader.get()));
            size_t written = 0;
            REQUIRE_EQ(
                bsvx_region_reader_decode_chunk_u32(ctx.get(), reader.get(),
                    lazy_info.local_chunk_x, lazy_info.local_chunk_y, lazy_info.local_chunk_z,
                    lazy.data(), lazy.size(), &written),
                BSVX_RESULT_OK);
            REQUIRE_EQ(written, lazy.size());

            const auto expected = decode_chunk(eager, 0, lazy_info.local_chunk_x, lazy_info.local_chunk_y, lazy_info.local_chunk_z);
            REQUIRE(lazy == expected);
        }

        bsvx_chunk_info first{};
        REQUIRE_EQ(bsvx_region_reader_get_chunk_info(reader.get(), 0, &first), BSVX_RESULT_OK);

        // Missing chunks and undersized buffers behave like the eager path.
        std::vector<uint32_t> scratch(bsvx_region_reader_required_voxel_count(reader.get()));
        size_t written = 0;
        REQUIRE_EQ(
            bsvx_region_reader_decode_chunk_u32(ctx.get(), reader.get(), 250, 250, 250, scratch.data(), scratch.size(), &written),
            BSVX_RESULT_RUNTIME_ERROR);
        REQUIRE(!ctx.last_error().empty());
        REQUIRE_EQ(
            bsvx_region_reader_decode_chunk_u32(ctx.get(), reader.get(),
                first.local_chunk_x, first.local_chunk_y, first.local_chunk_z, scratch.data(), 1u, &written),
            BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(written, scratch.size());

        // Promoting a reader to a full world.
        bsvx_world* promoted_raw = nullptr;
        REQUIRE_EQ(bsvx_region_reader_load_full(ctx.get(), reader.get(), &promoted_raw), BSVX_RESULT_OK);
        World promoted(promoted_raw);
        REQUIRE_EQ(bsvx_region_chunk_count(promoted.get(), 0), chunks);
        const auto promoted_voxels = decode_chunk(promoted, 0, first.local_chunk_x, first.local_chunk_y, first.local_chunk_z);
        const auto eager_voxels = decode_chunk(eager, 0, first.local_chunk_x, first.local_chunk_y, first.local_chunk_z);
        REQUIRE(promoted_voxels == eager_voxels);
    }

    void test_region_reader_manifest_region()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());
        const auto geometry = world_geometry(world);

        const fs::path region = fixture_world_root() / "regions" / "r_0_0_0.bvx";
        require_fixture_exists(region);

        // Same file, this time out of a buffer, borrowed rather than copied.
        const auto bytes = read_file_bytes(region);
        bsvx_region_reader* raw = nullptr;
        if (bsvx_region_reader_open_memory(ctx.get(), bytes.data(), bytes.size(), 0, &raw) != BSVX_RESULT_OK) {
            throw_last_error(ctx, "bsvx_region_reader_open_memory failed");
        }
        Reader reader(raw);

        // A region belonging to a manifest world has no geometry of its own.
        REQUIRE(bsvx_region_reader_is_standalone(reader.get()) == 0);
        bsvx_geometry_desc probe{};
        REQUIRE_EQ(bsvx_region_reader_geometry(reader.get(), &probe), BSVX_RESULT_NOT_FOUND);
        REQUIRE_EQ(bsvx_region_reader_required_voxel_count(reader.get()), 0u);

        REQUIRE_EQ(bsvx_region_reader_set_geometry(reader.get(), &geometry), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_reader_geometry(reader.get(), &probe), BSVX_RESULT_OK);
        REQUIRE_EQ(probe.chunk_size_x, geometry.chunk_size_x);
        REQUIRE_EQ(
            bsvx_region_reader_required_voxel_count(reader.get()),
            bsvx_region_required_voxel_count(world.get(), 0));

        const size_t chunks = bsvx_region_reader_chunk_count(reader.get());
        REQUIRE_EQ(chunks, bsvx_region_chunk_count(world.get(), 0));

        for (size_t i = 0; i < chunks; ++i) {
            bsvx_chunk_info info{};
            REQUIRE_EQ(bsvx_region_reader_get_chunk_info(reader.get(), i, &info), BSVX_RESULT_OK);

            std::vector<uint32_t> lazy(bsvx_region_reader_required_voxel_count(reader.get()));
            size_t written = 0;
            REQUIRE_EQ(
                bsvx_region_reader_decode_chunk_u32(ctx.get(), reader.get(),
                    info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                    lazy.data(), lazy.size(), &written),
                BSVX_RESULT_OK);

            const auto expected = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
            REQUIRE(lazy == expected);
        }

        // A non-standalone region cannot become a world on its own.
        bsvx_world* promoted = nullptr;
        REQUIRE_EQ(bsvx_region_reader_load_full(ctx.get(), reader.get(), &promoted), BSVX_RESULT_INVALID_ARGUMENT);
        REQUIRE(promoted == nullptr);
    }

    void test_region_reader_payload()
    {
        Context ctx;
        World world = load_region(ctx, fixture_region());

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);

        const std::array<unsigned char, 5> baked = { 1, 2, 3, 4, 5 };
        REQUIRE_EQ(
            bsvx_region_set_chunk_payload(world.get(), 0, 4u /*COLLISION*/,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                9u, baked.data(), baked.size(), 0u),
            BSVX_RESULT_OK);

        const fs::path out_dir = make_temp_dir("reader_payload");
        const fs::path out_region = out_dir / "region.bvx";
        REQUIRE_EQ(bsvx_world_save_region_ex(ctx.get(), world.get(), path_to_utf8_string(out_region).c_str()), BSVX_RESULT_OK);

        bsvx_region_reader* raw = nullptr;
        if (bsvx_region_reader_open(ctx.get(), path_to_utf8_string(out_region).c_str(), &raw) != BSVX_RESULT_OK) {
            throw_last_error(ctx, "bsvx_region_reader_open failed");
        }
        Reader reader(raw);

        size_t needed = 0;
        REQUIRE_EQ(
            bsvx_region_reader_get_chunk_payload(ctx.get(), reader.get(), 4u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                nullptr, 0u, &needed, nullptr),
            BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(needed, baked.size());

        std::vector<unsigned char> out(needed);
        uint16_t codec = 0;
        REQUIRE_EQ(
            bsvx_region_reader_get_chunk_payload(ctx.get(), reader.get(), 4u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                out.data(), out.size(), &needed, &codec),
            BSVX_RESULT_OK);
        REQUIRE_EQ(codec, 9u);
        REQUIRE(std::memcmp(out.data(), baked.data(), baked.size()) == 0);

        // Nothing was baked into LIGHT for this chunk.
        REQUIRE_EQ(
            bsvx_region_reader_get_chunk_payload(ctx.get(), reader.get(), 6u,
                info.local_chunk_x, info.local_chunk_y, info.local_chunk_z,
                out.data(), out.size(), &needed, &codec),
            BSVX_RESULT_NOT_FOUND);
        REQUIRE_EQ(needed, 0u);
    }

    // -----------------------------------------------------------------------------------------
    // ABI v3: authoring, metadata, region-to-buffer, VFS loading, .btx introspection
    // -----------------------------------------------------------------------------------------

    std::string get_string(const std::function<bsvx_result(char*, size_t, size_t*)>& getter)
    {
        size_t needed = 0;
        REQUIRE_EQ(getter(nullptr, 0u, &needed), BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE(needed >= 1u);

        std::vector<char> buffer(needed);
        REQUIRE_EQ(getter(buffer.data(), buffer.size(), &needed), BSVX_RESULT_OK);
        REQUIRE_EQ(buffer[needed - 1u], '\0');
        return std::string(buffer.data());
    }

    void test_world_metadata()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        bsvx_world_desc desc{};
        REQUIRE_EQ(bsvx_world_get_desc(world.get(), &desc), BSVX_RESULT_OK);

        const auto geometry = world_geometry(world);
        REQUIRE_EQ(desc.geometry.chunk_size_x, geometry.chunk_size_x);
        REQUIRE_EQ(desc.geometry.region_size_z, geometry.region_size_z);
        REQUIRE_EQ(desc.voxel_schema, 1u);          // dense_u32_voxel_key
        REQUIRE_EQ(desc.axis_convention, 0u);       // x_right_y_up_z_forward
        REQUIRE_EQ(desc.bounds_mode, 1u);           // the fixture declares explicit bounds
        REQUIRE_EQ(desc.max_region_x, 1);
        REQUIRE(desc.manifest_hash != 0u);

        REQUIRE_EQ(get_string([&](char* out, size_t cap, size_t* size) {
            return bsvx_world_get_name(world.get(), out, cap, size); }), std::string("FreshBasilWorld"));
        REQUIRE_EQ(get_string([&](char* out, size_t cap, size_t* size) {
            return bsvx_world_get_uuid(world.get(), out, cap, size); }), std::string("basil-world-2r2t"));

        // set_desc validates what save would have thrown on.
        bsvx_world_desc bad = desc;
        bad.geometry.chunk_size_x = 0;
        REQUIRE_EQ(bsvx_world_set_desc(world.get(), &bad), BSVX_RESULT_INVALID_ARGUMENT);
        bad = desc;
        bad.geometry.chunk_size_y = 256;            // ChunkSummary AABBs are uint8_t
        REQUIRE_EQ(bsvx_world_set_desc(world.get(), &bad), BSVX_RESULT_INVALID_ARGUMENT);
        bad = desc;
        bad.axis_convention = 3;
        REQUIRE_EQ(bsvx_world_set_desc(world.get(), &bad), BSVX_RESULT_INVALID_ARGUMENT);

        // Resizing chunks under authored voxels is refused; bounds are free to move.
        bsvx_world_desc resized = desc;
        resized.geometry.chunk_size_x = static_cast<uint16_t>(desc.geometry.chunk_size_x * 2u);
        REQUIRE(bsvx_region_chunk_count(world.get(), 0) > 0u);
        REQUIRE_EQ(bsvx_world_set_desc(world.get(), &resized), BSVX_RESULT_INVALID_ARGUMENT);

        bsvx_world_desc widened = desc;
        widened.max_region_x = 4;
        REQUIRE_EQ(bsvx_world_set_desc(world.get(), &widened), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_get_desc(world.get(), &desc), BSVX_RESULT_OK);
        REQUIRE_EQ(desc.max_region_x, 4);
        REQUIRE_EQ(desc.geometry.chunk_size_x, geometry.chunk_size_x);
    }

    void test_authoring_from_scratch()
    {
        Context ctx;

        bsvx_geometry_desc geometry{};
        geometry.chunk_size_x = geometry.chunk_size_y = geometry.chunk_size_z = 8;
        geometry.region_size_x = geometry.region_size_y = geometry.region_size_z = 2;

        bsvx_world* raw = nullptr;
        if (bsvx_world_create(ctx.get(), &geometry, &raw) != BSVX_RESULT_OK) {
            throw_last_error(ctx, "bsvx_world_create failed");
        }
        World world(raw);
        REQUIRE_EQ(bsvx_world_region_count(world.get()), 0u);

        // A chunk dimension a ChunkSummary cannot describe is refused up front.
        bsvx_geometry_desc oversized = geometry;
        oversized.chunk_size_x = 300;
        bsvx_world* rejected = nullptr;
        REQUIRE_EQ(bsvx_world_create(ctx.get(), &oversized, &rejected), BSVX_RESULT_INVALID_ARGUMENT);
        REQUIRE(rejected == nullptr);

        REQUIRE_EQ(bsvx_world_set_name(world.get(), "AuthoredWorld"), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_set_uuid(world.get(), "authored-0001"), BSVX_RESULT_OK);

        bsvx_registry_entry stone{};
        stone.voxel_key = 1u;
        stone.material_id = 0u;
        stone.flags = 1u;   // OPAQUE
        bsvx_registry_entry lamp{};
        lamp.voxel_key = 2u;
        lamp.material_id = 1u;
        lamp.flags = 2u;    // EMISSIVE
        REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &stone), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &lamp), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_registry_entry_count(world.get()), 2u);

        // Setting the same voxel key again replaces the row instead of appending one.
        stone.material_id = 7u;
        REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &stone), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_registry_entry_count(world.get()), 2u);

        // Key 0 is air by convention and has no registry row.
        bsvx_registry_entry air{};
        REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &air), BSVX_RESULT_INVALID_ARGUMENT);
        REQUIRE_EQ(bsvx_world_remove_registry_entry(world.get(), 99u), BSVX_RESULT_NOT_FOUND);

        size_t region = ~size_t{ 0 };
        REQUIRE_EQ(bsvx_world_add_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);
        REQUIRE_EQ(region, 0u);

        size_t second = ~size_t{ 0 };
        REQUIRE_EQ(bsvx_world_add_region(world.get(), 0, 0, 0, &second), BSVX_RESULT_INVALID_ARGUMENT);
        REQUIRE_EQ(bsvx_world_add_region(world.get(), 1, 0, 0, &second), BSVX_RESULT_OK);
        REQUIRE_EQ(second, 1u);
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 1, 0, 0, &second), BSVX_RESULT_OK);
        REQUIRE_EQ(second, 1u);
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 9, 9, 9, &second), BSVX_RESULT_NOT_FOUND);

        const size_t voxels_per_chunk = bsvx_region_required_voxel_count(world.get(), 0);
        REQUIRE_EQ(voxels_per_chunk, 8u * 8u * 8u);

        std::vector<uint32_t> dense(voxels_per_chunk, 0u);
        dense[0] = 1u;
        dense[1] = 1u;
        dense[2] = 2u;
        REQUIRE_EQ(
            bsvx_region_set_chunk_u32_ex(ctx.get(), world.get(), 0, 0, 0, 0, dense.data(), dense.size(), 0xFFFFu),
            BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_chunk_count(world.get(), 0), 1u);

        // The summary was built against the registry authored above, not a blind "all opaque".
        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);
        REQUIRE_EQ(info.summary.non_air_count, 3u);
        REQUIRE_EQ(info.summary.opaque_count, 2u);
        REQUIRE_EQ(info.summary.emissive_count, 1u);

        const fs::path out_root = make_temp_dir("authored_world");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str()), BSVX_RESULT_OK);
        REQUIRE(fs::exists(out_root / "manifest.toml"));
        REQUIRE(fs::exists(out_root / "regions" / "r_0_0_0.bvx"));
        REQUIRE(fs::exists(out_root / "regions" / "r_1_0_0.bvx"));

        World reloaded = load_world(ctx, out_root / "manifest.toml");
        REQUIRE_EQ(bsvx_world_region_count(reloaded.get()), 2u);
        REQUIRE_EQ(bsvx_world_registry_entry_count(reloaded.get()), 2u);
        REQUIRE_EQ(get_string([&](char* out, size_t cap, size_t* size) {
            return bsvx_world_get_name(reloaded.get(), out, cap, size); }), std::string("AuthoredWorld"));

        const auto reloaded_geometry = world_geometry(reloaded);
        REQUIRE_EQ(reloaded_geometry.chunk_size_x, 8u);
        REQUIRE_EQ(reloaded_geometry.region_size_y, 2u);

        size_t authored_region = 0;
        REQUIRE_EQ(bsvx_world_find_region(reloaded.get(), 0, 0, 0, &authored_region), BSVX_RESULT_OK);
        const auto round_tripped = decode_chunk(reloaded, authored_region, 0, 0, 0);
        REQUIRE(round_tripped == dense);
    }

    void test_save_region_memory()
    {
        Context ctx;
        World world = load_region(ctx, fixture_region());

        // (NULL, 0) sizes the buffer, exactly like the payload getters.
        size_t needed = 0;
        REQUIRE_EQ(
            bsvx_world_save_region_memory(ctx.get(), world.get(), nullptr, 0u, &needed),
            BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE(needed > 0u);

        std::vector<unsigned char> bytes(needed);
        size_t written = 0;
        REQUIRE_EQ(
            bsvx_world_save_region_memory(ctx.get(), world.get(), bytes.data(), bytes.size(), &written),
            BSVX_RESULT_OK);
        REQUIRE_EQ(written, needed);

        // Byte-identical to what save_region would have put on disk.
        const fs::path out_dir = make_temp_dir("save_region_memory");
        const fs::path out_region = out_dir / "region.bvx";
        REQUIRE_EQ(bsvx_world_save_region_ex(ctx.get(), world.get(), path_to_utf8_string(out_region).c_str()), BSVX_RESULT_OK);
        REQUIRE(read_file_bytes(out_region) == bytes);

        // And it loads back as a standalone region.
        bsvx_world* raw = nullptr;
        REQUIRE_EQ(bsvx_world_load_region_memory(ctx.get(), bytes.data(), bytes.size(), &raw), BSVX_RESULT_OK);
        World reloaded(raw);

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, 0, &info), BSVX_RESULT_OK);
        const auto expected = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        const auto actual = decode_chunk(reloaded, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(actual == expected);
    }

    // A stand-in for Godot's FileAccess/DirAccess behind res://: the library must never touch the
    // real filesystem here, because every path it is handed carries the scheme prefix.
    struct VfsHarness final {
        fs::path root;
        size_t reads = 0;
        size_t listings = 0;
    };

    fs::path vfs_resolve(void* user, const char* path)
    {
        auto* harness = static_cast<VfsHarness*>(user);
        std::string text(path ? path : "");
        const std::string scheme = "res://";
        if (text.rfind(scheme, 0) != 0) throw TestFailure("vfs got a path without the res:// scheme: " + text);
        text.erase(0, scheme.size());
        return harness->root / text;
    }

    int vfs_read_file(void* user, const char* path, void* out, size_t capacity, size_t* out_size)
    {
        const fs::path real = vfs_resolve(user, path);
        std::error_code ec;
        if (!fs::is_regular_file(real, ec)) return 0;

        const auto bytes = read_file_bytes(real);
        static_cast<VfsHarness*>(user)->reads++;
        if (out_size) *out_size = bytes.size();
        if (capacity < bytes.size()) return 1;      // size probe, or a buffer to grow
        if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
        return 1;
    }

    int vfs_file_exists(void* user, const char* path)
    {
        std::error_code ec;
        return fs::is_regular_file(vfs_resolve(user, path), ec) ? 1 : 0;
    }

    int vfs_list_dir(void* user, const char* dir_path, size_t index, char* out_name, size_t capacity, size_t* out_size)
    {
        const fs::path real = vfs_resolve(user, dir_path);
        std::error_code ec;
        if (!fs::is_directory(real, ec)) return 0;

        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(real, ec)) {
            if (entry.is_regular_file()) names.push_back(path_to_utf8_string(entry.path().filename()));
        }
        std::sort(names.begin(), names.end());
        if (index >= names.size()) return 0;

        static_cast<VfsHarness*>(user)->listings++;
        const std::string& name = names[index];
        if (out_size) *out_size = name.size() + 1u;
        if (capacity < name.size() + 1u) return 1;  // caller will come back with a bigger buffer
        std::memcpy(out_name, name.c_str(), name.size() + 1u);
        return 1;
    }

    void test_load_world_through_vfs()
    {
        Context ctx;
        World native = load_world(ctx, fixture_manifest());

        VfsHarness harness{ fixture_world_root(), 0, 0 };
        bsvx_vfs vfs{};
        vfs.user = &harness;
        vfs.read_file = &vfs_read_file;
        vfs.file_exists = &vfs_file_exists;
        vfs.list_dir = &vfs_list_dir;

        bsvx_world* raw = nullptr;
        if (bsvx_world_load_vfs(ctx.get(), "res://manifest.toml", &vfs, &raw) != BSVX_RESULT_OK) {
            throw_last_error(ctx, "bsvx_world_load_vfs failed");
        }
        World world(raw);

        // manifest + 2 regions + textures, all through the callback.
        REQUIRE(harness.reads >= 3u);
        REQUIRE_EQ(bsvx_world_region_count(world.get()), bsvx_world_region_count(native.get()));
        REQUIRE_EQ(bsvx_world_texture_count(world.get()), bsvx_world_texture_count(native.get()));
        REQUIRE_EQ(bsvx_world_registry_entry_count(world.get()), bsvx_world_registry_entry_count(native.get()));

        const size_t chunks = bsvx_region_chunk_count(world.get(), 0);
        REQUIRE_EQ(chunks, bsvx_region_chunk_count(native.get(), 0));
        for (size_t i = 0; i < chunks; ++i) {
            bsvx_chunk_info info{};
            REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, i, &info), BSVX_RESULT_OK);
            const auto through_vfs = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
            const auto expected = decode_chunk(native, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
            REQUIRE(through_vfs == expected);
        }

        // A directory works too, which is what exercises list_dir.
        harness.listings = 0;
        bsvx_world* from_dir = nullptr;
        if (bsvx_world_load_vfs(ctx.get(), "res://", &vfs, &from_dir) != BSVX_RESULT_OK) {
            throw_last_error(ctx, "bsvx_world_load_vfs on a directory failed");
        }
        World world_from_dir(from_dir);
        REQUIRE(harness.listings > 0u);
        REQUIRE_EQ(bsvx_world_region_count(world_from_dir.get()), bsvx_world_region_count(native.get()));

        // A missing manifest fails with a message rather than falling back to the real filesystem.
        bsvx_world* missing = nullptr;
        REQUIRE(bsvx_world_load_vfs(ctx.get(), "res://nope/manifest.toml", &vfs, &missing) != BSVX_RESULT_OK);
        REQUIRE(missing == nullptr);
        REQUIRE(!ctx.last_error().empty());

        // read_file is the one callback that is not optional.
        bsvx_vfs incomplete{};
        incomplete.user = &harness;
        bsvx_world* never = nullptr;
        REQUIRE_EQ(bsvx_world_load_vfs(ctx.get(), "res://manifest.toml", &incomplete, &never), BSVX_RESULT_INVALID_ARGUMENT);
        REQUIRE(never == nullptr);
    }

    void test_texture_introspection()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        const size_t archives = bsvx_world_texture_count(world.get());
        REQUIRE(archives >= 1u);

        const std::string id = get_string([&](char* out, size_t cap, size_t* size) {
            return bsvx_world_get_texture_id(world.get(), 0, out, cap, size); });
        const std::string path = get_string([&](char* out, size_t cap, size_t* size) {
            return bsvx_world_get_texture_path(world.get(), 0, out, cap, size); });
        REQUIRE(!id.empty());
        REQUIRE(path.size() > 4u);
        REQUIRE(path.rfind(".btx") == path.size() - 4u);

        REQUIRE_EQ(bsvx_world_get_texture_id(world.get(), archives, nullptr, 0u, nullptr), BSVX_RESULT_NOT_FOUND);

        const size_t textures = bsvx_texture_texture_count(world.get(), 0);
        REQUIRE(textures >= 1u);

        bsvx_texture_desc desc{};
        REQUIRE_EQ(bsvx_texture_get_desc(world.get(), 0, 0, &desc), BSVX_RESULT_OK);
        REQUIRE(desc.width > 0u);
        REQUIRE(desc.height > 0u);
        REQUIRE(desc.mip_levels >= 1u);
        REQUIRE(desc.array_layers >= 1u);
        REQUIRE_EQ(bsvx_texture_get_desc(world.get(), 0, textures, &desc), BSVX_RESULT_NOT_FOUND);

        // Only RGBA8 is describable today; a .btx cannot hold anything else.
        const uint32_t bpt = bsvx_format_bytes_per_texel(desc.vk_format);
        REQUIRE_EQ(bpt, 4u);
        REQUIRE_EQ(bsvx_format_bytes_per_texel(0u), 0u);

        const size_t subresources = bsvx_texture_subresource_count(world.get(), 0);
        REQUIRE(subresources >= 1u);

        bsvx_subresource_desc sub{};
        REQUIRE_EQ(bsvx_texture_get_subresource_desc(world.get(), 0, 0, &sub), BSVX_RESULT_OK);
        REQUIRE(sub.extent_x > 0u);
        REQUIRE(sub.extent_y > 0u);
        REQUIRE_EQ(sub.size, static_cast<uint64_t>(sub.extent_x) * sub.extent_y * bpt);

        size_t needed = 0;
        REQUIRE_EQ(
            bsvx_texture_get_subresource_bytes(world.get(), 0, 0, nullptr, 0u, &needed),
            BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(needed, static_cast<size_t>(sub.size));

        std::vector<unsigned char> texels(needed);
        size_t written = 0;
        REQUIRE_EQ(
            bsvx_texture_get_subresource_bytes(world.get(), 0, 0, texels.data(), texels.size(), &written),
            BSVX_RESULT_OK);
        REQUIRE_EQ(written, needed);
        REQUIRE_EQ(
            bsvx_texture_get_subresource_bytes(world.get(), 0, subresources, texels.data(), texels.size(), &written),
            BSVX_RESULT_NOT_FOUND);

        for (size_t i = 0; i < bsvx_texture_sampler_count(world.get(), 0); ++i) {
            bsvx_sampler_desc sampler{};
            REQUIRE_EQ(bsvx_texture_get_sampler(world.get(), 0, i, &sampler), BSVX_RESULT_OK);
            REQUIRE(sampler.min_filter <= 1u);
            REQUIRE(sampler.address_u <= 3u);
        }

        // Materials are what a registry entry's material_id points at.
        const size_t materials = bsvx_texture_material_count(world.get(), 0);
        for (size_t i = 0; i < materials; ++i) {
            bsvx_material_desc material{};
            REQUIRE_EQ(bsvx_texture_get_material(world.get(), 0, i, &material), BSVX_RESULT_OK);

            size_t found = ~size_t{ 0 };
            REQUIRE_EQ(bsvx_texture_find_material(world.get(), 0, material.material_id, &found), BSVX_RESULT_OK);
            REQUIRE_EQ(found, i);
        }

        size_t nowhere = 0;
        REQUIRE_EQ(bsvx_texture_find_material(world.get(), 0, 0xFFFFFFFFu, &nowhere), BSVX_RESULT_NOT_FOUND);
        REQUIRE_EQ(bsvx_texture_texture_count(world.get(), archives), 0u);
    }


    /* ------------------------------------------------------------------------------------- */
    /* ABI v4                                                                                 */
    /* ------------------------------------------------------------------------------------- */

    void test_abi_self_description()
    {
        REQUIRE(bsvx_abi_version() >= 4u);

        // The whole point of these is that a hand-written binding can check itself, so they must
        // agree with the compiler exactly.
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_GEOMETRY_DESC), sizeof(bsvx_geometry_desc));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_CHUNK_SUMMARY), sizeof(bsvx_chunk_summary));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_CHUNK_INFO), sizeof(bsvx_chunk_info));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_WORLD_DESC), sizeof(bsvx_world_desc));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_UNITS), sizeof(bsvx_units));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_VALIDATION_ISSUE), sizeof(bsvx_validation_issue));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_SAVE_REPORT), sizeof(bsvx_save_report));
        REQUIRE_EQ(bsvx_struct_size(BSVX_STRUCT_VOXEL_ADDRESS), sizeof(bsvx_voxel_address));
        REQUIRE_EQ(bsvx_struct_size(0xDEADBEEFu), 0u);

        REQUIRE(std::string(bsvx_result_string(BSVX_RESULT_OK)) == "ok");
        REQUIRE(std::string(bsvx_result_string(BSVX_RESULT_CANCELLED)) == "cancelled");
        REQUIRE(std::string(bsvx_build_info()).find("bsvx abi") == 0u);
    }

    // Chunk 4x4x4, region 2x2x2 chunks -> a region spans 8 voxels on each axis.
    World make_authoring_world(Context& ctx)
    {
        bsvx_geometry_desc geometry{ 4, 4, 4, 2, 2, 2 };
        bsvx_world* raw = nullptr;
        if (bsvx_world_create(ctx.get(), &geometry, &raw) != BSVX_RESULT_OK) throw_last_error(ctx, "bsvx_world_create failed");

        bsvx_registry_entry stone{ 1u, 0u, 1u /* opaque */, 0u };
        REQUIRE_EQ(bsvx_world_set_registry_entry(raw, &stone), BSVX_RESULT_OK);
        return World(raw);
    }

    void test_locate_voxel_negative_coords()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        bsvx_voxel_address address{};
        REQUIRE_EQ(bsvx_world_locate_voxel(world.get(), 0, 0, 0, &address), BSVX_RESULT_OK);
        REQUIRE_EQ(address.region_x, 0);
        REQUIRE_EQ(address.chunk_x, 0u);
        REQUIRE_EQ(address.local_x, 0u);

        // 5 -> chunk 1, local 1 inside region 0.
        REQUIRE_EQ(bsvx_world_locate_voxel(world.get(), 5, 0, 0, &address), BSVX_RESULT_OK);
        REQUIRE_EQ(address.region_x, 0);
        REQUIRE_EQ(address.chunk_x, 1u);
        REQUIRE_EQ(address.local_x, 1u);

        // The case truncating division gets wrong: -1 belongs to region -1, not region 0.
        REQUIRE_EQ(bsvx_world_locate_voxel(world.get(), -1, -1, -1, &address), BSVX_RESULT_OK);
        REQUIRE_EQ(address.region_x, -1);
        REQUIRE_EQ(address.region_y, -1);
        REQUIRE_EQ(address.chunk_x, 1u);
        REQUIRE_EQ(address.local_x, 3u);
        REQUIRE_EQ(address.local_index, 3u + 4u * (3u + 4u * 3u));

        REQUIRE_EQ(bsvx_world_locate_voxel(world.get(), -8, 0, 0, &address), BSVX_RESULT_OK);
        REQUIRE_EQ(address.region_x, -1);
        REQUIRE_EQ(address.chunk_x, 0u);
        REQUIRE_EQ(address.local_x, 0u);
    }

    void test_world_space_voxel_io()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        const std::vector<int64_t> xs{ 0, 5, -1, 100 };
        const std::vector<int64_t> ys{ 0, 1,  -1, 100 };
        const std::vector<int64_t> zs{ 0, 2,  -1, 100 };
        const std::vector<uint32_t> keys{ 1, 1, 1, 1 };

        // Nothing exists yet, so without create_missing nothing may be written.
        size_t written = 0;
        REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), xs.data(), ys.data(), zs.data(), keys.data(), keys.size(), 0, &written), BSVX_RESULT_OK);
        REQUIRE_EQ(written, 0u);
        REQUIRE_EQ(bsvx_world_region_count(world.get()), 0u);

        REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), xs.data(), ys.data(), zs.data(), keys.data(), keys.size(), 1, &written), BSVX_RESULT_OK);
        REQUIRE_EQ(written, 4u);
        // (0,0,0) and (5,1,2) share region 0; (-1,-1,-1) and (100,100,100) get their own.
        REQUIRE_EQ(bsvx_world_region_count(world.get()), 3u);

        std::vector<uint32_t> read_back(4, 0xFFFFFFFFu);
        REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), world.get(), xs.data(), ys.data(), zs.data(), read_back.data(), read_back.size()), BSVX_RESULT_OK);
        for (uint32_t key : read_back) REQUIRE_EQ(key, 1u);

        // An untouched coordinate reads back as air rather than failing.
        const int64_t nx = 3, ny = 3, nz = 3;
        uint32_t air = 0xFFFFFFFFu;
        REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), world.get(), &nx, &ny, &nz, &air, 1), BSVX_RESULT_OK);
        REQUIRE_EQ(air, 0u);

        // The summary must have been rebuilt through the registry, not left at zero.
        size_t region = 0;
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);
        size_t ordinal = 0;
        REQUIRE_EQ(bsvx_region_find_chunk(world.get(), region, 0, 0, 0, &ordinal), BSVX_RESULT_OK);
        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, ordinal, &info), BSVX_RESULT_OK);
        REQUIRE_EQ(info.summary.non_air_count, 1u);
        REQUIRE_EQ(info.summary.opaque_count, 1u);
    }

    void test_fill_box()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 3, 3, 3, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(count, 64u);

        // Exactly one 4x4x4 chunk, completely full.
        size_t region = 0;
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_chunk_count(world.get(), region), 1u);

        bsvx_chunk_info info{};
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, 0, &info), BSVX_RESULT_OK);
        REQUIRE_EQ(info.summary.non_air_count, 64u);

        // Erasing is the same call with key 0.
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 3, 3, 3, 0u, 0, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, 0, &info), BSVX_RESULT_OK);
        REQUIRE_EQ(info.summary.non_air_count, 0u);

        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 3, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_INVALID_ARGUMENT);
    }

    void test_registry_names_roundtrip()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("registry_names");

        {
            World world = make_authoring_world(ctx);
            REQUIRE_EQ(bsvx_world_set_registry_name(world.get(), 1u, "stone"), BSVX_RESULT_OK);

            bsvx_registry_entry glow{ 2u, 1u, 2u /* emissive */, 0u };
            REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &glow), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_set_registry_name(world.get(), 2u, "lamp glow"), BSVX_RESULT_OK);

            size_t written = 0;
            const int64_t x = 0, y = 0, z = 0;
            const uint32_t key = 1;
            REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), &x, &y, &z, &key, 1, 1, &written), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        }

        // The regression this exists for: names used to be hashed on parse and dropped on save, so
        // a load/save cycle renamed everything to nothing.
        World reloaded = load_world(ctx, out_root);
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_registry_name(reloaded.get(), 1u, b, c, n); }) == "stone");
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_registry_name(reloaded.get(), 2u, b, c, n); }) == "lamp glow");

        size_t needed = 0;
        REQUIRE_EQ(bsvx_world_get_registry_name(reloaded.get(), 99u, nullptr, 0, &needed), BSVX_RESULT_NOT_FOUND);

        // And through a standalone .bvx, which carries its own copy of the world desc.
        const fs::path region_path = out_root / "standalone.bvx";
        REQUIRE_EQ(bsvx_world_save_region_ex(ctx.get(), reloaded.get(), path_to_utf8_string(region_path).c_str()), BSVX_RESULT_OK);

        World standalone = load_region(ctx, region_path);
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_registry_name(standalone.get(), 1u, b, c, n); }) == "stone");
    }

    void test_units_roundtrip()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("units");

        {
            World world = make_authoring_world(ctx);

            bsvx_units units{ 0.25, 0.25, 0.5, 1.5, -2.0, 3.25 };
            REQUIRE_EQ(bsvx_world_set_units(world.get(), &units), BSVX_RESULT_OK);

            bsvx_units zero{ 0.0, 1.0, 1.0, 0.0, 0.0, 0.0 };
            REQUIRE_EQ(bsvx_world_set_units(world.get(), &zero), BSVX_RESULT_INVALID_ARGUMENT);

            REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        }

        World reloaded = load_world(ctx, out_root);
        bsvx_units units{};
        REQUIRE_EQ(bsvx_world_get_units(reloaded.get(), &units), BSVX_RESULT_OK);
        REQUIRE_EQ(units.voxel_size_x, 0.25);
        REQUIRE_EQ(units.voxel_size_z, 0.5);
        REQUIRE_EQ(units.origin_y, -2.0);
        REQUIRE_EQ(units.origin_z, 3.25);
    }

    void test_metadata_roundtrip()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("metadata");
        const std::vector<unsigned char> binary{ 0x00, 0x01, 0xFF, 0x7F, 0x00 };

        {
            World world = make_authoring_world(ctx);
            REQUIRE_EQ(bsvx_world_set_metadata(world.get(), "tool.name", "blender", 7), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_set_metadata(world.get(), "tool.blob", binary.data(), binary.size()), BSVX_RESULT_OK);

            size_t region = 0;
            REQUIRE_EQ(bsvx_world_add_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_region_set_metadata(world.get(), region, "tool.object", "Terrain", 7), BSVX_RESULT_OK);

            REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        }

        World reloaded = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_metadata_count(reloaded.get()), 2u);

        // Text goes into the manifest as a readable string; bytes that are not printable UTF-8 go
        // through a hex form. Both have to come back byte-identical. Values are raw bytes, so they
        // are not NUL-terminated the way the string getters are.
        const auto get_bytes = [&](const std::function<bsvx_result(void*, size_t, size_t*)>& getter) {
            size_t needed = 0;
            REQUIRE_EQ(getter(nullptr, 0u, &needed), BSVX_RESULT_BUFFER_TOO_SMALL);
            std::vector<unsigned char> buffer(needed);
            REQUIRE_EQ(getter(buffer.data(), buffer.size(), &needed), BSVX_RESULT_OK);
            return buffer;
            };

        const auto name = get_bytes([&](void* b, size_t c, size_t* n) { return bsvx_world_get_metadata(reloaded.get(), "tool.name", b, c, n); });
        REQUIRE(std::string(name.begin(), name.end()) == "blender");

        size_t size = 0;
        REQUIRE_EQ(bsvx_world_get_metadata(reloaded.get(), "tool.blob", nullptr, 0, &size), BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(size, binary.size());

        std::vector<unsigned char> got(size);
        REQUIRE_EQ(bsvx_world_get_metadata(reloaded.get(), "tool.blob", got.data(), got.size(), &size), BSVX_RESULT_OK);
        REQUIRE(got == binary);

        REQUIRE_EQ(bsvx_world_get_metadata(reloaded.get(), "nope", nullptr, 0, &size), BSVX_RESULT_NOT_FOUND);

        size_t region = 0;
        REQUIRE_EQ(bsvx_world_find_region(reloaded.get(), 0, 0, 0, &region), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_metadata_count(reloaded.get(), region), 1u);

        const auto object = get_bytes([&](void* b, size_t c, size_t* n) { return bsvx_region_get_metadata(reloaded.get(), region, "tool.object", b, c, n); });
        REQUIRE(std::string(object.begin(), object.end()) == "Terrain");

        REQUIRE_EQ(bsvx_world_remove_metadata(reloaded.get(), "tool.name"), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_remove_metadata(reloaded.get(), "tool.name"), BSVX_RESULT_NOT_FOUND);
        REQUIRE_EQ(bsvx_world_metadata_count(reloaded.get()), 1u);
    }

    void test_removal()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 7, 7, 7, 1u, 1, &count), BSVX_RESULT_OK);

        size_t region = 0;
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_chunk_count(world.get(), region), 8u);

        // Clearing keeps the chunk authored; removing takes it out of the map entirely.
        REQUIRE_EQ(bsvx_region_clear_chunk(world.get(), region, 0, 0, 0), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_chunk_count(world.get(), region), 8u);

        bsvx_chunk_info info{};
        size_t ordinal = 0;
        REQUIRE_EQ(bsvx_region_find_chunk(world.get(), region, 0, 0, 0, &ordinal), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, ordinal, &info), BSVX_RESULT_OK);
        REQUIRE_EQ(info.summary.non_air_count, 0u);

        REQUIRE_EQ(bsvx_region_remove_chunk(world.get(), region, 1, 1, 1), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_chunk_count(world.get(), region), 7u);
        REQUIRE_EQ(bsvx_region_find_chunk(world.get(), region, 1, 1, 1, &ordinal), BSVX_RESULT_NOT_FOUND);
        REQUIRE_EQ(bsvx_region_remove_chunk(world.get(), region, 1, 1, 1), BSVX_RESULT_NOT_FOUND);

        // Every surviving chunk must still decode: the summary table is addressed indirectly, so a
        // botched erase would leave the remaining entries pointing at the wrong summaries.
        for (size_t i = 0; i < bsvx_region_chunk_count(world.get(), region); ++i) {
            REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, i, &info), BSVX_RESULT_OK);
            const auto voxels = decode_chunk(world, region, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
            const size_t non_air = static_cast<size_t>(std::count_if(voxels.begin(), voxels.end(), [](uint32_t v) { return v != 0u; }));
            REQUIRE_EQ(non_air, static_cast<size_t>(info.summary.non_air_count));
        }

        REQUIRE_EQ(bsvx_world_remove_region(world.get(), region), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_region_count(world.get()), 0u);
        REQUIRE_EQ(bsvx_world_remove_region(world.get(), 0), BSVX_RESULT_INVALID_ARGUMENT);
    }

    void test_prune_orphans_on_save()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("prune_orphans");

        World world = make_authoring_world(ctx);
        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 16, 16, 16, 16, 16, 16, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);

        const auto count_regions = [&] {
            size_t files = 0;
            for (const auto& entry : fs::directory_iterator(out_root / "regions")) {
                if (entry.path().extension() == ".bvx") ++files;
            }
            return files;
            };
        REQUIRE_EQ(count_regions(), 2u);

        size_t victim = 0;
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 2, 2, 2, &victim), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_remove_region(world.get(), victim), BSVX_RESULT_OK);

        // Without pruning the file stays, and region auto-discovery finds it again on the next
        // load -- the deletion silently undoes itself.
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        REQUIRE_EQ(count_regions(), 2u);

        bsvx_save_report report{};
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_PRUNE_ORPHANS, &report), BSVX_RESULT_OK);
        REQUIRE_EQ(report.files_removed, 1u);
        REQUIRE_EQ(count_regions(), 1u);

        World reloaded = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_region_count(reloaded.get()), 1u);
    }

    void test_dirty_incremental_save()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("dirty_save");

        {
            World world = make_authoring_world(ctx);
            size_t count = 0;
            REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 16, 16, 16, 16, 16, 16, 1u, 1, &count), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        }

        World world = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_is_dirty(world.get()), 0);

        size_t region = 0;
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);

        const int64_t x = 1, y = 1, z = 1;
        const uint32_t key = 1;
        size_t written = 0;
        REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), &x, &y, &z, &key, 1, 0, &written), BSVX_RESULT_OK);

        REQUIRE_EQ(bsvx_world_is_dirty(world.get()), 1);
        REQUIRE_EQ(bsvx_region_is_dirty(world.get(), region), 1);
        REQUIRE_EQ(bsvx_region_is_dirty(world.get(), 1u - region), 0);

        // The manifest text is unchanged, so the manifest hash every region carries is still valid
        // and only the edited region needs writing.
        bsvx_save_report report{};
        REQUIRE_EQ(bsvx_world_save_dirty(ctx.get(), world.get(), &report), BSVX_RESULT_OK);
        REQUIRE_EQ(report.manifest_written, 0);
        REQUIRE_EQ(report.files_written, 1u);
        REQUIRE_EQ(report.files_skipped, 2u);
        REQUIRE_EQ(report.full_rewrite, 0);
        REQUIRE_EQ(bsvx_world_is_dirty(world.get()), 0);

        // Reload and confirm the untouched region survived being skipped.
        World reloaded = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_region_count(reloaded.get()), 2u);

        const int64_t qx[2] = { 1, 16 };
        const int64_t qy[2] = { 1, 16 };
        const int64_t qz[2] = { 1, 16 };
        uint32_t got[2] = { 0, 0 };
        REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), reloaded.get(), qx, qy, qz, got, 2), BSVX_RESULT_OK);
        REQUIRE_EQ(got[0], 1u);
        REQUIRE_EQ(got[1], 1u);

        // A manifest-level change forces the full rewrite, because the hash moves.
        REQUIRE_EQ(bsvx_world_set_name(reloaded.get(), "Renamed"), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_dirty(ctx.get(), reloaded.get(), &report), BSVX_RESULT_OK);
        REQUIRE_EQ(report.manifest_written, 1);
        REQUIRE_EQ(report.files_written, 3u);
        // The caller asked for a dirty-only save and got a full one; the report has to admit it.
        REQUIRE_EQ(report.full_rewrite, 1);

        World renamed = load_world(ctx, out_root);
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_name(renamed.get(), b, c, n); }) == "Renamed");
    }

    void test_hash_mismatch_tolerance_and_repair()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("hash_repair");

        {
            World world = make_authoring_world(ctx);
            size_t count = 0;
            REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        }

        // Exactly what a hand edit, or a checkout that rewrote line endings, does to the world.
        {
            std::ofstream os(out_root / "manifest.toml", std::ios::binary | std::ios::app);
            os << "\n# a human was here\n";
        }

        bsvx_world* raw = nullptr;
        REQUIRE(bsvx_world_load(ctx.get(), path_to_utf8_string(out_root).c_str(), &raw) != BSVX_RESULT_OK);
        REQUIRE(ctx.last_error().find("manifest hash mismatch") != std::string::npos);

        REQUIRE_EQ(bsvx_world_load_ex2(ctx.get(), path_to_utf8_string(out_root).c_str(), BSVX_LOAD_IGNORE_HASH_MISMATCH, &raw), BSVX_RESULT_OK);
        World world(raw);
        REQUIRE_EQ(bsvx_context_warning_count(ctx.get()), 1u);
        REQUIRE(std::string(bsvx_context_warning(ctx.get(), 0)).find("manifest hash mismatch") != std::string::npos);

        size_t restamped = 0;
        REQUIRE_EQ(bsvx_world_rehash(world.get(), &restamped), BSVX_RESULT_OK);
        REQUIRE_EQ(restamped, 1u);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);

        // Repaired: the strict path opens it again.
        World repaired = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_region_count(repaired.get()), 1u);
        REQUIRE_EQ(bsvx_context_warning_count(ctx.get()), 0u);
    }

    void test_load_flags_skip()
    {
        Context ctx;

        bsvx_world* raw = nullptr;
        REQUIRE_EQ(bsvx_world_load_ex2(ctx.get(), path_to_utf8_string(fixture_manifest()).c_str(), BSVX_LOAD_SKIP_TEXTURES, &raw), BSVX_RESULT_OK);
        World no_textures(raw);
        REQUIRE_EQ(bsvx_world_texture_count(no_textures.get()), 0u);
        REQUIRE(bsvx_world_region_count(no_textures.get()) > 0u);
        REQUIRE(bsvx_world_registry_entry_count(no_textures.get()) > 0u);

        REQUIRE_EQ(bsvx_world_load_ex2(ctx.get(), path_to_utf8_string(fixture_manifest()).c_str(), BSVX_LOAD_SKIP_REGIONS, &raw), BSVX_RESULT_OK);
        World no_regions(raw);
        REQUIRE_EQ(bsvx_world_region_count(no_regions.get()), 0u);
        REQUIRE(bsvx_world_texture_count(no_regions.get()) > 0u);
    }

    void test_atomic_save_and_backup()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("atomic_save");

        World world = make_authoring_world(ctx);
        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);

        // No temp file may survive a successful save.
        for (const auto& entry : fs::recursive_directory_iterator(out_root)) {
            REQUIRE(path_to_utf8_string(entry.path()).find(".bsvx-tmp-") == std::string::npos);
        }

        REQUIRE_EQ(bsvx_world_set_name(world.get(), "Second"), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_BACKUP, nullptr), BSVX_RESULT_OK);
        REQUIRE(fs::exists(out_root / "manifest.toml.bak"));

        // A dry run reports without touching anything.
        const fs::path dry_root = make_temp_dir("atomic_dry");
        bsvx_save_report report{};
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(dry_root).c_str(), BSVX_SAVE_DRY_RUN, &report), BSVX_RESULT_OK);
        REQUIRE(report.files_written >= 2u);
        REQUIRE(report.bytes_written > 0u);
        REQUIRE(!fs::exists(dry_root / "manifest.toml"));
    }

    void test_validation()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 1, 1, 1, 1u, 1, &count), BSVX_RESULT_OK);

        // Painting with a key, then deleting it from the registry, is exactly the mistake an
        // authoring tool has to catch before it writes.
        const int64_t x = 2, y = 2, z = 2;
        const uint32_t orphan = 77u;
        size_t written = 0;
        REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), &x, &y, &z, &orphan, 1, 1, &written), BSVX_RESULT_OK);

        size_t issues = 0;
        REQUIRE_EQ(bsvx_world_validate(ctx.get(), world.get(), BSVX_VALIDATE_DEEP, &issues), BSVX_RESULT_OK);
        REQUIRE(issues > 0u);

        bool found_orphan = false;
        for (size_t i = 0; i < issues; ++i) {
            bsvx_validation_issue issue{};
            REQUIRE_EQ(bsvx_world_get_validation_issue(ctx.get(), i, &issue), BSVX_RESULT_OK);
            if (issue.code == BSVX_ISSUE_VOXEL_KEY_NOT_IN_REGISTRY && issue.voxel_key == 77u) {
                found_orphan = true;
                REQUIRE(issue.region_index >= 0);
                REQUIRE(issue.chunk_ordinal >= 0);

                const std::string message = get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_validation_message(ctx.get(), i, b, c, n); });
                REQUIRE(message.find("77") != std::string::npos);
            }
        }
        REQUIRE(found_orphan);

        bsvx_validation_issue issue{};
        REQUIRE_EQ(bsvx_world_get_validation_issue(ctx.get(), issues, &issue), BSVX_RESULT_NOT_FOUND);
    }

    // Non-cubic chunks, a non-cubic region, and unoccupied slots between the occupied ones -- the
    // geometry the whole-region decode's stride arithmetic is easiest to get wrong on, and one the
    // fixture world (cubic chunks, every slot filled) never reaches. Run twice: once with content
    // sparse enough that the chunks encode through the codecs that describe only occupied voxels,
    // once dense enough that they encode through the ones that define every voxel. Whole-region
    // decode clears the destination differently for those two cases.
    void test_decode_all_non_cubic_with_holes()
    {
        Context ctx;
        const bsvx_geometry_desc geometry{ 3, 5, 7, 4, 2, 3 };
        const size_t per_chunk = 3u * 5u * 7u;
        const size_t span_x = 3u * 4u;
        const size_t span_y = 5u * 2u;

        const auto is_hole = [](uint16_t cx, uint16_t cy, uint16_t cz) { return ((cx + cy + cz) % 3u) == 0u; };

        for (int dense_pass = 0; dense_pass < 2; ++dense_pass) {
            bsvx_world* raw = nullptr;
            REQUIRE_EQ(bsvx_world_create(ctx.get(), &geometry, &raw), BSVX_RESULT_OK);
            World world(raw);

            bsvx_registry_entry stone{ 1u, 0u, 1u, 0u };
            bsvx_registry_entry dirt{ 2u, 0u, 1u, 0u };
            REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &stone), BSVX_RESULT_OK);
            REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &dirt), BSVX_RESULT_OK);

            size_t region = 0;
            REQUIRE_EQ(bsvx_world_add_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);

            size_t authored = 0;
            for (uint16_t cz = 0; cz < 3; ++cz) {
                for (uint16_t cy = 0; cy < 2; ++cy) {
                    for (uint16_t cx = 0; cx < 4; ++cx) {
                        if (is_hole(cx, cy, cz)) continue;

                        std::vector<uint32_t> dense(per_chunk, 0u);
                        for (size_t i = 0; i < per_chunk; ++i) {
                            if (dense_pass == 1) dense[i] = 1u + static_cast<uint32_t>((i + cx) % 2u);
                            else if ((i % 11u) == cy) dense[i] = 1u + static_cast<uint32_t>(i % 2u);
                        }
                        REQUIRE_EQ(bsvx_region_set_chunk_u32_ex(ctx.get(), world.get(), region, cx, cy, cz,
                            dense.data(), dense.size(), 0xFFFFu), BSVX_RESULT_OK);
                        ++authored;
                    }
                }
            }
            REQUIRE(authored > 0u);
            REQUIRE(authored < 4u * 2u * 3u);   // there really are holes to get wrong

            size_t needed = 0;
            REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), region, BSVX_LAYOUT_REGION_LINEAR, nullptr, 0, &needed), BSVX_RESULT_BUFFER_TOO_SMALL);
            REQUIRE_EQ(needed, per_chunk * 4u * 2u * 3u);

            // Poisoned, because an unoccupied slot has to be *written* as air rather than left as
            // whatever the caller's buffer already held.
            std::vector<uint32_t> linear(needed, 0xDEADBEEFu);
            REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), region, BSVX_LAYOUT_REGION_LINEAR, linear.data(), linear.size(), &needed), BSVX_RESULT_OK);

            for (uint16_t cz = 0; cz < 3; ++cz) {
                for (uint16_t cy = 0; cy < 2; ++cy) {
                    for (uint16_t cx = 0; cx < 4; ++cx) {
                        const std::vector<uint32_t> expected = is_hole(cx, cy, cz)
                            ? std::vector<uint32_t>(per_chunk, 0u)
                            : decode_chunk(world, region, cx, cy, cz);

                        for (uint16_t z = 0; z < 7; ++z) {
                            for (uint16_t y = 0; y < 5; ++y) {
                                for (uint16_t x = 0; x < 3; ++x) {
                                    const size_t wx = static_cast<size_t>(cx) * 3u + x;
                                    const size_t wy = static_cast<size_t>(cy) * 5u + y;
                                    const size_t wz = static_cast<size_t>(cz) * 7u + z;
                                    const size_t src = x + 3u * (y + 5u * static_cast<size_t>(z));
                                    REQUIRE_EQ(linear[wx + span_x * (wy + span_y * wz)], expected[src]);
                                }
                            }
                        }
                    }
                }
            }

            // Chunk order writes every element it reports, so a poisoned buffer must come back
            // fully overwritten there too.
            REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), region, BSVX_LAYOUT_CHUNK_ORDER, nullptr, 0, &needed), BSVX_RESULT_BUFFER_TOO_SMALL);
            REQUIRE_EQ(needed, per_chunk * authored);

            std::vector<uint32_t> ordered(needed, 0xDEADBEEFu);
            REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), region, BSVX_LAYOUT_CHUNK_ORDER, ordered.data(), ordered.size(), &needed), BSVX_RESULT_OK);

            std::vector<bsvx_chunk_info> infos(authored);
            size_t written = 0;
            REQUIRE_EQ(bsvx_region_get_chunk_infos(world.get(), region, 0, authored, infos.data(), &written), BSVX_RESULT_OK);
            for (size_t i = 0; i < authored; ++i) {
                const auto one = decode_chunk(world, region, infos[i].local_chunk_x, infos[i].local_chunk_y, infos[i].local_chunk_z);
                for (size_t v = 0; v < per_chunk; ++v) REQUIRE_EQ(ordered[i * per_chunk + v], one[v]);
            }
        }
    }

    void test_bulk_accessors()
    {
        Context ctx;
        World world = load_world(ctx, fixture_manifest());

        const size_t chunks = bsvx_region_chunk_count(world.get(), 0);
        REQUIRE(chunks > 0u);

        std::vector<bsvx_chunk_info> infos(chunks);
        size_t written = 0;
        REQUIRE_EQ(bsvx_region_get_chunk_infos(world.get(), 0, 0, chunks, infos.data(), &written), BSVX_RESULT_OK);
        REQUIRE_EQ(written, chunks);

        for (size_t i = 0; i < chunks; ++i) {
            bsvx_chunk_info one{};
            REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), 0, i, &one), BSVX_RESULT_OK);
            REQUIRE_EQ(infos[i].local_chunk_x, one.local_chunk_x);
            REQUIRE_EQ(infos[i].summary.non_air_count, one.summary.non_air_count);
        }

        const size_t entries = bsvx_world_registry_entry_count(world.get());
        std::vector<bsvx_registry_entry> registry(entries);
        REQUIRE_EQ(bsvx_world_get_registry_entries(world.get(), 0, entries, registry.data(), &written), BSVX_RESULT_OK);
        REQUIRE_EQ(written, entries);

        // Whole-region decode has to agree with the per-chunk path, in both layouts.
        size_t needed = 0;
        REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), 0, BSVX_LAYOUT_CHUNK_ORDER, nullptr, 0, &needed), BSVX_RESULT_BUFFER_TOO_SMALL);

        const size_t per_chunk = bsvx_region_required_voxel_count(world.get(), 0);
        REQUIRE_EQ(needed, per_chunk * chunks);

        std::vector<uint32_t> all(needed);
        REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), 0, BSVX_LAYOUT_CHUNK_ORDER, all.data(), all.size(), &needed), BSVX_RESULT_OK);

        for (size_t i = 0; i < chunks; ++i) {
            const auto one = decode_chunk(world, 0, infos[i].local_chunk_x, infos[i].local_chunk_y, infos[i].local_chunk_z);
            for (size_t v = 0; v < per_chunk; ++v) REQUIRE_EQ(all[i * per_chunk + v], one[v]);
        }

        const auto g = world_geometry(world);
        REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), 0, BSVX_LAYOUT_REGION_LINEAR, nullptr, 0, &needed), BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(needed, per_chunk * static_cast<size_t>(g.region_size_x) * g.region_size_y * g.region_size_z);

        std::vector<uint32_t> linear(needed);
        REQUIRE_EQ(bsvx_region_decode_all_u32(ctx.get(), world.get(), 0, BSVX_LAYOUT_REGION_LINEAR, linear.data(), linear.size(), &needed), BSVX_RESULT_OK);

        const size_t span_x = static_cast<size_t>(g.chunk_size_x) * g.region_size_x;
        const size_t span_y = static_cast<size_t>(g.chunk_size_y) * g.region_size_y;
        for (size_t i = 0; i < chunks; ++i) {
            const auto one = decode_chunk(world, 0, infos[i].local_chunk_x, infos[i].local_chunk_y, infos[i].local_chunk_z);
            for (uint16_t z = 0; z < g.chunk_size_z; ++z) {
                for (uint16_t y = 0; y < g.chunk_size_y; ++y) {
                    for (uint16_t x = 0; x < g.chunk_size_x; ++x) {
                        const size_t wx = static_cast<size_t>(infos[i].local_chunk_x) * g.chunk_size_x + x;
                        const size_t wy = static_cast<size_t>(infos[i].local_chunk_y) * g.chunk_size_y + y;
                        const size_t wz = static_cast<size_t>(infos[i].local_chunk_z) * g.chunk_size_z + z;
                        const size_t src = x + static_cast<size_t>(g.chunk_size_x) * (y + static_cast<size_t>(g.chunk_size_y) * z);
                        REQUIRE_EQ(linear[wx + span_x * (wy + span_y * wz)], one[src]);
                    }
                }
            }
        }
    }

    void test_chunk_content_hash()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 1, 1, 1, 1u, 1, &count), BSVX_RESULT_OK);

        size_t region = 0;
        REQUIRE_EQ(bsvx_world_find_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);

        uint64_t first = 0;
        REQUIRE_EQ(bsvx_region_chunk_content_hash(world.get(), region, 0, 0, 0, &first), BSVX_RESULT_OK);
        REQUIRE(first != 0u);

        // Rewriting identical content under a different codec must not move the hash: it is taken
        // over the decoded voxels, not the encoded bytes.
        const auto voxels = decode_chunk(world, region, 0, 0, 0);
        REQUIRE_EQ(bsvx_region_set_chunk_u32_ex(ctx.get(), world.get(), region, 0, 0, 0, voxels.data(), voxels.size(), 6 /* RAW_DENSE */), BSVX_RESULT_OK);

        uint64_t second = 0;
        REQUIRE_EQ(bsvx_region_chunk_content_hash(world.get(), region, 0, 0, 0, &second), BSVX_RESULT_OK);
        REQUIRE_EQ(first, second);

        const int64_t x = 0, y = 0, z = 0;
        const uint32_t air = 0;
        size_t written = 0;
        REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), &x, &y, &z, &air, 1, 0, &written), BSVX_RESULT_OK);

        uint64_t third = 0;
        REQUIRE_EQ(bsvx_region_chunk_content_hash(world.get(), region, 0, 0, 0, &third), BSVX_RESULT_OK);
        REQUIRE(third != first);
    }

    void test_texture_authoring()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("texture_authoring");

        bsvx_texture_builder* builder = nullptr;
        REQUIRE_EQ(bsvx_texture_builder_create(ctx.get(), &builder), BSVX_RESULT_OK);

        bsvx_sampler_desc sampler{};
        sampler.max_anisotropy_x100 = 100;
        uint32_t sampler_id = 0xFFFFFFFFu;
        REQUIRE_EQ(bsvx_texture_builder_add_sampler(ctx.get(), builder, &sampler, &sampler_id), BSVX_RESULT_OK);
        REQUIRE_EQ(sampler_id, 0u);

        bsvx_texture_desc texture{};
        texture.kind = 0;
        texture.vk_format = 43;   // R8G8B8A8_SRGB
        texture.width = 2;
        texture.height = 2;
        texture.depth = 1;
        texture.array_layers = 2;
        texture.mip_levels = 2;
        texture.sampler_id = sampler_id;

        uint32_t texture_id = 0xFFFFFFFFu;
        REQUIRE_EQ(bsvx_texture_builder_add_texture(ctx.get(), builder, &texture, &texture_id), BSVX_RESULT_OK);

        // A format the library cannot describe must be refused up front, not at serialize time.
        bsvx_texture_desc unsupported = texture;
        unsupported.vk_format = 999;
        REQUIRE_EQ(bsvx_texture_builder_add_texture(ctx.get(), builder, &unsupported, nullptr), BSVX_RESULT_INVALID_ARGUMENT);

        const std::vector<unsigned char> red(2 * 2 * 4, 0xC0);
        const std::vector<unsigned char> blue(2 * 2 * 4, 0x40);
        REQUIRE_EQ(bsvx_texture_builder_append_subresource(ctx.get(), builder, texture_id, 0, 0, 2, 2, red.data(), red.size(), 0, 0, nullptr), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_texture_builder_append_subresource(ctx.get(), builder, texture_id, 0, 1, 2, 2, blue.data(), blue.size(), 0, 0, nullptr), BSVX_RESULT_OK);

        // Wrong byte count for the extent.
        REQUIRE(bsvx_texture_builder_append_subresource(ctx.get(), builder, texture_id, 0, 0, 2, 2, red.data(), 4, 0, 0, nullptr) != BSVX_RESULT_OK);

        REQUIRE_EQ(bsvx_texture_builder_generate_mips(ctx.get(), builder, texture_id), BSVX_RESULT_OK);

        bsvx_material_desc material{};
        material.albedo_texture_id = texture_id;
        material.tint_rgba8 = 0xFF00FFFFu;
        for (int face = 0; face < 6; ++face) material.albedo_layer[face] = static_cast<uint16_t>(face % 2);
        uint32_t material_id = 0xFFFFFFFFu;
        REQUIRE_EQ(bsvx_texture_builder_add_material(ctx.get(), builder, &material, &material_id), BSVX_RESULT_OK);

        size_t issue_count = 1;
        size_t message_size = 0;
        REQUIRE_EQ(bsvx_texture_builder_validate(ctx.get(), builder, nullptr, 0, &message_size, &issue_count), BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(issue_count, 0u);

        World world = make_authoring_world(ctx);
        bsvx_registry_entry entry{ 1u, material_id, 1u, 0u };
        REQUIRE_EQ(bsvx_world_set_registry_entry(world.get(), &entry), BSVX_RESULT_OK);

        size_t tex_index = 0xFFFFu;
        REQUIRE_EQ(bsvx_world_add_texture(ctx.get(), world.get(), "palette", nullptr, builder, &tex_index), BSVX_RESULT_OK);
        REQUIRE_EQ(tex_index, 0u);
        // Duplicate ids would collide on disk.
        REQUIRE_EQ(bsvx_world_add_texture(ctx.get(), world.get(), "palette", nullptr, builder, nullptr), BSVX_RESULT_INVALID_ARGUMENT);

        bsvx_texture_builder_destroy(builder);   // the world owns its own copy now

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        REQUIRE(fs::exists(out_root / "textures" / "palette.btx"));

        // Read the authored archive back through the introspection API added in v3.
        World reloaded = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_texture_count(reloaded.get()), 1u);
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_texture_id(reloaded.get(), 0, b, c, n); }) == "palette");
        REQUIRE_EQ(bsvx_texture_texture_count(reloaded.get(), 0), 1u);
        REQUIRE_EQ(bsvx_texture_sampler_count(reloaded.get(), 0), 1u);
        REQUIRE_EQ(bsvx_texture_material_count(reloaded.get(), 0), 1u);
        // Two layers at mip 0, two at mip 1.
        REQUIRE_EQ(bsvx_texture_subresource_count(reloaded.get(), 0), 4u);

        bsvx_texture_desc got{};
        REQUIRE_EQ(bsvx_texture_get_desc(reloaded.get(), 0, 0, &got), BSVX_RESULT_OK);
        REQUIRE_EQ(got.width, 2u);
        REQUIRE_EQ(got.array_layers, 2u);
        REQUIRE_EQ(got.vk_format, 43u);

        size_t material_index = 0;
        REQUIRE_EQ(bsvx_texture_find_material(reloaded.get(), 0, material_id, &material_index), BSVX_RESULT_OK);
        bsvx_material_desc got_material{};
        REQUIRE_EQ(bsvx_texture_get_material(reloaded.get(), 0, material_index, &got_material), BSVX_RESULT_OK);
        REQUIRE_EQ(got_material.tint_rgba8, 0xFF00FFFFu);
        REQUIRE_EQ(got_material.albedo_layer[1], 1u);

        // Round-trip a texel block: 1x1 at mip 1, the box filter of four identical 0xC0 texels.
        for (size_t i = 0; i < 4; ++i) {
            bsvx_subresource_desc sub{};
            REQUIRE_EQ(bsvx_texture_get_subresource_desc(reloaded.get(), 0, i, &sub), BSVX_RESULT_OK);
            if (sub.mip_level != 1 || sub.layer_or_slice != 0) continue;

            REQUIRE_EQ(sub.extent_x, 1u);
            std::vector<unsigned char> texels(static_cast<size_t>(sub.size));
            size_t size = 0;
            REQUIRE_EQ(bsvx_texture_get_subresource_bytes(reloaded.get(), 0, i, texels.data(), texels.size(), &size), BSVX_RESULT_OK);
            REQUIRE_EQ(size, 4u);
            REQUIRE_EQ(texels[0], 0xC0);
        }
    }

    void test_paths_and_progress()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("paths_progress");

        World world = make_authoring_world(ctx);
        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);

        World reloaded = load_world(ctx, out_root);
        const std::string source = get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_source_path(reloaded.get(), b, c, n); });
        const std::string root = get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_root_dir(reloaded.get(), b, c, n); });
        REQUIRE(source.find("manifest.toml") != std::string::npos);
        REQUIRE(!root.empty());
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_region_path(reloaded.get(), 0, b, c, n); }) == "r_0_0_0.bvx");

        // Progress reporting, and a cancel that leaves nothing behind.
        static int calls = 0;
        static int cancel_after = 1000;
        calls = 0;
        bsvx_context_set_progress(ctx.get(), [](void*, const char*, size_t, size_t) -> int {
            return ++calls <= cancel_after ? 1 : 0;
            }, nullptr);

        const fs::path progress_root = make_temp_dir("progress_ok");
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), reloaded.get(), path_to_utf8_string(progress_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        REQUIRE(calls > 0);

        calls = 0;
        cancel_after = 0;
        const fs::path cancel_root = make_temp_dir("progress_cancel");
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), reloaded.get(), path_to_utf8_string(cancel_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_CANCELLED);
        REQUIRE(!fs::exists(cancel_root / "manifest.toml"));

        bsvx_context_set_progress(ctx.get(), nullptr, nullptr);
    }

    void test_manifest_text_matches_save()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("manifest_text");

        World world = make_authoring_world(ctx);
        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);

        World reloaded = load_world(ctx, out_root);
        const std::string text = get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_save_manifest_memory(ctx.get(), reloaded.get(), b, c, n); });

        // This is the whole point of exposing it: the text has to be byte-identical to the file, or
        // a host cannot use it to decide whether a save will move the manifest hash.
        const auto on_disk = read_file_bytes(out_root / "manifest.toml");
        REQUIRE_EQ(text.size(), on_disk.size());
        REQUIRE(std::equal(text.begin(), text.end(), on_disk.begin()));
    }



    /* ------------------------------------------------------------------------------------- */
    /* Tier 2                                                                                 */
    /* ------------------------------------------------------------------------------------- */

    void test_face_state()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        size_t region = 0;
        REQUIRE_EQ(bsvx_world_add_region(world.get(), 0, 0, 0, &region), BSVX_RESULT_OK);

        const size_t per_chunk = bsvx_region_required_voxel_count(world.get(), region);
        REQUIRE_EQ(per_chunk, 64u);   // 4x4x4

        // A solid chunk: every face is FULL. This used to be unreachable -- the occupied/total
        // comparison never fired because total was never incremented.
        std::vector<uint32_t> dense(per_chunk, 1u);
        REQUIRE_EQ(bsvx_region_set_chunk_u32_ex(ctx.get(), world.get(), region, 0, 0, 0, dense.data(), dense.size(), 0xFFFFu), BSVX_RESULT_OK);

        bsvx_chunk_info info{};
        size_t ordinal = 0;
        REQUIRE_EQ(bsvx_region_find_chunk(world.get(), region, 0, 0, 0, &ordinal), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, ordinal, &info), BSVX_RESULT_OK);

        constexpr uint8_t FACE_EMPTY = 0, FACE_FULL = 1, FACE_MIXED = 2;
        REQUIRE_EQ(info.summary.face_state_px, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_nx, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_py, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_ny, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_pz, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_nz, FACE_FULL);

        // Clear one voxel on the -x face and nothing else: only that face becomes MIXED. Under the
        // old code every axis tested x, so -y and -z would have moved too.
        std::fill(dense.begin(), dense.end(), 1u);
        dense[0] = 0u;   // (0,0,0): on -x, -y and -z
        dense[1] = 0u;   // (1,0,0): on -y and -z but NOT -x
        REQUIRE_EQ(bsvx_region_set_chunk_u32_ex(ctx.get(), world.get(), region, 0, 0, 0, dense.data(), dense.size(), 0xFFFFu), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, ordinal, &info), BSVX_RESULT_OK);

        REQUIRE_EQ(info.summary.face_state_px, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_nx, FACE_MIXED);
        REQUIRE_EQ(info.summary.face_state_py, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_ny, FACE_MIXED);
        REQUIRE_EQ(info.summary.face_state_pz, FACE_FULL);
        REQUIRE_EQ(info.summary.face_state_nz, FACE_MIXED);

        std::fill(dense.begin(), dense.end(), 0u);
        REQUIRE_EQ(bsvx_region_set_chunk_u32_ex(ctx.get(), world.get(), region, 0, 0, 0, dense.data(), dense.size(), 0xFFFFu), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_region_get_chunk_info(world.get(), region, ordinal, &info), BSVX_RESULT_OK);
        REQUIRE_EQ(info.summary.face_state_px, FACE_EMPTY);
        REQUIRE_EQ(info.summary.face_state_nz, FACE_EMPTY);
    }

    void test_texel_formats()
    {
        REQUIRE_EQ(bsvx_format_bytes_per_texel(BSVX_FORMAT_R8_UNORM), 1u);
        REQUIRE_EQ(bsvx_format_bytes_per_texel(BSVX_FORMAT_R8G8_UNORM), 2u);
        REQUIRE_EQ(bsvx_format_bytes_per_texel(BSVX_FORMAT_R8G8B8A8_SRGB), 4u);
        REQUIRE_EQ(bsvx_format_bytes_per_texel(BSVX_FORMAT_R16G16B16A16_SFLOAT), 8u);
        REQUIRE_EQ(bsvx_format_bytes_per_texel(BSVX_FORMAT_R32G32B32A32_SFLOAT), 16u);
        // Block formats have no per-texel size -- that is exactly why subresource_size exists.
        REQUIRE_EQ(bsvx_format_bytes_per_texel(BSVX_FORMAT_BC7_UNORM), 0u);
        REQUIRE_EQ(bsvx_format_is_supported(BSVX_FORMAT_BC7_UNORM), 1);
        REQUIRE_EQ(bsvx_format_is_supported(9999u), 0);

        REQUIRE_EQ(bsvx_format_is_block_compressed(BSVX_FORMAT_BC3_UNORM), 1);
        REQUIRE_EQ(bsvx_format_block_extent(BSVX_FORMAT_BC3_UNORM), 4u);
        REQUIRE_EQ(bsvx_format_block_size(BSVX_FORMAT_BC1_RGB_UNORM), 8u);
        REQUIRE_EQ(bsvx_format_block_size(BSVX_FORMAT_BC7_UNORM), 16u);

        REQUIRE_EQ(bsvx_format_subresource_size(BSVX_FORMAT_R8G8B8A8_SRGB, 4, 4), 64u);
        REQUIRE_EQ(bsvx_format_subresource_size(BSVX_FORMAT_BC7_UNORM, 4, 4), 16u);
        // Rounded up to whole blocks, never down: 5x5 is 2x2 blocks.
        REQUIRE_EQ(bsvx_format_subresource_size(BSVX_FORMAT_BC7_UNORM, 5, 5), 64u);
        REQUIRE_EQ(bsvx_format_subresource_size(BSVX_FORMAT_BC7_UNORM, 1, 1), 16u);
        REQUIRE_EQ(bsvx_format_subresource_size(BSVX_FORMAT_BC1_RGB_UNORM, 8, 8), 32u);

        Context ctx;
        bsvx_texture_builder* builder = nullptr;
        REQUIRE_EQ(bsvx_texture_builder_create(ctx.get(), &builder), BSVX_RESULT_OK);

        // A single-channel roughness map: 2 bytes per 1x1 texel would be wrong, 1 is right.
        bsvx_texture_desc r8{};
        r8.vk_format = BSVX_FORMAT_R8_UNORM;
        r8.width = 4;
        r8.height = 4;
        r8.depth = 1;
        r8.array_layers = 1;
        r8.mip_levels = 1;
        uint32_t r8_id = 0;
        REQUIRE_EQ(bsvx_texture_builder_add_texture(ctx.get(), builder, &r8, &r8_id), BSVX_RESULT_OK);

        const std::vector<unsigned char> gray(16, 0x80);
        REQUIRE_EQ(bsvx_texture_builder_append_subresource(ctx.get(), builder, r8_id, 0, 0, 4, 4, gray.data(), gray.size(), 0, 0, nullptr), BSVX_RESULT_OK);

        bsvx_texture_desc bc7{};
        bc7.vk_format = BSVX_FORMAT_BC7_UNORM;
        bc7.width = 8;
        bc7.height = 8;
        bc7.depth = 1;
        bc7.array_layers = 1;
        bc7.mip_levels = 1;
        uint32_t bc7_id = 0;
        REQUIRE_EQ(bsvx_texture_builder_add_texture(ctx.get(), builder, &bc7, &bc7_id), BSVX_RESULT_OK);

        const std::vector<unsigned char> blocks(static_cast<size_t>(bsvx_format_subresource_size(BSVX_FORMAT_BC7_UNORM, 8, 8)), 0x11);
        REQUIRE_EQ(blocks.size(), 64u);
        REQUIRE_EQ(bsvx_texture_builder_append_subresource(ctx.get(), builder, bc7_id, 0, 0, 8, 8, blocks.data(), blocks.size(), 0, 0, nullptr), BSVX_RESULT_OK);
        // Half the blocks is not a valid payload.
        REQUIRE(bsvx_texture_builder_append_subresource(ctx.get(), builder, bc7_id, 0, 0, 8, 8, blocks.data(), 32, 0, 0, nullptr) != BSVX_RESULT_OK);

        // Downsampling BCn means decoding and re-encoding, which this library does not do.
        REQUIRE(bsvx_texture_builder_generate_mips(ctx.get(), builder, bc7_id) != BSVX_RESULT_OK);

        size_t issues = 1;
        size_t size = 0;
        REQUIRE_EQ(bsvx_texture_builder_validate(ctx.get(), builder, nullptr, 0, &size, &issues), BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(issues, 0u);

        // Round-trip through bytes: the reader has to accept every format the writer produced.
        std::vector<unsigned char> archive;
        REQUIRE_EQ(bsvx_texture_builder_save_memory(ctx.get(), builder, nullptr, 0, &size), BSVX_RESULT_BUFFER_TOO_SMALL);
        archive.resize(size);
        REQUIRE_EQ(bsvx_texture_builder_save_memory(ctx.get(), builder, archive.data(), archive.size(), &size), BSVX_RESULT_OK);

        const fs::path out = make_temp_dir("texel_formats") / "formats.btx";
        REQUIRE_EQ(bsvx_texture_builder_save(ctx.get(), builder, path_to_utf8_string(out).c_str(), 0), BSVX_RESULT_OK);
        bsvx_texture_builder_destroy(builder);

        bsvx_texture_builder* reopened = nullptr;
        REQUIRE_EQ(bsvx_texture_builder_open(ctx.get(), path_to_utf8_string(out).c_str(), &reopened), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_texture_builder_validate(ctx.get(), reopened, nullptr, 0, &size, &issues), BSVX_RESULT_BUFFER_TOO_SMALL);
        REQUIRE_EQ(issues, 0u);
        bsvx_texture_builder_destroy(reopened);
    }

    void test_axis_conversion_math()
    {
        REQUIRE(std::string(bsvx_axis_convention_name(BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD)) == "x_right_z_up_y_forward");
        REQUIRE(std::string(bsvx_axis_convention_name(9999u)).empty());

        // Blender (x, y, z) -> canonical (x, z, -y), for continuous positions.
        double px = 0, py = 0, pz = 0;
        REQUIRE_EQ(bsvx_convert_position(BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, 1.0, 2.0, 3.0, &px, &py, &pz), BSVX_RESULT_OK);
        REQUIRE_EQ(px, 1.0);
        REQUIRE_EQ(py, 3.0);
        REQUIRE_EQ(pz, -2.0);

        // Cells carry the extra -1: cell 2 spans [2,3), whose mirror is [-3,-2), i.e. cell -3.
        int64_t cx = 0, cy = 0, cz = 0;
        REQUIRE_EQ(bsvx_convert_cell(BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, 1, 2, 3, &cx, &cy, &cz), BSVX_RESULT_OK);
        REQUIRE_EQ(cx, 1);
        REQUIRE_EQ(cy, 3);
        REQUIRE_EQ(cz, -3);

        // Round-tripping any cell through both directions must be the identity -- if the offset is
        // applied on only one side, this drifts by one voxel.
        for (int64_t x = -5; x <= 5; ++x) {
            for (int64_t y = -5; y <= 5; ++y) {
                for (int64_t z = -5; z <= 5; ++z) {
                    int64_t ax = 0, ay = 0, az = 0;
                    REQUIRE_EQ(bsvx_convert_cell(BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, x, y, z, &ax, &ay, &az), BSVX_RESULT_OK);

                    int64_t bx = 0, by = 0, bz = 0;
                    REQUIRE_EQ(bsvx_convert_cell(BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, ax, ay, az, &bx, &by, &bz), BSVX_RESULT_OK);
                    REQUIRE_EQ(bx, x);
                    REQUIRE_EQ(by, y);
                    REQUIRE_EQ(bz, z);
                }
            }
        }
    }

    void test_world_axis_conversion()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        // Deliberately asymmetric: a mirrored or transposed result is otherwise indistinguishable.
        const std::vector<std::array<int64_t, 3>> points{
            {0, 0, 0}, {1, 0, 0}, {0, 2, 0}, {0, 0, 3}, {-4, 5, -6}, {9, 9, 9},
        };
        for (size_t i = 0; i < points.size(); ++i) {
            const int64_t x = points[i][0], y = points[i][1], z = points[i][2];
            const uint32_t key = 1u;
            size_t written = 0;
            REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), world.get(), &x, &y, &z, &key, 1, 1, &written), BSVX_RESULT_OK);
            REQUIRE_EQ(written, 1u);
        }

        size_t moved = 0;
        REQUIRE_EQ(bsvx_world_convert_axis_convention(ctx.get(), world.get(), BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, &moved), BSVX_RESULT_OK);
        REQUIRE_EQ(moved, points.size());

        bsvx_world_desc desc{};
        REQUIRE_EQ(bsvx_world_get_desc(world.get(), &desc), BSVX_RESULT_OK);
        REQUIRE_EQ(desc.axis_convention, static_cast<uint16_t>(BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD));

        // Every voxel must be findable at its converted coordinate, and nowhere else.
        for (const auto& point : points) {
            int64_t nx = 0, ny = 0, nz = 0;
            REQUIRE_EQ(bsvx_convert_cell(BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD, point[0], point[1], point[2], &nx, &ny, &nz),
                BSVX_RESULT_OK);

            uint32_t key = 0;
            REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), world.get(), &nx, &ny, &nz, &key, 1), BSVX_RESULT_OK);
            REQUIRE_EQ(key, 1u);
        }

        // Converting back restores the original layout exactly.
        REQUIRE_EQ(bsvx_world_convert_axis_convention(ctx.get(), world.get(), BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, &moved), BSVX_RESULT_OK);
        REQUIRE_EQ(moved, points.size());

        for (const auto& point : points) {
            uint32_t key = 0;
            REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), world.get(), &point[0], &point[1], &point[2], &key, 1), BSVX_RESULT_OK);
            REQUIRE_EQ(key, 1u);
        }

        // A no-op conversion moves nothing.
        REQUIRE_EQ(bsvx_world_convert_axis_convention(ctx.get(), world.get(), BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD, &moved), BSVX_RESULT_OK);
        REQUIRE_EQ(moved, 0u);
        REQUIRE(bsvx_world_convert_axis_convention(ctx.get(), world.get(), 77u, &moved) != BSVX_RESULT_OK);
    }

    void test_registry_colors_and_palette()
    {
        Context ctx;
        const fs::path out_root = make_temp_dir("palette");

        bsvx_geometry_desc geometry{ 4, 4, 4, 2, 2, 2 };
        bsvx_world* raw = nullptr;
        REQUIRE_EQ(bsvx_world_create(ctx.get(), &geometry, &raw), BSVX_RESULT_OK);
        World world(raw);

        const std::vector<uint32_t> colors{ 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu };
        size_t tex_index = 0xFFFFu;
        REQUIRE_EQ(bsvx_world_make_palette(ctx.get(), world.get(), colors.data(), colors.size(), "palette", BSVX_REGISTRY_OPAQUE, &tex_index),
            BSVX_RESULT_OK);
        REQUIRE_EQ(tex_index, 0u);

        // One registry entry per colour, keyed from 1 because 0 is air.
        REQUIRE_EQ(bsvx_world_registry_entry_count(world.get()), colors.size());
        for (size_t i = 0; i < colors.size(); ++i) {
            uint32_t color = 0;
            REQUIRE_EQ(bsvx_world_get_registry_color(world.get(), static_cast<uint32_t>(i + 1u), &color), BSVX_RESULT_OK);
            REQUIRE_EQ(color, colors[i]);
        }
        REQUIRE_EQ(bsvx_world_get_registry_color(world.get(), 99u, nullptr), BSVX_RESULT_INVALID_ARGUMENT);

        uint32_t missing = 0xABCDu;
        REQUIRE_EQ(bsvx_world_get_registry_color(world.get(), 99u, &missing), BSVX_RESULT_NOT_FOUND);
        REQUIRE_EQ(missing, 0u);

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 1, 1, 1, 2u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), path_to_utf8_string(out_root).c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);

        World reloaded = load_world(ctx, out_root);
        for (size_t i = 0; i < colors.size(); ++i) {
            uint32_t color = 0;
            REQUIRE_EQ(bsvx_world_get_registry_color(reloaded.get(), static_cast<uint32_t>(i + 1u), &color), BSVX_RESULT_OK);
            REQUIRE_EQ(color, colors[i]);
        }

        // The material behind key 2 must carry the same colour as its tint.
        bsvx_registry_entry entry{};
        REQUIRE_EQ(bsvx_world_get_registry_entry(reloaded.get(), 1, &entry), BSVX_RESULT_OK);
        REQUIRE_EQ(entry.voxel_key, 2u);

        size_t material_index = 0;
        REQUIRE_EQ(bsvx_texture_find_material(reloaded.get(), 0, entry.material_id, &material_index), BSVX_RESULT_OK);
        bsvx_material_desc material{};
        REQUIRE_EQ(bsvx_texture_get_material(reloaded.get(), 0, material_index, &material), BSVX_RESULT_OK);
        REQUIRE_EQ(material.tint_rgba8, colors[1]);

        // Colours also survive a standalone region, which carries its own world desc.
        const fs::path region_path = out_root / "standalone.bvx";
        REQUIRE_EQ(bsvx_world_save_region_ex(ctx.get(), reloaded.get(), path_to_utf8_string(region_path).c_str()), BSVX_RESULT_OK);

        World standalone = load_region(ctx, region_path);
        uint32_t color = 0;
        REQUIRE_EQ(bsvx_world_get_registry_color(standalone.get(), 3u, &color), BSVX_RESULT_OK);
        REQUIRE_EQ(color, colors[2]);

        REQUIRE_EQ(bsvx_world_set_registry_color(reloaded.get(), 1u, 0u), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_get_registry_color(reloaded.get(), 1u, &color), BSVX_RESULT_NOT_FOUND);
    }

    void test_world_clone()
    {
        Context ctx;
        World world = make_authoring_world(ctx);

        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 1, 1, 1, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_set_name(world.get(), "Original"), BSVX_RESULT_OK);

        bsvx_world* raw = nullptr;
        REQUIRE_EQ(bsvx_world_clone(ctx.get(), world.get(), &raw), BSVX_RESULT_OK);
        World clone(raw);

        REQUIRE_EQ(bsvx_world_region_count(clone.get()), bsvx_world_region_count(world.get()));
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_name(clone.get(), b, c, n); }) == "Original");

        // Deep, not shared: editing one must not disturb the other.
        REQUIRE_EQ(bsvx_world_set_name(clone.get(), "Copy"), BSVX_RESULT_OK);
        const int64_t x = 0, y = 0, z = 0;
        const uint32_t air = 0;
        size_t written = 0;
        REQUIRE_EQ(bsvx_world_set_voxels(ctx.get(), clone.get(), &x, &y, &z, &air, 1, 0, &written), BSVX_RESULT_OK);

        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_name(world.get(), b, c, n); }) == "Original");

        uint32_t original_key = 0, clone_key = 0;
        REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), world.get(), &x, &y, &z, &original_key, 1), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_get_voxels(ctx.get(), clone.get(), &x, &y, &z, &clone_key, 1), BSVX_RESULT_OK);
        REQUIRE_EQ(original_key, 1u);
        REQUIRE_EQ(clone_key, 0u);
    }

    // An in-memory filesystem behind the vfs writer callbacks, so the save path can be exercised
    // without touching disk at all.
    struct MemoryVfs final {
        std::map<std::string, std::vector<unsigned char>> files;
        std::vector<std::string> directories;

        static MemoryVfs& from(void* user) { return *static_cast<MemoryVfs*>(user); }
    };

    void test_save_through_vfs()
    {
        Context ctx;
        MemoryVfs storage;

        bsvx_vfs_writer writer{};
        writer.user = &storage;
        writer.write_file = [](void* user, const char* path, const void* bytes, size_t size, int, int) -> int {
            const auto* raw = static_cast<const unsigned char*>(bytes);
            MemoryVfs::from(user).files[path] = std::vector<unsigned char>(raw, raw + size);
            return 1;
            };
        writer.make_directories = [](void* user, const char* path) -> int {
            MemoryVfs::from(user).directories.emplace_back(path);
            return 1;
            };
        writer.file_exists = [](void* user, const char* path) -> int {
            return MemoryVfs::from(user).files.contains(path) ? 1 : 0;
            };
        writer.remove_file = [](void* user, const char* path) -> int {
            MemoryVfs::from(user).files.erase(path);
            return 1;
            };
        writer.read_file = [](void* user, const char* path, void* out, size_t capacity, size_t* out_size) -> int {
            auto& files = MemoryVfs::from(user).files;
            const auto found = files.find(path);
            if (found == files.end()) return 0;

            *out_size = found->second.size();
            if (capacity >= found->second.size() && out) std::memcpy(out, found->second.data(), found->second.size());
            return 1;
            };

        World world = make_authoring_world(ctx);
        size_t count = 0;
        REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 0, 0, 0, 1u, 1, &count), BSVX_RESULT_OK);
        REQUIRE_EQ(bsvx_world_set_registry_name(world.get(), 1u, "stone"), BSVX_RESULT_OK);

        bsvx_save_report report{};
        REQUIRE_EQ(bsvx_world_save_vfs(ctx.get(), world.get(), "res://worlds/demo", &writer, BSVX_SAVE_DEFAULT, &report), BSVX_RESULT_OK);
        REQUIRE_EQ(report.files_written, 2u);   // manifest + one region

        REQUIRE(storage.files.contains("res://worlds/demo/manifest.toml"));
        REQUIRE(storage.files.contains("res://worlds/demo/regions/r_0_0_0.bvx"));
        // The scheme must survive intact -- std::filesystem::path would have collapsed the "//".
        for (const auto& [path, bytes] : storage.files) {
            REQUIRE(path.rfind("res://", 0) == 0u);
            REQUIRE(!bytes.empty());
        }

        // Round-trip: the bytes the VFS captured have to load as a real world.
        const fs::path out_root = make_temp_dir("vfs_save");
        for (const auto& [path, bytes] : storage.files) {
            const std::string relative = path.substr(std::string("res://worlds/demo/").size());
            const fs::path target = out_root / relative;
            fs::create_directories(target.parent_path());

            std::ofstream os(target, std::ios::binary);
            os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        World reloaded = load_world(ctx, out_root);
        REQUIRE_EQ(bsvx_world_region_count(reloaded.get()), 1u);
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_registry_name(reloaded.get(), 1u, b, c, n); }) == "stone");

        // Nothing dirty afterwards, and a dirty-only save through the same writer is a no-op.
        REQUIRE_EQ(bsvx_world_is_dirty(world.get()), 0);
        REQUIRE_EQ(bsvx_world_save_vfs(ctx.get(), world.get(), "res://worlds/demo", &writer, BSVX_SAVE_DIRTY_ONLY, &report), BSVX_RESULT_OK);
        REQUIRE_EQ(report.manifest_written, 0);
        REQUIRE_EQ(report.files_written, 0u);

        // A writer whose required callbacks are missing is rejected rather than half-used.
        bsvx_vfs_writer broken{};
        broken.user = &storage;
        REQUIRE_EQ(bsvx_world_save_vfs(ctx.get(), world.get(), "res://x", &broken, BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_INVALID_ARGUMENT);
    }

    void test_non_ascii_paths()
    {
        Context ctx;

        // Every path crossing the C boundary is UTF-8, including on Windows, where the default
        // conversion would decode these bytes in the active code page instead.
        const std::string folder = u8_to_string(u8"wörld-日本-\U0001F9CA");
        const fs::path out_root = make_temp_dir("non_ascii") / fs::path(std::u8string(reinterpret_cast<const char8_t*>(folder.data()), folder.size()));

        {
            World world = make_authoring_world(ctx);
            REQUIRE_EQ(bsvx_world_set_registry_name(world.get(), 1u, u8_to_string(u8"pierre grisâtre").c_str()), BSVX_RESULT_OK);

            size_t count = 0;
            REQUIRE_EQ(bsvx_world_fill_box(ctx.get(), world.get(), 0, 0, 0, 1, 1, 1, 1u, 1, &count), BSVX_RESULT_OK);

            const std::string root_utf8 = path_to_utf8_string(out_root);
            REQUIRE_EQ(bsvx_world_save_ex2(ctx.get(), world.get(), root_utf8.c_str(), BSVX_SAVE_DEFAULT, nullptr), BSVX_RESULT_OK);
        }

        REQUIRE(fs::exists(out_root / "manifest.toml"));

        const std::string root_utf8 = path_to_utf8_string(out_root);
        bsvx_world* raw = nullptr;
        REQUIRE_EQ(bsvx_world_load_ex2(ctx.get(), root_utf8.c_str(), BSVX_LOAD_DEFAULT, &raw), BSVX_RESULT_OK);
        World reloaded(raw);
        REQUIRE_EQ(bsvx_world_region_count(reloaded.get()), 1u);

        // The path the world reports back must be the same UTF-8 it was given.
        const std::string source = get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_source_path(reloaded.get(), b, c, n); });
        REQUIRE(source.find(folder) != std::string::npos);

        // An unnamed world takes its name from the directory it is saved into. That derivation used
        // path::string(), which on MSVC narrows through the active code page and throws for
        // characters it cannot represent -- so this assertion is the one that catches a regression
        // back to a narrow conversion anywhere in the save path.
        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_name(reloaded.get(), b, c, n); }) == folder);

        REQUIRE(get_string([&](char* b, size_t c, size_t* n) { return bsvx_world_get_registry_name(reloaded.get(), 1u, b, c, n); })
            == u8_to_string(u8"pierre grisâtre"));

        // The pre-v4 entry points take the same UTF-8 paths.
        bsvx_world* legacy = nullptr;
        REQUIRE_EQ(bsvx_world_load(ctx.get(), root_utf8.c_str(), &legacy), BSVX_RESULT_OK);
        World legacy_world(legacy);
        REQUIRE_EQ(bsvx_world_region_count(legacy_world.get()), 1u);

        const std::string region_utf8 = path_to_utf8_string(out_root / "regions" / "r_0_0_0.bvx");
        bsvx_region_reader* reader_raw = nullptr;
        REQUIRE_EQ(bsvx_region_reader_open(ctx.get(), region_utf8.c_str(), &reader_raw), BSVX_RESULT_OK);
        Reader reader(reader_raw);
        REQUIRE(bsvx_region_reader_chunk_count(reader.get()) > 0u);
    }

    int run_test(const char* name, const std::function<void()>& fn)
    {
        try {
            fn();
            std::cout << "[PASS] " << name << '\n';
            return 0;
        }
        catch (const std::exception& ex) {
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
            return 1;
        }
    }

} // namespace

int main()
{
    std::cout << "These tests are fixture-driven. Required test data:\n"
        << "  test/data/world_basic/manifest.toml\n"
        << "  test/data/standalone_region/standalone_region.bvx\n";

    int failures = 0;
    failures += run_test("load_standalone_region", test_load_standalone_region);
    failures += run_test("load_world_and_roundtrip_save", test_load_world_and_roundtrip_save);
    failures += run_test("mutate_chunk_and_save", test_mutate_chunk_and_save);
    failures += run_test("abi_version", test_abi_version);
    failures += run_test("load_region_from_memory", test_load_region_from_memory);
    failures += run_test("find_chunk", test_find_chunk);
    failures += run_test("summary_survives_write", test_summary_survives_write);
    failures += run_test("payload_readback", test_payload_readback);
    failures += run_test("error_reporting_ex", test_error_reporting_ex);
    failures += run_test("compaction", test_compaction);
    failures += run_test("region_reader_standalone", test_region_reader_standalone);
    failures += run_test("region_reader_manifest_region", test_region_reader_manifest_region);
    failures += run_test("region_reader_payload", test_region_reader_payload);
    failures += run_test("world_metadata", test_world_metadata);
    failures += run_test("authoring_from_scratch", test_authoring_from_scratch);
    failures += run_test("save_region_memory", test_save_region_memory);
    failures += run_test("load_world_through_vfs", test_load_world_through_vfs);
    failures += run_test("texture_introspection", test_texture_introspection);

    failures += run_test("abi_self_description", test_abi_self_description);
    failures += run_test("locate_voxel_negative_coords", test_locate_voxel_negative_coords);
    failures += run_test("world_space_voxel_io", test_world_space_voxel_io);
    failures += run_test("fill_box", test_fill_box);
    failures += run_test("registry_names_roundtrip", test_registry_names_roundtrip);
    failures += run_test("units_roundtrip", test_units_roundtrip);
    failures += run_test("metadata_roundtrip", test_metadata_roundtrip);
    failures += run_test("removal", test_removal);
    failures += run_test("prune_orphans_on_save", test_prune_orphans_on_save);
    failures += run_test("dirty_incremental_save", test_dirty_incremental_save);
    failures += run_test("hash_mismatch_tolerance_and_repair", test_hash_mismatch_tolerance_and_repair);
    failures += run_test("load_flags_skip", test_load_flags_skip);
    failures += run_test("atomic_save_and_backup", test_atomic_save_and_backup);
    failures += run_test("validation", test_validation);
    failures += run_test("bulk_accessors", test_bulk_accessors);
    failures += run_test("decode_all_non_cubic_with_holes", test_decode_all_non_cubic_with_holes);
    failures += run_test("chunk_content_hash", test_chunk_content_hash);
    failures += run_test("texture_authoring", test_texture_authoring);
    failures += run_test("paths_and_progress", test_paths_and_progress);
    failures += run_test("manifest_text_matches_save", test_manifest_text_matches_save);

    failures += run_test("face_state", test_face_state);
    failures += run_test("texel_formats", test_texel_formats);
    failures += run_test("axis_conversion_math", test_axis_conversion_math);
    failures += run_test("world_axis_conversion", test_world_axis_conversion);
    failures += run_test("registry_colors_and_palette", test_registry_colors_and_palette);
    failures += run_test("world_clone", test_world_clone);
    failures += run_test("save_through_vfs", test_save_through_vfs);
    failures += run_test("non_ascii_paths", test_non_ascii_paths);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}

#include <bsvx_dll.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

    std::vector<unsigned char> read_file_bytes(const fs::path& path)
    {
        std::ifstream is(path, std::ios::binary);
        if (!is) throw TestFailure("could not open " + path.string());
        is.seekg(0, std::ios::end);
        const auto size = static_cast<size_t>(is.tellg());
        is.seekg(0, std::ios::beg);
        std::vector<unsigned char> bytes(size);
        if (size != 0) is.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!is) throw TestFailure("could not read " + path.string());
        return bytes;
    }

    [[noreturn]] void throw_last_error(const Context& ctx, const char* what)
    {
        throw TestFailure(std::string(what) + ": " + ctx.last_error());
    }

    World load_world(Context& ctx, const fs::path& path)
    {
        bsvx_world* raw = nullptr;
        const auto rc = bsvx_world_load(ctx.get(), path.string().c_str(), &raw);
        if (rc != BSVX_RESULT_OK) throw_last_error(ctx, "bsvx_world_load failed");
        REQUIRE(raw != nullptr);
        return World(raw);
    }

    World load_region(Context& ctx, const fs::path& path)
    {
        bsvx_world* raw = nullptr;
        const auto rc = bsvx_world_load_region(ctx.get(), path.string().c_str(), &raw);
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
        REQUIRE_EQ(bsvx_world_save(world.get(), out_root.string().c_str()), BSVX_RESULT_OK);
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
        REQUIRE_EQ(bsvx_world_save(world.get(), out_root.string().c_str()), BSVX_RESULT_OK);

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
                region.parent_path().string().c_str(), &with_tex),
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
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), out_root.string().c_str()), BSVX_RESULT_OK);

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
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), out_root.string().c_str()), BSVX_RESULT_OK);

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

        // Saving somewhere impossible must report why.
        REQUIRE(bsvx_world_save_ex(ctx.get(), world.get(), "/proc/bsvx_does_not_exist/world") != BSVX_RESULT_OK);
        REQUIRE(!ctx.last_error().empty());

        // A successful call clears the previous message.
        const fs::path out_root = make_temp_dir("error_reporting");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), out_root.string().c_str()), BSVX_RESULT_OK);
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
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), fat_root.string().c_str()), BSVX_RESULT_OK);
        const auto fat_size = fs::file_size(fat_root / "regions" / "r_0_0_0.bvx");

        size_t reclaimed = 0;
        REQUIRE_EQ(bsvx_world_compact(world.get(), &reclaimed), BSVX_RESULT_OK);
        REQUIRE_EQ(reclaimed, reclaimable);
        REQUIRE_EQ(bsvx_region_reclaimable_bytes(world.get(), 0), 0u);

        // Compaction is data-preserving.
        const auto after = decode_chunk(world, 0, info.local_chunk_x, info.local_chunk_y, info.local_chunk_z);
        REQUIRE(after == dense);

        const fs::path lean_root = make_temp_dir("compaction_lean");
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), lean_root.string().c_str()), BSVX_RESULT_OK);
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
        if (bsvx_region_reader_open(ctx.get(), region.string().c_str(), &raw) != BSVX_RESULT_OK) {
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
        REQUIRE_EQ(bsvx_world_save_region_ex(ctx.get(), world.get(), out_region.string().c_str()), BSVX_RESULT_OK);

        bsvx_region_reader* raw = nullptr;
        if (bsvx_region_reader_open(ctx.get(), out_region.string().c_str(), &raw) != BSVX_RESULT_OK) {
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
        REQUIRE_EQ(bsvx_world_save_ex(ctx.get(), world.get(), out_root.string().c_str()), BSVX_RESULT_OK);
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
        REQUIRE_EQ(bsvx_world_save_region_ex(ctx.get(), world.get(), out_region.string().c_str()), BSVX_RESULT_OK);
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
            if (entry.is_regular_file()) names.push_back(entry.path().filename().string());
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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}

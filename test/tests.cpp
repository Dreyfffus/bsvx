#include <bsvx_dll.h>

#include <array>
#include <cstdint>
#include <filesystem>
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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}

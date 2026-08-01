#include "bsvx_dll.h"

#include "bsvx.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct bsvx_context {
	std::string last_error;
};

struct bsvx_world {
	bsvx::WorldPackage package;

	// Regions belonging to a manifest world carry no registry of their own, so the write path has
	// to be handed the world's or every rebuilt summary loses its opaque/emissive/special split.
	const bsvx::bvx::RegistryLookup& registry_lookup()
	{
		if (!registry_lookup_) registry_lookup_ = bsvx::bvx::build_registry_lookup(package.manifest.world_desc);
		return *registry_lookup_;
	}

	void invalidate_registry_lookup()
	{
		registry_lookup_.reset();
	}

private:
	std::optional<bsvx::bvx::RegistryLookup> registry_lookup_;
};

struct bsvx_region_reader {
	bsvx::bvx::RegionReader reader;
};

namespace {
    static void set_error(bsvx_context* ctx, std::string msg) {
        if (ctx) ctx->last_error = std::move(msg);
    }

    static bsvx_result catch_all_to_result(bsvx_context* ctx) noexcept {
        try {
            throw;
        }
        catch (const std::invalid_argument& e) {
            set_error(ctx, e.what());
            return BSVX_RESULT_INVALID_ARGUMENT;
        }
        catch (const std::out_of_range& e) {
            set_error(ctx, e.what());
            return BSVX_RESULT_NOT_FOUND;
        }
        catch (const std::exception& e) {
            set_error(ctx, e.what());
            return BSVX_RESULT_RUNTIME_ERROR;
        }
        catch (...) {
            set_error(ctx, "unknown error");
            return BSVX_RESULT_RUNTIME_ERROR;
        }
    }

    static void clear_error(bsvx_context* ctx) {
        if (ctx) ctx->last_error.clear();
    }

    static bool check_region_index(const bsvx_world* world, size_t region_index) {
        return world && region_index < world->package.regions.size();
    }

    static bsvx::bvx::Archive& region_archive_mut(bsvx_world* world, size_t region_index) {
        return world->package.regions[region_index].archive;
    }

    static const bsvx::bvx::Archive& region_archive(const bsvx_world* world, size_t region_index) {
        return world->package.regions[region_index].archive;
    }

    static const bsvx::bvx::WorldDesc& world_desc(const bsvx_world* world) {
        return world->package.manifest.world_desc;
    }

    static void fill_chunk_info(bsvx_chunk_info* out_info, const bsvx::bvx::ChunkMapEntry& map, const bsvx::bvx::ChunkSummary& sum)
    {
        out_info->local_chunk_x = map.local_chunk_x;
        out_info->local_chunk_y = map.local_chunk_y;
        out_info->local_chunk_z = map.local_chunk_z;
        out_info->flags = map.flags;
        out_info->summary_index = map.summary_index;

        out_info->summary.non_air_count = sum.non_air_count;
        out_info->summary.opaque_count = sum.opaque_count;
        out_info->summary.emissive_count = sum.emissive_count;
        out_info->summary.special_count = sum.special_count;
        out_info->summary.aabb_min_x = sum.aabb_min_x;
        out_info->summary.aabb_min_y = sum.aabb_min_y;
        out_info->summary.aabb_min_z = sum.aabb_min_z;
        out_info->summary.aabb_max_x = sum.aabb_max_x;
        out_info->summary.aabb_max_y = sum.aabb_max_y;
        out_info->summary.aabb_max_z = sum.aabb_max_z;
        out_info->summary.face_state_px = sum.face_state_px;
        out_info->summary.face_state_nx = sum.face_state_nx;
        out_info->summary.face_state_py = sum.face_state_py;
        out_info->summary.face_state_ny = sum.face_state_ny;
        out_info->summary.face_state_pz = sum.face_state_pz;
        out_info->summary.face_state_nz = sum.face_state_nz;
        out_info->summary.macro_occ_4x4x4 = sum.macro_occ_4x4x4;
        out_info->summary.top_id_0 = sum.top_id_0;
        out_info->summary.top_id_1 = sum.top_id_1;
        out_info->summary.top_id_2 = sum.top_id_2;
        out_info->summary.top_id_3 = sum.top_id_3;
        out_info->summary.top_count_0 = sum.top_count_0;
        out_info->summary.top_count_1 = sum.top_count_1;
        out_info->summary.top_count_2 = sum.top_count_2;
        out_info->summary.top_count_3 = sum.top_count_3;
    }

    static void fill_registry_entry(bsvx_registry_entry* out_entry, const bsvx::bvx::RegistryEntry& src)
    {
        out_entry->voxel_key = src.voxel_key;
        out_entry->material_id = src.material_id;
        out_entry->flags = src.flags;
        out_entry->name_hash = src.name_hash;
    }

    static void fill_geometry(bsvx_geometry_desc* out_geometry, const bsvx::bvx::GeometryDesc& g)
    {
        out_geometry->chunk_size_x = g.chunk_size_x;
        out_geometry->chunk_size_y = g.chunk_size_y;
        out_geometry->chunk_size_z = g.chunk_size_z;
        out_geometry->region_size_x = g.region_size_x;
        out_geometry->region_size_y = g.region_size_y;
        out_geometry->region_size_z = g.region_size_z;
    }

    static void fill_world_desc(bsvx_world_desc* out_desc, const bsvx::bvx::WorldDesc& d)
    {
        fill_geometry(&out_desc->geometry, d.geometry);
        out_desc->voxel_schema = bsvx::to_underlying(d.voxel_schema);
        out_desc->axis_convention = bsvx::to_underlying(d.axis_convention);
        out_desc->bounds_mode = bsvx::to_underlying(d.bounds_mode);
        out_desc->reserved = 0;
        out_desc->min_region_x = d.world_min_region_x;
        out_desc->min_region_y = d.world_min_region_y;
        out_desc->min_region_z = d.world_min_region_z;
        out_desc->max_region_x = d.world_max_region_x;
        out_desc->max_region_y = d.world_max_region_y;
        out_desc->max_region_z = d.world_max_region_z;
        out_desc->asset_name_hash = d.asset_name_hash;
        out_desc->registry_hash = d.registry_hash;
        out_desc->manifest_hash = d.manifest_hash;
    }

    static bsvx::bvx::GeometryDesc to_geometry(const bsvx_geometry_desc& g)
    {
        bsvx::bvx::GeometryDesc out{};
        out.chunk_size_x = g.chunk_size_x;
        out.chunk_size_y = g.chunk_size_y;
        out.chunk_size_z = g.chunk_size_z;
        out.region_size_x = g.region_size_x;
        out.region_size_y = g.region_size_y;
        out.region_size_z = g.region_size_z;
        return out;
    }

    // Shared bodies so the plain and _ex entry points cannot drift apart.

    static bsvx_result impl_world_save(bsvx_context* ctx, const bsvx_world* world, const char* root_or_manifest_path)
    {
        if (!world || !root_or_manifest_path) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);
        try {
            bsvx::Parser::save_world(world->package, root_or_manifest_path);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    static bsvx_result impl_world_save_region(bsvx_context* ctx, const bsvx_world* world, const char* region_path)
    {
        if (!world || !region_path) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);
        try {
            bsvx::Parser::save_region(world->package, region_path);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    static bsvx_result impl_decode_chunk(bsvx_context* ctx, const bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint32_t* out_voxels, size_t voxel_capacity, size_t* out_written)
    {
        if (!check_region_index(world, region_index) || !out_voxels) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto dense = region_archive(world, region_index).decode_chunk_voxels(chunk_x, chunk_y, chunk_z, &world_desc(world).geometry);
            if (voxel_capacity < dense.size()) {
                if (out_written) *out_written = dense.size();
                return BSVX_RESULT_BUFFER_TOO_SMALL;
            }
            for (size_t i = 0; i < dense.size(); ++i) out_voxels[i] = dense[i];
            if (out_written) *out_written = dense.size();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    static bsvx_result impl_set_chunk_u32(bsvx_context* ctx, bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const uint32_t* voxels, size_t voxel_count, uint16_t requested_codec)
    {
        if (!check_region_index(world, region_index) || !voxels) return BSVX_RESULT_INVALID_ARGUMENT;
        const size_t required = bsvx_region_required_voxel_count(world, region_index);
        if (voxel_count != required) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto view = std::span<const uint32_t>(voxels, voxel_count);
            const auto& registry = world->registry_lookup();
            region_archive_mut(world, region_index).set_chunk_voxels_dense(
                chunk_x,
                chunk_y,
                chunk_z,
                view,
                &world_desc(world).geometry,
                static_cast<bsvx::VoxelCodec>(requested_codec),
                registry.empty() ? nullptr : &registry);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    static bsvx_result impl_set_chunk_payload(bsvx_context* ctx, bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, const void* payload, size_t payload_size, uint16_t entry_flags)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        if (payload_size != 0 && !payload) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload), payload_size);
            region_archive_mut(world, region_index).set_chunk_payload(
                static_cast<bsvx::SectionType>(section_type),
                chunk_x,
                chunk_y,
                chunk_z,
                codec,
                bytes,
                entry_flags);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    // Copies a payload span out under the BUFFER_TOO_SMALL convention shared by decode.
    static bsvx_result emit_payload(std::span<const std::byte> payload, uint16_t codec, void* out_payload, size_t capacity, size_t* out_size, uint16_t* out_codec)
    {
        if (out_size) *out_size = payload.size();
        if (out_codec) *out_codec = codec;
        if (capacity < payload.size()) return BSVX_RESULT_BUFFER_TOO_SMALL;
        if (!payload.empty()) {
            if (!out_payload) return BSVX_RESULT_INVALID_ARGUMENT;
            std::memcpy(out_payload, payload.data(), payload.size());
        }
        return BSVX_RESULT_OK;
    }

    static bsvx_result impl_get_chunk_payload(bsvx_context* ctx, const bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, void* out_payload, size_t capacity, size_t* out_size, uint16_t* out_codec)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        if (capacity != 0 && !out_payload) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            uint16_t codec = 0;
            const auto payload = region_archive(world, region_index).get_chunk_payload(
                static_cast<bsvx::SectionType>(section_type), chunk_x, chunk_y, chunk_z, &codec);
            if (!payload) {
                if (out_size) *out_size = 0;
                set_error(ctx, "chunk has no payload in the requested section");
                return BSVX_RESULT_NOT_FOUND;
            }
            return emit_payload(*payload, codec, out_payload, capacity, out_size, out_codec);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    // Same convention as emit_payload, for NUL-terminated text: *out_size counts the terminator.
    static bsvx_result emit_string(const std::string& text, char* out, size_t capacity, size_t* out_size)
    {
        const size_t needed = text.size() + 1u;
        if (out_size) *out_size = needed;
        if (capacity < needed) return BSVX_RESULT_BUFFER_TOO_SMALL;
        if (!out) return BSVX_RESULT_INVALID_ARGUMENT;
        std::memcpy(out, text.c_str(), needed);
        return BSVX_RESULT_OK;
    }

    static bool check_texture_index(const bsvx_world* world, size_t tex_index)
    {
        return world && tex_index < world->package.textures.size();
    }

    static const bsvx::btx::Archive& texture_archive(const bsvx_world* world, size_t tex_index)
    {
        return world->package.textures[tex_index].archive;
    }

    static bsvx_result impl_load_region_memory(bsvx_context* ctx, const void* bytes, size_t size, const char* texture_root, bsvx_world** out_world)
    {
        if (!ctx || !bytes || size == 0 || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        ctx->last_error.clear();

        try {
            const auto view = std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes), size);
            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::load_region_memory(view, texture_root ? std::filesystem::path(texture_root) : std::filesystem::path{});
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    // "res://worlds/w/manifest.toml" -> { "res://", "worlds/w/manifest.toml" }. The scheme has to
    // come off before any std::filesystem::path arithmetic, which would collapse the "//" in it.
    static std::pair<std::string, std::string> split_scheme(const char* path)
    {
        const std::string full = path ? path : "";
        const size_t marker = full.find("://");
        if (marker == std::string::npos) return { std::string{}, full };
        return { full.substr(0, marker + 3u), full.substr(marker + 3u) };
    }

    // Routes every read the Parser makes through the host's callbacks.
    class VfsFileSystem final : public bsvx::FileSystem {
    public:
        VfsFileSystem(const bsvx_vfs& vfs, std::string scheme)
            : vfs_(vfs), scheme_(std::move(scheme))
        {
            if (!vfs_.read_file) throw std::invalid_argument("[bsvx]: vfs: read_file callback is required");
        }

        std::filesystem::path normalize(const std::filesystem::path& path) const override
        {
            return path.lexically_normal();
        }

        bool is_file(const std::filesystem::path& path) const override
        {
            const std::string full = external(path);
            if (vfs_.file_exists) return vfs_.file_exists(vfs_.user, full.c_str()) != 0;

            size_t size = 0;
            return vfs_.read_file(vfs_.user, full.c_str(), nullptr, 0, &size) != 0;
        }

        bool is_directory(const std::filesystem::path& path) const override
        {
            std::string first;
            return read_dir_entry(path, 0, first);
        }

        std::vector<std::byte> read_file(const std::filesystem::path& path) const override
        {
            const std::string full = external(path);

            size_t size = 0;
            if (!vfs_.read_file(vfs_.user, full.c_str(), nullptr, 0, &size)) {
                throw std::runtime_error("[bsvx]: vfs: could not read " + full);
            }

            std::vector<std::byte> bytes(size);
            if (size != 0) {
                size_t written = 0;
                if (!vfs_.read_file(vfs_.user, full.c_str(), bytes.data(), bytes.size(), &written)) {
                    throw std::runtime_error("[bsvx]: vfs: could not read " + full);
                }
                if (written != size) {
                    throw std::runtime_error("[bsvx]: vfs: short read for " + full);
                }
            }
            return bytes;
        }

        std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::string_view extension) const override
        {
            std::vector<std::filesystem::path> out;
            if (!vfs_.list_dir) return out;

            for (size_t i = 0; ; ++i) {
                std::string name;
                if (!read_dir_entry(dir, i, name)) break;

                std::filesystem::path entry(name);
                if (entry.extension() == extension) out.push_back(std::move(entry));
            }
            return out;
        }

    private:
        std::string external(const std::filesystem::path& path) const
        {
            return scheme_ + path.generic_string();
        }

        bool read_dir_entry(const std::filesystem::path& dir, size_t index, std::string& out_name) const
        {
            if (!vfs_.list_dir) return false;
            const std::string full = external(dir);

            char inline_buffer[256];
            size_t needed = 0;
            if (!vfs_.list_dir(vfs_.user, full.c_str(), index, inline_buffer, sizeof(inline_buffer), &needed)) return false;
            if (needed == 0) return false;

            if (needed <= sizeof(inline_buffer)) {
                out_name.assign(inline_buffer, needed - 1u);
                return true;
            }

            std::vector<char> heap(needed);
            if (!vfs_.list_dir(vfs_.user, full.c_str(), index, heap.data(), heap.size(), &needed)) return false;
            if (needed == 0 || needed > heap.size()) return false;

            out_name.assign(heap.data(), needed - 1u);
            return true;
        }

        const bsvx_vfs& vfs_;
        std::string scheme_;
    };

}

extern "C" {

    BSVX_API uint32_t bsvx_abi_version(void)
    {
        return BSVX_ABI_VERSION;
    }

    BSVX_API bsvx_context* bsvx_context_create(void)
    {
        try {
            return new bsvx_context{};
        }
        catch (...) {
            return nullptr;
        }
    }

    BSVX_API void bsvx_context_destroy(bsvx_context* ctx)
    {
        delete ctx;
    }

    BSVX_API const char* bsvx_context_last_error(const bsvx_context* ctx)
    {
        if (!ctx) return "Invalid bsvx context";
        return ctx->last_error.c_str();
    }

    BSVX_API bsvx_result bsvx_world_load(bsvx_context* ctx, const char* path, bsvx_world** out_world)
    {
        if (!ctx || !path || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        ctx->last_error.clear();

        try {
            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::load_world(path);
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_load_region(bsvx_context* ctx, const char* path, bsvx_world** out_world)
    {
        if (!ctx || !path || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        ctx->last_error.clear();

        try {
            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::load_region(path);
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_load_region_memory(bsvx_context* ctx, const void* bytes, size_t size, bsvx_world** out_world)
    {
        return impl_load_region_memory(ctx, bytes, size, nullptr, out_world);
    }

    BSVX_API bsvx_result bsvx_world_load_region_memory_ex(bsvx_context* ctx, const void* bytes, size_t size, const char* texture_root, bsvx_world** out_world)
    {
        return impl_load_region_memory(ctx, bytes, size, (texture_root && texture_root[0] != '\0') ? texture_root : nullptr, out_world);
    }

    BSVX_API void bsvx_world_destroy(bsvx_world* world)
    {
        delete world;
    }

    BSVX_API bsvx_result bsvx_world_save(const bsvx_world* world, const char* root_or_manifest_path)
    {
        return impl_world_save(nullptr, world, root_or_manifest_path);
    }

    BSVX_API bsvx_result bsvx_world_save_ex(bsvx_context* ctx, const bsvx_world* world, const char* root_or_manifest_path)
    {
        return impl_world_save(ctx, world, root_or_manifest_path);
    }

    BSVX_API bsvx_result bsvx_world_save_region(const bsvx_world* world, const char* region_path)
    {
        return impl_world_save_region(nullptr, world, region_path);
    }

    BSVX_API bsvx_result bsvx_world_save_region_ex(bsvx_context* ctx, const bsvx_world* world, const char* region_path)
    {
        return impl_world_save_region(ctx, world, region_path);
    }

    BSVX_API size_t bsvx_world_region_count(const bsvx_world* world)
    {
        return world ? world->package.regions.size() : 0u;
    }

    BSVX_API size_t bsvx_world_texture_count(const bsvx_world* world)
    {
        return world ? world->package.textures.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_world_geometry(const bsvx_world* world, bsvx_geometry_desc* out_geometry)
    {
        if (!world || !out_geometry) return BSVX_RESULT_INVALID_ARGUMENT;

        fill_geometry(out_geometry, world_desc(world).geometry);
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_world_registry_entry_count(const bsvx_world* world)
    {
        return world ? world_desc(world).registry_entries.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_world_get_registry_entry(const bsvx_world* world, size_t entry_index, bsvx_registry_entry* out_entry)
    {
        if (!world || !out_entry) return BSVX_RESULT_INVALID_ARGUMENT;
        const auto& entries = world_desc(world).registry_entries;
        if (entry_index >= entries.size()) return BSVX_RESULT_NOT_FOUND;

        fill_registry_entry(out_entry, entries[entry_index]);
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_get_region_coord(const bsvx_world* world, size_t region_index, int32_t* out_x, int32_t* out_y, int32_t* out_z)
    {
        if (!check_region_index(world, region_index) || !out_x || !out_y || !out_z) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& reg = region_archive(world, region_index);
        *out_x = reg.region_x;
        *out_y = reg.region_y;
        *out_z = reg.region_z;
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_region_chunk_count(const bsvx_world* world, size_t region_index)
    {
        return check_region_index(world, region_index) ? region_archive(world, region_index).chunk_map.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_region_get_chunk_info(const bsvx_world* world, size_t region_index, size_t chunk_ordinal, bsvx_chunk_info* out_info)
    {
        if (!check_region_index(world, region_index) || !out_info) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& reg = region_archive(world, region_index);
        if (chunk_ordinal >= reg.chunk_map.size()) return BSVX_RESULT_NOT_FOUND;

        const auto& map = reg.chunk_map[chunk_ordinal];
        if (map.summary_index >= reg.chunk_summaries.size()) return BSVX_RESULT_NOT_FOUND;

        fill_chunk_info(out_info, map, reg.chunk_summaries[map.summary_index]);
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_region_find_chunk(const bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, size_t* out_chunk_ordinal)
    {
        if (!check_region_index(world, region_index) || !out_chunk_ordinal) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto found = region_archive(world, region_index).try_find_chunk_index(chunk_x, chunk_y, chunk_z);
        if (!found) return BSVX_RESULT_NOT_FOUND;

        *out_chunk_ordinal = *found;
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_region_required_voxel_count(const bsvx_world* world, size_t region_index)
    {
        if (!check_region_index(world, region_index)) return 0u;
        const auto& g = world_desc(world).geometry;
        return static_cast<size_t>(g.chunk_size_x) * static_cast<size_t>(g.chunk_size_y) * static_cast<size_t>(g.chunk_size_z);
    }

    BSVX_API bsvx_result bsvx_region_decode_chunk_u32(const bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint32_t* out_voxels, size_t voxel_capacity, size_t* out_written)
    {
        return impl_decode_chunk(nullptr, world, region_index, chunk_x, chunk_y, chunk_z, out_voxels, voxel_capacity, out_written);
    }

    BSVX_API bsvx_result bsvx_region_decode_chunk_u32_ex(bsvx_context* ctx, const bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint32_t* out_voxels, size_t voxel_capacity, size_t* out_written)
    {
        return impl_decode_chunk(ctx, world, region_index, chunk_x, chunk_y, chunk_z, out_voxels, voxel_capacity, out_written);
    }

    BSVX_API bsvx_result bsvx_region_set_chunk_u32(bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const uint32_t* voxels, size_t voxel_count, uint16_t requested_codec)
    {
        return impl_set_chunk_u32(nullptr, world, region_index, chunk_x, chunk_y, chunk_z, voxels, voxel_count, requested_codec);
    }

    BSVX_API bsvx_result bsvx_region_set_chunk_u32_ex(bsvx_context* ctx, bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const uint32_t* voxels, size_t voxel_count, uint16_t requested_codec)
    {
        return impl_set_chunk_u32(ctx, world, region_index, chunk_x, chunk_y, chunk_z, voxels, voxel_count, requested_codec);
    }

    BSVX_API bsvx_result bsvx_region_set_chunk_payload(bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, const void* payload, size_t payload_size, uint16_t entry_flags)
    {
        return impl_set_chunk_payload(nullptr, world, region_index, section_type, chunk_x, chunk_y, chunk_z, codec, payload, payload_size, entry_flags);
    }

    BSVX_API bsvx_result bsvx_region_set_chunk_payload_ex(bsvx_context* ctx, bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, const void* payload, size_t payload_size, uint16_t entry_flags)
    {
        return impl_set_chunk_payload(ctx, world, region_index, section_type, chunk_x, chunk_y, chunk_z, codec, payload, payload_size, entry_flags);
    }

    BSVX_API bsvx_result bsvx_region_get_chunk_payload(const bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, void* out_payload, size_t capacity, size_t* out_size, uint16_t* out_codec)
    {
        return impl_get_chunk_payload(nullptr, world, region_index, section_type, chunk_x, chunk_y, chunk_z, out_payload, capacity, out_size, out_codec);
    }

    BSVX_API bsvx_result bsvx_region_get_chunk_payload_ex(bsvx_context* ctx, const bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, void* out_payload, size_t capacity, size_t* out_size, uint16_t* out_codec)
    {
        return impl_get_chunk_payload(ctx, world, region_index, section_type, chunk_x, chunk_y, chunk_z, out_payload, capacity, out_size, out_codec);
    }

    BSVX_API bsvx_result bsvx_region_get_chunk_payload_info(const bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, size_t* out_size, uint16_t* out_codec, uint16_t* out_entry_flags)
    {
        if (!check_region_index(world, region_index) || !out_size) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            uint16_t codec = 0;
            uint16_t flags = 0;
            const auto payload = region_archive(world, region_index).get_chunk_payload(
                static_cast<bsvx::SectionType>(section_type), chunk_x, chunk_y, chunk_z, &codec, &flags);
            if (!payload) {
                *out_size = 0;
                return BSVX_RESULT_NOT_FOUND;
            }

            *out_size = payload->size();
            if (out_codec) *out_codec = codec;
            if (out_entry_flags) *out_entry_flags = flags;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API size_t bsvx_region_reclaimable_bytes(const bsvx_world* world, size_t region_index)
    {
        if (!check_region_index(world, region_index)) return 0u;
        try {
            return region_archive(world, region_index).reclaimable_bytes();
        }
        catch (...) {
            return 0u;
        }
    }

    BSVX_API bsvx_result bsvx_region_compact(bsvx_world* world, size_t region_index, size_t* out_reclaimed)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            const size_t reclaimed = region_archive_mut(world, region_index).compact();
            if (out_reclaimed) *out_reclaimed = reclaimed;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_compact(bsvx_world* world, size_t* out_reclaimed)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            size_t reclaimed = 0;
            for (auto& region : world->package.regions) reclaimed += region.archive.compact();
            if (out_reclaimed) *out_reclaimed = reclaimed;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    /* -------------------------------------------------------------------------------------- */
    /* Streaming region reader                                                                 */
    /* -------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_region_reader_open(bsvx_context* ctx, const char* path, bsvx_region_reader** out_reader)
    {
        if (!ctx || !path || !out_reader) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_reader = nullptr;
        ctx->last_error.clear();

        try {
            auto reader = std::make_unique<bsvx_region_reader>();
            reader->reader = bsvx::bvx::RegionReader::open_file(path);
            *out_reader = reader.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_region_reader_open_memory(bsvx_context* ctx, const void* bytes, size_t size, int copy_bytes, bsvx_region_reader** out_reader)
    {
        if (!ctx || !bytes || size == 0 || !out_reader) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_reader = nullptr;
        ctx->last_error.clear();

        try {
            const auto view = std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes), size);
            auto reader = std::make_unique<bsvx_region_reader>();
            reader->reader = bsvx::bvx::RegionReader::open_memory(view, copy_bytes != 0);
            *out_reader = reader.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API void bsvx_region_reader_close(bsvx_region_reader* reader)
    {
        delete reader;
    }

    BSVX_API bsvx_result bsvx_region_reader_set_geometry(bsvx_region_reader* reader, const bsvx_geometry_desc* geometry)
    {
        if (!reader || !geometry) return BSVX_RESULT_INVALID_ARGUMENT;

        reader->reader.set_geometry_override(to_geometry(*geometry));
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_region_reader_geometry(const bsvx_region_reader* reader, bsvx_geometry_desc* out_geometry)
    {
        if (!reader || !out_geometry) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            fill_geometry(out_geometry, reader->reader.resolve_geometry());
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return BSVX_RESULT_NOT_FOUND;
        }
    }

    BSVX_API int bsvx_region_reader_is_standalone(const bsvx_region_reader* reader)
    {
        return (reader && reader->reader.is_standalone()) ? 1 : 0;
    }

    BSVX_API bsvx_result bsvx_region_reader_coord(const bsvx_region_reader* reader, int32_t* out_x, int32_t* out_y, int32_t* out_z)
    {
        if (!reader || !out_x || !out_y || !out_z) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_x = reader->reader.header().region_x;
        *out_y = reader->reader.header().region_y;
        *out_z = reader->reader.header().region_z;
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_region_reader_resident_bytes(const bsvx_region_reader* reader)
    {
        return reader ? static_cast<size_t>(reader->reader.resident_bytes()) : 0u;
    }

    BSVX_API size_t bsvx_region_reader_chunk_count(const bsvx_region_reader* reader)
    {
        return reader ? reader->reader.chunk_map().size() : 0u;
    }

    BSVX_API size_t bsvx_region_reader_required_voxel_count(const bsvx_region_reader* reader)
    {
        if (!reader) return 0u;
        try {
            return bsvx::bvx::Archive::chunk_voxel_count(reader->reader.resolve_geometry());
        }
        catch (...) {
            return 0u;
        }
    }

    BSVX_API bsvx_result bsvx_region_reader_get_chunk_info(const bsvx_region_reader* reader, size_t chunk_ordinal, bsvx_chunk_info* out_info)
    {
        if (!reader || !out_info) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& map = reader->reader.chunk_map();
        const auto& summaries = reader->reader.chunk_summaries();
        if (chunk_ordinal >= map.size()) return BSVX_RESULT_NOT_FOUND;
        if (map[chunk_ordinal].summary_index >= summaries.size()) return BSVX_RESULT_NOT_FOUND;

        fill_chunk_info(out_info, map[chunk_ordinal], summaries[map[chunk_ordinal].summary_index]);
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_region_reader_find_chunk(const bsvx_region_reader* reader, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, size_t* out_chunk_ordinal)
    {
        if (!reader || !out_chunk_ordinal) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto found = reader->reader.try_find_chunk_index(chunk_x, chunk_y, chunk_z);
        if (!found) return BSVX_RESULT_NOT_FOUND;

        *out_chunk_ordinal = *found;
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_region_reader_registry_entry_count(const bsvx_region_reader* reader)
    {
        if (!reader) return 0u;
        const auto* desc = reader->reader.world_desc();
        return desc ? desc->registry_entries.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_region_reader_get_registry_entry(const bsvx_region_reader* reader, size_t entry_index, bsvx_registry_entry* out_entry)
    {
        if (!reader || !out_entry) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto* desc = reader->reader.world_desc();
        if (!desc || entry_index >= desc->registry_entries.size()) return BSVX_RESULT_NOT_FOUND;

        fill_registry_entry(out_entry, desc->registry_entries[entry_index]);
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_region_reader_decode_chunk_u32(bsvx_context* ctx, const bsvx_region_reader* reader, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint32_t* out_voxels, size_t voxel_capacity, size_t* out_written)
    {
        if (!reader || !out_voxels) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto dense = reader->reader.decode_chunk_voxels(chunk_x, chunk_y, chunk_z);
            if (voxel_capacity < dense.size()) {
                if (out_written) *out_written = dense.size();
                return BSVX_RESULT_BUFFER_TOO_SMALL;
            }
            for (size_t i = 0; i < dense.size(); ++i) out_voxels[i] = dense[i];
            if (out_written) *out_written = dense.size();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_region_reader_get_chunk_payload(bsvx_context* ctx, const bsvx_region_reader* reader, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, void* out_payload, size_t capacity, size_t* out_size, uint16_t* out_codec)
    {
        if (!reader) return BSVX_RESULT_INVALID_ARGUMENT;
        if (capacity != 0 && !out_payload) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto chunk_index = reader->reader.try_find_chunk_index(chunk_x, chunk_y, chunk_z);
            if (!chunk_index) {
                set_error(ctx, "[bvx]: chunk not found");
                if (out_size) *out_size = 0;
                return BSVX_RESULT_NOT_FOUND;
            }

            uint16_t codec = 0;
            const auto payload = reader->reader.read_chunk_payload(static_cast<bsvx::SectionType>(section_type), *chunk_index, &codec);
            if (!payload) {
                set_error(ctx, "chunk has no payload in the requested section");
                if (out_size) *out_size = 0;
                return BSVX_RESULT_NOT_FOUND;
            }

            return emit_payload(std::span<const std::byte>(payload->data(), payload->size()), codec, out_payload, capacity, out_size, out_codec);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_region_reader_verify(bsvx_context* ctx, const bsvx_region_reader* reader)
    {
        if (!reader) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            if (!reader->reader.verify_integrity()) {
                set_error(ctx, "[bvx]: crc mismatch");
                return BSVX_RESULT_RUNTIME_ERROR;
            }
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_region_reader_load_full(bsvx_context* ctx, const bsvx_region_reader* reader, bsvx_world** out_world)
    {
        if (!ctx || !reader || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        ctx->last_error.clear();

        try {
            if (!reader->reader.is_standalone()) {
                set_error(ctx, "[bsvx]: only a standalone region can be promoted to a world");
                return BSVX_RESULT_INVALID_ARGUMENT;
            }

            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::wrap_standalone_region(reader->reader.load_full());
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* -------------------------------------------------------------------------------------- */
    /* Authoring                                                                               */
    /* -------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_create(bsvx_context* ctx, const bsvx_geometry_desc* geometry, bsvx_world** out_world)
    {
        if (!ctx || !geometry || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        ctx->last_error.clear();

        try {
            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::create_world(to_geometry(*geometry));
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_find_region(const bsvx_world* world, int32_t region_x, int32_t region_y, int32_t region_z, size_t* out_region_index)
    {
        if (!world || !out_region_index) return BSVX_RESULT_INVALID_ARGUMENT;

        for (size_t i = 0; i < world->package.regions.size(); ++i) {
            const auto& archive = world->package.regions[i].archive;
            if (archive.region_x == region_x && archive.region_y == region_y && archive.region_z == region_z) {
                *out_region_index = i;
                return BSVX_RESULT_OK;
            }
        }
        return BSVX_RESULT_NOT_FOUND;
    }

    BSVX_API bsvx_result bsvx_world_add_region(bsvx_world* world, int32_t region_x, int32_t region_y, int32_t region_z, size_t* out_region_index)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        size_t existing = 0;
        if (bsvx_world_find_region(world, region_x, region_y, region_z, &existing) == BSVX_RESULT_OK) {
            return BSVX_RESULT_INVALID_ARGUMENT;
        }

        try {
            bsvx::RegionAsset asset{};
            asset.ref.coord = std::array<int32_t, 3>{ region_x, region_y, region_z };
            asset.archive.region_x = region_x;
            asset.archive.region_y = region_y;
            asset.archive.region_z = region_z;
            world->package.regions.push_back(std::move(asset));

            if (out_region_index) *out_region_index = world->package.regions.size() - 1u;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_set_registry_entry(bsvx_world* world, const bsvx_registry_entry* entry)
    {
        if (!world || !entry) return BSVX_RESULT_INVALID_ARGUMENT;
        if (entry->voxel_key == 0u) return BSVX_RESULT_INVALID_ARGUMENT;   // 0 is air, everywhere

        try {
            bsvx::bvx::RegistryEntry native{};
            native.voxel_key = entry->voxel_key;
            native.material_id = entry->material_id;
            native.flags = entry->flags;
            native.name_hash = entry->name_hash;

            auto& entries = world->package.manifest.world_desc.registry_entries;
            const auto found = std::find_if(entries.begin(), entries.end(),
                [&](const bsvx::bvx::RegistryEntry& e) { return e.voxel_key == entry->voxel_key; });

            if (found != entries.end()) *found = native;
            else entries.push_back(native);

            // Zero means "recompute from the entries" on the next save.
            world->package.manifest.world_desc.registry_hash = 0;
            world->invalidate_registry_lookup();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_remove_registry_entry(bsvx_world* world, uint32_t voxel_key)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        auto& entries = world->package.manifest.world_desc.registry_entries;
        const auto found = std::find_if(entries.begin(), entries.end(),
            [&](const bsvx::bvx::RegistryEntry& e) { return e.voxel_key == voxel_key; });
        if (found == entries.end()) return BSVX_RESULT_NOT_FOUND;

        entries.erase(found);
        world->package.manifest.world_desc.registry_hash = 0;
        world->invalidate_registry_lookup();
        return BSVX_RESULT_OK;
    }

    /* -------------------------------------------------------------------------------------- */
    /* World metadata                                                                          */
    /* -------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_get_desc(const bsvx_world* world, bsvx_world_desc* out_desc)
    {
        if (!world || !out_desc) return BSVX_RESULT_INVALID_ARGUMENT;

        fill_world_desc(out_desc, world_desc(world));
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_set_desc(bsvx_world* world, const bsvx_world_desc* desc)
    {
        if (!world || !desc) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& g = desc->geometry;
        if (g.chunk_size_x == 0 || g.chunk_size_y == 0 || g.chunk_size_z == 0) return BSVX_RESULT_INVALID_ARGUMENT;
        if (g.chunk_size_x > 255 || g.chunk_size_y > 255 || g.chunk_size_z > 255) return BSVX_RESULT_INVALID_ARGUMENT;
        if (g.region_size_x == 0 || g.region_size_y == 0 || g.region_size_z == 0) return BSVX_RESULT_INVALID_ARGUMENT;

        // Only one value of each is implemented; anything else would throw on save instead.
        if (desc->voxel_schema != bsvx::to_underlying(bsvx::VoxelSchema::DENSE_U32_VOXEL_KEY)) return BSVX_RESULT_INVALID_ARGUMENT;
        if (desc->axis_convention != bsvx::to_underlying(bsvx::AxisConvention::X_RIGHT_Y_UP_Z_FORWARD)) return BSVX_RESULT_INVALID_ARGUMENT;
        if (desc->bounds_mode > bsvx::to_underlying(bsvx::BoundsMode::EXPLICIT)) return BSVX_RESULT_INVALID_ARGUMENT;

        auto& d = world->package.manifest.world_desc;

        // Voxel payloads are encoded against the geometry that was current when they were written,
        // so resizing chunks under an authored region would silently corrupt every decode.
        const auto next = to_geometry(g);
        const bool geometry_changed = std::memcmp(&next, &d.geometry, sizeof(next)) != 0;
        if (geometry_changed) {
            for (const auto& region : world->package.regions) {
                if (!region.archive.chunk_map.empty()) return BSVX_RESULT_INVALID_ARGUMENT;
            }
        }

        d.geometry = next;
        d.voxel_schema = static_cast<bsvx::VoxelSchema>(desc->voxel_schema);
        d.axis_convention = static_cast<bsvx::AxisConvention>(desc->axis_convention);
        d.bounds_mode = static_cast<bsvx::BoundsMode>(desc->bounds_mode);
        d.world_min_region_x = desc->min_region_x;
        d.world_min_region_y = desc->min_region_y;
        d.world_min_region_z = desc->min_region_z;
        d.world_max_region_x = desc->max_region_x;
        d.world_max_region_y = desc->max_region_y;
        d.world_max_region_z = desc->max_region_z;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_get_name(const bsvx_world* world, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        return emit_string(world->package.manifest.name, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_get_uuid(const bsvx_world* world, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        return emit_string(world->package.manifest.uuid, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_set_name(bsvx_world* world, const char* name)
    {
        if (!world || !name) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            world->package.manifest.name = name;
            world->package.manifest.world_desc.asset_name_hash = bsvx::fnv1a64(std::string_view(name));
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_set_uuid(bsvx_world* world, const char* uuid)
    {
        if (!world || !uuid) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            world->package.manifest.uuid = uuid;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    /* -------------------------------------------------------------------------------------- */
    /* Region serialization to a buffer                                                        */
    /* -------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_save_region_memory(bsvx_context* ctx, const bsvx_world* world, void* out_bytes, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        if (capacity != 0 && !out_bytes) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto bytes = bsvx::Parser::save_region_to_bytes(world->package);
            return emit_payload(std::span<const std::byte>(bytes.data(), bytes.size()), 0u, out_bytes, capacity, out_size, nullptr);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* -------------------------------------------------------------------------------------- */
    /* Manifest loading through a host VFS                                                     */
    /* -------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_load_vfs(bsvx_context* ctx, const char* path, const bsvx_vfs* vfs, bsvx_world** out_world)
    {
        if (!ctx || !path || !vfs || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        ctx->last_error.clear();

        try {
            const auto [scheme, rest] = split_scheme(path);
            const VfsFileSystem fs(*vfs, scheme);

            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::load_world(rest, fs);
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* -------------------------------------------------------------------------------------- */
    /* Texture (.btx) introspection                                                            */
    /* -------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_get_texture_id(const bsvx_world* world, size_t tex_index, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;
        return emit_string(world->package.textures[tex_index].ref.id, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_get_texture_path(const bsvx_world* world, size_t tex_index, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;
        return emit_string(world->package.textures[tex_index].ref.relative_path.generic_string(), out, capacity, out_size);
    }

    BSVX_API size_t bsvx_texture_texture_count(const bsvx_world* world, size_t tex_index)
    {
        return check_texture_index(world, tex_index) ? texture_archive(world, tex_index).textures.size() : 0u;
    }

    BSVX_API size_t bsvx_texture_subresource_count(const bsvx_world* world, size_t tex_index)
    {
        return check_texture_index(world, tex_index) ? texture_archive(world, tex_index).subresources.size() : 0u;
    }

    BSVX_API size_t bsvx_texture_material_count(const bsvx_world* world, size_t tex_index)
    {
        return check_texture_index(world, tex_index) ? texture_archive(world, tex_index).materials.size() : 0u;
    }

    BSVX_API size_t bsvx_texture_sampler_count(const bsvx_world* world, size_t tex_index)
    {
        return check_texture_index(world, tex_index) ? texture_archive(world, tex_index).samplers.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_texture_get_desc(const bsvx_world* world, size_t tex_index, size_t index, bsvx_texture_desc* out_desc)
    {
        if (!world || !out_desc) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        const auto& textures = texture_archive(world, tex_index).textures;
        if (index >= textures.size()) return BSVX_RESULT_NOT_FOUND;
        const auto& src = textures[index];

        out_desc->texture_id = src.texture_id;
        out_desc->kind = bsvx::to_underlying(src.kind);
        out_desc->vk_format = src.vk_format;
        out_desc->usage_flags = src.usage_flags;
        out_desc->width = src.width;
        out_desc->height = src.height;
        out_desc->depth = src.depth;
        out_desc->array_layers = src.array_layers;
        out_desc->mip_levels = src.mip_levels;
        out_desc->sampler_id = src.sampler_id;
        out_desc->name_hash = src.name_hash;
        out_desc->source_hash = src.source_hash;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_texture_get_subresource_desc(const bsvx_world* world, size_t tex_index, size_t index, bsvx_subresource_desc* out_desc)
    {
        if (!world || !out_desc) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        const auto& subresources = texture_archive(world, tex_index).subresources;
        if (index >= subresources.size()) return BSVX_RESULT_NOT_FOUND;
        const auto& src = subresources[index];

        out_desc->texture_id = src.texture_id;
        out_desc->mip_level = src.mip_level;
        out_desc->layer_or_slice = src.layer_or_slice;
        out_desc->extent_x = src.extent_x;
        out_desc->extent_y = src.extent_y;
        out_desc->extent_z = src.extent_z;
        out_desc->packed_row_length = src.packed_row_length;
        out_desc->packed_image_height = src.packed_image_height;
        out_desc->size = src.blob_size;
        out_desc->checksum = src.checksum;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_texture_get_material(const bsvx_world* world, size_t tex_index, size_t index, bsvx_material_desc* out_desc)
    {
        if (!world || !out_desc) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        const auto& materials = texture_archive(world, tex_index).materials;
        if (index >= materials.size()) return BSVX_RESULT_NOT_FOUND;
        const auto& src = materials[index];

        out_desc->material_id = src.material_id;
        out_desc->flags = src.flags;
        out_desc->albedo_texture_id = src.albedo_texture_id;
        out_desc->normal_texture_id = src.normal_texture_id;
        out_desc->orm_texture_id = src.orm_texture_id;
        out_desc->emissive_texture_id = src.emissive_texture_id;

        out_desc->albedo_layer[0] = src.albedo_layer_px;
        out_desc->albedo_layer[1] = src.albedo_layer_nx;
        out_desc->albedo_layer[2] = src.albedo_layer_py;
        out_desc->albedo_layer[3] = src.albedo_layer_ny;
        out_desc->albedo_layer[4] = src.albedo_layer_pz;
        out_desc->albedo_layer[5] = src.albedo_layer_nz;

        out_desc->normal_layer[0] = src.normal_layer_px;
        out_desc->normal_layer[1] = src.normal_layer_nx;
        out_desc->normal_layer[2] = src.normal_layer_py;
        out_desc->normal_layer[3] = src.normal_layer_ny;
        out_desc->normal_layer[4] = src.normal_layer_pz;
        out_desc->normal_layer[5] = src.normal_layer_nz;

        out_desc->tint_rgba8 = src.tint_rgba8;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_texture_get_sampler(const bsvx_world* world, size_t tex_index, size_t index, bsvx_sampler_desc* out_desc)
    {
        if (!world || !out_desc) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        const auto& samplers = texture_archive(world, tex_index).samplers;
        if (index >= samplers.size()) return BSVX_RESULT_NOT_FOUND;
        const auto& src = samplers[index];

        out_desc->sampler_id = src.sampler_id;
        out_desc->min_filter = bsvx::to_underlying(src.min_filter);
        out_desc->mag_filter = bsvx::to_underlying(src.mag_filter);
        out_desc->mip_filter = bsvx::to_underlying(src.mip_filter);
        out_desc->address_u = bsvx::to_underlying(src.address_u);
        out_desc->address_v = bsvx::to_underlying(src.address_v);
        out_desc->address_w = bsvx::to_underlying(src.address_w);
        out_desc->anisotropy_enable = src.anisotropy_enable;
        out_desc->compare_enable = src.compare_enable;
        out_desc->max_anisotropy_x100 = src.max_anisotropy_x100;
        out_desc->min_lod_x1000 = src.min_lod_x1000;
        out_desc->max_lod_x1000 = src.max_lod_x1000;
        out_desc->mip_lod_bias_x1000 = src.mip_lod_bias_x1000;
        out_desc->compare_op = src.compare_op;
        out_desc->border_color = bsvx::to_underlying(src.border_color);
        out_desc->reserved = 0;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_texture_find_material(const bsvx_world* world, size_t tex_index, uint32_t material_id, size_t* out_index)
    {
        if (!world || !out_index) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        const auto& materials = texture_archive(world, tex_index).materials;
        if (material_id < materials.size() && materials[material_id].material_id == material_id) {
            *out_index = material_id;
            return BSVX_RESULT_OK;
        }

        for (size_t i = 0; i < materials.size(); ++i) {
            if (materials[i].material_id == material_id) {
                *out_index = i;
                return BSVX_RESULT_OK;
            }
        }
        return BSVX_RESULT_NOT_FOUND;
    }

    BSVX_API bsvx_result bsvx_texture_get_subresource_bytes(const bsvx_world* world, size_t tex_index, size_t index, void* out_bytes, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        if (capacity != 0 && !out_bytes) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        const auto& archive = texture_archive(world, tex_index);
        if (index >= archive.subresources.size()) return BSVX_RESULT_NOT_FOUND;

        const auto& sub = archive.subresources[index];
        if (sub.blob_offset > archive.blob.size() || sub.blob_size > archive.blob.size() - sub.blob_offset) {
            if (out_size) *out_size = 0;
            return BSVX_RESULT_RUNTIME_ERROR;
        }

        const std::span<const std::byte> texels(archive.blob.data() + sub.blob_offset, static_cast<size_t>(sub.blob_size));
        return emit_payload(texels, 0u, out_bytes, capacity, out_size, nullptr);
    }

    BSVX_API uint32_t bsvx_format_bytes_per_texel(uint32_t vk_format)
    {
        return bsvx::btx::bytes_per_texel(vk_format);
    }

}

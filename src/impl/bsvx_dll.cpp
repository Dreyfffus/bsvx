#include "bsvx_dll.h"

#include "bsvx.h"

#include <exception>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct bsvx_context {
	std::string last_error;
};

struct bsvx_world {
	bsvx::WorldPackage package;
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

}

extern "C" {

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
    
    BSVX_API void bsvx_world_destroy(bsvx_world* world)
    {
        delete world;
    }
    
    BSVX_API bsvx_result bsvx_world_save(const bsvx_world* world, const char* root_or_manifest_path)
    {
        if (!world || !root_or_manifest_path) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            bsvx::Parser::save_world(world->package, root_or_manifest_path);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return BSVX_RESULT_RUNTIME_ERROR;
        }
    }
    
    BSVX_API bsvx_result bsvx_world_save_region(const bsvx_world* world, const char* region_path)
    {
        if (!world || !region_path) return BSVX_RESULT_INVALID_ARGUMENT;
        try {
            bsvx::Parser::save_region(world->package, region_path);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return BSVX_RESULT_RUNTIME_ERROR;
        }
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

        const auto& geometry = world_desc(world).geometry;
        out_geometry->chunk_size_x = geometry.chunk_size_x;
        out_geometry->chunk_size_y = geometry.chunk_size_y;
        out_geometry->chunk_size_z = geometry.chunk_size_z;
        out_geometry->region_size_x = geometry.region_size_x;
        out_geometry->region_size_y = geometry.region_size_y;
        out_geometry->region_size_z = geometry.region_size_z;
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

        const auto& src = entries[entry_index];
        out_entry->voxel_key = src.voxel_key;
        out_entry->material_id = src.material_id;
        out_entry->flags = src.flags;
        out_entry->name_hash = src.name_hash;
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
        const auto& sum = reg.chunk_summaries.at(map.summary_index);

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
        if (!check_region_index(world, region_index) || !out_voxels) return BSVX_RESULT_INVALID_ARGUMENT;

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
            return BSVX_RESULT_RUNTIME_ERROR;
        }
    }
    
    BSVX_API bsvx_result bsvx_region_set_chunk_u32(bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, const uint32_t* voxels, size_t voxel_count, uint16_t requested_codec)
    {
        if (!check_region_index(world, region_index) || !voxels) return BSVX_RESULT_INVALID_ARGUMENT;
        const size_t required = bsvx_region_required_voxel_count(world, region_index);
        if (voxel_count != required) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            const auto view = std::span<const uint32_t>(voxels, voxel_count);
            region_archive_mut(world, region_index).set_chunk_voxels_dense(
                chunk_x,
                chunk_y,
                chunk_z,
                view,
                &world_desc(world).geometry,
                static_cast<bsvx::VoxelCodec>(requested_codec));
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return BSVX_RESULT_RUNTIME_ERROR;
        }
    }
    
    BSVX_API bsvx_result bsvx_region_set_chunk_payload(bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, const void* payload, size_t payload_size, uint16_t entry_flags)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        if (payload_size != 0 && !payload) return BSVX_RESULT_INVALID_ARGUMENT;

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
            return BSVX_RESULT_RUNTIME_ERROR;
        }
    }

}

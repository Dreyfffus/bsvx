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

#define BSVX_STRINGIFY_(x) #x
#define BSVX_STRINGIFY(x) BSVX_STRINGIFY_(x)
#define BSVX_CPLUSPLUS_VALUE __cplusplus

#if defined(__clang__)
#define BSVX_COMPILER_ID "clang " BSVX_STRINGIFY(__clang_major__) "." BSVX_STRINGIFY(__clang_minor__)
#elif defined(__GNUC__)
#define BSVX_COMPILER_ID "gcc " BSVX_STRINGIFY(__GNUC__) "." BSVX_STRINGIFY(__GNUC_MINOR__)
#elif defined(_MSC_VER)
#define BSVX_COMPILER_ID "msvc " BSVX_STRINGIFY(_MSC_VER)
#else
#define BSVX_COMPILER_ID "unknown compiler"
#endif

struct bsvx_context {
	std::string last_error;
	std::vector<std::string> warnings;
	std::vector<bsvx::ValidationIssue> issues;
	bsvx_progress_fn progress = nullptr;
	void* progress_user = nullptr;

	// Wraps the host's C callback so the C++ side never sees a raw function pointer. Returns an
	// empty function when no callback is installed, which every consumer treats as "no progress".
	bsvx::ProgressFn make_progress() const
	{
		if (!progress) return {};

		bsvx_progress_fn callback = progress;
		void* user = progress_user;
		return [callback, user](std::string_view stage, size_t done, size_t total) {
			const std::string owned(stage);
			return callback(user, owned.c_str(), done, total) != 0;
			};
	}
};

struct bsvx_world {
	bsvx::WorldPackage package;
	// The manifest itself changed (name, registry, units, metadata, paths) as opposed to any one
	// asset. A dirty-only save still has to rewrite everything when this is set, because the
	// manifest hash is stamped into every region.
	bool manifest_dirty = false;

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

struct bsvx_texture_builder {
	bsvx::btx::Archive archive;
};

namespace {
    static void set_error(bsvx_context* ctx, std::string msg) {
        if (ctx) ctx->last_error = std::move(msg);
    }

    static bsvx_result catch_all_to_result(bsvx_context* ctx) noexcept {
        try {
            throw;
        }
        catch (const bsvx::CancelledError& e) {
            set_error(ctx, e.what());
            return BSVX_RESULT_CANCELLED;
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
        if (ctx) {
            ctx->last_error.clear();
            ctx->warnings.clear();
        }
    }

    static void mark_region_dirty(bsvx_world* world, size_t region_index) {
        if (world && region_index < world->package.regions.size()) world->package.regions[region_index].dirty = true;
    }

    static void mark_manifest_dirty(bsvx_world* world) {
        if (world) world->manifest_dirty = true;
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
            bsvx::Parser::save_world(world->package, bsvx::path_from_utf8(root_or_manifest_path));
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
            bsvx::Parser::save_region(world->package, bsvx::path_from_utf8(region_path));
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
            const auto& archive = region_archive(world, region_index);
            const auto& geometry = world_desc(world).geometry;
            const size_t required = bsvx::bvx::Archive::chunk_voxel_count(geometry);

            if (voxel_capacity < required) {
                // Decoded rather than answered from the geometry alone: a chunk that does not exist
                // has to keep surfacing as its own error here, which reporting on size would hide.
                const auto dense = archive.decode_chunk_voxels(chunk_x, chunk_y, chunk_z, &geometry);
                if (out_written) *out_written = dense.size();
                return BSVX_RESULT_BUFFER_TOO_SMALL;
            }

            archive.decode_chunk_voxels_into(chunk_x, chunk_y, chunk_z, std::span<uint32_t>(out_voxels, required), &geometry);
            if (out_written) *out_written = required;
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
            mark_region_dirty(world, region_index);
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
            mark_region_dirty(world, region_index);
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
            world->package = bsvx::Parser::load_region_memory(view, texture_root ? bsvx::path_from_utf8(texture_root) : std::filesystem::path{});
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    // Inbound conversions for the .btx builder. Ids are assigned by the archive on insert, so
    // whatever the caller put in those fields is ignored.

    static bsvx::btx::SamplerDesc to_sampler(const bsvx_sampler_desc& src)
    {
        bsvx::btx::SamplerDesc out{};
        out.min_filter = static_cast<bsvx::SamplerFilter>(src.min_filter);
        out.mag_filter = static_cast<bsvx::SamplerFilter>(src.mag_filter);
        out.mip_filter = static_cast<bsvx::SamplerFilter>(src.mip_filter);
        out.address_u = static_cast<bsvx::SamplerAddressMode>(src.address_u);
        out.address_v = static_cast<bsvx::SamplerAddressMode>(src.address_v);
        out.address_w = static_cast<bsvx::SamplerAddressMode>(src.address_w);
        out.anisotropy_enable = src.anisotropy_enable;
        out.max_anisotropy_x100 = src.max_anisotropy_x100;
        out.compare_enable = src.compare_enable;
        out.compare_op = src.compare_op;
        out.mip_lod_bias_x1000 = src.mip_lod_bias_x1000;
        out.min_lod_x1000 = src.min_lod_x1000;
        out.max_lod_x1000 = src.max_lod_x1000;
        out.border_color = static_cast<bsvx::BorderColor>(src.border_color);
        return out;
    }

    static bsvx::btx::TextureDesc to_texture(const bsvx_texture_desc& src)
    {
        bsvx::btx::TextureDesc out{};
        out.kind = static_cast<bsvx::TextureKind>(src.kind);
        out.vk_format = src.vk_format;
        out.usage_flags = src.usage_flags;
        out.width = src.width;
        out.height = src.height;
        out.depth = src.depth;
        out.array_layers = src.array_layers;
        out.mip_levels = src.mip_levels;
        out.sampler_id = src.sampler_id;
        out.name_hash = src.name_hash;
        out.source_hash = src.source_hash;
        return out;
    }

    static bsvx::btx::MaterialDesc to_material(const bsvx_material_desc& src)
    {
        bsvx::btx::MaterialDesc out{};
        out.flags = src.flags;
        out.albedo_texture_id = src.albedo_texture_id;
        out.normal_texture_id = src.normal_texture_id;
        out.orm_texture_id = src.orm_texture_id;
        out.emissive_texture_id = src.emissive_texture_id;

        out.albedo_layer_px = src.albedo_layer[0];
        out.albedo_layer_nx = src.albedo_layer[1];
        out.albedo_layer_py = src.albedo_layer[2];
        out.albedo_layer_ny = src.albedo_layer[3];
        out.albedo_layer_pz = src.albedo_layer[4];
        out.albedo_layer_nz = src.albedo_layer[5];

        out.normal_layer_px = src.normal_layer[0];
        out.normal_layer_nx = src.normal_layer[1];
        out.normal_layer_py = src.normal_layer[2];
        out.normal_layer_ny = src.normal_layer[3];
        out.normal_layer_pz = src.normal_layer[4];
        out.normal_layer_nz = src.normal_layer[5];

        out.tint_rgba8 = src.tint_rgba8;
        return out;
    }

    static void fill_voxel_address(bsvx_voxel_address* out, const bsvx::VoxelAddress& src)
    {
        out->region_x = src.region[0];
        out->region_y = src.region[1];
        out->region_z = src.region[2];
        out->chunk_x = src.chunk[0];
        out->chunk_y = src.chunk[1];
        out->chunk_z = src.chunk[2];
        out->local_x = src.local[0];
        out->local_y = src.local[1];
        out->local_z = src.local[2];
        out->reserved = 0;
        out->local_index = src.local_index;
    }

    // A scattered set of world-space voxels, grouped by the chunk they land in. Without this every
    // voxel would cost a decode and a re-encode of its whole chunk; with it each touched chunk is
    // decoded once, patched with all of its hits, and encoded once.
    struct VoxelBatch final {
        struct ChunkKey final {
            std::array<int32_t, 3> region{};
            std::array<uint16_t, 3> chunk{};

            bool operator<(const ChunkKey& other) const noexcept
            {
                if (region != other.region) return region < other.region;
                return chunk < other.chunk;
            }
        };

        // local voxel index -> payload (a voxel key when writing, a sample index when reading)
        std::map<ChunkKey, std::vector<std::pair<uint32_t, uint32_t>>> chunks;

        void add(const bsvx::VoxelAddress& address, uint32_t payload)
        {
            ChunkKey key{};
            key.region = address.region;
            key.chunk = address.chunk;
            chunks[key].emplace_back(address.local_index, payload);
        }
    };

    static bsvx_result apply_voxel_batch(bsvx_context* ctx, bsvx_world* world, const VoxelBatch& batch, bool create_missing, size_t* out_count)
    {
        const auto& geometry = world->package.manifest.world_desc.geometry;
        const size_t per_chunk = static_cast<size_t>(geometry.chunk_size_x) * geometry.chunk_size_y * geometry.chunk_size_z;

        size_t written = 0;
        size_t done = 0;

        for (const auto& [target, hits] : batch.chunks) {
            if (ctx && ctx->progress && !ctx->progress(ctx->progress_user, "voxels", done, batch.chunks.size())) throw bsvx::CancelledError{};
            ++done;

            size_t region_index = 0;
            if (bsvx_world_find_region(world, target.region[0], target.region[1], target.region[2], &region_index) != BSVX_RESULT_OK) {
                if (!create_missing) continue;
                if (bsvx_world_add_region(world, target.region[0], target.region[1], target.region[2], &region_index) != BSVX_RESULT_OK) continue;
            }

            auto& archive = region_archive_mut(world, region_index);
            const bool chunk_exists = archive.try_find_chunk_index(target.chunk[0], target.chunk[1], target.chunk[2]).has_value();
            if (!chunk_exists && !create_missing) continue;

            std::vector<uint32_t> dense = chunk_exists
                ? archive.decode_chunk_voxels(target.chunk[0], target.chunk[1], target.chunk[2], &geometry)
                : std::vector<uint32_t>(per_chunk, 0u);

            bool changed = !chunk_exists;
            for (const auto& [local_index, key] : hits) {
                if (local_index >= dense.size()) continue;
                if (dense[local_index] != key) changed = true;
                dense[local_index] = key;
                ++written;
            }
            if (!changed) continue;

            const auto& registry = world->registry_lookup();
            archive.set_chunk_voxels_dense(target.chunk[0], target.chunk[1], target.chunk[2],
                std::span<const uint32_t>(dense.data(), dense.size()), &geometry, bsvx::VoxelCodec::AUTO,
                registry.empty() ? nullptr : &registry);
            mark_region_dirty(world, region_index);
        }

        if (out_count) *out_count = written;
        return BSVX_RESULT_OK;
    }

    static void set_metadata(bsvx::bvx::MetadataMap& map, const char* key, const void* value, size_t size)
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(value);
        map[key] = size == 0 ? std::vector<std::byte>{} : std::vector<std::byte>(bytes, bytes + size);
    }

    static bsvx_result get_metadata(const bsvx::bvx::MetadataMap& map, const char* key, void* out, size_t capacity, size_t* out_size)
    {
        const auto found = map.find(key);
        if (found == map.end()) {
            if (out_size) *out_size = 0;
            return BSVX_RESULT_NOT_FOUND;
        }
        return emit_payload(std::span<const std::byte>(found->second.data(), found->second.size()), 0u, out, capacity, out_size, nullptr);
    }

    static bsvx_result get_metadata_key(const bsvx::bvx::MetadataMap& map, size_t index, char* out, size_t capacity, size_t* out_size)
    {
        if (index >= map.size()) return BSVX_RESULT_NOT_FOUND;
        auto it = map.begin();
        std::advance(it, static_cast<ptrdiff_t>(index));
        return emit_string(it->first, out, capacity, out_size);
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

    // Write-side mirror of VfsFileSystem. save_world does not care where the bytes go, so this is
    // the whole of "saving through a host VFS".
    class VfsFileWriter final : public bsvx::FileWriter {
    public:
        VfsFileWriter(const bsvx_vfs_writer& writer, std::string scheme)
            : writer_(writer), scheme_(std::move(scheme))
        {
            if (!writer_.write_file || !writer_.make_directories) throw std::invalid_argument("[bsvx]: vfs writer: write_file and make_directories are required");
        }

        void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes, bool atomic, bool backup) override
        {
            const std::string full = external(path);
            if (!writer_.write_file(writer_.user, full.c_str(), bytes.data(), bytes.size(), atomic ? 1 : 0, backup ? 1 : 0)) {
                throw std::runtime_error("[bsvx]: vfs writer: could not write " + full);
            }
        }

        void make_directories(const std::filesystem::path& dir) override
        {
            if (dir.empty()) return;
            const std::string full = external(dir);
            if (!writer_.make_directories(writer_.user, full.c_str())) {
                throw std::runtime_error("[bsvx]: vfs writer: could not create " + full);
            }
        }

        bool exists(const std::filesystem::path& path) const override
        {
            if (!writer_.file_exists) return false;
            const std::string full = external(path);
            return writer_.file_exists(writer_.user, full.c_str()) != 0;
        }

        void remove_file(const std::filesystem::path& path) override
        {
            if (!writer_.remove_file) return;
            const std::string full = external(path);
            if (!writer_.remove_file(writer_.user, full.c_str())) {
                throw std::runtime_error("[bsvx]: vfs writer: could not remove " + full);
            }
        }

        std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir, std::string_view extension) const override
        {
            std::vector<std::filesystem::path> out;
            if (!writer_.list_dir) return out;   // no enumeration means no pruning, which is safe

            const std::string full = external(dir);
            for (size_t i = 0; ; ++i) {
                char inline_buffer[256];
                size_t needed = 0;
                if (!writer_.list_dir(writer_.user, full.c_str(), i, inline_buffer, sizeof(inline_buffer), &needed)) break;
                if (needed == 0 || needed > sizeof(inline_buffer)) break;

                std::filesystem::path entry(std::string(inline_buffer, needed - 1u));
                if (entry.extension() == extension) out.push_back(std::move(entry));
            }
            return out;
        }

        std::vector<std::byte> read_file(const std::filesystem::path& path) const override
        {
            // Optional: without it a dirty-only save cannot tell whether the manifest changed, and
            // conservatively rewrites everything.
            if (!writer_.read_file) return {};

            const std::string full = external(path);
            size_t size = 0;
            if (!writer_.read_file(writer_.user, full.c_str(), nullptr, 0, &size)) return {};

            std::vector<std::byte> bytes(size);
            if (size != 0) {
                size_t written = 0;
                if (!writer_.read_file(writer_.user, full.c_str(), bytes.data(), bytes.size(), &written) || written != size) return {};
            }
            return bytes;
        }

    private:
        std::string external(const std::filesystem::path& path) const
        {
            return scheme_ + path.generic_string();
        }

        const bsvx_vfs_writer& writer_;
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
            world->package = bsvx::Parser::load_world(bsvx::path_from_utf8(path));
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
            world->package = bsvx::Parser::load_region(bsvx::path_from_utf8(path));
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
            if (reclaimed != 0) mark_region_dirty(world, region_index);
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
            for (auto& region : world->package.regions) {
                const size_t freed = region.archive.compact();
                if (freed != 0) region.dirty = true;
                reclaimed += freed;
            }
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
            reader->reader = bsvx::bvx::RegionReader::open_file(path);   // UTF-8 bytes; the byte source converts
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
            const size_t required = bsvx_region_reader_required_voxel_count(reader);
            if (voxel_capacity < required) {
                // See impl_decode_chunk: the slow path stays, so a missing chunk still reports as
                // one instead of as a sizing answer.
                const auto dense = reader->reader.decode_chunk_voxels(chunk_x, chunk_y, chunk_z);
                if (out_written) *out_written = dense.size();
                return BSVX_RESULT_BUFFER_TOO_SMALL;
            }

            reader->reader.decode_chunk_voxels_into(chunk_x, chunk_y, chunk_z, std::span<uint32_t>(out_voxels, required));
            if (out_written) *out_written = required;
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
            world->package.regions.back().dirty = true;
            mark_manifest_dirty(world);

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
            mark_manifest_dirty(world);
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
        world->package.manifest.world_desc.registry_names.erase(voxel_key);
        world->package.manifest.world_desc.registry_hash = 0;
        world->invalidate_registry_lookup();
        mark_manifest_dirty(world);
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
        mark_manifest_dirty(world);
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
            mark_manifest_dirty(world);
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
            mark_manifest_dirty(world);
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

    /* ======================================================================================= */
    /* ABI v4                                                                                  */
    /* ======================================================================================= */

    /* --------------------------------------------------------------------------------------- */
    /* ABI self-description                                                                     */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API size_t bsvx_struct_size(uint32_t struct_id)
    {
        switch (static_cast<bsvx_struct_id>(struct_id)) {
        case BSVX_STRUCT_GEOMETRY_DESC:     return sizeof(bsvx_geometry_desc);
        case BSVX_STRUCT_REGISTRY_ENTRY:    return sizeof(bsvx_registry_entry);
        case BSVX_STRUCT_CHUNK_SUMMARY:     return sizeof(bsvx_chunk_summary);
        case BSVX_STRUCT_CHUNK_INFO:        return sizeof(bsvx_chunk_info);
        case BSVX_STRUCT_WORLD_DESC:        return sizeof(bsvx_world_desc);
        case BSVX_STRUCT_TEXTURE_DESC:      return sizeof(bsvx_texture_desc);
        case BSVX_STRUCT_SUBRESOURCE_DESC:  return sizeof(bsvx_subresource_desc);
        case BSVX_STRUCT_MATERIAL_DESC:     return sizeof(bsvx_material_desc);
        case BSVX_STRUCT_SAMPLER_DESC:      return sizeof(bsvx_sampler_desc);
        case BSVX_STRUCT_VFS:               return sizeof(bsvx_vfs);
        case BSVX_STRUCT_UNITS:             return sizeof(bsvx_units);
        case BSVX_STRUCT_VALIDATION_ISSUE:  return sizeof(bsvx_validation_issue);
        case BSVX_STRUCT_SAVE_REPORT:       return sizeof(bsvx_save_report);
        case BSVX_STRUCT_VOXEL_ADDRESS:     return sizeof(bsvx_voxel_address);
        default:                            return 0u;
        }
    }

    BSVX_API const char* bsvx_result_string(uint32_t result)
    {
        switch (static_cast<bsvx_result>(result)) {
        case BSVX_RESULT_OK:                return "ok";
        case BSVX_RESULT_INVALID_ARGUMENT:  return "invalid argument";
        case BSVX_RESULT_NOT_FOUND:         return "not found";
        case BSVX_RESULT_BUFFER_TOO_SMALL:  return "buffer too small";
        case BSVX_RESULT_RUNTIME_ERROR:     return "runtime error";
        case BSVX_RESULT_CANCELLED:         return "cancelled";
        default:                            return "unknown result";
        }
    }

    BSVX_API const char* bsvx_build_info(void)
    {
        return "bsvx abi " BSVX_STRINGIFY(BSVX_ABI_VERSION) "; " BSVX_COMPILER_ID "; c++" BSVX_STRINGIFY(BSVX_CPLUSPLUS_VALUE);
    }

    /* --------------------------------------------------------------------------------------- */
    /* Progress, warnings                                                                       */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API void bsvx_context_set_progress(bsvx_context* ctx, bsvx_progress_fn callback, void* user)
    {
        if (!ctx) return;
        ctx->progress = callback;
        ctx->progress_user = user;
    }

    BSVX_API size_t bsvx_context_warning_count(const bsvx_context* ctx)
    {
        return ctx ? ctx->warnings.size() : 0u;
    }

    BSVX_API const char* bsvx_context_warning(const bsvx_context* ctx, size_t index)
    {
        if (!ctx || index >= ctx->warnings.size()) return "";
        return ctx->warnings[index].c_str();
    }

    /* --------------------------------------------------------------------------------------- */
    /* Loading with flags                                                                       */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_load_ex2(bsvx_context* ctx, const char* path, uint32_t flags, bsvx_world** out_world)
    {
        if (!ctx || !path || !out_world) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_world = nullptr;
        clear_error(ctx);

        try {
            bsvx::LoadOptions options{};
            options.ignore_hash_mismatch = (flags & BSVX_LOAD_IGNORE_HASH_MISMATCH) != 0u;
            options.skip_textures = (flags & BSVX_LOAD_SKIP_TEXTURES) != 0u;
            options.skip_regions = (flags & BSVX_LOAD_SKIP_REGIONS) != 0u;
            options.progress = ctx->make_progress();
            options.warnings = &ctx->warnings;

            auto world = std::make_unique<bsvx_world>();
            world->package = bsvx::Parser::load_world(bsvx::path_from_utf8(path), bsvx::native_filesystem(), options);
            *out_world = world.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_rehash(bsvx_world* world, size_t* out_regions_restamped)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        // The manifest hash is only known once the manifest text is final, which happens at save
        // time. Zeroing the stamps is what actually repairs the world: a zero on either side of the
        // comparison means "unstamped", which load accepts, and the next save writes the real one.
        size_t restamped = 0;
        for (auto& region : world->package.regions) {
            region.archive.manifest_hash = 0;
            region.archive.registry_hash = 0;
            if (region.archive.is_standalone()) {
                region.archive.standalone->manifest_hash = 0;
                region.archive.standalone->registry_hash = 0;
            }
            region.dirty = true;
            ++restamped;
        }
        world->manifest_dirty = true;

        if (out_regions_restamped) *out_regions_restamped = restamped;
        return BSVX_RESULT_OK;
    }

    /* --------------------------------------------------------------------------------------- */
    /* Saving with flags                                                                        */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_save_ex2(bsvx_context* ctx, bsvx_world* world, const char* root_or_manifest_path, uint32_t flags, bsvx_save_report* out_report)
    {
        if (!world || !root_or_manifest_path) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            bsvx::SaveOptions options{};
            options.atomic = (flags & BSVX_SAVE_NON_ATOMIC) == 0u;
            options.backup = (flags & BSVX_SAVE_BACKUP) != 0u;
            options.prune_orphans = (flags & BSVX_SAVE_PRUNE_ORPHANS) != 0u;
            options.dry_run = (flags & BSVX_SAVE_DRY_RUN) != 0u;
            if (ctx) options.progress = ctx->make_progress();

            // A manifest-level edit moves the manifest hash, which is stamped into every region, so
            // an incremental save is not possible -- it widens to everything. The report has to say
            // so, whether the widening was decided here or inside save_world.
            const bool wanted_dirty_only = (flags & BSVX_SAVE_DIRTY_ONLY) != 0u;
            const bool suppressed_here = wanted_dirty_only && world->manifest_dirty;
            options.dirty_only = wanted_dirty_only && !world->manifest_dirty;

            if ((flags & BSVX_SAVE_COMPACT_FIRST) != 0u && !options.dry_run) {
                for (auto& region : world->package.regions) {
                    if (region.archive.compact() != 0) region.dirty = true;
                }
            }

            bsvx::SaveReport report = bsvx::Parser::save_world(world->package, bsvx::path_from_utf8(root_or_manifest_path), options);
            if (suppressed_here) report.full_rewrite = true;

            if (!options.dry_run) {
                for (auto& region : world->package.regions) region.dirty = false;
                for (auto& texture : world->package.textures) texture.dirty = false;
                world->manifest_dirty = false;
            }

            if (out_report) {
                out_report->files_written = report.files_written;
                out_report->files_removed = report.files_removed;
                out_report->files_skipped = report.files_skipped;
                out_report->bytes_written = report.bytes_written;
                out_report->manifest_written = report.manifest_written ? 1 : 0;
                out_report->full_rewrite = report.full_rewrite ? 1 : 0;
            }
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_save_dirty(bsvx_context* ctx, bsvx_world* world, bsvx_save_report* out_report)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        const auto& manifest_path = world->package.manifest.manifest_path;
        if (manifest_path.empty()) {
            set_error(ctx, "[bsvx]: world was never loaded from or saved to a path; use bsvx_world_save_ex2");
            return BSVX_RESULT_NOT_FOUND;
        }

        const std::string path = bsvx::path_to_utf8(manifest_path);
        return bsvx_world_save_ex2(ctx, world, path.c_str(), BSVX_SAVE_DIRTY_ONLY, out_report);
    }

    BSVX_API int bsvx_world_is_dirty(const bsvx_world* world)
    {
        if (!world) return 0;
        if (world->manifest_dirty) return 1;
        for (const auto& region : world->package.regions) if (region.dirty) return 1;
        for (const auto& texture : world->package.textures) if (texture.dirty) return 1;
        return 0;
    }

    BSVX_API int bsvx_region_is_dirty(const bsvx_world* world, size_t region_index)
    {
        if (!check_region_index(world, region_index)) return 0;
        return world->package.regions[region_index].dirty ? 1 : 0;
    }

    BSVX_API bsvx_result bsvx_world_clear_dirty(bsvx_world* world)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        for (auto& region : world->package.regions) region.dirty = false;
        for (auto& texture : world->package.textures) texture.dirty = false;
        world->manifest_dirty = false;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_save_region_index(bsvx_context* ctx, const bsvx_world* world, size_t region_index, const char* path, uint32_t flags)
    {
        if (!check_region_index(world, region_index) || !path) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            // save_region works on a package holding exactly one region, so hand it a view of just
            // the one asked for rather than making the caller split the world up.
            bsvx::WorldPackage single{};
            single.manifest = world->package.manifest;
            single.manifest.regions.clear();
            single.textures = world->package.textures;
            single.regions.push_back(world->package.regions[region_index]);

            bsvx::SaveOptions options{};
            options.atomic = (flags & BSVX_SAVE_NON_ATOMIC) == 0u;
            options.backup = (flags & BSVX_SAVE_BACKUP) != 0u;
            options.dry_run = (flags & BSVX_SAVE_DRY_RUN) != 0u;

            bsvx::Parser::save_region(single, bsvx::path_from_utf8(path), options);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_save_manifest(bsvx_context* ctx, const bsvx_world* world, const char* manifest_path, uint32_t flags)
    {
        if (!world || !manifest_path) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto path = bsvx::path_from_utf8(manifest_path);
            const std::string text = bsvx::Parser::save_manifest_to_string(world->package, path);
            if ((flags & BSVX_SAVE_DRY_RUN) != 0u) return BSVX_RESULT_OK;

            const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
            const auto resolved = path.extension() == ".toml" ? path : (path / "manifest.toml");
            if ((flags & BSVX_SAVE_NON_ATOMIC) != 0u) bsvx::write_file_direct(resolved, bytes);
            else bsvx::write_file_atomic(resolved, bytes, (flags & BSVX_SAVE_BACKUP) != 0u);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_save_manifest_memory(bsvx_context* ctx, const bsvx_world* world, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            // The root the text is normalized against matters -- it decides the relative paths that
            // end up in it -- so use the world's own when it has one.
            const auto& manifest_path = world->package.manifest.manifest_path;
            const auto target = manifest_path.empty() ? std::filesystem::path("manifest.toml") : manifest_path;
            return emit_string(bsvx::Parser::save_manifest_to_string(world->package, target), out, capacity, out_size);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Units                                                                                    */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_get_units(const bsvx_world* world, bsvx_units* out_units)
    {
        if (!world || !out_units) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& u = world_desc(world).units;
        out_units->voxel_size_x = u.voxel_size_x;
        out_units->voxel_size_y = u.voxel_size_y;
        out_units->voxel_size_z = u.voxel_size_z;
        out_units->origin_x = u.origin_x;
        out_units->origin_y = u.origin_y;
        out_units->origin_z = u.origin_z;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_set_units(bsvx_world* world, const bsvx_units* units)
    {
        if (!world || !units) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!(units->voxel_size_x > 0.0) || !(units->voxel_size_y > 0.0) || !(units->voxel_size_z > 0.0)) return BSVX_RESULT_INVALID_ARGUMENT;

        auto& u = world->package.manifest.world_desc.units;
        u.voxel_size_x = units->voxel_size_x;
        u.voxel_size_y = units->voxel_size_y;
        u.voxel_size_z = units->voxel_size_z;
        u.origin_x = units->origin_x;
        u.origin_y = units->origin_y;
        u.origin_z = units->origin_z;
        mark_manifest_dirty(world);
        return BSVX_RESULT_OK;
    }

    /* --------------------------------------------------------------------------------------- */
    /* Registry names                                                                           */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_get_registry_name(const bsvx_world* world, uint32_t voxel_key, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& names = world_desc(world).registry_names;
        const auto found = names.find(voxel_key);
        if (found == names.end()) {
            if (out_size) *out_size = 0;
            return BSVX_RESULT_NOT_FOUND;
        }
        return emit_string(found->second, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_set_registry_name(bsvx_world* world, uint32_t voxel_key, const char* name)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            auto& desc = world->package.manifest.world_desc;
            if (!name || name[0] == '\0') {
                desc.registry_names.erase(voxel_key);
            }
            else {
                desc.registry_names[voxel_key] = name;
                // name_hash on the entry stays the discoverable-by-hash form; keep the two in step
                // so a consumer that only reads the hash sees the same name.
                for (auto& entry : desc.registry_entries) {
                    if (entry.voxel_key == voxel_key) entry.name_hash = bsvx::fnv1a64(std::string_view(name));
                }
            }
            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Asset paths                                                                              */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_get_source_path(const bsvx_world* world, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        return emit_string(bsvx::path_to_utf8(world->package.manifest.manifest_path), out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_get_root_dir(const bsvx_world* world, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        return emit_string(bsvx::path_to_utf8(world->package.manifest.root_dir), out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_get_region_path(const bsvx_world* world, size_t region_index, char* out, size_t capacity, size_t* out_size)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        return emit_string(bsvx::path_to_utf8(world->package.regions[region_index].ref.relative_path), out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_set_region_path(bsvx_world* world, size_t region_index, const char* relative_path)
    {
        if (!check_region_index(world, region_index) || !relative_path) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            auto path = bsvx::path_from_utf8(relative_path);
            if (path.is_absolute()) return BSVX_RESULT_INVALID_ARGUMENT;
            if (!path.has_extension()) path += ".bvx";

            world->package.regions[region_index].ref.relative_path = path.lexically_normal();
            world->package.regions[region_index].ref.absolute_path.clear();
            mark_region_dirty(world, region_index);
            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_set_paths(bsvx_world* world, const char* regions_dir, const char* textures_dir)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            if (regions_dir && regions_dir[0] != '\0') world->package.manifest.regions_dir = bsvx::path_from_utf8(regions_dir);
            if (textures_dir && textures_dir[0] != '\0') world->package.manifest.textures_dir = bsvx::path_from_utf8(textures_dir);
            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* World-space voxel access                                                                 */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_locate_voxel(const bsvx_world* world, int64_t x, int64_t y, int64_t z, bsvx_voxel_address* out_address)
    {
        if (!world || !out_address) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            fill_voxel_address(out_address, bsvx::locate_voxel(world_desc(world).geometry, x, y, z));
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_set_voxels(bsvx_context* ctx, bsvx_world* world, const int64_t* xs, const int64_t* ys, const int64_t* zs,
        const uint32_t* keys, size_t count, int create_missing, size_t* out_count)
    {
        if (!world || (count != 0 && (!xs || !ys || !zs || !keys))) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);
        if (out_count) *out_count = 0;
        if (count == 0) return BSVX_RESULT_OK;

        try {
            VoxelBatch batch;
            for (size_t i = 0; i < count; ++i) {
                batch.add(bsvx::locate_voxel(world_desc(world).geometry, xs[i], ys[i], zs[i]), keys[i]);
            }
            return apply_voxel_batch(ctx, world, batch, create_missing != 0, out_count);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_fill_box(bsvx_context* ctx, bsvx_world* world, int64_t min_x, int64_t min_y, int64_t min_z, int64_t max_x, int64_t max_y,
        int64_t max_z, uint32_t key, int create_missing, size_t* out_count)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        if (min_x > max_x || min_y > max_y || min_z > max_z) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);
        if (out_count) *out_count = 0;

        try {
            const auto& geometry = world_desc(world).geometry;

            VoxelBatch batch;
            for (int64_t z = min_z; z <= max_z; ++z) {
                for (int64_t y = min_y; y <= max_y; ++y) {
                    for (int64_t x = min_x; x <= max_x; ++x) {
                        batch.add(bsvx::locate_voxel(geometry, x, y, z), key);
                    }
                }
            }
            return apply_voxel_batch(ctx, world, batch, create_missing != 0, out_count);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_get_voxels(bsvx_context* ctx, const bsvx_world* world, const int64_t* xs, const int64_t* ys, const int64_t* zs,
        uint32_t* out_keys, size_t count)
    {
        if (!world || (count != 0 && (!xs || !ys || !zs || !out_keys))) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);
        if (count == 0) return BSVX_RESULT_OK;

        try {
            const auto& geometry = world_desc(world).geometry;

            // Same batching as the write path: one decode per touched chunk, however many samples
            // fall inside it.
            VoxelBatch batch;
            for (size_t i = 0; i < count; ++i) {
                batch.add(bsvx::locate_voxel(geometry, xs[i], ys[i], zs[i]), static_cast<uint32_t>(i));
            }

            std::fill_n(out_keys, count, 0u);

            for (const auto& [target, hits] : batch.chunks) {
                size_t region_index = 0;
                if (bsvx_world_find_region(world, target.region[0], target.region[1], target.region[2], &region_index) != BSVX_RESULT_OK) continue;

                const auto& archive = region_archive(world, region_index);
                if (!archive.try_find_chunk_index(target.chunk[0], target.chunk[1], target.chunk[2])) continue;

                const auto dense = archive.decode_chunk_voxels(target.chunk[0], target.chunk[1], target.chunk[2], &geometry);
                for (const auto& [local_index, sample] : hits) {
                    if (local_index < dense.size() && sample < count) out_keys[sample] = dense[local_index];
                }
            }
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Removal                                                                                  */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_remove_region(bsvx_world* world, size_t region_index)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;

        world->package.regions.erase(world->package.regions.begin() + static_cast<ptrdiff_t>(region_index));
        mark_manifest_dirty(world);
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_prune_empty_regions(bsvx_world* world, size_t* out_removed)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        auto& regions = world->package.regions;
        const size_t before = regions.size();
        std::erase_if(regions, [](const bsvx::RegionAsset& asset) {
            if (asset.archive.chunk_map.empty()) return true;
            // A region whose every chunk is empty is just as removable as one with no chunks; a
            // user who erased a structure expects the file to go away.
            for (const auto& summary : asset.archive.chunk_summaries) {
                if (summary.non_air_count != 0) return false;
            }
            return true;
            });

        const size_t removed = before - regions.size();
        if (removed != 0) mark_manifest_dirty(world);
        if (out_removed) *out_removed = removed;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_remove_texture(bsvx_world* world, size_t tex_index)
    {
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_INVALID_ARGUMENT;

        world->package.textures.erase(world->package.textures.begin() + static_cast<ptrdiff_t>(tex_index));
        mark_manifest_dirty(world);
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_region_remove_chunk(bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            if (!region_archive_mut(world, region_index).remove_chunk(chunk_x, chunk_y, chunk_z)) return BSVX_RESULT_NOT_FOUND;
            mark_region_dirty(world, region_index);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_region_clear_chunk(bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            if (!region_archive_mut(world, region_index).clear_chunk(chunk_x, chunk_y, chunk_z)) return BSVX_RESULT_NOT_FOUND;
            mark_region_dirty(world, region_index);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_region_remove_chunk_payload(bsvx_world* world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y,
        uint16_t chunk_z)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            if (!region_archive_mut(world, region_index).remove_chunk_payload(static_cast<bsvx::SectionType>(section_type), chunk_x, chunk_y, chunk_z)) {
                return BSVX_RESULT_NOT_FOUND;
            }
            mark_region_dirty(world, region_index);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Metadata                                                                                 */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_set_metadata(bsvx_world* world, const char* key, const void* value, size_t size)
    {
        if (!world || !key || key[0] == '\0') return BSVX_RESULT_INVALID_ARGUMENT;
        if (size != 0 && !value) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            set_metadata(world->package.manifest.metadata, key, value, size);
            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_get_metadata(const bsvx_world* world, const char* key, void* out, size_t capacity, size_t* out_size)
    {
        if (!world || !key) return BSVX_RESULT_INVALID_ARGUMENT;
        return get_metadata(world->package.manifest.metadata, key, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_world_remove_metadata(bsvx_world* world, const char* key)
    {
        if (!world || !key) return BSVX_RESULT_INVALID_ARGUMENT;
        if (world->package.manifest.metadata.erase(key) == 0) return BSVX_RESULT_NOT_FOUND;
        mark_manifest_dirty(world);
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_world_metadata_count(const bsvx_world* world)
    {
        return world ? world->package.manifest.metadata.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_world_get_metadata_key(const bsvx_world* world, size_t index, char* out, size_t capacity, size_t* out_size)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        return get_metadata_key(world->package.manifest.metadata, index, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_region_set_metadata(bsvx_world* world, size_t region_index, const char* key, const void* value, size_t size)
    {
        if (!check_region_index(world, region_index) || !key || key[0] == '\0') return BSVX_RESULT_INVALID_ARGUMENT;
        if (size != 0 && !value) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            set_metadata(region_archive_mut(world, region_index).metadata, key, value, size);
            mark_region_dirty(world, region_index);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_region_get_metadata(const bsvx_world* world, size_t region_index, const char* key, void* out, size_t capacity, size_t* out_size)
    {
        if (!check_region_index(world, region_index) || !key) return BSVX_RESULT_INVALID_ARGUMENT;
        return get_metadata(region_archive(world, region_index).metadata, key, out, capacity, out_size);
    }

    BSVX_API bsvx_result bsvx_region_remove_metadata(bsvx_world* world, size_t region_index, const char* key)
    {
        if (!check_region_index(world, region_index) || !key) return BSVX_RESULT_INVALID_ARGUMENT;
        if (region_archive_mut(world, region_index).metadata.erase(key) == 0) return BSVX_RESULT_NOT_FOUND;
        mark_region_dirty(world, region_index);
        return BSVX_RESULT_OK;
    }

    BSVX_API size_t bsvx_region_metadata_count(const bsvx_world* world, size_t region_index)
    {
        return check_region_index(world, region_index) ? region_archive(world, region_index).metadata.size() : 0u;
    }

    BSVX_API bsvx_result bsvx_region_get_metadata_key(const bsvx_world* world, size_t region_index, size_t index, char* out, size_t capacity, size_t* out_size)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        return get_metadata_key(region_archive(world, region_index).metadata, index, out, capacity, out_size);
    }

    /* --------------------------------------------------------------------------------------- */
    /* Validation                                                                               */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_validate(bsvx_context* ctx, const bsvx_world* world, uint32_t flags, size_t* out_issue_count)
    {
        if (!ctx || !world) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);
        ctx->issues.clear();
        if (out_issue_count) *out_issue_count = 0;

        try {
            bsvx::ValidateOptions options{};
            options.deep = (flags & BSVX_VALIDATE_DEEP) != 0u;
            options.progress = ctx->make_progress();

            ctx->issues = bsvx::Parser::validate(world->package, options);
            if (out_issue_count) *out_issue_count = ctx->issues.size();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_get_validation_issue(const bsvx_context* ctx, size_t index, bsvx_validation_issue* out_issue)
    {
        if (!ctx || !out_issue) return BSVX_RESULT_INVALID_ARGUMENT;
        if (index >= ctx->issues.size()) return BSVX_RESULT_NOT_FOUND;

        const bsvx::ValidationIssue& src = ctx->issues[index];
        out_issue->severity = bsvx::to_underlying(src.severity);
        out_issue->code = bsvx::to_underlying(src.code);
        out_issue->region_index = src.region_index;
        out_issue->chunk_ordinal = src.chunk_ordinal;
        out_issue->voxel_key = src.voxel_key;
        out_issue->reserved = 0;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_get_validation_message(const bsvx_context* ctx, size_t index, char* out, size_t capacity, size_t* out_size)
    {
        if (!ctx) return BSVX_RESULT_INVALID_ARGUMENT;
        if (index >= ctx->issues.size()) return BSVX_RESULT_NOT_FOUND;
        return emit_string(ctx->issues[index].message, out, capacity, out_size);
    }

    /* --------------------------------------------------------------------------------------- */
    /* Bulk accessors                                                                           */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_region_get_chunk_infos(const bsvx_world* world, size_t region_index, size_t first, size_t count, bsvx_chunk_info* out_array,
        size_t* out_written)
    {
        if (!check_region_index(world, region_index) || (count != 0 && !out_array)) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& reg = region_archive(world, region_index);
        if (out_written) *out_written = 0;
        if (first >= reg.chunk_map.size()) return BSVX_RESULT_NOT_FOUND;

        const size_t available = std::min(count, reg.chunk_map.size() - first);
        for (size_t i = 0; i < available; ++i) {
            const auto& map = reg.chunk_map[first + i];
            if (map.summary_index >= reg.chunk_summaries.size()) return BSVX_RESULT_RUNTIME_ERROR;
            fill_chunk_info(&out_array[i], map, reg.chunk_summaries[map.summary_index]);
        }

        if (out_written) *out_written = available;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_get_registry_entries(const bsvx_world* world, size_t first, size_t count, bsvx_registry_entry* out_array, size_t* out_written)
    {
        if (!world || (count != 0 && !out_array)) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& entries = world_desc(world).registry_entries;
        if (out_written) *out_written = 0;
        if (first >= entries.size()) return BSVX_RESULT_NOT_FOUND;

        const size_t available = std::min(count, entries.size() - first);
        for (size_t i = 0; i < available; ++i) fill_registry_entry(&out_array[i], entries[first + i]);

        if (out_written) *out_written = available;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_region_decode_all_u32(bsvx_context* ctx, const bsvx_world* world, size_t region_index, uint32_t layout, uint32_t* out_voxels,
        size_t voxel_capacity, size_t* out_written)
    {
        if (!check_region_index(world, region_index)) return BSVX_RESULT_INVALID_ARGUMENT;
        if (voxel_capacity != 0 && !out_voxels) return BSVX_RESULT_INVALID_ARGUMENT;
        if (layout != BSVX_LAYOUT_CHUNK_ORDER && layout != BSVX_LAYOUT_REGION_LINEAR) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto& g = world_desc(world).geometry;
            const auto& reg = region_archive(world, region_index);
            const size_t per_chunk = static_cast<size_t>(g.chunk_size_x) * g.chunk_size_y * g.chunk_size_z;

            const size_t required = (layout == BSVX_LAYOUT_CHUNK_ORDER)
                ? per_chunk * reg.chunk_map.size()
                : per_chunk * static_cast<size_t>(g.region_size_x) * g.region_size_y * g.region_size_z;

            if (out_written) *out_written = required;
            if (voxel_capacity < required) return BSVX_RESULT_BUFFER_TOO_SMALL;
            if (required == 0) return BSVX_RESULT_OK;

            const size_t span_x = static_cast<size_t>(g.chunk_size_x) * g.region_size_x;
            const size_t span_y = static_cast<size_t>(g.chunk_size_y) * g.region_size_y;

            // Chunk order puts each chunk in its own contiguous block; region-linear interleaves
            // them. Either way the decode writes the caller's buffer directly -- the chunk's rows
            // just land further apart in the second case.
            //
            // Neither layout pre-zeroes the whole output. Chunk order does not need to: every
            // element belongs to exactly one chunk and each decode defines all of its own. Region
            // linear clears only the slots no chunk occupies, below, which on a densely populated
            // region is nothing at all.
            const bool linear = (layout == BSVX_LAYOUT_REGION_LINEAR);

            // Two ways to make the air in this region read as air, and which is cheaper depends on
            // what the chunks are encoded with.
            //
            // The sparse codecs describe only the voxels that are there, so their destination has
            // to be clear before they write it. Letting each chunk clear its own box means clearing
            // it through that chunk's strided rows -- for a 256^3 region, a million 64-byte fills,
            // several times the cost of one flat pass over the same bytes. So when those codecs
            // cover most of the region, clear the whole output once, sequentially, and tell every
            // decode the destination is already air.
            //
            // When they do not -- a region of dense chunks, which overwrite everything they touch
            // anyway -- that pass is pure waste, and only the slots no chunk occupies need clearing.
            bool prezeroed = false;

            if (linear) {
                size_t sparse_covered = 0;
                for (const auto& map : reg.chunk_map) {
                    uint16_t codec = 0;
                    const auto payload = reg.get_chunk_payload(bsvx::SectionType::VOXELS, map.local_chunk_x, map.local_chunk_y, map.local_chunk_z, &codec);
                    if (!payload || bsvx::bvx::codec_describes_only_occupied(static_cast<bsvx::VoxelCodec>(codec))) sparse_covered += per_chunk;
                }
                prezeroed = (sparse_covered * 2 >= required);
            }

            if (linear && prezeroed) {
                std::fill_n(out_voxels, required, 0u);
            }
            else if (linear) {
                std::vector<bool> occupied(static_cast<size_t>(g.region_size_x) * g.region_size_y * g.region_size_z, false);
                for (const auto& map : reg.chunk_map) {
                    if (map.local_chunk_x >= g.region_size_x || map.local_chunk_y >= g.region_size_y || map.local_chunk_z >= g.region_size_z) continue;
                    occupied[map.local_chunk_x + g.region_size_x * (static_cast<size_t>(map.local_chunk_y) + g.region_size_y * map.local_chunk_z)] = true;
                }

                for (uint16_t cz = 0; cz < g.region_size_z; ++cz) {
                    for (uint16_t cy = 0; cy < g.region_size_y; ++cy) {
                        for (uint16_t cx = 0; cx < g.region_size_x; ++cx) {
                            if (occupied[cx + g.region_size_x * (static_cast<size_t>(cy) + g.region_size_y * cz)]) continue;
                            for (uint16_t z = 0; z < g.chunk_size_z; ++z) {
                                for (uint16_t y = 0; y < g.chunk_size_y; ++y) {
                                    const size_t wx = static_cast<size_t>(cx) * g.chunk_size_x;
                                    const size_t wy = static_cast<size_t>(cy) * g.chunk_size_y + y;
                                    const size_t wz = static_cast<size_t>(cz) * g.chunk_size_z + z;
                                    const size_t dst = wx + span_x * (wy + span_y * wz);
                                    if (dst + g.chunk_size_x > required) continue;
                                    std::fill_n(out_voxels + dst, g.chunk_size_x, 0u);
                                }
                            }
                        }
                    }
                }
            }

            for (size_t i = 0; i < reg.chunk_map.size(); ++i) {
                if (ctx && ctx->progress && !ctx->progress(ctx->progress_user, "chunks", i, reg.chunk_map.size())) throw bsvx::CancelledError{};

                const auto& map = reg.chunk_map[i];

                bsvx::bvx::VoxelDest dest;
                dest.sx = g.chunk_size_x;
                dest.sy = g.chunk_size_y;
                dest.sz = g.chunk_size_z;
                dest.already_air = prezeroed;

                if (!linear) {
                    dest.base = out_voxels + i * per_chunk;
                    dest.row_stride = g.chunk_size_x;
                    dest.plane_stride = static_cast<size_t>(g.chunk_size_x) * g.chunk_size_y;
                }
                else {
                    // A chunk sitting outside the declared region bounds has nowhere to land.
                    // Skipping it leaves its slot air, which the clearing pass above already wrote.
                    if (map.local_chunk_x >= g.region_size_x || map.local_chunk_y >= g.region_size_y || map.local_chunk_z >= g.region_size_z) continue;

                    const size_t wx = static_cast<size_t>(map.local_chunk_x) * g.chunk_size_x;
                    const size_t wy = static_cast<size_t>(map.local_chunk_y) * g.chunk_size_y;
                    const size_t wz = static_cast<size_t>(map.local_chunk_z) * g.chunk_size_z;

                    dest.base = out_voxels + wx + span_x * (wy + span_y * wz);
                    dest.row_stride = span_x;
                    dest.plane_stride = span_x * span_y;
                }

                reg.decode_chunk_voxels_into(map.local_chunk_x, map.local_chunk_y, map.local_chunk_z, dest, &g);
            }
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_region_chunk_content_hash(const bsvx_world* world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z,
        uint64_t* out_hash)
    {
        if (!check_region_index(world, region_index) || !out_hash) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            *out_hash = region_archive(world, region_index).chunk_content_hash(chunk_x, chunk_y, chunk_z, &world_desc(world).geometry);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Writing a .btx                                                                           */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_texture_builder_create(bsvx_context* ctx, bsvx_texture_builder** out_builder)
    {
        if (!out_builder) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_builder = nullptr;
        clear_error(ctx);

        try {
            *out_builder = new bsvx_texture_builder{};
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API void bsvx_texture_builder_destroy(bsvx_texture_builder* builder)
    {
        delete builder;
    }

    BSVX_API bsvx_result bsvx_texture_builder_open(bsvx_context* ctx, const char* path, bsvx_texture_builder** out_builder)
    {
        if (!ctx || !path || !out_builder) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_builder = nullptr;
        clear_error(ctx);

        try {
            auto loaded = bsvx::btx::Archive::load_from_file(path);   // UTF-8; Archive converts on open
            if (!loaded) {
                set_error(ctx, std::string("[btx]: could not open ") + path);
                return BSVX_RESULT_NOT_FOUND;
            }

            auto builder = std::make_unique<bsvx_texture_builder>();
            builder->archive = std::move(*loaded);
            *out_builder = builder.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_from_world(bsvx_context* ctx, const bsvx_world* world, size_t tex_index, bsvx_texture_builder** out_builder)
    {
        if (!world || !out_builder) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_builder = nullptr;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;
        clear_error(ctx);

        try {
            auto builder = std::make_unique<bsvx_texture_builder>();
            builder->archive = texture_archive(world, tex_index);
            *out_builder = builder.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_add_sampler(bsvx_context* ctx, bsvx_texture_builder* builder, const bsvx_sampler_desc* desc, uint32_t* out_id)
    {
        if (!builder || !desc) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const uint32_t id = builder->archive.add_sampler(to_sampler(*desc));
            if (out_id) *out_id = id;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_add_texture(bsvx_context* ctx, bsvx_texture_builder* builder, const bsvx_texture_desc* desc, uint32_t* out_id)
    {
        if (!builder || !desc) return BSVX_RESULT_INVALID_ARGUMENT;
        if (desc->width == 0 || desc->height == 0 || desc->mip_levels == 0 || desc->array_layers == 0) return BSVX_RESULT_INVALID_ARGUMENT;
        // Refuse up front rather than building an archive that serialize() will reject later.
        if (desc->kind != 0u) return BSVX_RESULT_INVALID_ARGUMENT;
        // bytes_per_texel is 0 for block-compressed formats, which are supported -- ask the
        // predicate, not the size.
        if (!bsvx::btx::format_is_supported(desc->vk_format)) {
            set_error(ctx, "[btx]: unsupported vk_format; see bsvx_format_is_supported");
            return BSVX_RESULT_INVALID_ARGUMENT;
        }
        clear_error(ctx);

        try {
            const uint32_t id = builder->archive.add_texture(to_texture(*desc));
            if (out_id) *out_id = id;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_add_material(bsvx_context* ctx, bsvx_texture_builder* builder, const bsvx_material_desc* desc, uint32_t* out_id)
    {
        if (!builder || !desc) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const uint32_t id = builder->archive.add_material(to_material(*desc));
            if (out_id) *out_id = id;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_append_subresource(bsvx_context* ctx, bsvx_texture_builder* builder, uint32_t texture_id, uint16_t mip_level,
        uint16_t layer, uint16_t width, uint16_t height, const void* texels, size_t size, uint16_t packed_row_length, uint16_t packed_image_height,
        uint32_t* out_index)
    {
        if (!builder || (size != 0 && !texels)) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(texels), size);
            const uint32_t index = builder->archive.append_subresource(texture_id, mip_level, layer, width, height, bytes, packed_row_length, packed_image_height);
            if (out_index) *out_index = index;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_generate_mips(bsvx_context* ctx, bsvx_texture_builder* builder, uint32_t texture_id)
    {
        if (!builder) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            builder->archive.generate_mips(texture_id);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_validate(bsvx_context* ctx, const bsvx_texture_builder* builder, char* out_messages, size_t capacity,
        size_t* out_size, size_t* out_issue_count)
    {
        if (!builder) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto issues = builder->archive.validate();
            if (out_issue_count) *out_issue_count = issues.size();

            std::string text;
            for (const auto& issue : issues) {
                text += issue.message;
                text += '\n';
            }
            return emit_string(text, out_messages, capacity, out_size);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_save(bsvx_context* ctx, const bsvx_texture_builder* builder, const char* path, uint32_t flags)
    {
        if (!builder || !path) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto bytes = builder->archive.serialize_to_bytes();
            if ((flags & BSVX_SAVE_DRY_RUN) != 0u) return BSVX_RESULT_OK;

            const auto target = bsvx::path_from_utf8(path);
            const auto view = std::span<const std::byte>(bytes.data(), bytes.size());
            if ((flags & BSVX_SAVE_NON_ATOMIC) != 0u) bsvx::write_file_direct(target, view);
            else bsvx::write_file_atomic(target, view, (flags & BSVX_SAVE_BACKUP) != 0u);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_texture_builder_save_memory(bsvx_context* ctx, const bsvx_texture_builder* builder, void* out_bytes, size_t capacity, size_t* out_size)
    {
        if (!builder) return BSVX_RESULT_INVALID_ARGUMENT;
        if (capacity != 0 && !out_bytes) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto bytes = builder->archive.serialize_to_bytes();
            return emit_payload(std::span<const std::byte>(bytes.data(), bytes.size()), 0u, out_bytes, capacity, out_size, nullptr);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_add_texture(bsvx_context* ctx, bsvx_world* world, const char* texture_id, const char* relative_path,
        const bsvx_texture_builder* builder, size_t* out_tex_index)
    {
        if (!world || !builder) return BSVX_RESULT_INVALID_ARGUMENT;
        if ((!texture_id || texture_id[0] == '\0') && (!relative_path || relative_path[0] == '\0')) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto issues = builder->archive.validate();
            if (!issues.empty()) {
                set_error(ctx, "[btx]: archive is invalid: " + issues.front().message);
                return BSVX_RESULT_INVALID_ARGUMENT;
            }

            const std::string id = (texture_id && texture_id[0] != '\0') ? texture_id : bsvx::path_to_utf8(bsvx::path_from_utf8(relative_path).stem());

            std::filesystem::path path = (relative_path && relative_path[0] != '\0') ? bsvx::path_from_utf8(relative_path) : std::filesystem::path(id);
            if (path.is_absolute()) return BSVX_RESULT_INVALID_ARGUMENT;
            if (!path.has_extension()) path += ".btx";
            path = path.lexically_normal();

            // A region's texture reference holds 127 characters. Rejecting here beats writing a
            // reference that silently resolves to nothing.
            const std::string joined = (world->package.manifest.textures_dir / path).generic_string();
            if (joined.size() > bsvx::bvx::BTX_REF_PATH_CAPACITY) {
                set_error(ctx, "[bsvx]: texture path exceeds " + std::to_string(bsvx::bvx::BTX_REF_PATH_CAPACITY) + " characters: " + joined);
                return BSVX_RESULT_INVALID_ARGUMENT;
            }

            for (const auto& existing : world->package.textures) {
                if (existing.ref.id == id) {
                    set_error(ctx, "[bsvx]: a texture with id '" + id + "' is already in this world");
                    return BSVX_RESULT_INVALID_ARGUMENT;
                }
            }

            bsvx::TextureAsset asset{};
            asset.archive = builder->archive;
            asset.ref.id = id;
            asset.ref.relative_path = path;
            asset.ref.path_hash = bsvx::fnv1a64(std::string_view(joined));
            const auto bytes = asset.archive.serialize_to_bytes();
            asset.ref.content_hash = bsvx::fnv1a64(std::span<const std::byte>(bytes.data(), bytes.size()));
            asset.dirty = true;

            world->package.textures.push_back(std::move(asset));
            mark_manifest_dirty(world);

            if (out_tex_index) *out_tex_index = world->package.textures.size() - 1u;
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Texel formats                                                                            */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API int bsvx_format_is_supported(uint32_t vk_format)
    {
        return bsvx::btx::format_is_supported(vk_format) ? 1 : 0;
    }

    BSVX_API int bsvx_format_is_block_compressed(uint32_t vk_format)
    {
        return bsvx::btx::is_block_compressed(vk_format) ? 1 : 0;
    }

    BSVX_API uint32_t bsvx_format_block_extent(uint32_t vk_format)
    {
        return bsvx::btx::format_block_extent(vk_format);
    }

    BSVX_API uint32_t bsvx_format_block_size(uint32_t vk_format)
    {
        return bsvx::btx::format_block_size(vk_format);
    }

    BSVX_API uint64_t bsvx_format_subresource_size(uint32_t vk_format, uint32_t width, uint32_t height)
    {
        return bsvx::btx::subresource_size(vk_format, width, height);
    }

    /* --------------------------------------------------------------------------------------- */
    /* Axis conventions                                                                         */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API const char* bsvx_axis_convention_name(uint32_t convention)
    {
        try {
            static thread_local std::string name;
            name = bsvx::axis_convention_name(static_cast<bsvx::AxisConvention>(convention));
            return name.c_str();
        }
        catch (...) {
            return "";
        }
    }

    BSVX_API bsvx_result bsvx_convert_position(uint32_t from, uint32_t to, double x, double y, double z, double* out_x, double* out_y, double* out_z)
    {
        if (!out_x || !out_y || !out_z) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            const auto result = bsvx::convert_position(static_cast<bsvx::AxisConvention>(from), static_cast<bsvx::AxisConvention>(to), x, y, z);
            *out_x = result[0];
            *out_y = result[1];
            *out_z = result[2];
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_convert_cell(uint32_t from, uint32_t to, int64_t x, int64_t y, int64_t z, int64_t* out_x, int64_t* out_y, int64_t* out_z)
    {
        if (!out_x || !out_y || !out_z) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            const auto result = bsvx::convert_cell(static_cast<bsvx::AxisConvention>(from), static_cast<bsvx::AxisConvention>(to), x, y, z);
            *out_x = result[0];
            *out_y = result[1];
            *out_z = result[2];
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_convert_axis_convention(bsvx_context* ctx, bsvx_world* world, uint32_t target, size_t* out_voxels_moved)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;
        if (target > bsvx::to_underlying(bsvx::AxisConvention::X_RIGHT_Y_UP_Z_BACK)) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const size_t moved = bsvx::convert_world_axis_convention(world->package, static_cast<bsvx::AxisConvention>(target),
                ctx ? ctx->make_progress() : bsvx::ProgressFn{});
            if (out_voxels_moved) *out_voxels_moved = moved;

            world->invalidate_registry_lookup();
            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Registry colours and palettes                                                            */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_get_registry_color(const bsvx_world* world, uint32_t voxel_key, uint32_t* out_rgba8)
    {
        if (!world || !out_rgba8) return BSVX_RESULT_INVALID_ARGUMENT;

        const auto& colors = world_desc(world).registry_colors;
        const auto found = colors.find(voxel_key);
        if (found == colors.end()) {
            *out_rgba8 = 0u;
            return BSVX_RESULT_NOT_FOUND;
        }

        *out_rgba8 = found->second;
        return BSVX_RESULT_OK;
    }

    BSVX_API bsvx_result bsvx_world_set_registry_color(bsvx_world* world, uint32_t voxel_key, uint32_t rgba8)
    {
        if (!world) return BSVX_RESULT_INVALID_ARGUMENT;

        try {
            auto& colors = world->package.manifest.world_desc.registry_colors;
            if (rgba8 == 0u) colors.erase(voxel_key);
            else colors[voxel_key] = rgba8;

            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

    BSVX_API bsvx_result bsvx_world_make_palette(bsvx_context* ctx, bsvx_world* world, const uint32_t* colors_rgba8, size_t count, const char* texture_id,
        uint32_t flags_for_all, size_t* out_tex_index)
    {
        if (!world || !colors_rgba8 || count == 0) return BSVX_RESULT_INVALID_ARGUMENT;
        // One array layer per colour, and array_layers is uint16_t.
        if (count > 0xFFFFu) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            bsvx::btx::Archive archive{};
            const uint32_t sampler_id = archive.add_sampler(bsvx::btx::SamplerDesc{});

            bsvx::btx::TextureDesc desc{};
            desc.kind = bsvx::TextureKind::TEXTURE_2D;
            desc.vk_format = bsvx::btx::VK_FORMAT_R8G8B8A8_SRGB_;
            desc.width = 1;
            desc.height = 1;
            desc.depth = 1;
            desc.array_layers = static_cast<uint16_t>(count);
            desc.mip_levels = 1;
            desc.sampler_id = static_cast<uint16_t>(sampler_id);
            const uint32_t texture_id_value = archive.add_texture(desc);

            for (size_t i = 0; i < count; ++i) {
                const uint32_t rgba = colors_rgba8[i];
                const std::array<std::byte, 4> texel{
                    static_cast<std::byte>((rgba >> 24) & 0xFFu),
                    static_cast<std::byte>((rgba >> 16) & 0xFFu),
                    static_cast<std::byte>((rgba >> 8) & 0xFFu),
                    static_cast<std::byte>(rgba & 0xFFu),
                };
                (void)archive.append_subresource(texture_id_value, 0, static_cast<uint16_t>(i), 1, 1,
                    std::span<const std::byte>(texel.data(), texel.size()), 0, 0);

                bsvx::btx::MaterialDesc material{};
                material.albedo_texture_id = texture_id_value;
                material.tint_rgba8 = rgba;
                material.albedo_layer_px = material.albedo_layer_nx = material.albedo_layer_py = static_cast<uint16_t>(i);
                material.albedo_layer_ny = material.albedo_layer_pz = material.albedo_layer_nz = static_cast<uint16_t>(i);
                (void)archive.add_material(material);
            }

            // Voxel key 0 is air, so colour i becomes key i+1 and material i.
            for (size_t i = 0; i < count; ++i) {
                bsvx_registry_entry entry{};
                entry.voxel_key = static_cast<uint32_t>(i + 1u);
                entry.material_id = static_cast<uint32_t>(i);
                entry.flags = flags_for_all;
                const bsvx_result rc = bsvx_world_set_registry_entry(world, &entry);
                if (rc != BSVX_RESULT_OK) return rc;

                bsvx_world_set_registry_color(world, entry.voxel_key, colors_rgba8[i]);
            }

            bsvx_texture_builder builder{};
            builder.archive = std::move(archive);
            return bsvx_world_add_texture(ctx, world, texture_id, nullptr, &builder, out_tex_index);
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Cloning                                                                                  */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_clone(bsvx_context* ctx, const bsvx_world* world, bsvx_world** out_clone)
    {
        if (!world || !out_clone) return BSVX_RESULT_INVALID_ARGUMENT;
        *out_clone = nullptr;
        clear_error(ctx);

        try {
            auto clone = std::make_unique<bsvx_world>();
            clone->package = world->package;
            clone->manifest_dirty = world->manifest_dirty;
            // The registry lookup is a cache; the clone rebuilds it on first use.
            *out_clone = clone.release();
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    /* --------------------------------------------------------------------------------------- */
    /* Saving through a host VFS                                                                */
    /* --------------------------------------------------------------------------------------- */

    BSVX_API bsvx_result bsvx_world_save_vfs(bsvx_context* ctx, bsvx_world* world, const char* root_or_manifest_path, const bsvx_vfs_writer* writer,
        uint32_t flags, bsvx_save_report* out_report)
    {
        if (!world || !root_or_manifest_path || !writer) return BSVX_RESULT_INVALID_ARGUMENT;
        if (!writer->write_file || !writer->make_directories) return BSVX_RESULT_INVALID_ARGUMENT;
        clear_error(ctx);

        try {
            const auto [scheme, rest] = split_scheme(root_or_manifest_path);
            VfsFileWriter vfs_writer(*writer, scheme);

            bsvx::SaveOptions options{};
            options.atomic = (flags & BSVX_SAVE_NON_ATOMIC) == 0u;
            options.backup = (flags & BSVX_SAVE_BACKUP) != 0u;
            options.prune_orphans = (flags & BSVX_SAVE_PRUNE_ORPHANS) != 0u;
            options.dry_run = (flags & BSVX_SAVE_DRY_RUN) != 0u;
            options.writer = &vfs_writer;
            if (ctx) options.progress = ctx->make_progress();

            const bool wanted_dirty_only = (flags & BSVX_SAVE_DIRTY_ONLY) != 0u;
            const bool suppressed_here = wanted_dirty_only && world->manifest_dirty;
            options.dirty_only = wanted_dirty_only && !world->manifest_dirty;

            if ((flags & BSVX_SAVE_COMPACT_FIRST) != 0u && !options.dry_run) {
                for (auto& region : world->package.regions) {
                    if (region.archive.compact() != 0) region.dirty = true;
                }
            }

            // A host VFS has its own root, so the paths must stay relative to it rather than being
            // resolved against this process's working directory.
            bsvx::SaveReport report = bsvx::Parser::save_world(world->package, std::filesystem::path(rest), options);
            if (suppressed_here) report.full_rewrite = true;

            if (!options.dry_run) {
                for (auto& region : world->package.regions) region.dirty = false;
                for (auto& texture : world->package.textures) texture.dirty = false;
                world->manifest_dirty = false;
            }

            if (out_report) {
                out_report->files_written = report.files_written;
                out_report->files_removed = report.files_removed;
                out_report->files_skipped = report.files_skipped;
                out_report->bytes_written = report.bytes_written;
                out_report->manifest_written = report.manifest_written ? 1 : 0;
                out_report->full_rewrite = report.full_rewrite ? 1 : 0;
            }
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(ctx);
        }
    }

    BSVX_API bsvx_result bsvx_world_set_texture_id(bsvx_world* world, size_t tex_index, const char* texture_id)
    {
        if (!world || !texture_id || texture_id[0] == '\0') return BSVX_RESULT_INVALID_ARGUMENT;
        if (!check_texture_index(world, tex_index)) return BSVX_RESULT_NOT_FOUND;

        try {
            world->package.textures[tex_index].ref.id = texture_id;
            world->package.textures[tex_index].dirty = true;
            mark_manifest_dirty(world);
            return BSVX_RESULT_OK;
        }
        catch (...) {
            return catch_all_to_result(nullptr);
        }
    }

}

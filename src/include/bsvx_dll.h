#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined (BSVX_ASSETS_BUILD_DLL)
#define BSVX_API __declspec(dllexport)
#elif defined(BSVX_ASSETS_USE_DLL)
#define BSVX_API __declspec(dllimport)
#else
#define BSVX_API
#endif
#else 
#if __GNUC__ >= 4
#define BSVX_API __attribute__((visibility("default")))
#else
#define BSVX_API
#endif
#endif

// Bumped whenever symbols are added, so a host can feature-detect at runtime with
// bsvx_abi_version() instead of dlsym-probing.
//   1 - original 20-symbol surface
//   2 - memory loading, payload read-back, streaming region reader, _ex error reporting, compaction
//   3 - world authoring, world metadata, region serialization to a buffer, manifest loading through
//       a host VFS, .btx introspection
#define BSVX_ABI_VERSION 3

#ifdef __cplusplus
extern "C" {
#endif

	typedef struct bsvx_context bsvx_context;
	typedef struct bsvx_world bsvx_world;
	// Partially-loaded region: header, chunk map, summaries and section tables are resident,
	// payload blobs are fetched per chunk on demand. See the streaming section below.
	typedef struct bsvx_region_reader bsvx_region_reader;

	typedef enum bsvx_result {
		BSVX_RESULT_OK					= 0,
		BSVX_RESULT_INVALID_ARGUMENT	= 1,
		BSVX_RESULT_NOT_FOUND			= 2,
		BSVX_RESULT_BUFFER_TOO_SMALL	= 3,
		BSVX_RESULT_RUNTIME_ERROR		= 4
	} bsvx_result;

	typedef struct bsvx_geometry_desc {
		uint16_t chunk_size_x;
		uint16_t chunk_size_y;
		uint16_t chunk_size_z;
		uint16_t region_size_x;
		uint16_t region_size_y;
		uint16_t region_size_z;
	} bsvx_geometry_desc;

	typedef struct bsvx_registry_entry {
		uint32_t voxel_key;
		uint32_t material_id;
		uint32_t flags;
		uint64_t name_hash;
	} bsvx_registry_entry;

    typedef struct bsvx_chunk_summary {
        uint32_t non_air_count;
        uint32_t opaque_count;
        uint16_t emissive_count;
        uint16_t special_count;

        uint8_t aabb_min_x;
        uint8_t aabb_min_y;
        uint8_t aabb_min_z;
        uint8_t aabb_max_x;
        uint8_t aabb_max_y;
        uint8_t aabb_max_z;

        uint8_t face_state_px;
        uint8_t face_state_nx;
        uint8_t face_state_py;
        uint8_t face_state_ny;
        uint8_t face_state_pz;
        uint8_t face_state_nz;

        uint64_t macro_occ_4x4x4;

        uint32_t top_id_0;
        uint32_t top_id_1;
        uint32_t top_id_2;
        uint32_t top_id_3;

        uint16_t top_count_0;
        uint16_t top_count_1;
        uint16_t top_count_2;
        uint16_t top_count_3;
    } bsvx_chunk_summary;

    typedef struct bsvx_chunk_info {
        uint16_t local_chunk_x;
        uint16_t local_chunk_y;
        uint16_t local_chunk_z;
        uint16_t flags;
        uint32_t summary_index;
        bsvx_chunk_summary summary;
    } bsvx_chunk_info;

    BSVX_API bsvx_context* bsvx_context_create(void);
    BSVX_API void bsvx_context_destroy(bsvx_context* ctx);
    BSVX_API const char* bsvx_context_last_error(const bsvx_context* ctx);

    BSVX_API bsvx_result bsvx_world_load(bsvx_context* ctx, const char* path, bsvx_world** out_world);
    BSVX_API bsvx_result bsvx_world_load_region(bsvx_context* ctx, const char* path, bsvx_world** out_world);
    BSVX_API void bsvx_world_destroy(bsvx_world* world);

    BSVX_API bsvx_result bsvx_world_save(const bsvx_world* world, const char* root_or_manifest_path);
    BSVX_API bsvx_result bsvx_world_save_region(const bsvx_world* world, const char* region_path);

    BSVX_API size_t bsvx_world_region_count(const bsvx_world* world);
    BSVX_API size_t bsvx_world_texture_count(const bsvx_world* world);
    BSVX_API bsvx_result bsvx_world_geometry(const bsvx_world* world, bsvx_geometry_desc* out_geometry);
    BSVX_API size_t bsvx_world_registry_entry_count(const bsvx_world* world);
    BSVX_API bsvx_result bsvx_world_get_registry_entry(const bsvx_world* world, size_t entry_index, bsvx_registry_entry* out_entry);

    BSVX_API bsvx_result bsvx_world_get_region_coord(const bsvx_world* world, size_t region_index, int32_t* out_x, int32_t* out_y, int32_t* out_z);
    BSVX_API size_t bsvx_region_chunk_count(const bsvx_world* world, size_t region_index);
    BSVX_API bsvx_result bsvx_region_get_chunk_info(const bsvx_world* world, size_t region_index, size_t chunk_ordinal, bsvx_chunk_info* out_info);
    BSVX_API size_t bsvx_region_required_voxel_count(const bsvx_world* world, size_t region_index);

    BSVX_API bsvx_result bsvx_region_decode_chunk_u32(
        const bsvx_world* world,
        size_t region_index,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        uint32_t* out_voxels,
        size_t voxel_capacity,
        size_t* out_written
    );

    BSVX_API bsvx_result bsvx_region_set_chunk_u32(
        bsvx_world* world,
        size_t region_index,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        const uint32_t* voxels,
        size_t voxel_count,
        uint16_t requested_codec
    );

    BSVX_API bsvx_result bsvx_region_set_chunk_payload(
        bsvx_world* world,
        size_t region_index,
        uint32_t section_type,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        uint16_t codec,
        const void* payload,
        size_t payload_size,
        uint16_t entry_flags
    );

    /* ---------------------------------------------------------------------------------------- */
    /* ABI version                                                                              */
    /* ---------------------------------------------------------------------------------------- */

    BSVX_API uint32_t bsvx_abi_version(void);

    /* ---------------------------------------------------------------------------------------- */
    /* In-memory loading                                                                        */
    /* ---------------------------------------------------------------------------------------- */

    /* Loads a standalone .bvx straight out of a buffer -- res://, a .pck, an async I/O buffer, a
       network stream. The bytes are copied, so the caller may free them on return. No textures are
       loaded; use the _ex form if the .btx files are reachable on disk. */
    BSVX_API bsvx_result bsvx_world_load_region_memory(
        bsvx_context* ctx,
        const void* bytes,
        size_t size,
        bsvx_world** out_world
    );

    /* texture_root may be NULL or "" to skip texture loading entirely. */
    BSVX_API bsvx_result bsvx_world_load_region_memory_ex(
        bsvx_context* ctx,
        const void* bytes,
        size_t size,
        const char* texture_root,
        bsvx_world** out_world
    );

    /* ---------------------------------------------------------------------------------------- */
    /* Chunk lookup and baked payload read-back                                                  */
    /* ---------------------------------------------------------------------------------------- */

    /* O(1) chunk-coordinate -> chunk ordinal lookup. Returns NOT_FOUND for unauthored chunks. */
    BSVX_API bsvx_result bsvx_region_find_chunk(
        const bsvx_world* world,
        size_t region_index,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        size_t* out_chunk_ordinal
    );

    /* Size/codec of a baked payload without copying it. out_size is 0 when the chunk has no
       payload in that section. */
    BSVX_API bsvx_result bsvx_region_get_chunk_payload_info(
        const bsvx_world* world,
        size_t region_index,
        uint32_t section_type,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        size_t* out_size,
        uint16_t* out_codec,
        uint16_t* out_entry_flags
    );

    /* Mirror of bsvx_region_set_chunk_payload. Follows the decode convention: if capacity is too
       small the required size lands in *out_size and BUFFER_TOO_SMALL is returned without touching
       the output buffer. Returns NOT_FOUND when the chunk has no payload in that section. */
    BSVX_API bsvx_result bsvx_region_get_chunk_payload(
        const bsvx_world* world,
        size_t region_index,
        uint32_t section_type,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        void* out_payload,
        size_t capacity,
        size_t* out_size,
        uint16_t* out_codec
    );

    /* ---------------------------------------------------------------------------------------- */
    /* Compaction                                                                                */
    /* ---------------------------------------------------------------------------------------- */

    /* Every set_chunk_* appends to a section blob and repoints the entry, so an edited region grows
       monotonically. Compaction rewrites the blobs to hold only live, deduplicated ranges. */
    BSVX_API size_t bsvx_region_reclaimable_bytes(const bsvx_world* world, size_t region_index);
    BSVX_API bsvx_result bsvx_region_compact(bsvx_world* world, size_t region_index, size_t* out_reclaimed);
    BSVX_API bsvx_result bsvx_world_compact(bsvx_world* world, size_t* out_reclaimed);

    /* ---------------------------------------------------------------------------------------- */
    /* Error-reporting variants                                                                  */
    /* ---------------------------------------------------------------------------------------- */

    /* Identical to the versions above but they populate ctx's last_error on failure. The context is
       not thread-safe: use one per thread. */
    BSVX_API bsvx_result bsvx_world_save_ex(bsvx_context* ctx, const bsvx_world* world, const char* root_or_manifest_path);
    BSVX_API bsvx_result bsvx_world_save_region_ex(bsvx_context* ctx, const bsvx_world* world, const char* region_path);

    BSVX_API bsvx_result bsvx_region_decode_chunk_u32_ex(
        bsvx_context* ctx,
        const bsvx_world* world,
        size_t region_index,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        uint32_t* out_voxels,
        size_t voxel_capacity,
        size_t* out_written
    );

    BSVX_API bsvx_result bsvx_region_set_chunk_u32_ex(
        bsvx_context* ctx,
        bsvx_world* world,
        size_t region_index,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        const uint32_t* voxels,
        size_t voxel_count,
        uint16_t requested_codec
    );

    BSVX_API bsvx_result bsvx_region_set_chunk_payload_ex(
        bsvx_context* ctx,
        bsvx_world* world,
        size_t region_index,
        uint32_t section_type,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        uint16_t codec,
        const void* payload,
        size_t payload_size,
        uint16_t entry_flags
    );

    BSVX_API bsvx_result bsvx_region_get_chunk_payload_ex(
        bsvx_context* ctx,
        const bsvx_world* world,
        size_t region_index,
        uint32_t section_type,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        void* out_payload,
        size_t capacity,
        size_t* out_size,
        uint16_t* out_codec
    );

    /* ---------------------------------------------------------------------------------------- */
    /* Streaming: partial / lazy region loading                                                  */
    /* ---------------------------------------------------------------------------------------- */

    /* A reader parses only the metadata tier at open (header, chunk map, summaries, section
       directory and entry tables -- tens of KB) and reads a chunk's payload bytes from the backing
       source when you ask for it. That is chunk-granularity streaming rather than
       region-granularity: you can inspect every chunk's summary, decide what to page in, and pay
       only for the chunks you decode.

       Readers are independent of bsvx_world and are read-only. All accessors except _open/_close
       are safe to call concurrently from several threads on the same reader (the file source
       serializes its seeks internally); the bsvx_context passed to the _ex-style calls is not, so
       use one context per thread. */

    BSVX_API bsvx_result bsvx_region_reader_open(bsvx_context* ctx, const char* path, bsvx_region_reader** out_reader);

    /* copy_bytes == 0 borrows the caller's buffer, which must then outlive the reader; anything
       else takes a private copy. */
    BSVX_API bsvx_result bsvx_region_reader_open_memory(
        bsvx_context* ctx,
        const void* bytes,
        size_t size,
        int copy_bytes,
        bsvx_region_reader** out_reader
    );

    BSVX_API void bsvx_region_reader_close(bsvx_region_reader* reader);

    /* Non-standalone regions (those belonging to a manifest world) carry no geometry of their own;
       hand them the world's before decoding. Returns NOT_FOUND from _geometry until then. */
    BSVX_API bsvx_result bsvx_region_reader_set_geometry(bsvx_region_reader* reader, const bsvx_geometry_desc* geometry);
    BSVX_API bsvx_result bsvx_region_reader_geometry(const bsvx_region_reader* reader, bsvx_geometry_desc* out_geometry);
    BSVX_API int bsvx_region_reader_is_standalone(const bsvx_region_reader* reader);
    BSVX_API bsvx_result bsvx_region_reader_coord(const bsvx_region_reader* reader, int32_t* out_x, int32_t* out_y, int32_t* out_z);

    /* Bytes this reader is holding resident -- what a residency manager should budget against. */
    BSVX_API size_t bsvx_region_reader_resident_bytes(const bsvx_region_reader* reader);

    BSVX_API size_t bsvx_region_reader_chunk_count(const bsvx_region_reader* reader);
    BSVX_API size_t bsvx_region_reader_required_voxel_count(const bsvx_region_reader* reader);
    BSVX_API bsvx_result bsvx_region_reader_get_chunk_info(const bsvx_region_reader* reader, size_t chunk_ordinal, bsvx_chunk_info* out_info);
    BSVX_API bsvx_result bsvx_region_reader_find_chunk(
        const bsvx_region_reader* reader,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        size_t* out_chunk_ordinal
    );

    BSVX_API size_t bsvx_region_reader_registry_entry_count(const bsvx_region_reader* reader);
    BSVX_API bsvx_result bsvx_region_reader_get_registry_entry(const bsvx_region_reader* reader, size_t entry_index, bsvx_registry_entry* out_entry);

    /* Reads and decodes exactly one chunk's voxel payload. */
    BSVX_API bsvx_result bsvx_region_reader_decode_chunk_u32(
        bsvx_context* ctx,
        const bsvx_region_reader* reader,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        uint32_t* out_voxels,
        size_t voxel_capacity,
        size_t* out_written
    );

    BSVX_API bsvx_result bsvx_region_reader_get_chunk_payload(
        bsvx_context* ctx,
        const bsvx_region_reader* reader,
        uint32_t section_type,
        uint16_t chunk_x,
        uint16_t chunk_y,
        uint16_t chunk_z,
        void* out_payload,
        size_t capacity,
        size_t* out_size,
        uint16_t* out_codec
    );

    /* open() deliberately does not hash the file. Call this when you want the integrity check. */
    BSVX_API bsvx_result bsvx_region_reader_verify(bsvx_context* ctx, const bsvx_region_reader* reader);

    /* Promotes a partially-loaded region to a fully resident world (reads every blob). */
    BSVX_API bsvx_result bsvx_region_reader_load_full(bsvx_context* ctx, const bsvx_region_reader* reader, bsvx_world** out_world);

    /* ---------------------------------------------------------------------------------------- */
    /* Authoring a world from scratch                                                            */
    /* ---------------------------------------------------------------------------------------- */

    /* An empty world: geometry only, no regions, no textures, no registry. Nothing is written
       anywhere until bsvx_world_save. Chunk dimensions must be in [1, 255] (a ChunkSummary AABB is
       stored as uint8_t); region dimensions must be non-zero. */
    BSVX_API bsvx_result bsvx_world_create(bsvx_context* ctx, const bsvx_geometry_desc* geometry, bsvx_world** out_world);

    /* Adds an empty region at a region coordinate. Returns INVALID_ARGUMENT if one already sits
       there; use bsvx_world_find_region to check first. Chunks come into existence on the first
       bsvx_region_set_chunk_u32 / _set_chunk_payload. */
    BSVX_API bsvx_result bsvx_world_add_region(bsvx_world* world, int32_t region_x, int32_t region_y, int32_t region_z, size_t* out_region_index);
    BSVX_API bsvx_result bsvx_world_find_region(const bsvx_world* world, int32_t region_x, int32_t region_y, int32_t region_z, size_t* out_region_index);

    /* Inserts or replaces the registry entry for entry->voxel_key. Voxel key 0 is air by convention
       and cannot be registered. Rewrites the world's registry hash, so every region saved afterwards
       is stamped with the new one. */
    BSVX_API bsvx_result bsvx_world_set_registry_entry(bsvx_world* world, const bsvx_registry_entry* entry);
    BSVX_API bsvx_result bsvx_world_remove_registry_entry(bsvx_world* world, uint32_t voxel_key);

    /* ---------------------------------------------------------------------------------------- */
    /* World metadata                                                                            */
    /* ---------------------------------------------------------------------------------------- */

    typedef struct bsvx_world_desc {
        bsvx_geometry_desc geometry;

        uint16_t voxel_schema;      /* 1 = dense u32 voxel key */
        uint16_t axis_convention;   /* 0 = x_right_y_up_z_forward */
        uint16_t bounds_mode;       /* 0 = unbounded, 1 = explicit */
        uint16_t reserved;

        int32_t min_region_x;
        int32_t min_region_y;
        int32_t min_region_z;
        int32_t max_region_x;
        int32_t max_region_y;
        int32_t max_region_z;

        uint64_t asset_name_hash;
        uint64_t registry_hash;
        uint64_t manifest_hash;
    } bsvx_world_desc;

    BSVX_API bsvx_result bsvx_world_get_desc(const bsvx_world* world, bsvx_world_desc* out_desc);

    /* Geometry, bounds and schema; the three hashes are recomputed on save and are ignored here.
       Changing the geometry is refused (INVALID_ARGUMENT) once any region holds a chunk -- the
       voxel payloads were encoded against the old chunk dimensions. */
    BSVX_API bsvx_result bsvx_world_set_desc(bsvx_world* world, const bsvx_world_desc* desc);

    /* String getters share the payload convention: the required size *including* the terminating
       NUL lands in *out_size, BUFFER_TOO_SMALL means nothing was written, and (NULL, 0) is a legal
       size probe. */
    BSVX_API bsvx_result bsvx_world_get_name(const bsvx_world* world, char* out, size_t capacity, size_t* out_size);
    BSVX_API bsvx_result bsvx_world_get_uuid(const bsvx_world* world, char* out, size_t capacity, size_t* out_size);
    BSVX_API bsvx_result bsvx_world_set_name(bsvx_world* world, const char* name);
    BSVX_API bsvx_result bsvx_world_set_uuid(bsvx_world* world, const char* uuid);

    /* ---------------------------------------------------------------------------------------- */
    /* Serializing a region to a buffer                                                          */
    /* ---------------------------------------------------------------------------------------- */

    /* The bytes bsvx_world_save_region would have written, for a package holding exactly one
       region. Same convention as the payload getters, so (NULL, 0) sizes the buffer. The package's
       .btx files are not included -- a standalone .bvx only references them. */
    BSVX_API bsvx_result bsvx_world_save_region_memory(
        bsvx_context* ctx,
        const bsvx_world* world,
        void* out_bytes,
        size_t capacity,
        size_t* out_size
    );

    /* ---------------------------------------------------------------------------------------- */
    /* Loading a manifest world through a host VFS                                               */
    /* ---------------------------------------------------------------------------------------- */

    /* bsvx_world_load needs a real path because it opens files itself. Behind Godot's res:// (or any
       pack file) there is no such path, so hand the library the host's reader instead.

       Paths given to these callbacks are built from the path passed to bsvx_world_load_vfs, keep its
       scheme prefix ("res://", "user://", ...) and always use '/' separators. */
    typedef struct bsvx_vfs {
        void* user;

        /* Reads a whole file. Return 1 on success, 0 if it cannot be read. Always set *out_size to
           the file's size; when capacity is smaller than that, write nothing and still return 1 --
           the library calls back with a large enough buffer. It probes sizes with (NULL, 0). */
        int (*read_file)(void* user, const char* path, void* out, size_t capacity, size_t* out_size);

        /* 1 if the path names a readable file. May be NULL, in which case read_file is used as the
           existence probe. */
        int (*file_exists)(void* user, const char* path);

        /* Directory enumeration, used only when the manifest has no explicit [[regions]] /
           [[textures]] table. Write entry `index`'s file name (not a path) and return 1; return 0
           once index is past the end or the directory does not exist. May be NULL -- then a
           manifest that relies on auto-discovery loads no regions or textures. */
        int (*list_dir)(void* user, const char* dir_path, size_t index, char* out_name, size_t capacity, size_t* out_size);
    } bsvx_vfs;

    /* path is a manifest.toml, a directory holding one, or a standalone .bvx -- same three forms
       bsvx_world_load accepts, only read through vfs. */
    BSVX_API bsvx_result bsvx_world_load_vfs(bsvx_context* ctx, const char* path, const bsvx_vfs* vfs, bsvx_world** out_world);

    /* ---------------------------------------------------------------------------------------- */
    /* Texture (.btx) introspection                                                              */
    /* ---------------------------------------------------------------------------------------- */

    /* A world holds bsvx_world_texture_count() .btx archives; each archive holds its own tables of
       textures, subresources, materials and samplers. tex_index selects the archive, the trailing
       index selects a row inside it. Nothing here allocates: bytes are copied into your buffer. */

    typedef struct bsvx_texture_desc {
        uint32_t texture_id;
        uint32_t kind;            /* 0 = 2D, 1 = 3D, 2 = texel buffer (only 2D is implemented) */
        uint32_t vk_format;       /* raw Vulkan format enum; 37 = R8G8B8A8_UNORM, 43 = _SRGB */
        uint32_t usage_flags;     /* raw Vulkan usage flags */

        uint16_t width;
        uint16_t height;
        uint16_t depth;
        uint16_t array_layers;
        uint16_t mip_levels;
        uint16_t sampler_id;

        uint64_t name_hash;
        uint64_t source_hash;
    } bsvx_texture_desc;

    typedef struct bsvx_subresource_desc {
        uint32_t texture_id;
        uint16_t mip_level;
        uint16_t layer_or_slice;

        uint16_t extent_x;
        uint16_t extent_y;
        uint16_t extent_z;

        /* Map straight onto Vulkan's bufferRowLength / bufferImageHeight; 0 means tightly packed. */
        uint16_t packed_row_length;
        uint16_t packed_image_height;

        uint64_t size;      /* bytes of texel data */
        uint64_t checksum;  /* FNV-1a-64 over those bytes */
    } bsvx_subresource_desc;

    typedef struct bsvx_material_desc {
        uint32_t material_id;
        uint32_t flags;

        uint32_t albedo_texture_id;
        uint32_t normal_texture_id;
        uint32_t orm_texture_id;
        uint32_t emissive_texture_id;

        /* Array layer per face, in +x -x +y -y +z -z order. */
        uint16_t albedo_layer[6];
        uint16_t normal_layer[6];

        uint32_t tint_rgba8;
    } bsvx_material_desc;

    typedef struct bsvx_sampler_desc {
        uint32_t sampler_id;

        uint8_t min_filter;      /* 0 = nearest, 1 = linear */
        uint8_t mag_filter;
        uint8_t mip_filter;
        uint8_t address_u;       /* 0 = repeat, 1 = mirror, 2 = clamp to edge, 3 = clamp to border */
        uint8_t address_v;
        uint8_t address_w;
        uint8_t anisotropy_enable;
        uint8_t compare_enable;

        uint16_t max_anisotropy_x100;
        uint16_t min_lod_x1000;
        uint16_t max_lod_x1000;
        int16_t  mip_lod_bias_x1000;

        uint8_t compare_op;      /* raw Vulkan compare op */
        uint8_t border_color;
        uint16_t reserved;
    } bsvx_sampler_desc;

    /* id and relative path of a .btx as the manifest names it; string convention as above. */
    BSVX_API bsvx_result bsvx_world_get_texture_id(const bsvx_world* world, size_t tex_index, char* out, size_t capacity, size_t* out_size);
    BSVX_API bsvx_result bsvx_world_get_texture_path(const bsvx_world* world, size_t tex_index, char* out, size_t capacity, size_t* out_size);

    BSVX_API size_t bsvx_texture_texture_count(const bsvx_world* world, size_t tex_index);
    BSVX_API size_t bsvx_texture_subresource_count(const bsvx_world* world, size_t tex_index);
    BSVX_API size_t bsvx_texture_material_count(const bsvx_world* world, size_t tex_index);
    BSVX_API size_t bsvx_texture_sampler_count(const bsvx_world* world, size_t tex_index);

    BSVX_API bsvx_result bsvx_texture_get_desc(const bsvx_world* world, size_t tex_index, size_t index, bsvx_texture_desc* out_desc);
    BSVX_API bsvx_result bsvx_texture_get_subresource_desc(const bsvx_world* world, size_t tex_index, size_t index, bsvx_subresource_desc* out_desc);
    BSVX_API bsvx_result bsvx_texture_get_material(const bsvx_world* world, size_t tex_index, size_t index, bsvx_material_desc* out_desc);
    BSVX_API bsvx_result bsvx_texture_get_sampler(const bsvx_world* world, size_t tex_index, size_t index, bsvx_sampler_desc* out_desc);

    /* Row of the material table carrying material_id -- what a registry entry's material_id refers
       to. Materials are usually stored in id order, so this is a checked shortcut, not a search. */
    BSVX_API bsvx_result bsvx_texture_find_material(const bsvx_world* world, size_t tex_index, uint32_t material_id, size_t* out_index);

    /* The texels of one subresource. Same buffer convention as the payload getters. */
    BSVX_API bsvx_result bsvx_texture_get_subresource_bytes(
        const bsvx_world* world,
        size_t tex_index,
        size_t index,
        void* out_bytes,
        size_t capacity,
        size_t* out_size
    );

    /* 0 for formats this build cannot describe -- everything except R8G8B8A8_UNORM/SRGB today. */
    BSVX_API uint32_t bsvx_format_bytes_per_texel(uint32_t vk_format);


#ifdef __cplusplus
}
#endif
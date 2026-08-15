#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(BSVX_ASSETS_BUILD_DLL)
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
//   4 - authoring surface: ABI self-description, .btx writing, round-trippable registry names,
//       units, world-space sparse voxel I/O, removal, free-form metadata, incremental and atomic
//       saves, load flags, validation, progress/cancel, bulk accessors, asset paths
#define BSVX_ABI_VERSION 4

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bsvx_context bsvx_context;
typedef struct bsvx_world bsvx_world;
// Partially-loaded region: header, chunk map, summaries and section tables are resident,
// payload blobs are fetched per chunk on demand. See the streaming section below.
typedef struct bsvx_region_reader bsvx_region_reader;

typedef enum bsvx_result {
  BSVX_RESULT_OK = 0,
  BSVX_RESULT_INVALID_ARGUMENT = 1,
  BSVX_RESULT_NOT_FOUND = 2,
  BSVX_RESULT_BUFFER_TOO_SMALL = 3,
  BSVX_RESULT_RUNTIME_ERROR = 4,
  /* A progress callback asked to stop. An atomic save leaves the destination untouched. */
  BSVX_RESULT_CANCELLED = 5
} bsvx_result;

/* Every path crossing this boundary is UTF-8 on every platform, including Windows. */

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

BSVX_API bsvx_context *bsvx_context_create(void);
BSVX_API void bsvx_context_destroy(bsvx_context *ctx);
BSVX_API const char *bsvx_context_last_error(const bsvx_context *ctx);

BSVX_API bsvx_result bsvx_world_load(bsvx_context *ctx, const char *path, bsvx_world **out_world);
BSVX_API bsvx_result bsvx_world_load_region(bsvx_context *ctx, const char *path, bsvx_world **out_world);
BSVX_API void bsvx_world_destroy(bsvx_world *world);

BSVX_API bsvx_result bsvx_world_save(const bsvx_world *world, const char *root_or_manifest_path);
BSVX_API bsvx_result bsvx_world_save_region(const bsvx_world *world, const char *region_path);

BSVX_API size_t bsvx_world_region_count(const bsvx_world *world);
BSVX_API size_t bsvx_world_texture_count(const bsvx_world *world);
BSVX_API bsvx_result bsvx_world_geometry(const bsvx_world *world, bsvx_geometry_desc *out_geometry);
BSVX_API size_t bsvx_world_registry_entry_count(const bsvx_world *world);
BSVX_API bsvx_result bsvx_world_get_registry_entry(const bsvx_world *world, size_t entry_index, bsvx_registry_entry *out_entry);

BSVX_API bsvx_result bsvx_world_get_region_coord(const bsvx_world *world, size_t region_index, int32_t *out_x, int32_t *out_y, int32_t *out_z);
BSVX_API size_t bsvx_region_chunk_count(const bsvx_world *world, size_t region_index);
BSVX_API bsvx_result bsvx_region_get_chunk_info(const bsvx_world *world, size_t region_index, size_t chunk_ordinal, bsvx_chunk_info *out_info);
BSVX_API size_t bsvx_region_required_voxel_count(const bsvx_world *world, size_t region_index);

BSVX_API bsvx_result bsvx_region_decode_chunk_u32(const bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z,
                                                  uint32_t *out_voxels, size_t voxel_capacity, size_t *out_written);

BSVX_API bsvx_result bsvx_region_set_chunk_u32(bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z,
                                               const uint32_t *voxels, size_t voxel_count, uint16_t requested_codec);

BSVX_API bsvx_result bsvx_region_set_chunk_payload(bsvx_world *world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y,
                                                   uint16_t chunk_z, uint16_t codec, const void *payload, size_t payload_size, uint16_t entry_flags);

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
BSVX_API bsvx_result bsvx_world_load_region_memory(bsvx_context *ctx, const void *bytes, size_t size, bsvx_world **out_world);

/* texture_root may be NULL or "" to skip texture loading entirely. */
BSVX_API bsvx_result bsvx_world_load_region_memory_ex(bsvx_context *ctx, const void *bytes, size_t size, const char *texture_root, bsvx_world **out_world);

/* ---------------------------------------------------------------------------------------- */
/* Chunk lookup and baked payload read-back                                                  */
/* ---------------------------------------------------------------------------------------- */

/* O(1) chunk-coordinate -> chunk ordinal lookup. Returns NOT_FOUND for unauthored chunks. */
BSVX_API bsvx_result bsvx_region_find_chunk(const bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z,
                                            size_t *out_chunk_ordinal);

/* Size/codec of a baked payload without copying it. out_size is 0 when the chunk has no
   payload in that section. */
BSVX_API bsvx_result bsvx_region_get_chunk_payload_info(const bsvx_world *world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y,
                                                        uint16_t chunk_z, size_t *out_size, uint16_t *out_codec, uint16_t *out_entry_flags);

/* Mirror of bsvx_region_set_chunk_payload. Follows the decode convention: if capacity is too
   small the required size lands in *out_size and BUFFER_TOO_SMALL is returned without touching
   the output buffer. Returns NOT_FOUND when the chunk has no payload in that section. */
BSVX_API bsvx_result bsvx_region_get_chunk_payload(const bsvx_world *world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y,
                                                   uint16_t chunk_z, void *out_payload, size_t capacity, size_t *out_size, uint16_t *out_codec);

/* ---------------------------------------------------------------------------------------- */
/* Compaction                                                                                */
/* ---------------------------------------------------------------------------------------- */

/* Every set_chunk_* appends to a section blob and repoints the entry, so an edited region grows
   monotonically. Compaction rewrites the blobs to hold only live, deduplicated ranges. */
BSVX_API size_t bsvx_region_reclaimable_bytes(const bsvx_world *world, size_t region_index);
BSVX_API bsvx_result bsvx_region_compact(bsvx_world *world, size_t region_index, size_t *out_reclaimed);
BSVX_API bsvx_result bsvx_world_compact(bsvx_world *world, size_t *out_reclaimed);

/* ---------------------------------------------------------------------------------------- */
/* Error-reporting variants                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* Identical to the versions above but they populate ctx's last_error on failure. The context is
   not thread-safe: use one per thread. */
BSVX_API bsvx_result bsvx_world_save_ex(bsvx_context *ctx, const bsvx_world *world, const char *root_or_manifest_path);
BSVX_API bsvx_result bsvx_world_save_region_ex(bsvx_context *ctx, const bsvx_world *world, const char *region_path);

BSVX_API bsvx_result bsvx_region_decode_chunk_u32_ex(bsvx_context *ctx, const bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y,
                                                     uint16_t chunk_z, uint32_t *out_voxels, size_t voxel_capacity, size_t *out_written);

BSVX_API bsvx_result bsvx_region_set_chunk_u32_ex(bsvx_context *ctx, bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y,
                                                  uint16_t chunk_z, const uint32_t *voxels, size_t voxel_count, uint16_t requested_codec);

BSVX_API bsvx_result bsvx_region_set_chunk_payload_ex(bsvx_context *ctx, bsvx_world *world, size_t region_index, uint32_t section_type, uint16_t chunk_x,
                                                      uint16_t chunk_y, uint16_t chunk_z, uint16_t codec, const void *payload, size_t payload_size,
                                                      uint16_t entry_flags);

BSVX_API bsvx_result bsvx_region_get_chunk_payload_ex(bsvx_context *ctx, const bsvx_world *world, size_t region_index, uint32_t section_type, uint16_t chunk_x,
                                                      uint16_t chunk_y, uint16_t chunk_z, void *out_payload, size_t capacity, size_t *out_size,
                                                      uint16_t *out_codec);

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

BSVX_API bsvx_result bsvx_region_reader_open(bsvx_context *ctx, const char *path, bsvx_region_reader **out_reader);

/* copy_bytes == 0 borrows the caller's buffer, which must then outlive the reader; anything
   else takes a private copy. */
BSVX_API bsvx_result bsvx_region_reader_open_memory(bsvx_context *ctx, const void *bytes, size_t size, int copy_bytes, bsvx_region_reader **out_reader);

BSVX_API void bsvx_region_reader_close(bsvx_region_reader *reader);

/* Non-standalone regions (those belonging to a manifest world) carry no geometry of their own;
   hand them the world's before decoding. Returns NOT_FOUND from _geometry until then. */
BSVX_API bsvx_result bsvx_region_reader_set_geometry(bsvx_region_reader *reader, const bsvx_geometry_desc *geometry);
BSVX_API bsvx_result bsvx_region_reader_geometry(const bsvx_region_reader *reader, bsvx_geometry_desc *out_geometry);
BSVX_API int bsvx_region_reader_is_standalone(const bsvx_region_reader *reader);
BSVX_API bsvx_result bsvx_region_reader_coord(const bsvx_region_reader *reader, int32_t *out_x, int32_t *out_y, int32_t *out_z);

/* Bytes this reader is holding resident -- what a residency manager should budget against. */
BSVX_API size_t bsvx_region_reader_resident_bytes(const bsvx_region_reader *reader);

BSVX_API size_t bsvx_region_reader_chunk_count(const bsvx_region_reader *reader);
BSVX_API size_t bsvx_region_reader_required_voxel_count(const bsvx_region_reader *reader);
BSVX_API bsvx_result bsvx_region_reader_get_chunk_info(const bsvx_region_reader *reader, size_t chunk_ordinal, bsvx_chunk_info *out_info);
BSVX_API bsvx_result bsvx_region_reader_find_chunk(const bsvx_region_reader *reader, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z,
                                                   size_t *out_chunk_ordinal);

BSVX_API size_t bsvx_region_reader_registry_entry_count(const bsvx_region_reader *reader);
BSVX_API bsvx_result bsvx_region_reader_get_registry_entry(const bsvx_region_reader *reader, size_t entry_index, bsvx_registry_entry *out_entry);

/* Reads and decodes exactly one chunk's voxel payload. */
BSVX_API bsvx_result bsvx_region_reader_decode_chunk_u32(bsvx_context *ctx, const bsvx_region_reader *reader, uint16_t chunk_x, uint16_t chunk_y,
                                                         uint16_t chunk_z, uint32_t *out_voxels, size_t voxel_capacity, size_t *out_written);

BSVX_API bsvx_result bsvx_region_reader_get_chunk_payload(bsvx_context *ctx, const bsvx_region_reader *reader, uint32_t section_type, uint16_t chunk_x,
                                                          uint16_t chunk_y, uint16_t chunk_z, void *out_payload, size_t capacity, size_t *out_size,
                                                          uint16_t *out_codec);

/* open() deliberately does not hash the file. Call this when you want the integrity check. */
BSVX_API bsvx_result bsvx_region_reader_verify(bsvx_context *ctx, const bsvx_region_reader *reader);

/* Promotes a partially-loaded region to a fully resident world (reads every blob). */
BSVX_API bsvx_result bsvx_region_reader_load_full(bsvx_context *ctx, const bsvx_region_reader *reader, bsvx_world **out_world);

/* ---------------------------------------------------------------------------------------- */
/* Authoring a world from scratch                                                            */
/* ---------------------------------------------------------------------------------------- */

/* An empty world: geometry only, no regions, no textures, no registry. Nothing is written
   anywhere until bsvx_world_save. Chunk dimensions must be in [1, 255] (a ChunkSummary AABB is
   stored as uint8_t); region dimensions must be non-zero. */
BSVX_API bsvx_result bsvx_world_create(bsvx_context *ctx, const bsvx_geometry_desc *geometry, bsvx_world **out_world);

/* Adds an empty region at a region coordinate. Returns INVALID_ARGUMENT if one already sits
   there; use bsvx_world_find_region to check first. Chunks come into existence on the first
   bsvx_region_set_chunk_u32 / _set_chunk_payload. */
BSVX_API bsvx_result bsvx_world_add_region(bsvx_world *world, int32_t region_x, int32_t region_y, int32_t region_z, size_t *out_region_index);
BSVX_API bsvx_result bsvx_world_find_region(const bsvx_world *world, int32_t region_x, int32_t region_y, int32_t region_z, size_t *out_region_index);

/* Inserts or replaces the registry entry for entry->voxel_key. Voxel key 0 is air by convention
   and cannot be registered. Rewrites the world's registry hash, so every region saved afterwards
   is stamped with the new one. */
BSVX_API bsvx_result bsvx_world_set_registry_entry(bsvx_world *world, const bsvx_registry_entry *entry);
BSVX_API bsvx_result bsvx_world_remove_registry_entry(bsvx_world *world, uint32_t voxel_key);

/* ---------------------------------------------------------------------------------------- */
/* World metadata                                                                            */
/* ---------------------------------------------------------------------------------------- */

typedef struct bsvx_world_desc {
  bsvx_geometry_desc geometry;

  uint16_t voxel_schema;    /* 1 = dense u32 voxel key */
  uint16_t axis_convention; /* 0 = x_right_y_up_z_forward */
  uint16_t bounds_mode;     /* 0 = unbounded, 1 = explicit */
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

BSVX_API bsvx_result bsvx_world_get_desc(const bsvx_world *world, bsvx_world_desc *out_desc);

/* Geometry, bounds and schema; the three hashes are recomputed on save and are ignored here.
   Changing the geometry is refused (INVALID_ARGUMENT) once any region holds a chunk -- the
   voxel payloads were encoded against the old chunk dimensions. */
BSVX_API bsvx_result bsvx_world_set_desc(bsvx_world *world, const bsvx_world_desc *desc);

/* String getters share the payload convention: the required size *including* the terminating
   NUL lands in *out_size, BUFFER_TOO_SMALL means nothing was written, and (NULL, 0) is a legal
   size probe. */
BSVX_API bsvx_result bsvx_world_get_name(const bsvx_world *world, char *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_get_uuid(const bsvx_world *world, char *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_set_name(bsvx_world *world, const char *name);
BSVX_API bsvx_result bsvx_world_set_uuid(bsvx_world *world, const char *uuid);

/* ---------------------------------------------------------------------------------------- */
/* Serializing a region to a buffer                                                          */
/* ---------------------------------------------------------------------------------------- */

/* The bytes bsvx_world_save_region would have written, for a package holding exactly one
   region. Same convention as the payload getters, so (NULL, 0) sizes the buffer. The package's
   .btx files are not included -- a standalone .bvx only references them. */
BSVX_API bsvx_result bsvx_world_save_region_memory(bsvx_context *ctx, const bsvx_world *world, void *out_bytes, size_t capacity, size_t *out_size);

/* ---------------------------------------------------------------------------------------- */
/* Loading a manifest world through a host VFS                                               */
/* ---------------------------------------------------------------------------------------- */

/* bsvx_world_load needs a real path because it opens files itself. Behind Godot's res:// (or any
   pack file) there is no such path, so hand the library the host's reader instead.

   Paths given to these callbacks are built from the path passed to bsvx_world_load_vfs, keep its
   scheme prefix ("res://", "user://", ...) and always use '/' separators. */
typedef struct bsvx_vfs {
  void *user;

  /* Reads a whole file. Return 1 on success, 0 if it cannot be read. Always set *out_size to
     the file's size; when capacity is smaller than that, write nothing and still return 1 --
     the library calls back with a large enough buffer. It probes sizes with (NULL, 0). */
  int (*read_file)(void *user, const char *path, void *out, size_t capacity, size_t *out_size);

  /* 1 if the path names a readable file. May be NULL, in which case read_file is used as the
     existence probe. */
  int (*file_exists)(void *user, const char *path);

  /* Directory enumeration, used only when the manifest has no explicit [[regions]] /
     [[textures]] table. Write entry `index`'s file name (not a path) and return 1; return 0
     once index is past the end or the directory does not exist. May be NULL -- then a
     manifest that relies on auto-discovery loads no regions or textures. */
  int (*list_dir)(void *user, const char *dir_path, size_t index, char *out_name, size_t capacity, size_t *out_size);
} bsvx_vfs;

/* path is a manifest.toml, a directory holding one, or a standalone .bvx -- same three forms
   bsvx_world_load accepts, only read through vfs. */
BSVX_API bsvx_result bsvx_world_load_vfs(bsvx_context *ctx, const char *path, const bsvx_vfs *vfs, bsvx_world **out_world);

/* ---------------------------------------------------------------------------------------- */
/* Texture (.btx) introspection                                                              */
/* ---------------------------------------------------------------------------------------- */

/* A world holds bsvx_world_texture_count() .btx archives; each archive holds its own tables of
   textures, subresources, materials and samplers. tex_index selects the archive, the trailing
   index selects a row inside it. Nothing here allocates: bytes are copied into your buffer. */

typedef struct bsvx_texture_desc {
  uint32_t texture_id;
  uint32_t kind;        /* 0 = 2D, 1 = 3D, 2 = texel buffer (only 2D is implemented) */
  uint32_t vk_format;   /* raw Vulkan format enum; 37 = R8G8B8A8_UNORM, 43 = _SRGB */
  uint32_t usage_flags; /* raw Vulkan usage flags */

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

  uint64_t size;     /* bytes of texel data */
  uint64_t checksum; /* FNV-1a-64 over those bytes */
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

  uint8_t min_filter; /* 0 = nearest, 1 = linear */
  uint8_t mag_filter;
  uint8_t mip_filter;
  uint8_t address_u; /* 0 = repeat, 1 = mirror, 2 = clamp to edge, 3 = clamp to border */
  uint8_t address_v;
  uint8_t address_w;
  uint8_t anisotropy_enable;
  uint8_t compare_enable;

  uint16_t max_anisotropy_x100;
  uint16_t min_lod_x1000;
  uint16_t max_lod_x1000;
  int16_t mip_lod_bias_x1000;

  uint8_t compare_op; /* raw Vulkan compare op */
  uint8_t border_color;
  uint16_t reserved;
} bsvx_sampler_desc;

/* id and relative path of a .btx as the manifest names it; string convention as above. */
BSVX_API bsvx_result bsvx_world_get_texture_id(const bsvx_world *world, size_t tex_index, char *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_get_texture_path(const bsvx_world *world, size_t tex_index, char *out, size_t capacity, size_t *out_size);

BSVX_API size_t bsvx_texture_texture_count(const bsvx_world *world, size_t tex_index);
BSVX_API size_t bsvx_texture_subresource_count(const bsvx_world *world, size_t tex_index);
BSVX_API size_t bsvx_texture_material_count(const bsvx_world *world, size_t tex_index);
BSVX_API size_t bsvx_texture_sampler_count(const bsvx_world *world, size_t tex_index);

BSVX_API bsvx_result bsvx_texture_get_desc(const bsvx_world *world, size_t tex_index, size_t index, bsvx_texture_desc *out_desc);
BSVX_API bsvx_result bsvx_texture_get_subresource_desc(const bsvx_world *world, size_t tex_index, size_t index, bsvx_subresource_desc *out_desc);
BSVX_API bsvx_result bsvx_texture_get_material(const bsvx_world *world, size_t tex_index, size_t index, bsvx_material_desc *out_desc);
BSVX_API bsvx_result bsvx_texture_get_sampler(const bsvx_world *world, size_t tex_index, size_t index, bsvx_sampler_desc *out_desc);

/* Row of the material table carrying material_id -- what a registry entry's material_id refers
   to. Materials are usually stored in id order, so this is a checked shortcut, not a search. */
BSVX_API bsvx_result bsvx_texture_find_material(const bsvx_world *world, size_t tex_index, uint32_t material_id, size_t *out_index);

/* The texels of one subresource. Same buffer convention as the payload getters. */
BSVX_API bsvx_result bsvx_texture_get_subresource_bytes(const bsvx_world *world, size_t tex_index, size_t index, void *out_bytes, size_t capacity,
                                                        size_t *out_size);

/* 0 for formats this build cannot describe -- everything except R8G8B8A8_UNORM/SRGB today. */
BSVX_API uint32_t bsvx_format_bytes_per_texel(uint32_t vk_format);

/* ========================================================================================== */
/* ABI v4                                                                                     */
/* ========================================================================================== */

/* ---------------------------------------------------------------------------------------- */
/* ABI self-description                                                                       */
/* ---------------------------------------------------------------------------------------- */

/* A binding written in a language without a compiler -- ctypes, an FFI table -- mirrors these
   structs by hand and nothing checks the mirror. Compare sizes at load time and refuse to run on
   a mismatch, rather than reading garbled fields for the rest of the session. */
typedef enum bsvx_struct_id {
  BSVX_STRUCT_GEOMETRY_DESC = 0,
  BSVX_STRUCT_REGISTRY_ENTRY = 1,
  BSVX_STRUCT_CHUNK_SUMMARY = 2,
  BSVX_STRUCT_CHUNK_INFO = 3,
  BSVX_STRUCT_WORLD_DESC = 4,
  BSVX_STRUCT_TEXTURE_DESC = 5,
  BSVX_STRUCT_SUBRESOURCE_DESC = 6,
  BSVX_STRUCT_MATERIAL_DESC = 7,
  BSVX_STRUCT_SAMPLER_DESC = 8,
  BSVX_STRUCT_VFS = 9,
  BSVX_STRUCT_UNITS = 10,
  BSVX_STRUCT_VALIDATION_ISSUE = 11,
  BSVX_STRUCT_SAVE_REPORT = 12,
  BSVX_STRUCT_VOXEL_ADDRESS = 13,
  BSVX_STRUCT_COUNT = 14
} bsvx_struct_id;

BSVX_API size_t bsvx_struct_size(uint32_t struct_id); /* 0 for an unknown id */

/* Static storage, never NULL, valid for the life of the library. */
BSVX_API const char *bsvx_result_string(uint32_t result);
BSVX_API const char *bsvx_build_info(void); /* compiler, language standard, build options */

/* ---------------------------------------------------------------------------------------- */
/* Progress and cancellation                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* Return 0 to cancel: the operation fails with BSVX_RESULT_CANCELLED, and an atomic save leaves
   the destination untouched. Called on the calling thread only, so a ctypes callback already
   holds the GIL -- but it must never let an exception escape, which crashes the interpreter.
   `stage` is one of "manifest", "textures", "regions", "prune", "voxels", "chunks". */
typedef int (*bsvx_progress_fn)(void *user, const char *stage, size_t done, size_t total);

/* The callback applies to every subsequent operation performed through this context. Pass NULL
   to clear it. */
BSVX_API void bsvx_context_set_progress(bsvx_context *ctx, bsvx_progress_fn callback, void *user);

/* Non-fatal problems from the last operation -- a hash mismatch tolerated because of
   BSVX_LOAD_IGNORE_HASH_MISMATCH, for instance. Cleared at the start of each operation. */
BSVX_API size_t bsvx_context_warning_count(const bsvx_context *ctx);
BSVX_API const char *bsvx_context_warning(const bsvx_context *ctx, size_t index);

/* ---------------------------------------------------------------------------------------- */
/* Loading with flags                                                                         */
/* ---------------------------------------------------------------------------------------- */

typedef enum bsvx_load_flags {
  BSVX_LOAD_DEFAULT = 0,
  /* Every .bvx stores the hash of the manifest it was baked against, and that hash is taken over
     the manifest's raw bytes -- so a trailing newline, a CRLF checkout or a hand edit makes the
     whole world refuse to load. An editor needs to open it anyway in order to repair it. The
     mismatches come back through bsvx_context_warning. */
  BSVX_LOAD_IGNORE_HASH_MISMATCH = 1u << 0,
  BSVX_LOAD_SKIP_TEXTURES = 1u << 1, /* metadata-only open: name, bounds, registry, region list */
  BSVX_LOAD_SKIP_REGIONS = 1u << 2
} bsvx_load_flags;

BSVX_API bsvx_result bsvx_world_load_ex2(bsvx_context *ctx, const char *path, uint32_t flags, bsvx_world **out_world);

/* Restamps every region with the world's current manifest and registry hashes, which is the
   repair for a world opened through BSVX_LOAD_IGNORE_HASH_MISMATCH. The regions are marked dirty,
   so the next save writes them. */
BSVX_API bsvx_result bsvx_world_rehash(bsvx_world *world, size_t *out_regions_restamped);

/* ---------------------------------------------------------------------------------------- */
/* Saving with flags                                                                          */
/* ---------------------------------------------------------------------------------------- */

typedef enum bsvx_save_flags {
  BSVX_SAVE_DEFAULT = 0,
  /* Temp file + fsync + rename, so an interrupted save keeps the previous world. On by default;
     BSVX_SAVE_NON_ATOMIC turns it off. */
  BSVX_SAVE_NON_ATOMIC = 1u << 0,
  BSVX_SAVE_BACKUP = 1u << 1, /* keep the previous contents as "<file>.bak" */
  /* Deletes .bvx / .btx in the managed directories that the manifest no longer references.
     Required after removing a region: otherwise auto-discovery finds the orphaned file on the
     next load and the deletion undoes itself. Off by default because a "save as" into a
     populated directory would delete files this world never owned. */
  BSVX_SAVE_PRUNE_ORPHANS = 1u << 2,
  /* Writes only what changed. Falls back to a full write when the manifest text changes, because
     the manifest hash is stamped into every region -- out_report tells you which happened. */
  BSVX_SAVE_DIRTY_ONLY = 1u << 3,
  BSVX_SAVE_COMPACT_FIRST = 1u << 4, /* compact every region before serializing */
  BSVX_SAVE_DRY_RUN = 1u << 5        /* report what would be written; touch nothing */
} bsvx_save_flags;

typedef struct bsvx_save_report {
  size_t files_written;
  size_t files_removed;
  size_t files_skipped;
  uint64_t bytes_written;
  int manifest_written;
  int full_rewrite; /* a dirty-only save that had to widen to everything */
} bsvx_save_report;

/* out_report may be NULL. Clears the dirty flags of everything it wrote. */
BSVX_API bsvx_result bsvx_world_save_ex2(bsvx_context *ctx, bsvx_world *world, const char *root_or_manifest_path, uint32_t flags,
                                         bsvx_save_report *out_report);

/* Writes back to the path the world was loaded from, dirty assets only. Fails with NOT_FOUND for
   a world created in memory that has never been saved -- use bsvx_world_save_ex2 for those. */
BSVX_API bsvx_result bsvx_world_save_dirty(bsvx_context *ctx, bsvx_world *world, bsvx_save_report *out_report);

BSVX_API int bsvx_world_is_dirty(const bsvx_world *world);
BSVX_API int bsvx_region_is_dirty(const bsvx_world *world, size_t region_index);
BSVX_API bsvx_result bsvx_world_clear_dirty(bsvx_world *world);

/* One region as a standalone .bvx at an explicit path, leaving the rest of the world alone. */
BSVX_API bsvx_result bsvx_world_save_region_index(bsvx_context *ctx, const bsvx_world *world, size_t region_index, const char *path, uint32_t flags);

/* The manifest alone. _memory gives you the exact text the manifest hash is taken over, which is
   what a host compares against the file on disk to find out whether a save must restamp every
   region. Same string convention as the other getters. */
BSVX_API bsvx_result bsvx_world_save_manifest(bsvx_context *ctx, const bsvx_world *world, const char *manifest_path, uint32_t flags);
BSVX_API bsvx_result bsvx_world_save_manifest_memory(bsvx_context *ctx, const bsvx_world *world, char *out, size_t capacity, size_t *out_size);

/* ---------------------------------------------------------------------------------------- */
/* Units: metres per voxel and world origin                                                   */
/* ---------------------------------------------------------------------------------------- */

/* Nothing in the format recorded scale before v4, so a DCC tool had to keep it outside the file
   and every re-import guessed. Defaults are 1.0 per axis and a zero origin. */
typedef struct bsvx_units {
  double voxel_size_x;
  double voxel_size_y;
  double voxel_size_z;
  double origin_x; /* world-space position of voxel (0,0,0) in region (0,0,0) */
  double origin_y;
  double origin_z;
} bsvx_units;

BSVX_API bsvx_result bsvx_world_get_units(const bsvx_world *world, bsvx_units *out_units);
BSVX_API bsvx_result bsvx_world_set_units(bsvx_world *world, const bsvx_units *units);

/* ---------------------------------------------------------------------------------------- */
/* Registry names                                                                             */
/* ---------------------------------------------------------------------------------------- */

/* bsvx_registry_entry carries only name_hash, and FNV-1a is not invertible -- so names live
   beside the entry table instead. Setting one leaves the registry hash alone (it is taken over
   the raw entry bytes), but it does change the manifest text, and therefore the manifest hash. */
BSVX_API bsvx_result bsvx_world_get_registry_name(const bsvx_world *world, uint32_t voxel_key, char *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_set_registry_name(bsvx_world *world, uint32_t voxel_key, const char *name);

/* ---------------------------------------------------------------------------------------- */
/* Asset paths                                                                                */
/* ---------------------------------------------------------------------------------------- */

/* Where the world came from, and where each region lives -- what a tool that rewrites in place
   needs in order to write back to the files it read. Empty for a world created in memory. */
BSVX_API bsvx_result bsvx_world_get_source_path(const bsvx_world *world, char *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_get_root_dir(const bsvx_world *world, char *out, size_t capacity, size_t *out_size);

/* Relative to the world's regions directory. */
BSVX_API bsvx_result bsvx_world_get_region_path(const bsvx_world *world, size_t region_index, char *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_set_region_path(bsvx_world *world, size_t region_index, const char *relative_path);
BSVX_API bsvx_result bsvx_world_set_paths(bsvx_world *world, const char *regions_dir, const char *textures_dir);

/* ---------------------------------------------------------------------------------------- */
/* World-space voxel access                                                                   */
/* ---------------------------------------------------------------------------------------- */

typedef struct bsvx_voxel_address {
  int32_t region_x;
  int32_t region_y;
  int32_t region_z;
  uint16_t chunk_x;
  uint16_t chunk_y;
  uint16_t chunk_z;
  uint16_t local_x;
  uint16_t local_y;
  uint16_t local_z;
  uint16_t reserved;
  uint32_t local_index; /* into the dense chunk array */
} bsvx_voxel_address;

/* The region/chunk/local decomposition, exposed so hosts stop re-deriving it -- and getting the
   negative-coordinate case wrong, because C's division truncates where this floors. */
BSVX_API bsvx_result bsvx_world_locate_voxel(const bsvx_world *world, int64_t x, int64_t y, int64_t z, bsvx_voxel_address *out_address);

/* Scattered writes in world voxel coordinates. Batched by chunk internally: each touched chunk is
   decoded once, every hit inside it applied, and re-encoded once -- which is what makes a brush
   stroke or a mesh voxelization affordable from a scripting language.

   create_missing != 0 creates regions and chunks as needed. Voxels that would need a region that
   does not exist are skipped when it is 0. *out_count receives the number actually written. */
BSVX_API bsvx_result bsvx_world_set_voxels(bsvx_context *ctx, bsvx_world *world, const int64_t *xs, const int64_t *ys, const int64_t *zs,
                                           const uint32_t *keys, size_t count, int create_missing, size_t *out_count);

/* Gather. Coordinates in unauthored regions or chunks read back as 0 (air). */
BSVX_API bsvx_result bsvx_world_get_voxels(bsvx_context *ctx, const bsvx_world *world, const int64_t *xs, const int64_t *ys, const int64_t *zs,
                                           uint32_t *out_keys, size_t count);

/* Inclusive box fill -- blocking out and erasing without materializing a coordinate list. */
BSVX_API bsvx_result bsvx_world_fill_box(bsvx_context *ctx, bsvx_world *world, int64_t min_x, int64_t min_y, int64_t min_z, int64_t max_x, int64_t max_y,
                                         int64_t max_z, uint32_t key, int create_missing, size_t *out_count);

/* ---------------------------------------------------------------------------------------- */
/* Removal                                                                                    */
/* ---------------------------------------------------------------------------------------- */

/* Every region index above the removed one shifts down by one. Never cache an index across a
   removal -- look it up again with bsvx_world_find_region. The region's file is deleted only by a
   save carrying BSVX_SAVE_PRUNE_ORPHANS. */
BSVX_API bsvx_result bsvx_world_remove_region(bsvx_world *world, size_t region_index);
BSVX_API bsvx_result bsvx_world_prune_empty_regions(bsvx_world *world, size_t *out_removed);

/* Same index-shifting caveat for textures. */
BSVX_API bsvx_result bsvx_world_remove_texture(bsvx_world *world, size_t tex_index);

/* Drops the chunk from the region's map entirely; chunk ordinals after it shift down. */
BSVX_API bsvx_result bsvx_region_remove_chunk(bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z);

/* Keeps the chunk authored but drops every payload it owns -- deliberately empty space, as
   distinct from space nobody has touched. */
BSVX_API bsvx_result bsvx_region_clear_chunk(bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z);

BSVX_API bsvx_result bsvx_region_remove_chunk_payload(bsvx_world *world, size_t region_index, uint32_t section_type, uint16_t chunk_x, uint16_t chunk_y,
                                                      uint16_t chunk_z);

/* ---------------------------------------------------------------------------------------- */
/* Free-form metadata                                                                         */
/* ---------------------------------------------------------------------------------------- */

/* Opaque byte values under string keys, at the world level (stored in the manifest's [metadata]
   table) and per region (a non-chunk section inside the .bvx).

   The contract that makes third-party rewriting safe: a tool must carry keys it does not
   understand through a load/save cycle unchanged. This library does; yours should too. Namespace
   your keys ("blender.voxel_size", not "voxel_size"). */
BSVX_API bsvx_result bsvx_world_set_metadata(bsvx_world *world, const char *key, const void *value, size_t size);
BSVX_API bsvx_result bsvx_world_get_metadata(const bsvx_world *world, const char *key, void *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_world_remove_metadata(bsvx_world *world, const char *key);
BSVX_API size_t bsvx_world_metadata_count(const bsvx_world *world);
BSVX_API bsvx_result bsvx_world_get_metadata_key(const bsvx_world *world, size_t index, char *out, size_t capacity, size_t *out_size);

BSVX_API bsvx_result bsvx_region_set_metadata(bsvx_world *world, size_t region_index, const char *key, const void *value, size_t size);
BSVX_API bsvx_result bsvx_region_get_metadata(const bsvx_world *world, size_t region_index, const char *key, void *out, size_t capacity, size_t *out_size);
BSVX_API bsvx_result bsvx_region_remove_metadata(bsvx_world *world, size_t region_index, const char *key);
BSVX_API size_t bsvx_region_metadata_count(const bsvx_world *world, size_t region_index);
BSVX_API bsvx_result bsvx_region_get_metadata_key(const bsvx_world *world, size_t region_index, size_t index, char *out, size_t capacity, size_t *out_size);

/* ---------------------------------------------------------------------------------------- */
/* Validation                                                                                 */
/* ---------------------------------------------------------------------------------------- */

typedef enum bsvx_severity { BSVX_SEVERITY_INFO = 0, BSVX_SEVERITY_WARNING = 1, BSVX_SEVERITY_ERROR = 2 } bsvx_severity;

/* Stable numbers -- a host maps them to its own text and to "select the offending thing". */
typedef enum bsvx_validation_code {
  BSVX_ISSUE_NONE = 0,
  BSVX_ISSUE_VOXEL_KEY_NOT_IN_REGISTRY = 1,
  BSVX_ISSUE_MATERIAL_NOT_FOUND = 2,
  BSVX_ISSUE_REGION_OUT_OF_BOUNDS = 3,
  BSVX_ISSUE_CHUNK_OUT_OF_REGION = 4,
  BSVX_ISSUE_DUPLICATE_REGION_COORD = 5,
  BSVX_ISSUE_DUPLICATE_VOXEL_KEY = 6,
  BSVX_ISSUE_AIR_KEY_REGISTERED = 7,
  BSVX_ISSUE_TEXTURE_PATH_TOO_LONG = 8,
  BSVX_ISSUE_TEXTURE_ARCHIVE_INVALID = 9,
  BSVX_ISSUE_TEXTURE_REF_UNRESOLVED = 10,
  BSVX_ISSUE_GEOMETRY_INVALID = 11,
  BSVX_ISSUE_EMPTY_REGION = 12,
  BSVX_ISSUE_REGISTRY_EMPTY = 13,
  BSVX_ISSUE_NO_TEXTURES = 14,
  BSVX_ISSUE_UNITS_UNSET = 15
} bsvx_validation_code;

typedef struct bsvx_validation_issue {
  uint32_t severity;
  uint32_t code;
  int64_t region_index;  /* -1 when not region-scoped */
  int64_t chunk_ordinal; /* -1 when not chunk-scoped */
  uint32_t voxel_key;    /* 0 when irrelevant */
  uint32_t reserved;
} bsvx_validation_issue;

typedef enum bsvx_validate_flags {
  BSVX_VALIDATE_DEFAULT = 0,
  /* Decodes every chunk and checks every voxel key actually used. Without it only the four
     dominant keys each chunk summary records are checked -- cheap, and partial. */
  BSVX_VALIDATE_DEEP = 1u << 0
} bsvx_validate_flags;

/* Issues are held on the context until the next call to this function. Errors in the world are
   reported through *out_issue_count, not through the return value: OK means the check ran. */
BSVX_API bsvx_result bsvx_world_validate(bsvx_context *ctx, const bsvx_world *world, uint32_t flags, size_t *out_issue_count);
BSVX_API bsvx_result bsvx_world_get_validation_issue(const bsvx_context *ctx, size_t index, bsvx_validation_issue *out_issue);
BSVX_API bsvx_result bsvx_world_get_validation_message(const bsvx_context *ctx, size_t index, char *out, size_t capacity, size_t *out_size);

/* ---------------------------------------------------------------------------------------- */
/* Bulk accessors                                                                             */
/* ---------------------------------------------------------------------------------------- */

/* One call per chunk costs a few microseconds of FFI overhead, which is minutes across a large
   world. These fill an array in one crossing. */
BSVX_API bsvx_result bsvx_region_get_chunk_infos(const bsvx_world *world, size_t region_index, size_t first, size_t count, bsvx_chunk_info *out_array,
                                                 size_t *out_written);

BSVX_API bsvx_result bsvx_world_get_registry_entries(const bsvx_world *world, size_t first, size_t count, bsvx_registry_entry *out_array, size_t *out_written);

typedef enum bsvx_voxel_layout {
  /* Chunks concatenated in chunk-map order, each dense as x + sx*(y + sy*z). Pair with
     bsvx_region_get_chunk_infos to know which chunk each block belongs to. */
  BSVX_LAYOUT_CHUNK_ORDER = 0,
  /* One dense array covering the whole region, indexed
     wx + (cs_x*rs_x) * (wy + (cs_y*rs_y) * wz). Unauthored chunks read as air. */
  BSVX_LAYOUT_REGION_LINEAR = 1
} bsvx_voxel_layout;

/* Required capacity lands in *out_written under the usual BUFFER_TOO_SMALL convention, so
   (NULL, 0) sizes the buffer. */
BSVX_API bsvx_result bsvx_region_decode_all_u32(bsvx_context *ctx, const bsvx_world *world, size_t region_index, uint32_t layout, uint32_t *out_voxels,
                                                size_t voxel_capacity, size_t *out_written);

/* FNV-1a over a chunk's *decoded* voxels, so re-encoding the same content under another codec
   does not change it. This is how a host tells which chunks a user actually edited. */
BSVX_API bsvx_result bsvx_region_chunk_content_hash(const bsvx_world *world, size_t region_index, uint16_t chunk_x, uint16_t chunk_y, uint16_t chunk_z,
                                                    uint64_t *out_hash);

/* ---------------------------------------------------------------------------------------- */
/* Writing a .btx                                                                             */
/* ---------------------------------------------------------------------------------------- */

/* A builder assembles a texture archive incrementally, mirroring the C++ btx::Archive. Ids are
   assigned by the library and returned; they are table indices, so add order is id order.

   Only R8G8B8A8_UNORM (37) and _SRGB (43) are implemented, and only 2D textures and 2D arrays --
   append_subresource rejects anything else rather than writing an archive nothing can read. */
typedef struct bsvx_texture_builder bsvx_texture_builder;

BSVX_API bsvx_result bsvx_texture_builder_create(bsvx_context *ctx, bsvx_texture_builder **out_builder);
BSVX_API void bsvx_texture_builder_destroy(bsvx_texture_builder *builder);

BSVX_API bsvx_result bsvx_texture_builder_add_sampler(bsvx_context *ctx, bsvx_texture_builder *builder, const bsvx_sampler_desc *desc, uint32_t *out_id);
BSVX_API bsvx_result bsvx_texture_builder_add_texture(bsvx_context *ctx, bsvx_texture_builder *builder, const bsvx_texture_desc *desc, uint32_t *out_id);
BSVX_API bsvx_result bsvx_texture_builder_add_material(bsvx_context *ctx, bsvx_texture_builder *builder, const bsvx_material_desc *desc, uint32_t *out_id);

/* texels must be exactly row_length * image_height * bytes_per_texel bytes, where a zero
   packed_* means "tightly packed at the given extent". */
BSVX_API bsvx_result bsvx_texture_builder_append_subresource(bsvx_context *ctx, bsvx_texture_builder *builder, uint32_t texture_id, uint16_t mip_level,
                                                             uint16_t layer, uint16_t width, uint16_t height, const void *texels, size_t size,
                                                             uint16_t packed_row_length, uint16_t packed_image_height, uint32_t *out_index);

/* Box-filters every mip above 0 from the level-0 subresources already appended for that texture,
   for each array layer. Existing higher mips are replaced. */
BSVX_API bsvx_result bsvx_texture_builder_generate_mips(bsvx_context *ctx, bsvx_texture_builder *builder, uint32_t texture_id);

/* Structural problems, one per line, before anything reaches disk. *out_issue_count is the number
   of problems found; the text follows the usual string convention. */
BSVX_API bsvx_result bsvx_texture_builder_validate(bsvx_context *ctx, const bsvx_texture_builder *builder, char *out_messages, size_t capacity, size_t *out_size,
                                                   size_t *out_issue_count);

BSVX_API bsvx_result bsvx_texture_builder_save(bsvx_context *ctx, const bsvx_texture_builder *builder, const char *path, uint32_t flags);
BSVX_API bsvx_result bsvx_texture_builder_save_memory(bsvx_context *ctx, const bsvx_texture_builder *builder, void *out_bytes, size_t capacity, size_t *out_size);

/* Copies the builder's archive into the world under the given id and relative path (relative to
   the world's textures directory; ".btx" is appended when the path has no extension, and NULL
   derives it from the id). The builder is unchanged and may be destroyed afterwards.

   The relative path, joined with the textures directory, must fit in 127 characters -- that is
   what a region's texture reference can hold. Longer paths are rejected, not truncated. */
BSVX_API bsvx_result bsvx_world_add_texture(bsvx_context *ctx, bsvx_world *world, const char *texture_id, const char *relative_path,
                                            const bsvx_texture_builder *builder, size_t *out_tex_index);

BSVX_API bsvx_result bsvx_world_set_texture_id(bsvx_world *world, size_t tex_index, const char *texture_id);

/* Loads an existing .btx into a builder so it can be edited and written back. */
BSVX_API bsvx_result bsvx_texture_builder_open(bsvx_context *ctx, const char *path, bsvx_texture_builder **out_builder);
BSVX_API bsvx_result bsvx_texture_builder_from_world(bsvx_context *ctx, const bsvx_world *world, size_t tex_index, bsvx_texture_builder **out_builder);

/* ---------------------------------------------------------------------------------------- */
/* Texel formats                                                                              */
/* ---------------------------------------------------------------------------------------- */

/* Raw VkFormat values this build can size and write. Anything else is refused rather than stored
   as bytes nothing can interpret. */
typedef enum bsvx_format {
  BSVX_FORMAT_R8_UNORM = 9,
  BSVX_FORMAT_R8G8_UNORM = 16,
  BSVX_FORMAT_R8G8B8A8_UNORM = 37,
  BSVX_FORMAT_R8G8B8A8_SRGB = 43,
  BSVX_FORMAT_B8G8R8A8_UNORM = 44,
  BSVX_FORMAT_B8G8R8A8_SRGB = 50,
  BSVX_FORMAT_R16_SFLOAT = 76,
  BSVX_FORMAT_R16G16B16A16_SFLOAT = 97,
  BSVX_FORMAT_R32_SFLOAT = 100,
  BSVX_FORMAT_R32G32B32A32_SFLOAT = 109,
  BSVX_FORMAT_BC1_RGB_UNORM = 131,
  BSVX_FORMAT_BC1_RGB_SRGB = 132,
  BSVX_FORMAT_BC1_RGBA_UNORM = 133,
  BSVX_FORMAT_BC1_RGBA_SRGB = 134,
  BSVX_FORMAT_BC3_UNORM = 137,
  BSVX_FORMAT_BC3_SRGB = 138,
  BSVX_FORMAT_BC4_UNORM = 139,
  BSVX_FORMAT_BC5_UNORM = 141,
  BSVX_FORMAT_BC7_UNORM = 145,
  BSVX_FORMAT_BC7_SRGB = 146
} bsvx_format;

BSVX_API int bsvx_format_is_supported(uint32_t vk_format);
BSVX_API int bsvx_format_is_block_compressed(uint32_t vk_format);
BSVX_API uint32_t bsvx_format_block_extent(uint32_t vk_format); /* 4 for BCn, 1 otherwise */
BSVX_API uint32_t bsvx_format_block_size(uint32_t vk_format);

/* Bytes one mip/layer of this extent occupies. Block formats round up to whole blocks, so this --
   not bsvx_format_bytes_per_texel, which returns 0 for them -- is what sizes a buffer. */
BSVX_API uint64_t bsvx_format_subresource_size(uint32_t vk_format, uint32_t width, uint32_t height);

/* ---------------------------------------------------------------------------------------- */
/* Axis conventions                                                                           */
/* ---------------------------------------------------------------------------------------- */

typedef enum bsvx_axis_convention {
  BSVX_AXIS_X_RIGHT_Y_UP_Z_FORWARD = 0, /* Godot. Right-handed. The canonical frame. */
  BSVX_AXIS_X_RIGHT_Z_UP_Y_FORWARD = 1, /* Blender, 3ds Max. Right-handed, Z up. */
  BSVX_AXIS_X_RIGHT_Y_UP_Z_BACK = 2     /* Unity. Left-handed. */
} bsvx_axis_convention;

BSVX_API const char *bsvx_axis_convention_name(uint32_t convention); /* "" for an unknown value */

/* Continuous positions: a signed axis permutation. */
BSVX_API bsvx_result bsvx_convert_position(uint32_t from, uint32_t to, double x, double y, double z, double *out_x, double *out_y, double *out_z);

/* Integer *cell* indices. A mirrored axis needs a one-cell offset on top of the negation, because
   cell c covers [c, c+1) and its mirror is -c-1. Skip the offset and the world shifts by one voxel
   along the flipped axis — invisible on symmetric content, obvious on real geometry. */
BSVX_API bsvx_result bsvx_convert_cell(uint32_t from, uint32_t to, int64_t x, int64_t y, int64_t z, int64_t *out_x, int64_t *out_y, int64_t *out_z);

/* Rewrites every voxel into another convention, re-deriving the region and chunk decomposition --
   a flipped axis moves voxels across region boundaries, so nothing cheaper is possible. Baked
   payload sections are DROPPED: they describe the old frame and cannot be reinterpreted. Every
   region index is invalidated. */
BSVX_API bsvx_result bsvx_world_convert_axis_convention(bsvx_context *ctx, bsvx_world *world, uint32_t target, size_t *out_voxels_moved);

/* ---------------------------------------------------------------------------------------- */
/* Registry colours and palettes                                                              */
/* ---------------------------------------------------------------------------------------- */

/* 0xRRGGBBAA display colour per voxel key, stored beside the registry. Authoritative only when no
   material resolves — it exists so a palette means something before a world has any .btx at all.
   0 means unset, so a fully transparent black is not expressible; use 0x00000001 if you need it. */
BSVX_API bsvx_result bsvx_world_get_registry_color(const bsvx_world *world, uint32_t voxel_key, uint32_t *out_rgba8);
BSVX_API bsvx_result bsvx_world_set_registry_color(bsvx_world *world, uint32_t voxel_key, uint32_t rgba8);

/* The whole colour-first authoring path in one call: builds a 1x1 array texture with one layer per
   colour, one material per colour, and one registry entry per colour (voxel key i+1 -> material i),
   attaches it to the world under texture_id, and records each colour on its registry entry.

   flags_for_all is applied to every entry (BSVX_REGISTRY_OPAQUE is the usual choice). */
BSVX_API bsvx_result bsvx_world_make_palette(bsvx_context *ctx, bsvx_world *world, const uint32_t *colors_rgba8, size_t count, const char *texture_id,
                                             uint32_t flags_for_all, size_t *out_tex_index);

/* Registry flag bits, mirroring RegistryFlags. */
typedef enum bsvx_registry_flags {
  BSVX_REGISTRY_OPAQUE = 1u << 0,
  BSVX_REGISTRY_EMISSIVE = 1u << 1,
  BSVX_REGISTRY_SPECIAL = 1u << 2,
  BSVX_REGISTRY_COLLIDABLE = 1u << 3
} bsvx_registry_flags;

/* ---------------------------------------------------------------------------------------- */
/* Cloning                                                                                    */
/* ---------------------------------------------------------------------------------------- */

/* A deep copy, sharing nothing. The threading contract for a bsvx_world is **one writer or many
   readers** — there is no internal locking. Clone before handing a world to a background thread so
   the user can keep editing the original. */
BSVX_API bsvx_result bsvx_world_clone(bsvx_context *ctx, const bsvx_world *world, bsvx_world **out_clone);

/* ---------------------------------------------------------------------------------------- */
/* Saving through a host VFS                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* The write-side counterpart of bsvx_vfs. Paths keep the scheme of the path passed to
   bsvx_world_save_vfs ("res://", "user://", ...) and always use '/' separators.

   Every callback returns 1 on success and 0 on failure; a failure aborts the save with
   BSVX_RESULT_RUNTIME_ERROR. Atomicity is the host's business here: this library cannot rename
   inside someone else's filesystem, so BSVX_SAVE_BACKUP and the atomic flag are passed through to
   write_file rather than acted on. */
typedef struct bsvx_vfs_writer {
  void *user;

  int (*write_file)(void *user, const char *path, const void *bytes, size_t size, int atomic, int backup);
  int (*make_directories)(void *user, const char *path);
  int (*file_exists)(void *user, const char *path);
  int (*remove_file)(void *user, const char *path);

  /* Directory enumeration, used only by BSVX_SAVE_PRUNE_ORPHANS. Write entry `index`'s file name
     (not a path) and return 1; return 0 once index is past the end. May be NULL, which disables
     pruning. */
  int (*list_dir)(void *user, const char *dir_path, size_t index, char *out_name, size_t capacity, size_t *out_size);

  /* Reads a whole file, same convention as bsvx_vfs::read_file. Used only by BSVX_SAVE_DIRTY_ONLY,
     to compare the manifest against what is already there. May be NULL, in which case a dirty-only
     save conservatively rewrites everything. */
  int (*read_file)(void *user, const char *path, void *out, size_t capacity, size_t *out_size);
} bsvx_vfs_writer;

BSVX_API bsvx_result bsvx_world_save_vfs(bsvx_context *ctx, bsvx_world *world, const char *root_or_manifest_path, const bsvx_vfs_writer *writer, uint32_t flags,
                                         bsvx_save_report *out_report);

#ifdef __cplusplus
}
#endif

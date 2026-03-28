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

#ifdef __cplusplus
extern "C" {
#endif

	typedef struct bsvx_context bsvx_context;
	typedef struct bsvx_world bsvx_world;

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


#ifdef __cplusplus
}
#endif
"""ctypes mirrors of the structs in ``src/include/bsvx_dll.h``.

Every one of these is checked against ``bsvx_struct_size()`` at import time (see ``_lib.py``).
Nothing here may be edited without editing the header to match: a silent layout drift produces
garbled field values rather than an error, which is exactly what the size check exists to prevent.
"""

from __future__ import annotations

import ctypes as C

# --- enums (kept as plain ints; ctypes enums buy nothing here) --------------------------------

RESULT_OK = 0
RESULT_INVALID_ARGUMENT = 1
RESULT_NOT_FOUND = 2
RESULT_BUFFER_TOO_SMALL = 3
RESULT_RUNTIME_ERROR = 4
RESULT_CANCELLED = 5

LOAD_DEFAULT = 0
LOAD_IGNORE_HASH_MISMATCH = 1 << 0
LOAD_SKIP_TEXTURES = 1 << 1
LOAD_SKIP_REGIONS = 1 << 2

SAVE_DEFAULT = 0
SAVE_NON_ATOMIC = 1 << 0
SAVE_BACKUP = 1 << 1
SAVE_PRUNE_ORPHANS = 1 << 2
SAVE_DIRTY_ONLY = 1 << 3
SAVE_COMPACT_FIRST = 1 << 4
SAVE_DRY_RUN = 1 << 5

VALIDATE_DEFAULT = 0
VALIDATE_DEEP = 1 << 0

LAYOUT_CHUNK_ORDER = 0
LAYOUT_REGION_LINEAR = 1

SEVERITY_INFO = 0
SEVERITY_WARNING = 1
SEVERITY_ERROR = 2

SECTION_WORLD_DESC = 1
SECTION_VOXELS = 2
SECTION_SURFACE = 3
SECTION_COLLISION = 4
SECTION_DISTANCE_FIELD = 5
SECTION_LIGHT = 6
SECTION_METADATA = 7
SECTION_USER_BASE = 0x80000000

CODEC_AUTO = 0xFFFF

REGISTRY_OPAQUE = 1 << 0
REGISTRY_EMISSIVE = 1 << 1
REGISTRY_SPECIAL = 1 << 2
REGISTRY_COLLIDABLE = 1 << 3

CHUNK_PRESENT = 1 << 0
CHUNK_EMPTY = 1 << 1
CHUNK_HAS_VOXELS = 1 << 2

# Raw VkFormat values this build can size and write.
VK_FORMAT_R8_UNORM = 9
VK_FORMAT_R8G8_UNORM = 16
VK_FORMAT_R8G8B8A8_UNORM = 37
VK_FORMAT_R8G8B8A8_SRGB = 43
VK_FORMAT_B8G8R8A8_UNORM = 44
VK_FORMAT_B8G8R8A8_SRGB = 50
VK_FORMAT_R16_SFLOAT = 76
VK_FORMAT_R16G16B16A16_SFLOAT = 97
VK_FORMAT_R32_SFLOAT = 100
VK_FORMAT_R32G32B32A32_SFLOAT = 109
VK_FORMAT_BC1_RGB_UNORM = 131
VK_FORMAT_BC1_RGB_SRGB = 132
VK_FORMAT_BC1_RGBA_UNORM = 133
VK_FORMAT_BC1_RGBA_SRGB = 134
VK_FORMAT_BC3_UNORM = 137
VK_FORMAT_BC3_SRGB = 138
VK_FORMAT_BC4_UNORM = 139
VK_FORMAT_BC5_UNORM = 141
VK_FORMAT_BC7_UNORM = 145
VK_FORMAT_BC7_SRGB = 146

# Axis conventions. 0 is canonical; conversions are defined relative to it.
AXIS_X_RIGHT_Y_UP_Z_FORWARD = 0  # Godot. Right-handed.
AXIS_X_RIGHT_Z_UP_Y_FORWARD = 1  # Blender, 3ds Max. Right-handed, Z up.
AXIS_X_RIGHT_Y_UP_Z_BACK = 2     # Unity. Left-handed.

# Struct ids for bsvx_struct_size(); must match bsvx_struct_id in the header.
STRUCT_GEOMETRY_DESC = 0
STRUCT_REGISTRY_ENTRY = 1
STRUCT_CHUNK_SUMMARY = 2
STRUCT_CHUNK_INFO = 3
STRUCT_WORLD_DESC = 4
STRUCT_TEXTURE_DESC = 5
STRUCT_SUBRESOURCE_DESC = 6
STRUCT_MATERIAL_DESC = 7
STRUCT_SAMPLER_DESC = 8
STRUCT_VFS = 9
STRUCT_UNITS = 10
STRUCT_VALIDATION_ISSUE = 11
STRUCT_SAVE_REPORT = 12
STRUCT_VOXEL_ADDRESS = 13


class GeometryDesc(C.Structure):
    _fields_ = [
        ("chunk_size_x", C.c_uint16),
        ("chunk_size_y", C.c_uint16),
        ("chunk_size_z", C.c_uint16),
        ("region_size_x", C.c_uint16),
        ("region_size_y", C.c_uint16),
        ("region_size_z", C.c_uint16),
    ]

    @property
    def voxels_per_chunk(self) -> int:
        return self.chunk_size_x * self.chunk_size_y * self.chunk_size_z


class RegistryEntry(C.Structure):
    _fields_ = [
        ("voxel_key", C.c_uint32),
        ("material_id", C.c_uint32),
        ("flags", C.c_uint32),
        ("name_hash", C.c_uint64),
    ]


class ChunkSummary(C.Structure):
    _fields_ = [
        ("non_air_count", C.c_uint32),
        ("opaque_count", C.c_uint32),
        ("emissive_count", C.c_uint16),
        ("special_count", C.c_uint16),
        ("aabb_min_x", C.c_uint8),
        ("aabb_min_y", C.c_uint8),
        ("aabb_min_z", C.c_uint8),
        ("aabb_max_x", C.c_uint8),
        ("aabb_max_y", C.c_uint8),
        ("aabb_max_z", C.c_uint8),
        ("face_state_px", C.c_uint8),
        ("face_state_nx", C.c_uint8),
        ("face_state_py", C.c_uint8),
        ("face_state_ny", C.c_uint8),
        ("face_state_pz", C.c_uint8),
        ("face_state_nz", C.c_uint8),
        ("macro_occ_4x4x4", C.c_uint64),
        ("top_id_0", C.c_uint32),
        ("top_id_1", C.c_uint32),
        ("top_id_2", C.c_uint32),
        ("top_id_3", C.c_uint32),
        ("top_count_0", C.c_uint16),
        ("top_count_1", C.c_uint16),
        ("top_count_2", C.c_uint16),
        ("top_count_3", C.c_uint16),
    ]


class ChunkInfo(C.Structure):
    _fields_ = [
        ("local_chunk_x", C.c_uint16),
        ("local_chunk_y", C.c_uint16),
        ("local_chunk_z", C.c_uint16),
        ("flags", C.c_uint16),
        ("summary_index", C.c_uint32),
        ("summary", ChunkSummary),
    ]

    @property
    def coord(self) -> tuple[int, int, int]:
        return (self.local_chunk_x, self.local_chunk_y, self.local_chunk_z)


class WorldDesc(C.Structure):
    _fields_ = [
        ("geometry", GeometryDesc),
        ("voxel_schema", C.c_uint16),
        ("axis_convention", C.c_uint16),
        ("bounds_mode", C.c_uint16),
        ("reserved", C.c_uint16),
        ("min_region_x", C.c_int32),
        ("min_region_y", C.c_int32),
        ("min_region_z", C.c_int32),
        ("max_region_x", C.c_int32),
        ("max_region_y", C.c_int32),
        ("max_region_z", C.c_int32),
        ("asset_name_hash", C.c_uint64),
        ("registry_hash", C.c_uint64),
        ("manifest_hash", C.c_uint64),
    ]


class Units(C.Structure):
    _fields_ = [
        ("voxel_size_x", C.c_double),
        ("voxel_size_y", C.c_double),
        ("voxel_size_z", C.c_double),
        ("origin_x", C.c_double),
        ("origin_y", C.c_double),
        ("origin_z", C.c_double),
    ]


class VoxelAddress(C.Structure):
    _fields_ = [
        ("region_x", C.c_int32),
        ("region_y", C.c_int32),
        ("region_z", C.c_int32),
        ("chunk_x", C.c_uint16),
        ("chunk_y", C.c_uint16),
        ("chunk_z", C.c_uint16),
        ("local_x", C.c_uint16),
        ("local_y", C.c_uint16),
        ("local_z", C.c_uint16),
        ("reserved", C.c_uint16),
        ("local_index", C.c_uint32),
    ]


class SaveReport(C.Structure):
    _fields_ = [
        ("files_written", C.c_size_t),
        ("files_removed", C.c_size_t),
        ("files_skipped", C.c_size_t),
        ("bytes_written", C.c_uint64),
        ("manifest_written", C.c_int),
        ("full_rewrite", C.c_int),
    ]


class ValidationIssue(C.Structure):
    _fields_ = [
        ("severity", C.c_uint32),
        ("code", C.c_uint32),
        ("region_index", C.c_int64),
        ("chunk_ordinal", C.c_int64),
        ("voxel_key", C.c_uint32),
        ("reserved", C.c_uint32),
    ]


class TextureDesc(C.Structure):
    _fields_ = [
        ("texture_id", C.c_uint32),
        ("kind", C.c_uint32),
        ("vk_format", C.c_uint32),
        ("usage_flags", C.c_uint32),
        ("width", C.c_uint16),
        ("height", C.c_uint16),
        ("depth", C.c_uint16),
        ("array_layers", C.c_uint16),
        ("mip_levels", C.c_uint16),
        ("sampler_id", C.c_uint16),
        ("name_hash", C.c_uint64),
        ("source_hash", C.c_uint64),
    ]


class SubresourceDesc(C.Structure):
    _fields_ = [
        ("texture_id", C.c_uint32),
        ("mip_level", C.c_uint16),
        ("layer_or_slice", C.c_uint16),
        ("extent_x", C.c_uint16),
        ("extent_y", C.c_uint16),
        ("extent_z", C.c_uint16),
        ("packed_row_length", C.c_uint16),
        ("packed_image_height", C.c_uint16),
        ("size", C.c_uint64),
        ("checksum", C.c_uint64),
    ]


class MaterialDesc(C.Structure):
    _fields_ = [
        ("material_id", C.c_uint32),
        ("flags", C.c_uint32),
        ("albedo_texture_id", C.c_uint32),
        ("normal_texture_id", C.c_uint32),
        ("orm_texture_id", C.c_uint32),
        ("emissive_texture_id", C.c_uint32),
        ("albedo_layer", C.c_uint16 * 6),
        ("normal_layer", C.c_uint16 * 6),
        ("tint_rgba8", C.c_uint32),
    ]


class SamplerDesc(C.Structure):
    _fields_ = [
        ("sampler_id", C.c_uint32),
        ("min_filter", C.c_uint8),
        ("mag_filter", C.c_uint8),
        ("mip_filter", C.c_uint8),
        ("address_u", C.c_uint8),
        ("address_v", C.c_uint8),
        ("address_w", C.c_uint8),
        ("anisotropy_enable", C.c_uint8),
        ("compare_enable", C.c_uint8),
        ("max_anisotropy_x100", C.c_uint16),
        ("min_lod_x1000", C.c_uint16),
        ("max_lod_x1000", C.c_uint16),
        ("mip_lod_bias_x1000", C.c_int16),
        ("compare_op", C.c_uint8),
        ("border_color", C.c_uint8),
        ("reserved", C.c_uint16),
    ]


READ_FILE_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t))
FILE_EXISTS_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p)
LIST_DIR_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.POINTER(C.c_size_t))
PROGRESS_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_size_t, C.c_size_t)


WRITE_FILE_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p, C.c_void_p, C.c_size_t, C.c_int, C.c_int)
MAKE_DIRS_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p)
REMOVE_FILE_FN = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p)


class VfsWriter(C.Structure):
    _fields_ = [
        ("user", C.c_void_p),
        ("write_file", WRITE_FILE_FN),
        ("make_directories", MAKE_DIRS_FN),
        ("file_exists", FILE_EXISTS_FN),
        ("remove_file", REMOVE_FILE_FN),
        ("list_dir", LIST_DIR_FN),
        ("read_file", READ_FILE_FN),
    ]


class Vfs(C.Structure):
    _fields_ = [
        ("user", C.c_void_p),
        ("read_file", READ_FILE_FN),
        ("file_exists", FILE_EXISTS_FN),
        ("list_dir", LIST_DIR_FN),
    ]


# (struct id, python type) pairs verified against the library at import.
CHECKED_STRUCTS = (
    (STRUCT_GEOMETRY_DESC, GeometryDesc),
    (STRUCT_REGISTRY_ENTRY, RegistryEntry),
    (STRUCT_CHUNK_SUMMARY, ChunkSummary),
    (STRUCT_CHUNK_INFO, ChunkInfo),
    (STRUCT_WORLD_DESC, WorldDesc),
    (STRUCT_TEXTURE_DESC, TextureDesc),
    (STRUCT_SUBRESOURCE_DESC, SubresourceDesc),
    (STRUCT_MATERIAL_DESC, MaterialDesc),
    (STRUCT_SAMPLER_DESC, SamplerDesc),
    (STRUCT_VFS, Vfs),
    (STRUCT_UNITS, Units),
    (STRUCT_VALIDATION_ISSUE, ValidationIssue),
    (STRUCT_SAVE_REPORT, SaveReport),
    (STRUCT_VOXEL_ADDRESS, VoxelAddress),
)

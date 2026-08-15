"""Loads libbsvx and declares every entry point.

Two rules hold everywhere below, and both exist because ctypes fails silently otherwise:

* ``argtypes`` and ``restype`` are declared for **every** function. Without them ctypes assumes an
  ``int`` return, which truncates every returned ``size_t`` and every pointer on 64-bit.
* The library is verified before use: ABI version first, then the size of every struct we mirror.
  A mismatch raises here rather than producing garbled fields for the rest of the session.
"""

from __future__ import annotations

import ctypes as C
import os
import platform
import sys
from pathlib import Path

from . import _structs as S

#: The ABI this binding is written against. The library may be newer (it stays backward
#: compatible within a major version) but never older.
REQUIRED_ABI = 4


class BsvxError(RuntimeError):
    """A library call returned a non-OK result."""

    def __init__(self, message: str, result: int = S.RESULT_RUNTIME_ERROR) -> None:
        super().__init__(message)
        self.result = result


def _candidate_names() -> tuple[str, ...]:
    if sys.platform == "win32":
        return ("bsvx.dll",)
    if sys.platform == "darwin":
        return ("libbsvx.dylib",)
    return ("libbsvx.so", f"libbsvx.so.{REQUIRED_ABI}")


def _platform_dir() -> str:
    machine = platform.machine().lower()
    if sys.platform == "win32":
        return "windows_x86_64"
    if sys.platform == "darwin":
        return "macos_universal"
    return f"linux_{machine}"


def _search_paths() -> list[Path]:
    """Where to look, in order: an explicit override, the bundled binaries, then the build tree."""
    here = Path(__file__).resolve().parent
    paths: list[Path] = []

    override = os.environ.get("BSVX_LIBRARY")
    if override:
        paths.append(Path(override))

    for name in _candidate_names():
        paths.append(here / "bin" / _platform_dir() / name)
        paths.append(here / "bin" / name)

    # Running straight out of a source checkout.
    repo = here.parent.parent
    for name in _candidate_names():
        paths.append(repo / "build" / name)
        paths.append(repo / name)

    return paths


def _load() -> C.CDLL:
    tried: list[str] = []
    for path in _search_paths():
        if not path.is_file():
            tried.append(f"{path} (missing)")
            continue
        try:
            return C.CDLL(str(path))
        except OSError as exc:  # wrong architecture, missing dependency, ...
            tried.append(f"{path} ({exc})")

    # Last resort: whatever the system loader can find.
    for name in _candidate_names():
        try:
            return C.CDLL(name)
        except OSError as exc:
            tried.append(f"{name} ({exc})")

    raise BsvxError(
        "could not load the bsvx shared library. Tried:\n  " + "\n  ".join(tried) +
        "\nSet BSVX_LIBRARY to its full path, or build it with "
        "`cmake -S . -B build && cmake --build build`."
    )


lib = _load()


def _declare(name: str, restype, argtypes) -> object:
    try:
        fn = getattr(lib, name)
    except AttributeError as exc:
        raise BsvxError(f"the loaded bsvx library is missing {name}: it is older than ABI {REQUIRED_ABI}") from exc
    fn.restype = restype
    fn.argtypes = argtypes
    return fn


c_void_pp = C.POINTER(C.c_void_p)
c_size_p = C.POINTER(C.c_size_t)
u32_p = C.POINTER(C.c_uint32)
u64_p = C.POINTER(C.c_uint64)
i32_p = C.POINTER(C.c_int32)
u16_p = C.POINTER(C.c_uint16)
i64_p = C.POINTER(C.c_int64)

# --- version and self-description --------------------------------------------------------------

abi_version = _declare("bsvx_abi_version", C.c_uint32, [])
struct_size = _declare("bsvx_struct_size", C.c_size_t, [C.c_uint32])
result_string = _declare("bsvx_result_string", C.c_char_p, [C.c_uint32])
build_info = _declare("bsvx_build_info", C.c_char_p, [])

_actual_abi = abi_version()
if _actual_abi < REQUIRED_ABI:
    raise BsvxError(
        f"bsvx library reports ABI {_actual_abi} but this binding needs {REQUIRED_ABI}. "
        "Update the shared library."
    )

for _struct_id, _type in S.CHECKED_STRUCTS:
    _expected = struct_size(_struct_id)
    _actual = C.sizeof(_type)
    if _expected != _actual:
        raise BsvxError(
            f"struct layout mismatch for {_type.__name__}: the library says {_expected} bytes, "
            f"this binding mirrors {_actual}. The binding and the library are out of step."
        )
del _struct_id, _type, _expected, _actual

# --- context -----------------------------------------------------------------------------------

context_create = _declare("bsvx_context_create", C.c_void_p, [])
context_destroy = _declare("bsvx_context_destroy", None, [C.c_void_p])
context_last_error = _declare("bsvx_context_last_error", C.c_char_p, [C.c_void_p])
context_set_progress = _declare("bsvx_context_set_progress", None, [C.c_void_p, S.PROGRESS_FN, C.c_void_p])
context_warning_count = _declare("bsvx_context_warning_count", C.c_size_t, [C.c_void_p])
context_warning = _declare("bsvx_context_warning", C.c_char_p, [C.c_void_p, C.c_size_t])

# --- loading -----------------------------------------------------------------------------------

world_load = _declare("bsvx_world_load", C.c_int, [C.c_void_p, C.c_char_p, c_void_pp])
world_load_ex2 = _declare("bsvx_world_load_ex2", C.c_int, [C.c_void_p, C.c_char_p, C.c_uint32, c_void_pp])
world_load_region = _declare("bsvx_world_load_region", C.c_int, [C.c_void_p, C.c_char_p, c_void_pp])
world_load_region_memory_ex = _declare(
    "bsvx_world_load_region_memory_ex", C.c_int, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_char_p, c_void_pp]
)
world_load_vfs = _declare("bsvx_world_load_vfs", C.c_int, [C.c_void_p, C.c_char_p, C.POINTER(S.Vfs), c_void_pp])
world_create = _declare("bsvx_world_create", C.c_int, [C.c_void_p, C.POINTER(S.GeometryDesc), c_void_pp])
world_destroy = _declare("bsvx_world_destroy", None, [C.c_void_p])
world_rehash = _declare("bsvx_world_rehash", C.c_int, [C.c_void_p, c_size_p])

# --- saving ------------------------------------------------------------------------------------

world_save_ex2 = _declare("bsvx_world_save_ex2", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.c_uint32, C.POINTER(S.SaveReport)])
world_save_dirty = _declare("bsvx_world_save_dirty", C.c_int, [C.c_void_p, C.c_void_p, C.POINTER(S.SaveReport)])
world_save_region_ex = _declare("bsvx_world_save_region_ex", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p])
world_save_region_index = _declare("bsvx_world_save_region_index", C.c_int, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_char_p, C.c_uint32])
world_save_region_memory = _declare("bsvx_world_save_region_memory", C.c_int, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, c_size_p])
world_save_manifest = _declare("bsvx_world_save_manifest", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.c_uint32])
world_save_manifest_memory = _declare("bsvx_world_save_manifest_memory", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.c_size_t, c_size_p])
world_is_dirty = _declare("bsvx_world_is_dirty", C.c_int, [C.c_void_p])
region_is_dirty = _declare("bsvx_region_is_dirty", C.c_int, [C.c_void_p, C.c_size_t])
world_clear_dirty = _declare("bsvx_world_clear_dirty", C.c_int, [C.c_void_p])

# --- world metadata ----------------------------------------------------------------------------

world_geometry = _declare("bsvx_world_geometry", C.c_int, [C.c_void_p, C.POINTER(S.GeometryDesc)])
world_get_desc = _declare("bsvx_world_get_desc", C.c_int, [C.c_void_p, C.POINTER(S.WorldDesc)])
world_set_desc = _declare("bsvx_world_set_desc", C.c_int, [C.c_void_p, C.POINTER(S.WorldDesc)])
world_get_name = _declare("bsvx_world_get_name", C.c_int, [C.c_void_p, C.c_char_p, C.c_size_t, c_size_p])
world_set_name = _declare("bsvx_world_set_name", C.c_int, [C.c_void_p, C.c_char_p])
world_get_uuid = _declare("bsvx_world_get_uuid", C.c_int, [C.c_void_p, C.c_char_p, C.c_size_t, c_size_p])
world_set_uuid = _declare("bsvx_world_set_uuid", C.c_int, [C.c_void_p, C.c_char_p])
world_get_units = _declare("bsvx_world_get_units", C.c_int, [C.c_void_p, C.POINTER(S.Units)])
world_set_units = _declare("bsvx_world_set_units", C.c_int, [C.c_void_p, C.POINTER(S.Units)])

world_get_source_path = _declare("bsvx_world_get_source_path", C.c_int, [C.c_void_p, C.c_char_p, C.c_size_t, c_size_p])
world_get_root_dir = _declare("bsvx_world_get_root_dir", C.c_int, [C.c_void_p, C.c_char_p, C.c_size_t, c_size_p])
world_get_region_path = _declare("bsvx_world_get_region_path", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t, c_size_p])
world_set_region_path = _declare("bsvx_world_set_region_path", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p])
world_set_paths = _declare("bsvx_world_set_paths", C.c_int, [C.c_void_p, C.c_char_p, C.c_char_p])

# --- registry ----------------------------------------------------------------------------------

world_registry_entry_count = _declare("bsvx_world_registry_entry_count", C.c_size_t, [C.c_void_p])
world_get_registry_entry = _declare("bsvx_world_get_registry_entry", C.c_int, [C.c_void_p, C.c_size_t, C.POINTER(S.RegistryEntry)])
world_get_registry_entries = _declare(
    "bsvx_world_get_registry_entries", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(S.RegistryEntry), c_size_p]
)
world_set_registry_entry = _declare("bsvx_world_set_registry_entry", C.c_int, [C.c_void_p, C.POINTER(S.RegistryEntry)])
world_remove_registry_entry = _declare("bsvx_world_remove_registry_entry", C.c_int, [C.c_void_p, C.c_uint32])
world_get_registry_name = _declare("bsvx_world_get_registry_name", C.c_int, [C.c_void_p, C.c_uint32, C.c_char_p, C.c_size_t, c_size_p])
world_set_registry_name = _declare("bsvx_world_set_registry_name", C.c_int, [C.c_void_p, C.c_uint32, C.c_char_p])

# --- regions and chunks -------------------------------------------------------------------------

world_region_count = _declare("bsvx_world_region_count", C.c_size_t, [C.c_void_p])
world_add_region = _declare("bsvx_world_add_region", C.c_int, [C.c_void_p, C.c_int32, C.c_int32, C.c_int32, c_size_p])
world_find_region = _declare("bsvx_world_find_region", C.c_int, [C.c_void_p, C.c_int32, C.c_int32, C.c_int32, c_size_p])
world_remove_region = _declare("bsvx_world_remove_region", C.c_int, [C.c_void_p, C.c_size_t])
world_prune_empty_regions = _declare("bsvx_world_prune_empty_regions", C.c_int, [C.c_void_p, c_size_p])
world_get_region_coord = _declare("bsvx_world_get_region_coord", C.c_int, [C.c_void_p, C.c_size_t, i32_p, i32_p, i32_p])

region_chunk_count = _declare("bsvx_region_chunk_count", C.c_size_t, [C.c_void_p, C.c_size_t])
region_required_voxel_count = _declare("bsvx_region_required_voxel_count", C.c_size_t, [C.c_void_p, C.c_size_t])
region_get_chunk_info = _declare("bsvx_region_get_chunk_info", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(S.ChunkInfo)])
region_get_chunk_infos = _declare(
    "bsvx_region_get_chunk_infos", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.c_size_t, C.POINTER(S.ChunkInfo), c_size_p]
)
region_find_chunk = _declare("bsvx_region_find_chunk", C.c_int, [C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.c_uint16, c_size_p])
region_decode_chunk_u32_ex = _declare(
    "bsvx_region_decode_chunk_u32_ex",
    C.c_int,
    [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.c_uint16, u32_p, C.c_size_t, c_size_p],
)
region_decode_all_u32 = _declare(
    "bsvx_region_decode_all_u32", C.c_int, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint32, u32_p, C.c_size_t, c_size_p]
)
region_set_chunk_u32_ex = _declare(
    "bsvx_region_set_chunk_u32_ex",
    C.c_int,
    [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.c_uint16, u32_p, C.c_size_t, C.c_uint16],
)
region_remove_chunk = _declare("bsvx_region_remove_chunk", C.c_int, [C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.c_uint16])
region_clear_chunk = _declare("bsvx_region_clear_chunk", C.c_int, [C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.c_uint16])
region_chunk_content_hash = _declare(
    "bsvx_region_chunk_content_hash", C.c_int, [C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.c_uint16, u64_p]
)
region_compact = _declare("bsvx_region_compact", C.c_int, [C.c_void_p, C.c_size_t, c_size_p])
world_compact = _declare("bsvx_world_compact", C.c_int, [C.c_void_p, c_size_p])
region_reclaimable_bytes = _declare("bsvx_region_reclaimable_bytes", C.c_size_t, [C.c_void_p, C.c_size_t])

# --- payloads ------------------------------------------------------------------------------------

region_set_chunk_payload_ex = _declare(
    "bsvx_region_set_chunk_payload_ex",
    C.c_int,
    [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint16, C.c_uint16, C.c_uint16, C.c_uint16, C.c_void_p, C.c_size_t, C.c_uint16],
)
region_get_chunk_payload_ex = _declare(
    "bsvx_region_get_chunk_payload_ex",
    C.c_int,
    [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint16, C.c_uint16, C.c_uint16, C.c_void_p, C.c_size_t, c_size_p, u16_p],
)
region_remove_chunk_payload = _declare(
    "bsvx_region_remove_chunk_payload", C.c_int, [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint16, C.c_uint16, C.c_uint16]
)

# --- world-space voxels ---------------------------------------------------------------------------

world_locate_voxel = _declare("bsvx_world_locate_voxel", C.c_int, [C.c_void_p, C.c_int64, C.c_int64, C.c_int64, C.POINTER(S.VoxelAddress)])
world_set_voxels = _declare(
    "bsvx_world_set_voxels", C.c_int, [C.c_void_p, C.c_void_p, i64_p, i64_p, i64_p, u32_p, C.c_size_t, C.c_int, c_size_p]
)
world_get_voxels = _declare("bsvx_world_get_voxels", C.c_int, [C.c_void_p, C.c_void_p, i64_p, i64_p, i64_p, u32_p, C.c_size_t])
world_fill_box = _declare(
    "bsvx_world_fill_box",
    C.c_int,
    [C.c_void_p, C.c_void_p, C.c_int64, C.c_int64, C.c_int64, C.c_int64, C.c_int64, C.c_int64, C.c_uint32, C.c_int, c_size_p],
)

# --- metadata --------------------------------------------------------------------------------------

world_set_metadata = _declare("bsvx_world_set_metadata", C.c_int, [C.c_void_p, C.c_char_p, C.c_void_p, C.c_size_t])
world_get_metadata = _declare("bsvx_world_get_metadata", C.c_int, [C.c_void_p, C.c_char_p, C.c_void_p, C.c_size_t, c_size_p])
world_remove_metadata = _declare("bsvx_world_remove_metadata", C.c_int, [C.c_void_p, C.c_char_p])
world_metadata_count = _declare("bsvx_world_metadata_count", C.c_size_t, [C.c_void_p])
world_get_metadata_key = _declare("bsvx_world_get_metadata_key", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t, c_size_p])

region_set_metadata = _declare("bsvx_region_set_metadata", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_void_p, C.c_size_t])
region_get_metadata = _declare("bsvx_region_get_metadata", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_void_p, C.c_size_t, c_size_p])
region_remove_metadata = _declare("bsvx_region_remove_metadata", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p])
region_metadata_count = _declare("bsvx_region_metadata_count", C.c_size_t, [C.c_void_p, C.c_size_t])
region_get_metadata_key = _declare(
    "bsvx_region_get_metadata_key", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.c_char_p, C.c_size_t, c_size_p]
)

# --- validation ------------------------------------------------------------------------------------

world_validate = _declare("bsvx_world_validate", C.c_int, [C.c_void_p, C.c_void_p, C.c_uint32, c_size_p])
world_get_validation_issue = _declare("bsvx_world_get_validation_issue", C.c_int, [C.c_void_p, C.c_size_t, C.POINTER(S.ValidationIssue)])
world_get_validation_message = _declare("bsvx_world_get_validation_message", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t, c_size_p])

# --- textures ----------------------------------------------------------------------------------------

world_texture_count = _declare("bsvx_world_texture_count", C.c_size_t, [C.c_void_p])
world_get_texture_id = _declare("bsvx_world_get_texture_id", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t, c_size_p])
world_get_texture_path = _declare("bsvx_world_get_texture_path", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t, c_size_p])
world_set_texture_id = _declare("bsvx_world_set_texture_id", C.c_int, [C.c_void_p, C.c_size_t, C.c_char_p])
world_remove_texture = _declare("bsvx_world_remove_texture", C.c_int, [C.c_void_p, C.c_size_t])
world_add_texture = _declare("bsvx_world_add_texture", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.c_char_p, C.c_void_p, c_size_p])

texture_texture_count = _declare("bsvx_texture_texture_count", C.c_size_t, [C.c_void_p, C.c_size_t])
texture_subresource_count = _declare("bsvx_texture_subresource_count", C.c_size_t, [C.c_void_p, C.c_size_t])
texture_material_count = _declare("bsvx_texture_material_count", C.c_size_t, [C.c_void_p, C.c_size_t])
texture_sampler_count = _declare("bsvx_texture_sampler_count", C.c_size_t, [C.c_void_p, C.c_size_t])
texture_get_desc = _declare("bsvx_texture_get_desc", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(S.TextureDesc)])
texture_get_subresource_desc = _declare(
    "bsvx_texture_get_subresource_desc", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(S.SubresourceDesc)]
)
texture_get_material = _declare("bsvx_texture_get_material", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(S.MaterialDesc)])
texture_get_sampler = _declare("bsvx_texture_get_sampler", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(S.SamplerDesc)])
texture_find_material = _declare("bsvx_texture_find_material", C.c_int, [C.c_void_p, C.c_size_t, C.c_uint32, c_size_p])
texture_get_subresource_bytes = _declare(
    "bsvx_texture_get_subresource_bytes", C.c_int, [C.c_void_p, C.c_size_t, C.c_size_t, C.c_void_p, C.c_size_t, c_size_p]
)
format_bytes_per_texel = _declare("bsvx_format_bytes_per_texel", C.c_uint32, [C.c_uint32])

# --- texture builder -----------------------------------------------------------------------------------

texture_builder_create = _declare("bsvx_texture_builder_create", C.c_int, [C.c_void_p, c_void_pp])
texture_builder_destroy = _declare("bsvx_texture_builder_destroy", None, [C.c_void_p])
texture_builder_open = _declare("bsvx_texture_builder_open", C.c_int, [C.c_void_p, C.c_char_p, c_void_pp])
texture_builder_from_world = _declare("bsvx_texture_builder_from_world", C.c_int, [C.c_void_p, C.c_void_p, C.c_size_t, c_void_pp])
texture_builder_add_sampler = _declare("bsvx_texture_builder_add_sampler", C.c_int, [C.c_void_p, C.c_void_p, C.POINTER(S.SamplerDesc), u32_p])
texture_builder_add_texture = _declare("bsvx_texture_builder_add_texture", C.c_int, [C.c_void_p, C.c_void_p, C.POINTER(S.TextureDesc), u32_p])
texture_builder_add_material = _declare("bsvx_texture_builder_add_material", C.c_int, [C.c_void_p, C.c_void_p, C.POINTER(S.MaterialDesc), u32_p])
texture_builder_append_subresource = _declare(
    "bsvx_texture_builder_append_subresource",
    C.c_int,
    [C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint16, C.c_uint16, C.c_uint16, C.c_uint16, C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, u32_p],
)
texture_builder_generate_mips = _declare("bsvx_texture_builder_generate_mips", C.c_int, [C.c_void_p, C.c_void_p, C.c_uint32])
texture_builder_validate = _declare(
    "bsvx_texture_builder_validate", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.c_size_t, c_size_p, c_size_p]
)
texture_builder_save = _declare("bsvx_texture_builder_save", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.c_uint32])
texture_builder_save_memory = _declare("bsvx_texture_builder_save_memory", C.c_int, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, c_size_p])

# --- texel formats -------------------------------------------------------------------------------------

format_is_supported = _declare("bsvx_format_is_supported", C.c_int, [C.c_uint32])
format_is_block_compressed = _declare("bsvx_format_is_block_compressed", C.c_int, [C.c_uint32])
format_block_extent = _declare("bsvx_format_block_extent", C.c_uint32, [C.c_uint32])
format_block_size = _declare("bsvx_format_block_size", C.c_uint32, [C.c_uint32])
format_subresource_size = _declare("bsvx_format_subresource_size", C.c_uint64, [C.c_uint32, C.c_uint32, C.c_uint32])

# --- axis conventions ----------------------------------------------------------------------------------

axis_convention_name = _declare("bsvx_axis_convention_name", C.c_char_p, [C.c_uint32])
convert_position = _declare(
    "bsvx_convert_position",
    C.c_int,
    [C.c_uint32, C.c_uint32, C.c_double, C.c_double, C.c_double, C.POINTER(C.c_double), C.POINTER(C.c_double), C.POINTER(C.c_double)],
)
convert_cell = _declare("bsvx_convert_cell", C.c_int, [C.c_uint32, C.c_uint32, C.c_int64, C.c_int64, C.c_int64, i64_p, i64_p, i64_p])
world_convert_axis_convention = _declare("bsvx_world_convert_axis_convention", C.c_int, [C.c_void_p, C.c_void_p, C.c_uint32, c_size_p])

# --- registry colours, palettes, cloning, vfs saving ---------------------------------------------------

world_get_registry_color = _declare("bsvx_world_get_registry_color", C.c_int, [C.c_void_p, C.c_uint32, u32_p])
world_set_registry_color = _declare("bsvx_world_set_registry_color", C.c_int, [C.c_void_p, C.c_uint32, C.c_uint32])
world_make_palette = _declare("bsvx_world_make_palette", C.c_int, [C.c_void_p, C.c_void_p, u32_p, C.c_size_t, C.c_char_p, C.c_uint32, c_size_p])
world_clone = _declare("bsvx_world_clone", C.c_int, [C.c_void_p, C.c_void_p, c_void_pp])
world_save_vfs = _declare(
    "bsvx_world_save_vfs", C.c_int, [C.c_void_p, C.c_void_p, C.c_char_p, C.POINTER(S.VfsWriter), C.c_uint32, C.POINTER(S.SaveReport)]
)

# --- streaming reader ----------------------------------------------------------------------------------

region_reader_open = _declare("bsvx_region_reader_open", C.c_int, [C.c_void_p, C.c_char_p, c_void_pp])
region_reader_close = _declare("bsvx_region_reader_close", None, [C.c_void_p])
region_reader_set_geometry = _declare("bsvx_region_reader_set_geometry", C.c_int, [C.c_void_p, C.POINTER(S.GeometryDesc)])
region_reader_chunk_count = _declare("bsvx_region_reader_chunk_count", C.c_size_t, [C.c_void_p])
region_reader_required_voxel_count = _declare("bsvx_region_reader_required_voxel_count", C.c_size_t, [C.c_void_p])
region_reader_get_chunk_info = _declare("bsvx_region_reader_get_chunk_info", C.c_int, [C.c_void_p, C.c_size_t, C.POINTER(S.ChunkInfo)])
region_reader_decode_chunk_u32 = _declare(
    "bsvx_region_reader_decode_chunk_u32", C.c_int, [C.c_void_p, C.c_void_p, C.c_uint16, C.c_uint16, C.c_uint16, u32_p, C.c_size_t, c_size_p]
)
region_reader_resident_bytes = _declare("bsvx_region_reader_resident_bytes", C.c_size_t, [C.c_void_p])
region_reader_verify = _declare("bsvx_region_reader_verify", C.c_int, [C.c_void_p, C.c_void_p])

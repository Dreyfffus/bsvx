"""Pythonic wrappers over the bsvx C ABI.

Every call checks its result and raises :class:`BsvxError` carrying the library's own message, so
nothing fails silently. Handles are freed by ``__exit__`` or by the garbage collector; use the
context-manager form where you can.

Bulk paths (:meth:`World.set_voxels`, :meth:`World.decode_region`) accept and return array-like
buffers so the per-voxel work happens in C. Never loop per voxel across the FFI boundary.
"""

from __future__ import annotations

import ctypes as C
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator, Sequence

from . import _lib as L
from . import _structs as S
from ._lib import BsvxError

__all__ = [
    "BsvxError",
    "Context",
    "World",
    "TextureBuilder",
    "VfsWriter",
    "convert_cell",
    "convert_position",
    "axis_convention_name",
    "format_is_supported",
    "format_subresource_size",
    "ChunkInfo",
    "GeometryDesc",
    "RegistryEntry",
    "SaveReport",
    "Units",
    "ValidationIssue",
    "VoxelAddress",
    "abi_version",
    "build_info",
]

ChunkInfo = S.ChunkInfo
GeometryDesc = S.GeometryDesc
RegistryEntry = S.RegistryEntry
Units = S.Units
VoxelAddress = S.VoxelAddress


def abi_version() -> int:
    return int(L.abi_version())


def build_info() -> str:
    return L.build_info().decode("utf-8", "replace")


# --- texel formats ------------------------------------------------------------------------------


def format_is_supported(vk_format: int) -> bool:
    return bool(L.format_is_supported(vk_format))


def format_is_block_compressed(vk_format: int) -> bool:
    return bool(L.format_is_block_compressed(vk_format))


def format_subresource_size(vk_format: int, width: int, height: int) -> int:
    """Bytes one mip/layer occupies. Block formats round up to whole blocks, so this — not
    bytes-per-texel, which is 0 for them — is what sizes a buffer."""
    return int(L.format_subresource_size(vk_format, width, height))


# --- axis conventions ---------------------------------------------------------------------------


def axis_convention_name(convention: int) -> str:
    return L.axis_convention_name(convention).decode("utf-8", "replace")


def convert_position(from_convention: int, to_convention: int, x: float, y: float, z: float) -> tuple[float, float, float]:
    """Continuous positions: a signed axis permutation."""
    ox, oy, oz = C.c_double(), C.c_double(), C.c_double()
    result = L.convert_position(from_convention, to_convention, x, y, z, C.byref(ox), C.byref(oy), C.byref(oz))
    if result != S.RESULT_OK:
        raise BsvxError(f"convert_position: {L.result_string(result).decode()}", result)
    return (ox.value, oy.value, oz.value)


def convert_cell(from_convention: int, to_convention: int, x: int, y: int, z: int) -> tuple[int, int, int]:
    """Integer cell indices. A mirrored axis carries a one-cell offset on top of the negation —
    cell c covers [c, c+1), so its mirror is -c-1. Skipping it shifts the world by one voxel."""
    ox, oy, oz = C.c_int64(), C.c_int64(), C.c_int64()
    result = L.convert_cell(from_convention, to_convention, x, y, z, C.byref(ox), C.byref(oy), C.byref(oz))
    if result != S.RESULT_OK:
        raise BsvxError(f"convert_cell: {L.result_string(result).decode()}", result)
    return (ox.value, oy.value, oz.value)


class VfsWriter:
    """Adapts Python callables to the C write callbacks used by :meth:`World.save_vfs`.

    Subclass and override the methods, or pass callables. Exceptions raised inside a callback are
    swallowed and reported to the library as a failure — letting one escape a ctypes callback
    crashes the interpreter.
    """

    def __init__(self) -> None:
        # ctypes does not keep trampolines alive; losing them dangles the pointers the library holds.
        self._trampolines: list[Any] = []
        self._struct = S.VfsWriter()
        self._struct.user = None
        self._struct.write_file = self._bind(S.WRITE_FILE_FN, self._write_file)
        self._struct.make_directories = self._bind(S.MAKE_DIRS_FN, self._make_directories)
        self._struct.file_exists = self._bind(S.FILE_EXISTS_FN, self._file_exists)
        self._struct.remove_file = self._bind(S.REMOVE_FILE_FN, self._remove_file)
        self._struct.list_dir = self._bind(S.LIST_DIR_FN, self._list_dir)
        self._struct.read_file = self._bind(S.READ_FILE_FN, self._read_file)

    def _bind(self, signature, method):
        trampoline = signature(method)
        self._trampolines.append(trampoline)
        return trampoline

    # --- override these -------------------------------------------------------------------

    def write_file(self, path: str, data: bytes, atomic: bool, backup: bool) -> None:
        raise NotImplementedError

    def make_directories(self, path: str) -> None:
        raise NotImplementedError

    def file_exists(self, path: str) -> bool:
        return False

    def remove_file(self, path: str) -> None:
        raise NotImplementedError

    def list_dir(self, path: str) -> list[str]:
        """File names, not paths. Returning [] disables orphan pruning."""
        return []

    def read_file(self, path: str) -> bytes | None:
        """Returning None disables the dirty-only manifest comparison, which then rewrites all."""
        return None

    # --- C plumbing -----------------------------------------------------------------------

    def _write_file(self, _user, path, data, size, atomic, backup) -> int:
        try:
            raw = C.string_at(data, size) if size else b""
            self.write_file(path.decode("utf-8", "replace"), raw, bool(atomic), bool(backup))
            return 1
        except Exception:
            return 0

    def _make_directories(self, _user, path) -> int:
        try:
            self.make_directories(path.decode("utf-8", "replace"))
            return 1
        except Exception:
            return 0

    def _file_exists(self, _user, path) -> int:
        try:
            return 1 if self.file_exists(path.decode("utf-8", "replace")) else 0
        except Exception:
            return 0

    def _remove_file(self, _user, path) -> int:
        try:
            self.remove_file(path.decode("utf-8", "replace"))
            return 1
        except Exception:
            return 0

    def _list_dir(self, _user, path, index, out_name, capacity, out_size) -> int:
        try:
            entries = self.list_dir(path.decode("utf-8", "replace"))
            if index >= len(entries):
                return 0

            encoded = entries[index].encode("utf-8") + b"\0"
            out_size[0] = len(encoded)
            if out_name and capacity >= len(encoded):
                C.memmove(out_name, encoded, len(encoded))
            return 1
        except Exception:
            return 0

    def _read_file(self, _user, path, out, capacity, out_size) -> int:
        try:
            data = self.read_file(path.decode("utf-8", "replace"))
            if data is None:
                return 0

            out_size[0] = len(data)
            if out and capacity >= len(data):
                C.memmove(out, data, len(data))
            return 1
        except Exception:
            return 0


def _utf8(value: str | Path | None) -> bytes | None:
    if value is None:
        return None
    return str(value).encode("utf-8")


@dataclass(frozen=True)
class SaveReport:
    files_written: int
    files_removed: int
    files_skipped: int
    bytes_written: int
    manifest_written: bool
    full_rewrite: bool

    @classmethod
    def _from_c(cls, raw: S.SaveReport) -> "SaveReport":
        return cls(
            files_written=raw.files_written,
            files_removed=raw.files_removed,
            files_skipped=raw.files_skipped,
            bytes_written=raw.bytes_written,
            manifest_written=bool(raw.manifest_written),
            full_rewrite=bool(raw.full_rewrite),
        )


@dataclass(frozen=True)
class ValidationIssue:
    severity: int
    code: int
    region_index: int
    chunk_ordinal: int
    voxel_key: int
    message: str

    @property
    def is_error(self) -> bool:
        return self.severity == S.SEVERITY_ERROR

    def __str__(self) -> str:
        name = {S.SEVERITY_INFO: "info", S.SEVERITY_WARNING: "warning", S.SEVERITY_ERROR: "error"}.get(self.severity, "?")
        where = ""
        if self.region_index >= 0:
            where = f" [region {self.region_index}"
            where += f", chunk {self.chunk_ordinal}]" if self.chunk_ordinal >= 0 else "]"
        return f"{name}: {self.message}{where}"


class Context:
    """One per thread. Carries the last error, warnings, validation issues and the progress hook."""

    def __init__(self) -> None:
        handle = L.context_create()
        if not handle:
            raise BsvxError("bsvx_context_create returned NULL")
        self._handle = C.c_void_p(handle)
        # ctypes does not keep the trampoline alive on its own; dropping it would leave the
        # library holding a dangling function pointer.
        self._progress_trampoline: Any = None

    @property
    def handle(self) -> C.c_void_p:
        return self._handle

    def close(self) -> None:
        if getattr(self, "_handle", None) and self._handle.value:
            L.context_destroy(self._handle)
            self._handle = C.c_void_p(0)
        self._progress_trampoline = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "Context":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    @property
    def last_error(self) -> str:
        return L.context_last_error(self._handle).decode("utf-8", "replace")

    @property
    def warnings(self) -> list[str]:
        count = L.context_warning_count(self._handle)
        return [L.context_warning(self._handle, i).decode("utf-8", "replace") for i in range(count)]

    def set_progress(self, callback: Callable[[str, int, int], bool] | None) -> None:
        """``callback(stage, done, total) -> keep_going``. Return False to cancel.

        An exception escaping a ctypes callback crashes the interpreter, so anything raised inside
        is swallowed and treated as a cancel request.
        """
        if callback is None:
            L.context_set_progress(self._handle, S.PROGRESS_FN(), None)
            self._progress_trampoline = None
            return

        def trampoline(_user: int, stage: bytes, done: int, total: int) -> int:
            try:
                return 1 if callback(stage.decode("utf-8", "replace"), done, total) else 0
            except Exception:
                return 0

        self._progress_trampoline = S.PROGRESS_FN(trampoline)
        L.context_set_progress(self._handle, self._progress_trampoline, None)

    def check(self, result: int, what: str) -> None:
        if result == S.RESULT_OK:
            return
        detail = self.last_error or L.result_string(result).decode("utf-8", "replace")
        raise BsvxError(f"{what}: {detail}", result)


def _string_out(call: Callable[[C.c_char_p, int, Any], int], what: str, ctx: Context | None = None) -> str:
    """Runs the two-step size probe every string getter in the ABI uses."""
    needed = C.c_size_t(0)
    result = call(None, 0, C.byref(needed))
    if result == S.RESULT_NOT_FOUND:
        raise BsvxError(f"{what}: not found", result)
    if result not in (S.RESULT_OK, S.RESULT_BUFFER_TOO_SMALL):
        if ctx:
            ctx.check(result, what)
        raise BsvxError(f"{what}: {L.result_string(result).decode()}", result)
    if needed.value == 0:
        return ""

    buffer = C.create_string_buffer(needed.value)
    result = call(buffer, needed.value, C.byref(needed))
    if result != S.RESULT_OK:
        raise BsvxError(f"{what}: {L.result_string(result).decode()}", result)
    return buffer.value.decode("utf-8", "replace")


def _bytes_out(call: Callable[[Any, int, Any], int], what: str) -> bytes:
    needed = C.c_size_t(0)
    result = call(None, 0, C.byref(needed))
    if result == S.RESULT_NOT_FOUND:
        raise BsvxError(f"{what}: not found", result)
    if result not in (S.RESULT_OK, S.RESULT_BUFFER_TOO_SMALL):
        raise BsvxError(f"{what}: {L.result_string(result).decode()}", result)
    if needed.value == 0:
        return b""

    buffer = (C.c_ubyte * needed.value)()
    result = call(buffer, needed.value, C.byref(needed))
    if result != S.RESULT_OK:
        raise BsvxError(f"{what}: {L.result_string(result).decode()}", result)
    return bytes(buffer)


class TextureBuilder:
    """Assembles a ``.btx`` archive. Only RGBA8 UNORM/SRGB 2D textures and arrays are supported."""

    def __init__(self, ctx: Context) -> None:
        self._ctx = ctx
        handle = C.c_void_p()
        ctx.check(L.texture_builder_create(ctx.handle, C.byref(handle)), "texture_builder_create")
        self._handle = handle

    @classmethod
    def open(cls, ctx: Context, path: str | Path) -> "TextureBuilder":
        handle = C.c_void_p()
        ctx.check(L.texture_builder_open(ctx.handle, _utf8(path), C.byref(handle)), f"texture_builder_open({path})")
        builder = cls.__new__(cls)
        builder._ctx = ctx
        builder._handle = handle
        return builder

    @property
    def handle(self) -> C.c_void_p:
        return self._handle

    def close(self) -> None:
        if getattr(self, "_handle", None) and self._handle.value:
            L.texture_builder_destroy(self._handle)
            self._handle = C.c_void_p(0)

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "TextureBuilder":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def add_sampler(self, desc: S.SamplerDesc | None = None) -> int:
        desc = desc or S.SamplerDesc(max_anisotropy_x100=100)
        out = C.c_uint32()
        self._ctx.check(L.texture_builder_add_sampler(self._ctx.handle, self._handle, C.byref(desc), C.byref(out)), "add_sampler")
        return out.value

    def add_texture(
        self,
        width: int,
        height: int,
        *,
        vk_format: int = S.VK_FORMAT_R8G8B8A8_SRGB,
        array_layers: int = 1,
        mip_levels: int = 1,
        sampler_id: int = 0,
        name_hash: int = 0,
    ) -> int:
        desc = S.TextureDesc(
            kind=0,
            vk_format=vk_format,
            width=width,
            height=height,
            depth=1,
            array_layers=array_layers,
            mip_levels=mip_levels,
            sampler_id=sampler_id,
            name_hash=name_hash,
        )
        out = C.c_uint32()
        self._ctx.check(L.texture_builder_add_texture(self._ctx.handle, self._handle, C.byref(desc), C.byref(out)), "add_texture")
        return out.value

    def add_material(self, desc: S.MaterialDesc) -> int:
        out = C.c_uint32()
        self._ctx.check(L.texture_builder_add_material(self._ctx.handle, self._handle, C.byref(desc), C.byref(out)), "add_material")
        return out.value

    def append_layer(self, texture_id: int, layer: int, texels: bytes, width: int, height: int, *, mip_level: int = 0) -> int:
        buffer = (C.c_ubyte * len(texels)).from_buffer_copy(texels)
        out = C.c_uint32()
        self._ctx.check(
            L.texture_builder_append_subresource(
                self._ctx.handle, self._handle, texture_id, mip_level, layer, width, height, buffer, len(texels), 0, 0, C.byref(out)
            ),
            "append_subresource",
        )
        return out.value

    def generate_mips(self, texture_id: int) -> None:
        self._ctx.check(L.texture_builder_generate_mips(self._ctx.handle, self._handle, texture_id), "generate_mips")

    def validate(self) -> list[str]:
        count = C.c_size_t(0)
        text = _string_out(
            lambda buf, cap, size: L.texture_builder_validate(self._ctx.handle, self._handle, buf, cap, size, C.byref(count)),
            "texture_builder_validate",
        )
        return [line for line in text.splitlines() if line]

    def save(self, path: str | Path, *, atomic: bool = True, backup: bool = False) -> None:
        flags = (0 if atomic else S.SAVE_NON_ATOMIC) | (S.SAVE_BACKUP if backup else 0)
        self._ctx.check(L.texture_builder_save(self._ctx.handle, self._handle, _utf8(path), flags), f"texture_builder_save({path})")

    def to_bytes(self) -> bytes:
        return _bytes_out(
            lambda buf, cap, size: L.texture_builder_save_memory(self._ctx.handle, self._handle, buf, cap, size), "texture_builder_save_memory"
        )

    @staticmethod
    def solid_color_palette(ctx: Context, colors: Sequence[tuple[int, int, int, int]], *, texel_size: int = 1) -> "TextureBuilder":
        """A palette texture: one array layer per colour, plus a material per colour.

        This is the shortest path from "the user picked some colours" to a world that renders --
        material ``i`` corresponds to ``colors[i]``, so a registry entry's ``material_id`` indexes
        straight into the palette.
        """
        builder = TextureBuilder(ctx)
        builder.add_sampler()
        texture_id = builder.add_texture(texel_size, texel_size, array_layers=max(1, len(colors)))

        for layer, (r, g, b, a) in enumerate(colors):
            texels = bytes([r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF]) * (texel_size * texel_size)
            builder.append_layer(texture_id, layer, texels, texel_size, texel_size)

        for layer, (r, g, b, a) in enumerate(colors):
            material = S.MaterialDesc(albedo_texture_id=texture_id, tint_rgba8=(r << 24) | (g << 16) | (b << 8) | a)
            for face in range(6):
                material.albedo_layer[face] = layer
            builder.add_material(material)

        return builder


class World:
    """A loaded or authored world. Not safe to mutate from two threads at once."""

    def __init__(self, ctx: Context, handle: C.c_void_p) -> None:
        self._ctx = ctx
        self._handle = handle

    # --- lifetime -----------------------------------------------------------------------------

    @classmethod
    def load(
        cls,
        path: str | Path,
        ctx: Context | None = None,
        *,
        ignore_hash_mismatch: bool = False,
        skip_textures: bool = False,
        skip_regions: bool = False,
    ) -> "World":
        ctx = ctx or Context()
        flags = 0
        if ignore_hash_mismatch:
            flags |= S.LOAD_IGNORE_HASH_MISMATCH
        if skip_textures:
            flags |= S.LOAD_SKIP_TEXTURES
        if skip_regions:
            flags |= S.LOAD_SKIP_REGIONS

        handle = C.c_void_p()
        ctx.check(L.world_load_ex2(ctx.handle, _utf8(path), flags, C.byref(handle)), f"world_load({path})")
        return cls(ctx, handle)

    @classmethod
    def load_region(cls, path: str | Path, ctx: Context | None = None) -> "World":
        ctx = ctx or Context()
        handle = C.c_void_p()
        ctx.check(L.world_load_region(ctx.handle, _utf8(path), C.byref(handle)), f"world_load_region({path})")
        return cls(ctx, handle)

    @classmethod
    def create(
        cls,
        chunk_size: tuple[int, int, int] = (16, 16, 16),
        region_size: tuple[int, int, int] = (16, 16, 16),
        ctx: Context | None = None,
    ) -> "World":
        ctx = ctx or Context()
        geometry = S.GeometryDesc(*chunk_size, *region_size)
        handle = C.c_void_p()
        ctx.check(L.world_create(ctx.handle, C.byref(geometry), C.byref(handle)), "world_create")
        return cls(ctx, handle)

    def close(self) -> None:
        if getattr(self, "_handle", None) and self._handle.value:
            L.world_destroy(self._handle)
            self._handle = C.c_void_p(0)

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self) -> "World":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    @property
    def context(self) -> Context:
        return self._ctx

    @property
    def warnings(self) -> list[str]:
        return self._ctx.warnings

    def _check(self, result: int, what: str) -> None:
        self._ctx.check(result, what)

    # --- description --------------------------------------------------------------------------

    @property
    def geometry(self) -> S.GeometryDesc:
        out = S.GeometryDesc()
        self._check(L.world_geometry(self._handle, C.byref(out)), "world_geometry")
        return out

    @property
    def desc(self) -> S.WorldDesc:
        out = S.WorldDesc()
        self._check(L.world_get_desc(self._handle, C.byref(out)), "world_get_desc")
        return out

    @property
    def name(self) -> str:
        return _string_out(lambda buf, cap, size: L.world_get_name(self._handle, buf, cap, size), "world_get_name")

    @name.setter
    def name(self, value: str) -> None:
        self._check(L.world_set_name(self._handle, _utf8(value)), "world_set_name")

    @property
    def uuid(self) -> str:
        return _string_out(lambda buf, cap, size: L.world_get_uuid(self._handle, buf, cap, size), "world_get_uuid")

    @uuid.setter
    def uuid(self, value: str) -> None:
        self._check(L.world_set_uuid(self._handle, _utf8(value)), "world_set_uuid")

    @property
    def units(self) -> S.Units:
        out = S.Units()
        self._check(L.world_get_units(self._handle, C.byref(out)), "world_get_units")
        return out

    def set_units(self, voxel_size: float | tuple[float, float, float] = 1.0, origin: tuple[float, float, float] = (0.0, 0.0, 0.0)) -> None:
        sizes = (voxel_size, voxel_size, voxel_size) if isinstance(voxel_size, (int, float)) else tuple(voxel_size)
        units = S.Units(*sizes, *origin)
        self._check(L.world_set_units(self._handle, C.byref(units)), "world_set_units")

    @property
    def source_path(self) -> str:
        return _string_out(lambda buf, cap, size: L.world_get_source_path(self._handle, buf, cap, size), "world_get_source_path")

    @property
    def root_dir(self) -> str:
        return _string_out(lambda buf, cap, size: L.world_get_root_dir(self._handle, buf, cap, size), "world_get_root_dir")

    def set_paths(self, regions_dir: str | None = None, textures_dir: str | None = None) -> None:
        self._check(L.world_set_paths(self._handle, _utf8(regions_dir), _utf8(textures_dir)), "world_set_paths")

    # --- registry ------------------------------------------------------------------------------

    @property
    def registry(self) -> list[S.RegistryEntry]:
        count = L.world_registry_entry_count(self._handle)
        if count == 0:
            return []
        array = (S.RegistryEntry * count)()
        written = C.c_size_t(0)
        self._check(L.world_get_registry_entries(self._handle, 0, count, array, C.byref(written)), "world_get_registry_entries")
        return list(array[: written.value])

    def set_registry_entry(self, voxel_key: int, material_id: int = 0, flags: int = 0, name: str | None = None) -> None:
        entry = S.RegistryEntry(voxel_key=voxel_key, material_id=material_id, flags=flags, name_hash=0)
        self._check(L.world_set_registry_entry(self._handle, C.byref(entry)), f"world_set_registry_entry({voxel_key})")
        if name is not None:
            self.set_registry_name(voxel_key, name)

    def remove_registry_entry(self, voxel_key: int) -> None:
        self._check(L.world_remove_registry_entry(self._handle, voxel_key), f"world_remove_registry_entry({voxel_key})")

    def registry_name(self, voxel_key: int) -> str:
        try:
            return _string_out(lambda buf, cap, size: L.world_get_registry_name(self._handle, voxel_key, buf, cap, size), "world_get_registry_name")
        except BsvxError as exc:
            if exc.result == S.RESULT_NOT_FOUND:
                return ""
            raise

    def set_registry_name(self, voxel_key: int, name: str) -> None:
        self._check(L.world_set_registry_name(self._handle, voxel_key, _utf8(name)), f"world_set_registry_name({voxel_key})")

    # --- regions --------------------------------------------------------------------------------

    @property
    def region_count(self) -> int:
        return L.world_region_count(self._handle)

    def add_region(self, x: int, y: int, z: int) -> int:
        out = C.c_size_t(0)
        self._check(L.world_add_region(self._handle, x, y, z, C.byref(out)), f"world_add_region({x},{y},{z})")
        return out.value

    def find_region(self, x: int, y: int, z: int) -> int | None:
        out = C.c_size_t(0)
        result = L.world_find_region(self._handle, x, y, z, C.byref(out))
        return out.value if result == S.RESULT_OK else None

    def remove_region(self, region_index: int) -> None:
        """Note that every later region index shifts down by one."""
        self._check(L.world_remove_region(self._handle, region_index), f"world_remove_region({region_index})")

    def prune_empty_regions(self) -> int:
        out = C.c_size_t(0)
        self._check(L.world_prune_empty_regions(self._handle, C.byref(out)), "world_prune_empty_regions")
        return out.value

    def region_coord(self, region_index: int) -> tuple[int, int, int]:
        x, y, z = C.c_int32(), C.c_int32(), C.c_int32()
        self._check(L.world_get_region_coord(self._handle, region_index, C.byref(x), C.byref(y), C.byref(z)), "world_get_region_coord")
        return (x.value, y.value, z.value)

    def region_path(self, region_index: int) -> str:
        return _string_out(lambda buf, cap, size: L.world_get_region_path(self._handle, region_index, buf, cap, size), "world_get_region_path")

    def chunk_count(self, region_index: int) -> int:
        return L.region_chunk_count(self._handle, region_index)

    def chunk_infos(self, region_index: int) -> list[S.ChunkInfo]:
        """Every chunk's map entry and summary in one crossing of the FFI boundary."""
        count = L.region_chunk_count(self._handle, region_index)
        if count == 0:
            return []
        array = (S.ChunkInfo * count)()
        written = C.c_size_t(0)
        self._check(L.region_get_chunk_infos(self._handle, region_index, 0, count, array, C.byref(written)), "region_get_chunk_infos")
        return list(array[: written.value])

    def remove_chunk(self, region_index: int, cx: int, cy: int, cz: int) -> None:
        self._check(L.region_remove_chunk(self._handle, region_index, cx, cy, cz), "region_remove_chunk")

    def clear_chunk(self, region_index: int, cx: int, cy: int, cz: int) -> None:
        self._check(L.region_clear_chunk(self._handle, region_index, cx, cy, cz), "region_clear_chunk")

    def chunk_content_hash(self, region_index: int, cx: int, cy: int, cz: int) -> int:
        out = C.c_uint64(0)
        self._check(L.region_chunk_content_hash(self._handle, region_index, cx, cy, cz, C.byref(out)), "region_chunk_content_hash")
        return out.value

    # --- voxels ----------------------------------------------------------------------------------

    def decode_chunk(self, region_index: int, cx: int, cy: int, cz: int) -> Sequence[int]:
        count = L.region_required_voxel_count(self._handle, region_index)
        buffer = (C.c_uint32 * count)()
        written = C.c_size_t(0)
        self._check(
            L.region_decode_chunk_u32_ex(self._ctx.handle, self._handle, region_index, cx, cy, cz, buffer, count, C.byref(written)),
            f"decode_chunk({cx},{cy},{cz})",
        )
        return buffer

    def set_chunk(self, region_index: int, cx: int, cy: int, cz: int, voxels: Sequence[int], codec: int = S.CODEC_AUTO) -> None:
        count = L.region_required_voxel_count(self._handle, region_index)
        if len(voxels) != count:
            raise BsvxError(f"set_chunk expects {count} voxels, got {len(voxels)}", S.RESULT_INVALID_ARGUMENT)
        buffer = (C.c_uint32 * count)(*voxels) if not isinstance(voxels, C.Array) else voxels
        self._check(
            L.region_set_chunk_u32_ex(self._ctx.handle, self._handle, region_index, cx, cy, cz, buffer, count, codec),
            f"set_chunk({cx},{cy},{cz})",
        )

    def decode_region(self, region_index: int, layout: int = S.LAYOUT_CHUNK_ORDER) -> Sequence[int]:
        needed = C.c_size_t(0)
        L.region_decode_all_u32(self._ctx.handle, self._handle, region_index, layout, None, 0, C.byref(needed))
        buffer = (C.c_uint32 * needed.value)()
        self._check(
            L.region_decode_all_u32(self._ctx.handle, self._handle, region_index, layout, buffer, needed.value, C.byref(needed)),
            "region_decode_all_u32",
        )
        return buffer

    def locate_voxel(self, x: int, y: int, z: int) -> S.VoxelAddress:
        out = S.VoxelAddress()
        self._check(L.world_locate_voxel(self._handle, x, y, z, C.byref(out)), "world_locate_voxel")
        return out

    def set_voxels(self, coords: Iterable[tuple[int, int, int]], keys: Iterable[int] | int, *, create_missing: bool = True) -> int:
        """Scattered world-space writes, batched by chunk inside the library.

        ``keys`` may be one value applied to every coordinate, or one value per coordinate.
        """
        coords = list(coords)
        if not coords:
            return 0
        key_list = [keys] * len(coords) if isinstance(keys, int) else list(keys)
        if len(key_list) != len(coords):
            raise BsvxError("set_voxels: coords and keys differ in length", S.RESULT_INVALID_ARGUMENT)

        count = len(coords)
        xs = (C.c_int64 * count)(*(c[0] for c in coords))
        ys = (C.c_int64 * count)(*(c[1] for c in coords))
        zs = (C.c_int64 * count)(*(c[2] for c in coords))
        ks = (C.c_uint32 * count)(*key_list)

        out = C.c_size_t(0)
        self._check(
            L.world_set_voxels(self._ctx.handle, self._handle, xs, ys, zs, ks, count, 1 if create_missing else 0, C.byref(out)),
            "world_set_voxels",
        )
        return out.value

    def get_voxels(self, coords: Iterable[tuple[int, int, int]]) -> list[int]:
        coords = list(coords)
        if not coords:
            return []

        count = len(coords)
        xs = (C.c_int64 * count)(*(c[0] for c in coords))
        ys = (C.c_int64 * count)(*(c[1] for c in coords))
        zs = (C.c_int64 * count)(*(c[2] for c in coords))
        out = (C.c_uint32 * count)()

        self._check(L.world_get_voxels(self._ctx.handle, self._handle, xs, ys, zs, out, count), "world_get_voxels")
        return list(out)

    def fill_box(self, minimum: tuple[int, int, int], maximum: tuple[int, int, int], key: int, *, create_missing: bool = True) -> int:
        out = C.c_size_t(0)
        self._check(
            L.world_fill_box(self._ctx.handle, self._handle, *minimum, *maximum, key, 1 if create_missing else 0, C.byref(out)),
            "world_fill_box",
        )
        return out.value

    # --- metadata ----------------------------------------------------------------------------------

    def set_metadata(self, key: str, value: bytes | str) -> None:
        raw = value.encode("utf-8") if isinstance(value, str) else bytes(value)
        buffer = (C.c_ubyte * len(raw)).from_buffer_copy(raw) if raw else None
        self._check(L.world_set_metadata(self._handle, _utf8(key), buffer, len(raw)), f"world_set_metadata({key})")

    def get_metadata(self, key: str, default: bytes | None = None) -> bytes | None:
        try:
            return _bytes_out(lambda buf, cap, size: L.world_get_metadata(self._handle, _utf8(key), buf, cap, size), "world_get_metadata")
        except BsvxError as exc:
            if exc.result == S.RESULT_NOT_FOUND:
                return default
            raise

    def remove_metadata(self, key: str) -> None:
        self._check(L.world_remove_metadata(self._handle, _utf8(key)), f"world_remove_metadata({key})")

    def metadata_keys(self) -> list[str]:
        count = L.world_metadata_count(self._handle)
        return [
            _string_out(lambda buf, cap, size, i=i: L.world_get_metadata_key(self._handle, i, buf, cap, size), "world_get_metadata_key")
            for i in range(count)
        ]

    def set_region_metadata(self, region_index: int, key: str, value: bytes | str) -> None:
        raw = value.encode("utf-8") if isinstance(value, str) else bytes(value)
        buffer = (C.c_ubyte * len(raw)).from_buffer_copy(raw) if raw else None
        self._check(L.region_set_metadata(self._handle, region_index, _utf8(key), buffer, len(raw)), "region_set_metadata")

    def get_region_metadata(self, region_index: int, key: str, default: bytes | None = None) -> bytes | None:
        try:
            return _bytes_out(
                lambda buf, cap, size: L.region_get_metadata(self._handle, region_index, _utf8(key), buf, cap, size), "region_get_metadata"
            )
        except BsvxError as exc:
            if exc.result == S.RESULT_NOT_FOUND:
                return default
            raise

    # --- textures ------------------------------------------------------------------------------------

    @property
    def texture_count(self) -> int:
        return L.world_texture_count(self._handle)

    def texture_id(self, tex_index: int) -> str:
        return _string_out(lambda buf, cap, size: L.world_get_texture_id(self._handle, tex_index, buf, cap, size), "world_get_texture_id")

    def add_texture(self, builder: TextureBuilder, texture_id: str, relative_path: str | None = None) -> int:
        out = C.c_size_t(0)
        self._check(
            L.world_add_texture(self._ctx.handle, self._handle, _utf8(texture_id), _utf8(relative_path), builder.handle, C.byref(out)),
            f"world_add_texture({texture_id})",
        )
        return out.value

    def remove_texture(self, tex_index: int) -> None:
        self._check(L.world_remove_texture(self._handle, tex_index), f"world_remove_texture({tex_index})")

    def material(self, tex_index: int, material_id: int) -> S.MaterialDesc:
        index = C.c_size_t(0)
        self._check(L.texture_find_material(self._handle, tex_index, material_id, C.byref(index)), "texture_find_material")
        out = S.MaterialDesc()
        self._check(L.texture_get_material(self._handle, tex_index, index.value, C.byref(out)), "texture_get_material")
        return out

    # --- validation ------------------------------------------------------------------------------------

    def validate(self, *, deep: bool = False) -> list[ValidationIssue]:
        count = C.c_size_t(0)
        flags = S.VALIDATE_DEEP if deep else 0
        self._check(L.world_validate(self._ctx.handle, self._handle, flags, C.byref(count)), "world_validate")

        issues: list[ValidationIssue] = []
        for i in range(count.value):
            raw = S.ValidationIssue()
            self._check(L.world_get_validation_issue(self._ctx.handle, i, C.byref(raw)), "world_get_validation_issue")
            message = _string_out(
                lambda buf, cap, size, index=i: L.world_get_validation_message(self._ctx.handle, index, buf, cap, size),
                "world_get_validation_message",
            )
            issues.append(
                ValidationIssue(
                    severity=raw.severity,
                    code=raw.code,
                    region_index=raw.region_index,
                    chunk_ordinal=raw.chunk_ordinal,
                    voxel_key=raw.voxel_key,
                    message=message,
                )
            )
        return issues

    # --- saving ------------------------------------------------------------------------------------------

    @property
    def is_dirty(self) -> bool:
        return bool(L.world_is_dirty(self._handle))

    def region_is_dirty(self, region_index: int) -> bool:
        return bool(L.region_is_dirty(self._handle, region_index))

    def compact(self) -> int:
        out = C.c_size_t(0)
        self._check(L.world_compact(self._handle, C.byref(out)), "world_compact")
        return out.value

    def save(
        self,
        path: str | Path,
        *,
        atomic: bool = True,
        backup: bool = False,
        prune_orphans: bool = False,
        dirty_only: bool = False,
        compact_first: bool = False,
        dry_run: bool = False,
    ) -> SaveReport:
        flags = 0
        if not atomic:
            flags |= S.SAVE_NON_ATOMIC
        if backup:
            flags |= S.SAVE_BACKUP
        if prune_orphans:
            flags |= S.SAVE_PRUNE_ORPHANS
        if dirty_only:
            flags |= S.SAVE_DIRTY_ONLY
        if compact_first:
            flags |= S.SAVE_COMPACT_FIRST
        if dry_run:
            flags |= S.SAVE_DRY_RUN

        report = S.SaveReport()
        self._check(L.world_save_ex2(self._ctx.handle, self._handle, _utf8(path), flags, C.byref(report)), f"world_save({path})")
        return SaveReport._from_c(report)

    def save_dirty(self) -> SaveReport:
        """Writes back to the path this world came from, touching only what changed."""
        report = S.SaveReport()
        self._check(L.world_save_dirty(self._ctx.handle, self._handle, C.byref(report)), "world_save_dirty")
        return SaveReport._from_c(report)

    def save_region(self, path: str | Path) -> None:
        self._check(L.world_save_region_ex(self._ctx.handle, self._handle, _utf8(path)), f"world_save_region({path})")

    def manifest_text(self) -> str:
        return _string_out(
            lambda buf, cap, size: L.world_save_manifest_memory(self._ctx.handle, self._handle, buf, cap, size), "world_save_manifest_memory"
        )

    def rehash(self) -> int:
        """Clears every region's manifest/registry stamp so the next save re-issues them."""
        out = C.c_size_t(0)
        self._check(L.world_rehash(self._handle, C.byref(out)), "world_rehash")
        return out.value

    def save_vfs(self, path: str, writer: VfsWriter, **kwargs: bool) -> SaveReport:
        """Saves through host callbacks instead of the filesystem — into an archive, a pack file,
        anywhere ``std::filesystem`` cannot reach. ``path`` keeps its scheme (``res://``, …)."""
        flags = 0
        if kwargs.get("non_atomic"):
            flags |= S.SAVE_NON_ATOMIC
        if kwargs.get("backup"):
            flags |= S.SAVE_BACKUP
        if kwargs.get("prune_orphans"):
            flags |= S.SAVE_PRUNE_ORPHANS
        if kwargs.get("dirty_only"):
            flags |= S.SAVE_DIRTY_ONLY
        if kwargs.get("dry_run"):
            flags |= S.SAVE_DRY_RUN

        report = S.SaveReport()
        self._check(
            L.world_save_vfs(self._ctx.handle, self._handle, _utf8(path), C.byref(writer._struct), flags, C.byref(report)),
            f"world_save_vfs({path})",
        )
        return SaveReport._from_c(report)

    # --- axis conventions, colours, cloning ---------------------------------------------------

    @property
    def axis_convention(self) -> int:
        return self.desc.axis_convention

    def convert_axis_convention(self, target: int) -> int:
        """Rewrites every voxel into another convention, re-deriving the region and chunk
        decomposition. Baked payload sections are dropped — they describe the old frame — and every
        region index is invalidated. Returns the number of voxels moved."""
        out = C.c_size_t(0)
        self._check(
            L.world_convert_axis_convention(self._ctx.handle, self._handle, target, C.byref(out)),
            f"world_convert_axis_convention({target})",
        )
        return out.value

    def registry_color(self, voxel_key: int) -> int:
        """0xRRGGBBAA, or 0 when unset."""
        out = C.c_uint32(0)
        result = L.world_get_registry_color(self._handle, voxel_key, C.byref(out))
        if result == S.RESULT_NOT_FOUND:
            return 0
        self._check(result, f"world_get_registry_color({voxel_key})")
        return out.value

    def set_registry_color(self, voxel_key: int, rgba8: int) -> None:
        self._check(L.world_set_registry_color(self._handle, voxel_key, rgba8), f"world_set_registry_color({voxel_key})")

    def make_palette(self, colors: Sequence[int | tuple[int, int, int, int]], texture_id: str = "palette", flags: int = S.REGISTRY_OPAQUE) -> int:
        """The whole colour-first path in one call: a texture with one layer per colour, one
        material each, and registry entries keyed from 1 carrying those colours."""
        packed = [
            c if isinstance(c, int) else ((c[0] & 0xFF) << 24) | ((c[1] & 0xFF) << 16) | ((c[2] & 0xFF) << 8) | (c[3] & 0xFF)
            for c in colors
        ]
        if not packed:
            raise BsvxError("make_palette: no colours given", S.RESULT_INVALID_ARGUMENT)

        array = (C.c_uint32 * len(packed))(*packed)
        out = C.c_size_t(0)
        self._check(
            L.world_make_palette(self._ctx.handle, self._handle, array, len(packed), _utf8(texture_id), flags, C.byref(out)),
            "world_make_palette",
        )
        return out.value

    def clone(self) -> "World":
        """A deep copy sharing nothing. A bsvx_world allows one writer or many readers and has no
        internal locking, so clone before handing one to a background thread."""
        handle = C.c_void_p()
        self._check(L.world_clone(self._ctx.handle, self._handle, C.byref(handle)), "world_clone")
        return World(self._ctx, handle)

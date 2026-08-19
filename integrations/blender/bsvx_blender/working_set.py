"""What a voxel *is* in Blender, and how it gets back out again.

Blender has no datablock that is a uint32 voxel grid, so something has to be chosen. This add-on
chooses: **a voxel is a vertex.**

* Its position is the voxel cell's centre, in Blender-space metres (see ``frame.py``).
* Its voxel key is an integer attribute, ``bsvx_key``, on the POINT domain.
* Its palette colour is a byte-colour attribute, ``bsvx_color``, also on POINT -- display only, and
  rebuilt from the registry on every checkout. The registry is authoritative, never this.

The alternative -- a cube of eight vertices and six faces per voxel -- was rejected on both counts
that matter. It is twenty-four times the data for the same information, and it is *ambiguous on
read-back*: after a user has merged, extruded or dissolved anything, no rule recovers "which voxels
did they mean". A vertex is a bijection with a cell, and flooring its position is the whole of the
inverse.

Because the world can be far larger than any Blender mesh, geometry is a **checked-out working
set**, not the world itself: a bounded box of voxels materialized for editing. The object records
the box it owns, and committing *replaces* that box -- which is what makes deleting a vertex mean
deleting a voxel, rather than meaning nothing at all.
"""

from __future__ import annotations

import numpy as np

KEY_ATTRIBUTE = "bsvx_key"
COLOR_ATTRIBUTE = "bsvx_color"

PROP_KIND = "bsvx_kind"
PROP_SESSION = "bsvx_session"
PROP_BOUNDS_MIN = "bsvx_bounds_min"
PROP_BOUNDS_MAX = "bsvx_bounds_max"
PROP_REGION = "bsvx_region"

KIND_WORKING_SET = "working_set"


class WorkingSetError(RuntimeError):
    pass


# ----------------------------------------------------------------------------------------------
# Reading voxels out of a world
# ----------------------------------------------------------------------------------------------


def region_extent(world) -> np.ndarray:
    geometry = world.geometry
    return np.array(
        [
            geometry.chunk_size_x * geometry.region_size_x,
            geometry.chunk_size_y * geometry.region_size_y,
            geometry.chunk_size_z * geometry.region_size_z,
        ],
        dtype=np.int64,
    )


def collect_region(world, region_index: int) -> tuple[np.ndarray, np.ndarray]:
    """Every non-air voxel of one region, as (cells (N,3) int64, keys (N,) uint32).

    Goes through the region-linear bulk decode rather than one call per chunk: a populated region
    holds thousands of chunks, and the per-call cost is what makes the naive version take minutes.
    """
    api = _api()
    extent = region_extent(world)
    raw = world.decode_region(region_index, api.LAYOUT_REGION_LINEAR)

    dense = np.ctypeslib.as_array(raw)
    expected = int(extent[0] * extent[1] * extent[2])
    if dense.size != expected:
        raise WorkingSetError(f"region decode returned {dense.size} voxels, expected {expected}")

    flat = np.flatnonzero(dense)
    if flat.size == 0:
        return np.zeros((0, 3), dtype=np.int64), np.zeros((0,), dtype=np.uint32)

    # Region-linear order is wx + ex * (wy + ey * wz).
    ex, ey = int(extent[0]), int(extent[1])
    wx = flat % ex
    rest = flat // ex
    wy = rest % ey
    wz = rest // ey

    origin = np.asarray(world.region_coord(region_index), dtype=np.int64) * extent
    cells = np.stack([wx, wy, wz], axis=1).astype(np.int64) + origin
    return cells, dense[flat].astype(np.uint32)


def collect_box(world, lo, hi) -> tuple[np.ndarray, np.ndarray]:
    """Every non-air voxel inside the inclusive cell box, gathered region by region."""
    lo = np.asarray(lo, dtype=np.int64)
    hi = np.asarray(hi, dtype=np.int64)
    if np.any(hi < lo):
        raise WorkingSetError("box maximum is below its minimum")

    cells_out: list[np.ndarray] = []
    keys_out: list[np.ndarray] = []

    for region_index in range(world.region_count):
        cells, keys = collect_region(world, region_index)
        if cells.size == 0:
            continue
        inside = np.all((cells >= lo) & (cells <= hi), axis=1)
        if not inside.any():
            continue
        cells_out.append(cells[inside])
        keys_out.append(keys[inside])

    if not cells_out:
        return np.zeros((0, 3), dtype=np.int64), np.zeros((0,), dtype=np.uint32)
    return np.concatenate(cells_out), np.concatenate(keys_out)


def world_bounds(world) -> tuple[np.ndarray, np.ndarray] | None:
    """The inclusive cell box spanned by every region the world holds, or None when it holds none."""
    if world.region_count == 0:
        return None
    extent = region_extent(world)
    coords = np.array([world.region_coord(i) for i in range(world.region_count)], dtype=np.int64)
    return coords.min(axis=0) * extent, (coords.max(axis=0) + 1) * extent - 1


# ----------------------------------------------------------------------------------------------
# Palette
# ----------------------------------------------------------------------------------------------


def palette_lookup(world) -> dict[int, tuple[float, float, float, float]]:
    """voxel key -> linear RGBA, resolved the way the format defines it.

    A material tint from an attached .btx wins; the registry colour is the fallback that lets a
    palette mean something before a world has any texture archive at all.
    """
    colors: dict[int, tuple[float, float, float, float]] = {}
    has_textures = world.texture_count > 0

    for entry in world.registry:
        rgba = 0
        if has_textures:
            try:
                material = world.material(0, entry.material_id)
                rgba = material.tint_rgba8
            except Exception:
                rgba = 0
        if rgba == 0:
            try:
                rgba = world.registry_color(entry.voxel_key)
            except Exception:
                rgba = 0
        if rgba == 0:
            continue
        colors[entry.voxel_key] = (
            ((rgba >> 24) & 0xFF) / 255.0,
            ((rgba >> 16) & 0xFF) / 255.0,
            ((rgba >> 8) & 0xFF) / 255.0,
            (rgba & 0xFF) / 255.0,
        )
    return colors


def _colors_for(keys: np.ndarray, lookup: dict) -> np.ndarray:
    out = np.full((keys.size, 4), 0.8, dtype=np.float32)
    out[:, 3] = 1.0
    if not lookup:
        return out
    highest = max(lookup)
    table = np.full((highest + 1, 4), 0.8, dtype=np.float32)
    table[:, 3] = 1.0
    for key, rgba in lookup.items():
        table[key] = rgba
    in_range = keys <= highest
    out[in_range] = table[keys[in_range]]
    return out


# ----------------------------------------------------------------------------------------------
# Building the Blender object
# ----------------------------------------------------------------------------------------------


def build_object(name: str, cells: np.ndarray, keys: np.ndarray, frame, palette: dict) -> object:
    import bpy

    mesh = bpy.data.meshes.new(name)
    count = int(cells.shape[0])

    if count:
        positions = frame.cells_to_points(cells).astype(np.float32)
        mesh.vertices.add(count)
        mesh.vertices.foreach_set("co", positions.ravel())

    mesh.update()

    key_attr = mesh.attributes.new(name=KEY_ATTRIBUTE, type="INT", domain="POINT")
    if count:
        # Keys are uint32 and the attribute is int32. The reinterpretation is bit-exact, so a key
        # above 2^31 reads back negative rather than wrong; commit() undoes it the same way.
        key_attr.data.foreach_set("value", keys.astype(np.int32).ravel())

    color_attr = mesh.attributes.new(name=COLOR_ATTRIBUTE, type="BYTE_COLOR", domain="POINT")
    if count:
        color_attr.data.foreach_set("color", _colors_for(keys, palette).ravel())

    obj = bpy.data.objects.new(name, mesh)
    obj[PROP_KIND] = KIND_WORKING_SET
    return obj


def tag_bounds(obj, lo, hi, region_index: int | None = None) -> None:
    obj[PROP_BOUNDS_MIN] = [int(v) for v in lo]
    obj[PROP_BOUNDS_MAX] = [int(v) for v in hi]
    if region_index is None:
        obj.pop(PROP_REGION, None)
    else:
        obj[PROP_REGION] = int(region_index)


def is_working_set(obj) -> bool:
    return obj is not None and obj.get(PROP_KIND) == KIND_WORKING_SET


def bounds_of(obj) -> tuple[np.ndarray, np.ndarray]:
    lo = obj.get(PROP_BOUNDS_MIN)
    hi = obj.get(PROP_BOUNDS_MAX)
    if lo is None or hi is None:
        raise WorkingSetError(f"'{obj.name}' has no recorded BSVX bounds; it was not checked out")
    return np.asarray(list(lo), dtype=np.int64), np.asarray(list(hi), dtype=np.int64)


# ----------------------------------------------------------------------------------------------
# Reading the Blender object back
# ----------------------------------------------------------------------------------------------


def read_object(obj, frame) -> tuple[np.ndarray, np.ndarray]:
    """(cells (N,3) int64, keys (N,) uint32) from a working-set object, transform applied.

    The object's world matrix is honoured, so moving or rotating the whole object moves the voxels
    -- which is the only reading of "I moved it" that does not silently discard the user's edit.
    """
    mesh = obj.data
    count = len(mesh.vertices)
    if count == 0:
        return np.zeros((0, 3), dtype=np.int64), np.zeros((0,), dtype=np.uint32)

    positions = np.empty(count * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    positions = positions.reshape(count, 3).astype(np.float64)

    matrix = np.array(obj.matrix_world, dtype=np.float64)  # 4x4, row-major
    if not np.allclose(matrix, np.eye(4)):
        homogeneous = np.concatenate([positions, np.ones((count, 1))], axis=1)
        positions = (homogeneous @ matrix.T)[:, :3]

    attribute = mesh.attributes.get(KEY_ATTRIBUTE)
    if attribute is None:
        raise WorkingSetError(
            f"'{obj.name}' has no '{KEY_ATTRIBUTE}' attribute -- it is not a BSVX working set"
        )
    raw = np.empty(count, dtype=np.int32)
    attribute.data.foreach_get("value", raw)

    return frame.points_to_cells(positions), raw.view(np.uint32)


def commit(world, obj, frame) -> dict:
    """Writes a working set back into the world. Returns a small report for the operator to print.

    The recorded box is cleared first. That is what gives deletion meaning: a vertex the user
    removed leaves no trace in the mesh, so only "everything in this box is what the mesh says"
    reproduces their intent. Voxels that moved *outside* the box are still written -- dropping
    them would silently undo a deliberate drag -- and are counted separately so the operator can
    say so.
    """
    lo, hi = bounds_of(obj)
    cells, keys = read_object(obj, frame)

    world.fill_box(tuple(int(v) for v in lo), tuple(int(v) for v in hi), 0, create_missing=False)

    live = keys != 0
    cells, keys = cells[live], keys[live]

    if cells.size == 0:
        return {"written": 0, "outside": 0, "collisions": 0, "dropped_air": int((~live).sum())}

    outside = int((~np.all((cells >= lo) & (cells <= hi), axis=1)).sum())

    # Two vertices in one cell is a real possibility after a duplicate-and-nudge; the format holds
    # one key per cell, so the later one wins and the count is reported rather than hidden.
    unique, index = np.unique(cells, axis=0, return_index=True)
    collisions = int(cells.shape[0] - unique.shape[0])
    if collisions:
        keys = keys[index]
        cells = unique

    written = world.set_voxels(
        (tuple(int(v) for v in cell) for cell in cells),
        [int(k) for k in keys],
        create_missing=True,
    )
    return {
        "written": int(written),
        "outside": outside,
        "collisions": collisions,
        "dropped_air": int((~live).sum()),
    }


def _api():
    from . import binding

    return binding.api()

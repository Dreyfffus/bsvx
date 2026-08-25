"""What a voxel *is* in Blender, and how it gets back out again.

**A voxel is a cube in a mesh.** Not because that is the cheapest encoding -- it is not; a vertex
per voxel would be a quarter of the data and a bijection with the cell -- but because it is the one
Blender's voxel ecosystem already speaks. Vox Cleaner imports, decimates, unwraps and bakes a cube
mesh with one material per palette entry; the MagicaVoxel importers it is built on produce exactly
that; every Blender modifier, boolean and sculpt tool operates on it. This add-on's job is to get
BSVX worlds into and out of that shape, not to be a voxel editor of its own.

So the shape is fixed by the neighbours, and the interesting work is making the round trip survive
what those tools do to a mesh:

* **Face culling.** Interior faces are not emitted. A solid world is a shell, which is what every
  consumer wants -- and it is also what makes the *inverse* recoverable, see below.
* **Welded corners.** Two neighbouring cubes name the same lattice corner by the same integer, so
  the mesh comes out welded rather than as loose quads. Nothing downstream has to merge by distance
  first.
* **The key is on the face.** ``bsvx_key`` is an INT attribute on the FACE domain. Materials and
  colours are for people and for bakers; this is what a lossless read-back uses.
* **Read-back rasterizes.** A face is not assumed to be one voxel. Vox Cleaner's decimate merges
  coplanar quads into big ones, and greedy-meshed voxel models arrive that way in the first place,
  so an axis-aligned quad is rasterized into every cell it covers.
* **Interior fill by parity.** A culled shell has lost its inside. It is recovered exactly, not
  guessed: along one axis, each cell owning a face pointing back down that axis opens a solid run
  and the next cell owning a face pointing up it closes one. For a closed shell that pairing is
  exact and costs O(faces), where a flood fill would cost O(volume).
"""

from __future__ import annotations

import numpy as np

#: INT on the FACE domain. The authoritative key: it survives everything short of retopology.
KEY_ATTRIBUTE = "bsvx_key"
#: BYTE_COLOR on the CORNER domain, and the mesh's active colour attribute. Display and baking
#: only -- rebuilt from the registry on every export, never read back unless asked for by name.
COLOR_ATTRIBUTE = "bsvx_color"

PROP_KIND = "bsvx_kind"
PROP_SESSION = "bsvx_session"
PROP_BOUNDS_MIN = "bsvx_bounds_min"
PROP_BOUNDS_MAX = "bsvx_bounds_max"
PROP_REGION = "bsvx_region"

KIND_VOXEL_MESH = "voxel_mesh"

#: How far into a cell a probe point is placed, as a fraction of the voxel. A face centre sits
#: exactly on a cell boundary, where flooring is a coin toss; a quarter of a voxel in is not.
_INSET = 0.25

#: Cosine tolerance for "this normal points along an axis". 0.999 is about 2.5 degrees.
_AXIS_TOLERANCE = 0.999


class VoxelMeshError(RuntimeError):
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
        raise VoxelMeshError(f"region decode returned {dense.size} voxels, expected {expected}")

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
        raise VoxelMeshError("box maximum is below its minimum")

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


def colors_for(keys: np.ndarray, lookup: dict) -> np.ndarray:
    """(N,) keys -> (N, 4) float32 RGBA, falling back to a neutral grey for unregistered keys."""
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
    out[in_range] = table[keys[in_range].astype(np.int64)]
    return out


# ----------------------------------------------------------------------------------------------
# Cell coding
# ----------------------------------------------------------------------------------------------


class _Grid:
    """Bijection between integer cells in a box and a single int64, for set membership."""

    def __init__(self, lo, hi):
        self.lo = np.asarray(lo, dtype=np.int64)
        self.span = np.asarray(hi, dtype=np.int64) - self.lo + 1
        if np.any(self.span <= 0):
            raise VoxelMeshError("empty cell box")
        volume = int(self.span[0]) * int(self.span[1]) * int(self.span[2])
        if volume >= (1 << 62):
            raise VoxelMeshError(
                "the cell box spans more than 2^62 cells, which no single mesh can index. "
                "Export a region or an explicit box instead of the whole world"
            )

    def encode(self, cells: np.ndarray) -> np.ndarray:
        rel = np.asarray(cells, dtype=np.int64) - self.lo
        return rel[:, 0] + self.span[0] * (rel[:, 1] + self.span[1] * rel[:, 2])

    def decode(self, codes: np.ndarray) -> np.ndarray:
        codes = np.asarray(codes, dtype=np.int64)
        x = codes % self.span[0]
        rest = codes // self.span[0]
        y = rest % self.span[1]
        z = rest // self.span[1]
        return np.stack([x, y, z], axis=1) + self.lo

    def inside(self, cells: np.ndarray) -> np.ndarray:
        rel = np.asarray(cells, dtype=np.int64) - self.lo
        return np.all((rel >= 0) & (rel < self.span), axis=1)


def _unique_last(cells: np.ndarray, keys: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Deduplicate cells, keeping the *last* key written for each. Order is not preserved."""
    if cells.shape[0] == 0:
        return cells, keys
    grid = _Grid(cells.min(axis=0), cells.max(axis=0))
    codes = grid.encode(cells)
    # np.unique reports the first occurrence; reversing turns that into the last.
    _, first = np.unique(codes[::-1], return_index=True)
    keep = cells.shape[0] - 1 - first
    return cells[keep], keys[keep]


# ----------------------------------------------------------------------------------------------
# World -> cube mesh
# ----------------------------------------------------------------------------------------------

#: (bsvx axis, direction) for the six faces of a cube.
_FACES = tuple((axis, sign) for axis in range(3) for sign in (-1, 1))


def _face_corner_offsets(frame, axis: int, sign: int) -> np.ndarray:
    """The four (3,) corner offsets of one cube face, wound so its normal points outward.

    The winding is *measured*, not derived by hand: the four corners are laid out in a fixed order,
    the resulting normal is compared against where this axis convention says the face should point,
    and the order is reversed if they disagree. A signed axis permutation flips handedness on two
    of the three conventions, and getting that wrong produces a mesh that looks right until it is
    rendered with backface culling.
    """
    u, v = (i for i in range(3) if i != axis)
    fixed = 1 if sign > 0 else 0

    offsets = np.zeros((4, 3), dtype=np.int64)
    offsets[:, axis] = fixed
    offsets[:, u] = (0, 1, 1, 0)
    offsets[:, v] = (0, 0, 1, 1)

    points = frame.corner_points(offsets)
    normal = np.cross(points[1] - points[0], points[2] - points[0])

    blender_axis, blender_sign = frame.axis_map()[axis]
    if normal[blender_axis] * sign * blender_sign < 0:
        offsets = offsets[::-1].copy()
    return offsets


def build_cube_mesh(
    name: str,
    cells: np.ndarray,
    keys: np.ndarray,
    frame,
    palette: dict,
    *,
    cull_interior: bool = True,
    with_colors: bool = True,
):
    """Builds a welded, face-culled cube mesh. Returns (mesh, per-face keys) with no object yet.

    The caller assigns materials, because that needs the world's registry and this does not.
    """
    import bpy

    cells = np.asarray(cells, dtype=np.int64).reshape(-1, 3)
    keys = np.asarray(keys, dtype=np.uint32).reshape(-1)

    mesh = bpy.data.meshes.new(name)
    if cells.shape[0] == 0:
        _add_attributes(mesh, np.zeros((0,), np.uint32), palette, with_colors)
        return mesh, np.zeros((0,), np.uint32)

    grid = _Grid(cells.min(axis=0), cells.max(axis=0))
    codes = np.sort(grid.encode(cells))

    face_cells: list[np.ndarray] = []
    face_keys: list[np.ndarray] = []
    face_offsets: list[np.ndarray] = []

    for axis, sign in _FACES:
        step = np.zeros(3, dtype=np.int64)
        step[axis] = sign
        neighbour = cells + step

        if cull_interior:
            occupied = np.zeros(cells.shape[0], dtype=bool)
            inside = grid.inside(neighbour)
            if inside.any():
                wanted = grid.encode(neighbour[inside])
                slot = np.clip(np.searchsorted(codes, wanted), 0, codes.size - 1)
                occupied[inside] = codes[slot] == wanted
            emit = ~occupied
        else:
            emit = np.ones(cells.shape[0], dtype=bool)

        if not emit.any():
            continue

        face_cells.append(cells[emit])
        face_keys.append(keys[emit])
        face_offsets.append(_face_corner_offsets(frame, axis, sign))

    if not face_cells:
        _add_attributes(mesh, np.zeros((0,), np.uint32), palette, with_colors)
        return mesh, np.zeros((0,), np.uint32)

    # Corner integers, (F, 4, 3). The lattice is shared between neighbouring cubes, so identical
    # triples here are the same vertex and the mesh comes out welded.
    corners = np.concatenate(
        [c[:, None, :] + o[None, :, :] for c, o in zip(face_cells, face_offsets)]
    )
    per_face_keys = np.concatenate(face_keys).astype(np.uint32)
    face_count = corners.shape[0]

    corner_grid = _Grid(cells.min(axis=0), cells.max(axis=0) + 1)
    flat = corners.reshape(-1, 3)
    unique_codes, loop_vertex = np.unique(corner_grid.encode(flat), return_inverse=True)
    vertices = frame.corner_points(corner_grid.decode(unique_codes)).astype(np.float32)

    mesh.vertices.add(vertices.shape[0])
    mesh.loops.add(face_count * 4)
    mesh.polygons.add(face_count)
    mesh.vertices.foreach_set("co", vertices.ravel())
    mesh.loops.foreach_set("vertex_index", loop_vertex.astype(np.int32).ravel())
    mesh.polygons.foreach_set("loop_start", (np.arange(face_count, dtype=np.int32) * 4))
    mesh.polygons.foreach_set("loop_total", np.full(face_count, 4, dtype=np.int32))
    mesh.update(calc_edges=True)

    _add_attributes(mesh, per_face_keys, palette, with_colors)
    return mesh, per_face_keys


def _add_attributes(mesh, per_face_keys: np.ndarray, palette: dict, with_colors: bool) -> None:
    key_attr = mesh.attributes.new(name=KEY_ATTRIBUTE, type="INT", domain="FACE")
    if per_face_keys.size:
        # Keys are uint32 and the attribute is int32. The reinterpretation is bit-exact, so a key
        # above 2^31 reads back negative rather than wrong; the inverse undoes it the same way.
        key_attr.data.foreach_set("value", per_face_keys.astype(np.int32).ravel())

    if not with_colors:
        return

    color_attr = mesh.attributes.new(name=COLOR_ATTRIBUTE, type="BYTE_COLOR", domain="CORNER")
    if per_face_keys.size:
        per_face = colors_for(per_face_keys, palette)
        color_attr.data.foreach_set("color", np.repeat(per_face, 4, axis=0).ravel())
    try:
        mesh.color_attributes.active_color = mesh.color_attributes[COLOR_ATTRIBUTE]
        mesh.color_attributes.render_color_index = mesh.color_attributes.find(COLOR_ATTRIBUTE)
    except Exception:
        pass


# ----------------------------------------------------------------------------------------------
# Object bookkeeping
# ----------------------------------------------------------------------------------------------


def tag_bounds(obj, lo, hi, region_index: int | None = None) -> None:
    obj[PROP_KIND] = KIND_VOXEL_MESH
    obj[PROP_BOUNDS_MIN] = [int(v) for v in lo]
    obj[PROP_BOUNDS_MAX] = [int(v) for v in hi]
    if region_index is None:
        obj.pop(PROP_REGION, None)
    else:
        obj[PROP_REGION] = int(region_index)


def is_voxel_mesh(obj) -> bool:
    return obj is not None and obj.get(PROP_KIND) == KIND_VOXEL_MESH


def bounds_of(obj):
    """The cell box this object was exported from, or None if it was not exported from one."""
    lo = obj.get(PROP_BOUNDS_MIN)
    hi = obj.get(PROP_BOUNDS_MAX)
    if lo is None or hi is None:
        return None
    return np.asarray(list(lo), dtype=np.int64), np.asarray(list(hi), dtype=np.int64)


def _api():
    from . import binding

    return binding.api()

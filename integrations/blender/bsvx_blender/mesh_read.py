"""Reading a Blender mesh back into voxels.

This is the half of the bridge that has to survive other people's tools. A mesh that leaves here
as one quad per exposed voxel face comes back after Vox Cleaner, a MagicaVoxel importer, a boolean
or a decimate, and almost nothing about it is guaranteed except that voxel models stay axis
aligned. So the read-back assumes only that:

* **A quad is a rectangle of cells, not a cell.** Coplanar merging -- greedy meshing on the way in,
  Decimate on the way out -- turns runs of faces into single big ones. Every axis-aligned face is
  rasterized into the cells its rectangle covers, which makes the per-voxel case a special case of
  the general one rather than the only one that works.
* **The inside has to be reconstructed.** A culled shell has no interior faces, so a naive read of
  a solid world gives back a hollow one -- silent data loss, and exactly the kind that shows up
  three exports later. It is recovered by parity along one axis: a cell owning a face that points
  down the axis opens a solid run, the next cell owning a face that points up it closes one. For a
  closed shell this is exact, and it costs O(faces) rather than the O(volume) of a flood fill.
* **The key can come from several places, and the mesh says which.** The ``bsvx_key`` face
  attribute if this add-on wrote it; otherwise materials, which is what every voxel importer
  produces; otherwise colours; otherwise the active key.

Anything not axis aligned is not a voxel mesh at all, and goes through ``voxelize.py`` instead.
"""

from __future__ import annotations

import numpy as np

from .voxel_mesh import (
    COLOR_ATTRIBUTE,
    KEY_ATTRIBUTE,
    VoxelMeshError,
    _INSET,
    _AXIS_TOLERANCE,
    _Grid,
    _unique_last,
)

#: Squared RGB distance beyond which a face colour is not considered a registry colour. Colour
#: attributes are 8-bit sRGB on disk, so an exact match is not something to insist on.
_COLOR_TOLERANCE = 0.02


class Faces:
    """The world-space geometry of a mesh, reduced to what the read-back needs."""

    __slots__ = ("count", "normals", "minimum", "maximum", "material_index", "mesh")

    def __init__(self, obj):
        mesh = obj.data
        self.mesh = mesh
        self.count = len(mesh.polygons)
        if self.count == 0:
            self.normals = np.zeros((0, 3))
            self.minimum = np.zeros((0, 3))
            self.maximum = np.zeros((0, 3))
            self.material_index = np.zeros((0,), dtype=np.int32)
            return

        matrix = np.array(obj.matrix_world, dtype=np.float64)
        linear = matrix[:3, :3]

        coordinates = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
        mesh.vertices.foreach_get("co", coordinates)
        coordinates = coordinates.reshape(-1, 3).astype(np.float64) @ linear.T + matrix[:3, 3]

        loop_vertex = np.empty(len(mesh.loops), dtype=np.int32)
        mesh.loops.foreach_get("vertex_index", loop_vertex)
        loop_start = np.empty(self.count, dtype=np.int32)
        mesh.polygons.foreach_get("loop_start", loop_start)

        # Loops are contiguous per face and loop_start increases, which is exactly reduceat's
        # contract -- so per-face extents cost one pass instead of a Python loop over polygons.
        corners = coordinates[loop_vertex]
        self.minimum = np.minimum.reduceat(corners, loop_start, axis=0)
        self.maximum = np.maximum.reduceat(corners, loop_start, axis=0)

        normals = np.empty(self.count * 3, dtype=np.float32)
        mesh.polygons.foreach_get("normal", normals)
        # A normal transforms by the inverse transpose; under a non-uniform scale the plain matrix
        # would tilt it off the axis and the whole mesh would read as "not a voxel mesh".
        normals = normals.reshape(-1, 3).astype(np.float64) @ np.linalg.inv(linear)
        lengths = np.linalg.norm(normals, axis=1, keepdims=True)
        self.normals = normals / np.where(lengths > 0, lengths, 1.0)

        self.material_index = np.empty(self.count, dtype=np.int32)
        mesh.polygons.foreach_get("material_index", self.material_index)


def axis_aligned(faces: Faces, frame) -> np.ndarray:
    """Per-face: does this face lie in an axis plane, tightly enough to be a voxel face?"""
    if faces.count == 0:
        return np.zeros((0,), dtype=bool)
    axis = np.argmax(np.abs(faces.normals), axis=1)
    rows = np.arange(faces.count)
    pointing = np.abs(faces.normals[rows, axis]) > _AXIS_TOLERANCE

    extent = np.asarray(frame.blender_voxel_size(), dtype=np.float64)
    thickness = (faces.maximum - faces.minimum)[rows, axis]
    flat = thickness < 1e-3 * float(extent.min())
    return pointing & flat


def is_voxel_shaped(obj, frame, *, threshold: float = 0.999) -> bool:
    """True when essentially every face is axis aligned, i.e. the mesh is a voxel model."""
    faces = Faces(obj)
    if faces.count == 0:
        return False
    return float(axis_aligned(faces, frame).mean()) >= threshold


# ----------------------------------------------------------------------------------------------
# Rasterizing faces into cells
# ----------------------------------------------------------------------------------------------


def _face_cell_range(faces: Faces, frame, selected: np.ndarray):
    """The inclusive cell range each selected face covers, plus its outward BSVX axis and sign."""
    rows = np.nonzero(selected)[0]
    normals = faces.normals[rows]
    blender_axis = np.argmax(np.abs(normals), axis=1)
    blender_sign = np.sign(normals[np.arange(rows.size), blender_axis])

    extent = np.asarray(frame.blender_voxel_size(), dtype=np.float64)
    inset = _INSET * extent

    low = faces.minimum[rows].copy()
    high = faces.maximum[rows].copy()

    # Shrink in plane so the probe cannot land on a cell boundary. A face narrower than two insets
    # is not a grid face; sampling its midpoint is the only sane reading left.
    shrunk_low = low + inset
    shrunk_high = high - inset
    degenerate = shrunk_low > shrunk_high
    midpoint = 0.5 * (low + high)
    shrunk_low = np.where(degenerate, midpoint, shrunk_low)
    shrunk_high = np.where(degenerate, midpoint, shrunk_high)

    # On the face's own axis, step *into* the cell the face bounds.
    plane = midpoint[np.arange(rows.size), blender_axis] - blender_sign * inset[blender_axis]
    shrunk_low[np.arange(rows.size), blender_axis] = plane
    shrunk_high[np.arange(rows.size), blender_axis] = plane

    a = frame.points_to_cells(shrunk_low)
    b = frame.points_to_cells(shrunk_high)
    cell_low = np.minimum(a, b)
    cell_high = np.maximum(a, b)

    # The outward direction, expressed in BSVX axes.
    axis_map = frame.axis_map()
    to_bsvx = {source: (index, sign) for index, (source, sign) in enumerate(axis_map)}
    bsvx_axis = np.empty(rows.size, dtype=np.int64)
    bsvx_sign = np.empty(rows.size, dtype=np.int64)
    for source, (index, sign) in to_bsvx.items():
        hit = blender_axis == source
        bsvx_axis[hit] = index
        bsvx_sign[hit] = np.sign(blender_sign[hit] * sign).astype(np.int64)

    return rows, cell_low, cell_high, bsvx_axis, bsvx_sign


def _expand(cell_low: np.ndarray, cell_high: np.ndarray, max_cells: int):
    """Every cell in each inclusive range, as (cells (M,3), face index per cell (M,))."""
    counts = np.prod(cell_high - cell_low + 1, axis=1)
    total = int(counts.sum())
    if total > max_cells:
        raise VoxelMeshError(
            f"the mesh covers {total:,} cells, above the {max_cells:,} limit. "
            "Raise the limit, or check that the object's scale matches the world's voxel size"
        )
    if total == 0:
        return np.zeros((0, 3), dtype=np.int64), np.zeros((0,), dtype=np.int64)

    single = counts == 1
    if single.all():
        return cell_low.copy(), np.arange(cell_low.shape[0], dtype=np.int64)

    cells = [cell_low[single]]
    owners = [np.nonzero(single)[0]]
    for index in np.nonzero(~single)[0]:
        lo, hi = cell_low[index], cell_high[index]
        grids = np.meshgrid(
            np.arange(lo[0], hi[0] + 1, dtype=np.int64),
            np.arange(lo[1], hi[1] + 1, dtype=np.int64),
            np.arange(lo[2], hi[2] + 1, dtype=np.int64),
            indexing="ij",
        )
        block = np.stack([g.ravel() for g in grids], axis=1)
        cells.append(block)
        owners.append(np.full(block.shape[0], index, dtype=np.int64))

    return np.concatenate(cells), np.concatenate(owners)


# ----------------------------------------------------------------------------------------------
# Interior fill
# ----------------------------------------------------------------------------------------------


def _fill_interior(cells, keys, bsvx_axis, bsvx_sign, max_cells: int):
    """Solid runs recovered from the shell by parity along whichever axis carries the most faces.

    Returns (cells, keys, unpaired) -- ``unpaired`` counting the faces whose open/close alternation
    broke, which is what an open or self-intersecting shell looks like from here.
    """
    if cells.shape[0] == 0:
        return cells, keys, 0

    axis = int(np.bincount(bsvx_axis, minlength=3).argmax())
    on_axis = bsvx_axis == axis
    if not on_axis.any():
        return np.zeros((0, 3), np.int64), np.zeros((0,), np.uint32), 0

    others = [i for i in range(3) if i != axis]
    picked = cells[on_axis]
    x = picked[:, axis]
    closing = (bsvx_sign[on_axis] > 0).astype(np.int64)
    line_cells = picked[:, others]

    flat_lines = np.stack([line_cells[:, 0], line_cells[:, 1], np.zeros_like(x)], axis=1)
    grid = _Grid(flat_lines.min(axis=0), flat_lines.max(axis=0))
    lines = grid.encode(flat_lines)

    # Primary key last: line, then position along the axis, then openings before closings so a
    # one-cell-thick run pairs with itself instead of with its neighbour.
    order = np.lexsort((closing, x, lines))
    lines, x, closing = lines[order], x[order], closing[order]
    run_keys = keys[on_axis][order]

    boundary = np.flatnonzero(np.diff(lines)) + 1
    starts = np.concatenate([[0], boundary])
    counts = np.diff(np.concatenate([starts, [lines.size]]))
    rank = np.arange(lines.size) - np.repeat(starts, counts)

    even = rank % 2 == 0
    # An opening must be even and a closing odd; anything else means the shell is not closed here.
    unpaired = int(np.count_nonzero(even == (closing == 1)))

    # Pair an opening with the closing immediately after it in the same line. Requiring both the
    # rank to advance and the partner to actually be a closing is what keeps a broken shell from
    # pairing two openings and filling everything between them.
    pairable = np.zeros(lines.size, dtype=bool)
    pairable[:-1] = even[:-1] & (closing[:-1] == 0) & (rank[1:] == rank[:-1] + 1) & (closing[1:] == 1)
    open_at = np.nonzero(pairable)[0]
    if open_at.size == 0:
        return np.zeros((0, 3), np.int64), np.zeros((0,), np.uint32), unpaired

    span_start = x[open_at]
    span_end = x[open_at + 1]
    span_key = run_keys[open_at]
    span_line = lines[open_at]

    lengths = span_end - span_start + 1
    valid = lengths > 0
    span_start, span_end, span_key, span_line, lengths = (
        span_start[valid], span_end[valid], span_key[valid], span_line[valid], lengths[valid]
    )
    total = int(lengths.sum())
    if total == 0:
        return np.zeros((0, 3), np.int64), np.zeros((0,), np.uint32), unpaired
    if total > max_cells:
        raise VoxelMeshError(
            f"filling the interior would write {total:,} cells, above the {max_cells:,} limit"
        )

    offsets = np.arange(total) - np.repeat(np.cumsum(lengths) - lengths, lengths)
    filled = np.empty((total, 3), dtype=np.int64)
    filled[:, axis] = np.repeat(span_start, lengths) + offsets
    line_coords = grid.decode(span_line)
    filled[:, others[0]] = np.repeat(line_coords[:, 0], lengths)
    filled[:, others[1]] = np.repeat(line_coords[:, 1], lengths)

    return filled, np.repeat(span_key, lengths), unpaired


# ----------------------------------------------------------------------------------------------
# Where the key comes from
# ----------------------------------------------------------------------------------------------


def keys_from_attribute(faces: Faces) -> np.ndarray | None:
    attribute = faces.mesh.attributes.get(KEY_ATTRIBUTE)
    if attribute is None or attribute.domain != "FACE" or attribute.data_type != "INT":
        return None
    raw = np.empty(faces.count, dtype=np.int32)
    attribute.data.foreach_get("value", raw)
    return raw.view(np.uint32)


def material_key_map(obj, registry) -> dict[int, int] | None:
    """Material slot -> voxel key, from what this add-on stamped on the material, then by name.

    Falls back to slot index + 1, which is the convention every MagicaVoxel importer follows and
    the reason a freshly imported .vox model exports sensibly with no setup at all.
    """
    if not obj.material_slots:
        return None
    by_name = {entry.name: entry.voxel_key for entry in registry if entry.name}
    mapping: dict[int, int] = {}
    for index, slot in enumerate(obj.material_slots):
        material = slot.material
        key = None
        if material is not None:
            stamped = material.get("bsvx_key")
            if stamped is not None:
                key = int(stamped)
            elif material.name in by_name:
                key = int(by_name[material.name])
        mapping[index] = int(key) if key is not None else index + 1
    return mapping


def face_colors(faces: Faces, name: str | None = None) -> np.ndarray | None:
    """Per-face RGBA, averaged over the face's corners, from a colour attribute on any domain."""
    mesh = faces.mesh
    attribute = mesh.attributes.get(name or COLOR_ATTRIBUTE)
    if attribute is None:
        colors = getattr(mesh, "color_attributes", None)
        attribute = colors.active_color if colors and colors.active_color else None
    if attribute is None or attribute.data_type not in {"BYTE_COLOR", "FLOAT_COLOR"}:
        return None

    values = np.empty(len(attribute.data) * 4, dtype=np.float32)
    attribute.data.foreach_get("color", values)
    values = values.reshape(-1, 4).astype(np.float64)

    loop_start = np.empty(faces.count, dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", loop_start)

    if attribute.domain == "CORNER":
        per_loop = values
    elif attribute.domain == "POINT":
        loop_vertex = np.empty(len(mesh.loops), dtype=np.int32)
        mesh.loops.foreach_get("vertex_index", loop_vertex)
        per_loop = values[loop_vertex]
    else:
        return values  # already per face

    counts = np.diff(np.concatenate([loop_start, [len(mesh.loops)]])).astype(np.float64)
    return np.add.reduceat(per_loop, loop_start, axis=0) / counts[:, None]


def keys_from_colors(colors: np.ndarray, registry, *, auto_register: bool):
    """Nearest registry colour per face. Returns (keys, [(key, rgba) to register]).

    Colour attributes are 8-bit sRGB on disk, so this matches by nearest rather than by equality;
    an exported palette does not survive the round trip bit for bit and is not meant to.
    """
    palette = [(entry.voxel_key, tuple(entry.color)) for entry in registry]
    quantized = np.round(np.clip(colors[:, :3], 0.0, 1.0) * 255.0).astype(np.int64)
    unique, inverse = np.unique(quantized, axis=0, return_inverse=True)
    resolved = np.zeros(unique.shape[0], dtype=np.uint32)
    additions: list[tuple[int, tuple[float, float, float, float]]] = []

    reference = np.array([c[:3] for _, c in palette], dtype=np.float64) if palette else None
    next_key = max((k for k, _ in palette), default=0) + 1

    for index, row in enumerate(unique / 255.0):
        best, distance = 0, float("inf")
        if reference is not None and reference.size:
            squared = np.sum((reference - row) ** 2, axis=1)
            nearest = int(np.argmin(squared))
            best, distance = palette[nearest][0], float(squared[nearest])
        if distance > _COLOR_TOLERANCE:
            if not auto_register:
                continue
            best = next_key
            next_key += 1
            rgba = (float(row[0]), float(row[1]), float(row[2]), 1.0)
            additions.append((best, rgba))
            palette.append((best, rgba))
            reference = np.array([c[:3] for _, c in palette], dtype=np.float64)
        resolved[index] = best

    return resolved[inverse], additions


# ----------------------------------------------------------------------------------------------
# The whole read
# ----------------------------------------------------------------------------------------------


def read(
    obj,
    frame,
    *,
    keys: np.ndarray,
    faces: "Faces | None" = None,
    fill_interior: bool = True,
    max_cells: int = 16_000_000,
) -> tuple[np.ndarray, np.ndarray, dict]:
    """(cells, keys, report) for one axis-aligned voxel mesh, with ``keys`` given per face."""
    faces = faces if faces is not None else Faces(obj)
    report = {"faces": faces.count, "skipped_faces": 0, "unpaired": 0, "interior": 0}
    if faces.count == 0:
        return np.zeros((0, 3), np.int64), np.zeros((0,), np.uint32), report

    selected = axis_aligned(faces, frame)
    report["skipped_faces"] = int((~selected).sum())
    if not selected.any():
        return np.zeros((0, 3), np.int64), np.zeros((0,), np.uint32), report

    rows, cell_low, cell_high, bsvx_axis, bsvx_sign = _face_cell_range(faces, frame, selected)
    cells, owners = _expand(cell_low, cell_high, max_cells)
    if cells.shape[0] == 0:
        return cells, np.zeros((0,), np.uint32), report

    surface_keys = np.asarray(keys, dtype=np.uint32)[rows][owners]

    if fill_interior:
        inside, inside_keys, unpaired = _fill_interior(
            cells, surface_keys, bsvx_axis[owners], bsvx_sign[owners], max_cells
        )
        report["unpaired"] = unpaired
        report["interior"] = int(inside.shape[0])
        # Surface last, so a shell voxel keeps the key its own face carried rather than the one
        # the run it belongs to opened with.
        cells = np.concatenate([inside, cells])
        surface_keys = np.concatenate([inside_keys, surface_keys])

    cells, surface_keys = _unique_last(cells, surface_keys)
    live = surface_keys != 0
    return cells[live], surface_keys[live], report

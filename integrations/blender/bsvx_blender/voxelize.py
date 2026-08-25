"""Turning Blender geometry into voxels.

The method is ray parity, not point sampling: for every line of cells parallel to Blender's +X, the
object is ray-cast repeatedly to collect all crossings, and cells between an odd and an even
crossing are inside. That makes the cost proportional to the *cross-section* rather than the volume,
which is the difference between seconds and minutes on anything worth voxelizing, and it fills
solids correctly rather than leaving a hollow shell.

The caveat is the one ray parity always has: it needs a closed, manifold surface. An open mesh has
an odd number of crossings somewhere and the fill leaks along that line. ``surface_only`` avoids the
question by marking only the cells a ray actually hits.
"""

from __future__ import annotations

import numpy as np

#: How far past a hit the next ray starts. Small enough not to skip a thin wall, large enough that
#: a ray does not re-hit the triangle it just left and loop forever.
_EPSILON = 1e-6

_MAX_CROSSINGS = 512


def object_cell_bounds(obj, frame) -> tuple[np.ndarray, np.ndarray]:
    """The inclusive BSVX cell box covering an object's world-space bounding box."""
    matrix = np.array(obj.matrix_world, dtype=np.float64)
    corners = np.array([list(c) for c in obj.bound_box], dtype=np.float64)
    homogeneous = np.concatenate([corners, np.ones((8, 1))], axis=1)
    world_corners = (homogeneous @ matrix.T)[:, :3]

    cells = frame.points_to_cells(world_corners)
    return cells.min(axis=0), cells.max(axis=0)


def voxelize(obj, frame, lo, hi, *, surface_only: bool = False, max_cells: int = 8_000_000):
    """Returns (cells (N, 3) int64, source polygon per cell (N,) int64) inside the inclusive box.

    The polygon index is what lets a multi-material mesh voxelize into more than one key: the
    caller maps polygon -> material slot -> voxel key. For a solid fill the polygon reported is the
    one the ray *entered* through, which is the surface the run belongs to.

    `obj` must already be evaluated (apply modifiers with `obj.evaluated_get(depsgraph)` first) --
    this only reads its geometry.
    """
    from mathutils import Vector

    lo = np.asarray(lo, dtype=np.int64)
    hi = np.asarray(hi, dtype=np.int64)
    counts = hi - lo + 1
    if np.any(counts <= 0):
        return np.zeros((0, 3), dtype=np.int64), np.zeros((0,), dtype=np.int64)

    total = int(np.prod(counts))
    if total > max_cells:
        raise ValueError(
            f"the box covers {total:,} cells, above the {max_cells:,} limit. "
            "Raise the voxel size or shrink the object"
        )

    axis = frame.blender_x_axis()
    others = [i for i in range(3) if i != axis]

    # One grid of every cell in the box, with the ray axis last so a line is a contiguous row.
    grids = np.meshgrid(
        np.arange(lo[others[0]], hi[others[0]] + 1, dtype=np.int64),
        np.arange(lo[others[1]], hi[others[1]] + 1, dtype=np.int64),
        np.arange(lo[axis], hi[axis] + 1, dtype=np.int64),
        indexing="ij",
    )
    cells = np.empty(grids[0].shape + (3,), dtype=np.int64)
    cells[..., others[0]] = grids[0]
    cells[..., others[1]] = grids[1]
    cells[..., axis] = grids[2]

    points = frame.cells_to_points(cells.reshape(-1, 3)).reshape(cells.shape)

    matrix_inv = obj.matrix_world.inverted()
    local_direction = (matrix_inv.to_3x3() @ Vector((1.0, 0.0, 0.0))).normalized()

    n0, n1, n2 = grids[0].shape
    selected: list[np.ndarray] = []
    sources: list[np.ndarray] = []

    for i in range(n0):
        for j in range(n1):
            line = points[i, j]  # (n2, 3), Blender-space centres along the ray
            xs = line[:, 0]

            start_x = float(min(xs.min(), xs.max())) - 1.0
            origin_world = Vector((start_x, float(line[0, 1]), float(line[0, 2])))

            crossings, polygons = _crossings(obj, matrix_inv @ origin_world, local_direction)
            if not crossings:
                continue

            source = np.full(n2, -1, dtype=np.int64)

            if surface_only:
                inside = np.zeros(n2, dtype=bool)
                order = np.argsort(xs)
                sorted_xs = xs[order]
                # Nearest cell centre to each hit, which is the cell the surface passes through.
                hit_index = np.clip(np.searchsorted(sorted_xs, crossings), 0, n2 - 1)
                inside[order[hit_index]] = True
                source[order[hit_index]] = polygons
            else:
                if len(crossings) % 2:
                    # An open surface: one crossing has no partner and everything past it would
                    # fill. Dropping the last one keeps the leak bounded instead of unbounded.
                    crossings, polygons = crossings[:-1], polygons[:-1]
                if not crossings:
                    continue
                spans = np.asarray(crossings, dtype=np.float64).reshape(-1, 2)
                entry = np.asarray(polygons, dtype=np.int64).reshape(-1, 2)[:, 0]
                inside = np.zeros(n2, dtype=bool)
                for (start, end), polygon in zip(spans, entry):
                    span = (xs >= start) & (xs <= end)
                    inside |= span
                    source[span] = polygon

            if inside.any():
                selected.append(cells[i, j][inside])
                sources.append(source[inside])

    if not selected:
        return np.zeros((0, 3), dtype=np.int64), np.zeros((0,), dtype=np.int64)
    return np.concatenate(selected), np.concatenate(sources)


def _crossings(obj, local_origin, local_direction):
    """Every surface crossing along the ray: (world-space X, polygon index), sorted by X."""
    hits: list[tuple[float, int]] = []
    origin = local_origin.copy()

    for _ in range(_MAX_CROSSINGS):
        hit, location, _normal, index = obj.ray_cast(origin, local_direction)
        if not hit:
            break
        hits.append((float((obj.matrix_world @ location).x), int(index)))
        origin = location + local_direction * _EPSILON

    hits.sort()
    return [x for x, _ in hits], [p for _, p in hits]

"""The one place the Blender <-> BSVX coordinate mapping is written down.

Blender is right-handed and +Z up. BSVX's canonical convention is right-handed and +Y up, and a
world can also declare itself Z-up (the frame Blender and 3ds Max use) or Y-up-Z-back (Unity's,
left-handed). So the mapping is not one transform but three, selected by what the world says it is.

Each is a signed axis permutation, expressed once as a table:

    bsvx[i] = SIGN[i] * blender[SOURCE[i]]

and inverted by transposing it, which a signed permutation allows exactly. Cell indices go through
the *continuous* mapping and are then floored, which is what produces the one-cell offset a negated
axis needs -- cell c covers [c, c+1) and its mirror is -c-1. Writing that offset out by hand is the
classic way to shift a world by exactly one voxel: invisible on symmetric test content, obvious on
the first real piece of geometry. Deriving it costs nothing and cannot be got wrong.

``tests/test_headless.py`` checks every convention in this table against the library's own
``bsvx.convert_cell`` on deliberately asymmetric input.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

#: bsvx_axis_convention values, mirrored so this module does not need the binding to describe a
#: transform. The test asserts they still agree with the library's.
AXIS_Y_UP_Z_FORWARD = 0  # Godot. The canonical frame.
AXIS_Z_UP_Y_FORWARD = 1  # Blender, 3ds Max.
AXIS_Y_UP_Z_BACK = 2  # Unity. Left-handed.

#: (source blender axis, sign) for each BSVX axis, per convention.
_PERMUTATION: dict[int, tuple[tuple[int, float], ...]] = {
    AXIS_Y_UP_Z_FORWARD: ((0, 1.0), (2, 1.0), (1, -1.0)),
    AXIS_Z_UP_Y_FORWARD: ((0, 1.0), (1, 1.0), (2, 1.0)),
    AXIS_Y_UP_Z_BACK: ((0, 1.0), (2, 1.0), (1, 1.0)),
}


@dataclass(frozen=True)
class Frame:
    """Converts between one world's voxel cells and Blender-space metres.

    ``voxel_size`` and ``origin`` are in BSVX axis order, exactly as ``bsvx_units`` stores them.
    """

    voxel_size: tuple[float, float, float] = (1.0, 1.0, 1.0)
    origin: tuple[float, float, float] = (0.0, 0.0, 0.0)
    axis_convention: int = AXIS_Y_UP_Z_FORWARD

    @classmethod
    def from_world(cls, world) -> "Frame":
        units = world.units
        return cls(
            voxel_size=(units.voxel_size_x, units.voxel_size_y, units.voxel_size_z),
            origin=(units.origin_x, units.origin_y, units.origin_z),
            axis_convention=world.axis_convention,
        )

    def _permutation(self):
        try:
            return _PERMUTATION[self.axis_convention]
        except KeyError:
            raise ValueError(f"unknown axis convention {self.axis_convention}") from None

    # --- continuous ---------------------------------------------------------------------------

    def blender_to_bsvx(self, points: np.ndarray) -> np.ndarray:
        points = np.asarray(points, dtype=np.float64).reshape(-1, 3)
        out = np.empty_like(points)
        for i, (source, sign) in enumerate(self._permutation()):
            out[:, i] = sign * points[:, source]
        return out

    def bsvx_to_blender(self, points: np.ndarray) -> np.ndarray:
        points = np.asarray(points, dtype=np.float64).reshape(-1, 3)
        out = np.empty_like(points)
        for i, (source, sign) in enumerate(self._permutation()):
            out[:, source] = sign * points[:, i]
        return out

    # --- cells --------------------------------------------------------------------------------

    def cells_to_points(self, cells: np.ndarray) -> np.ndarray:
        """(N, 3) integer BSVX cells -> (N, 3) Blender-space positions of their centres.

        Centres, not corners: a cube instanced on the point then sits where the voxel is, and the
        inverse is a plain floor with no half-voxel bias to remember.
        """
        cells = np.asarray(cells, dtype=np.float64).reshape(-1, 3)
        size = np.asarray(self.voxel_size, dtype=np.float64)
        origin = np.asarray(self.origin, dtype=np.float64)
        return self.bsvx_to_blender(origin + (cells + 0.5) * size)

    def points_to_cells(self, points: np.ndarray) -> np.ndarray:
        """(N, 3) Blender-space positions -> (N, 3) int64 BSVX cells containing them."""
        size = np.asarray(self.voxel_size, dtype=np.float64)
        origin = np.asarray(self.origin, dtype=np.float64)
        return np.floor((self.blender_to_bsvx(points) - origin) / size).astype(np.int64)

    def point_to_cell(self, point) -> tuple[int, int, int]:
        cell = self.points_to_cells(np.asarray([point], dtype=np.float64))[0]
        return (int(cell[0]), int(cell[1]), int(cell[2]))

    def cell_to_point(self, cell) -> tuple[float, float, float]:
        point = self.cells_to_points(np.asarray([cell], dtype=np.int64))[0]
        return (float(point[0]), float(point[1]), float(point[2]))

    def cell_from_blender_cell(self, cell) -> tuple[int, int, int]:
        """A Blender-axis integer cell -> the same cell in this world's BSVX axes, units aside."""
        centre = (np.asarray(cell, dtype=np.float64) + 0.5).reshape(1, 3)
        return tuple(int(v) for v in np.floor(self.blender_to_bsvx(centre))[0])

    def cell_to_blender_cell(self, cell) -> tuple[int, int, int]:
        centre = (np.asarray(cell, dtype=np.float64) + 0.5).reshape(1, 3)
        return tuple(int(v) for v in np.floor(self.bsvx_to_blender(centre))[0])

    def blender_x_axis(self) -> int:
        """The BSVX axis index that runs along Blender's +X under this convention.

        Voxelization casts its rays along Blender +X, so it needs to know which row of a cell grid
        is a line in that direction. Every convention in the table maps some BSVX axis onto it.
        """
        for index, (source, _sign) in enumerate(self._permutation()):
            if source == 0:
                return index
        raise ValueError("no BSVX axis maps to Blender X")

    def blender_voxel_size(self) -> tuple[float, float, float]:
        """The voxel's extent along Blender's own axes -- what a display cube is scaled to."""
        size = np.abs(self.bsvx_to_blender(np.asarray([self.voxel_size], dtype=np.float64))[0])
        return (float(size[0]), float(size[1]), float(size[2]))

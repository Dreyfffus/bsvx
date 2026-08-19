"""Headless test for the BSVX add-on.

    blender --background --python integrations/blender/tests/test_headless.py

Exits non-zero on the first failure. Everything here runs through the real operators against a real
world on disk -- the coordinate mapping is checked against the library's own ``convert_cell``, and
the round trip is checked by reading voxels back out of a saved file, not out of memory.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import bpy
import numpy as np

_ADDON_PARENT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ADDON_PARENT))

import bsvx_blender  # noqa: E402
from bsvx_blender import binding, session, working_set  # noqa: E402
from bsvx_blender.frame import (  # noqa: E402
    AXIS_Y_UP_Z_BACK,
    AXIS_Y_UP_Z_FORWARD,
    AXIS_Z_UP_Y_FORWARD,
    Frame,
)

FAILURES: list[str] = []
CHECKS = 0


def check(condition: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(what)
        print(f"  FAIL  {what}")


def check_eq(actual, expected, what: str) -> None:
    check(actual == expected, f"{what} (got {actual!r}, expected {expected!r})")


def section(title: str) -> None:
    print(f"\n[{title}]")


# ----------------------------------------------------------------------------------------------


def test_axis_mapping() -> None:
    section("axis mapping vs the library")
    api = binding.api()

    library_convention = {
        AXIS_Y_UP_Z_FORWARD: api.AXIS_X_RIGHT_Y_UP_Z_FORWARD,
        AXIS_Z_UP_Y_FORWARD: api.AXIS_X_RIGHT_Z_UP_Y_FORWARD,
        AXIS_Y_UP_Z_BACK: api.AXIS_X_RIGHT_Y_UP_Z_BACK,
    }
    for ours, theirs in library_convention.items():
        check_eq(ours, theirs, f"convention {ours} matches the ABI's numbering")

    # Deliberately asymmetric, and straddling zero on every axis: a mirrored or one-cell-shifted
    # mapping is invisible on symmetric content and obvious here.
    samples = [(1, 2, 3), (-1, -2, -3), (0, 0, 0), (-1, 0, 5), (7, -3, 0), (-4, 9, -8)]

    for ours, theirs in library_convention.items():
        frame = Frame(axis_convention=ours)
        for cell in samples:
            expected = api.convert_cell(api.AXIS_X_RIGHT_Z_UP_Y_FORWARD, theirs, *cell)
            check_eq(frame.cell_from_blender_cell(cell), expected, f"blender {cell} -> convention {ours}")

            back = frame.cell_to_blender_cell(expected)
            check_eq(back, cell, f"convention {ours} {expected} -> blender, round trip")


def test_cell_point_round_trip() -> None:
    section("cell <-> point round trip")
    frame = Frame(voxel_size=(0.25, 0.25, 0.25), origin=(1.5, -2.0, 0.75))

    cells = np.array([[0, 0, 0], [3, -4, 5], [-7, 8, -9]], dtype=np.int64)
    points = frame.cells_to_points(cells)
    check(np.array_equal(frame.points_to_cells(points), cells), "centres floor back to their own cells")

    # A point anywhere inside the cell must land on the same cell, not just its centre.
    jittered = points + np.array([0.09, -0.09, 0.09])
    check(np.array_equal(frame.points_to_cells(jittered), cells), "an off-centre point stays in its cell")

    # Non-uniform voxel size, so a transposed axis cannot pass by accident.
    skewed = Frame(voxel_size=(0.5, 0.25, 2.0))
    check(
        np.array_equal(skewed.points_to_cells(skewed.cells_to_points(cells)), cells),
        "non-uniform voxel size round trips",
    )


def test_authoring_and_round_trip(tmp: Path) -> None:
    section("author, save, reopen")
    scene = bpy.context.scene
    settings = scene.bsvx
    settings.chunk_size = (16, 16, 16)
    settings.region_size = (2, 2, 2)
    settings.voxel_size = 0.25

    check_eq(bpy.ops.bsvx.new_world(), {"FINISHED"}, "new_world")
    current = session.active(bpy.context)
    check(current is not None, "a session is open")

    settings.registry.clear()
    for key, name, color in ((1, "stone", (0.4, 0.4, 0.4, 1.0)), (2, "grass", (0.3, 0.6, 0.2, 1.0))):
        item = settings.registry.add()
        item.voxel_key = key
        item.name = name
        item.color = color
    check_eq(bpy.ops.bsvx.registry_push(), {"FINISHED"}, "registry_push")

    settings.active_key = 1
    check_eq(bpy.ops.bsvx.fill_box(minimum=(0, 0, 0), maximum=(7, 0, 7)), {"FINISHED"}, "fill_box")
    settings.active_key = 2
    check_eq(bpy.ops.bsvx.fill_box(minimum=(3, 1, 3), maximum=(4, 1, 4)), {"FINISHED"}, "fill_box again")

    world = current.world
    check_eq(world.get_voxels([(0, 0, 0)])[0], 1, "the first fill landed")
    check_eq(world.get_voxels([(3, 1, 3)])[0], 2, "the second fill landed")
    check_eq(world.get_voxels([(0, 1, 0)])[0], 0, "untouched space is air")

    root = tmp / "world"
    check_eq(bpy.ops.bsvx.save_world_as(filepath=str(root)), {"FINISHED"}, "save_world_as")
    check((root / "manifest.toml").is_file(), "a manifest was written")

    check_eq(bpy.ops.bsvx.close_world(), {"FINISHED"}, "close_world")
    check(session.active(bpy.context) is None, "the session closed")

    check_eq(bpy.ops.bsvx.open_world(filepath=str(root / "manifest.toml")), {"FINISHED"}, "open_world")
    reopened = session.active(bpy.context)
    check(reopened is not None, "reopened")

    check_eq(reopened.world.get_voxels([(0, 0, 0)])[0], 1, "voxels survived the file")
    check_eq(reopened.world.get_voxels([(3, 1, 3)])[0], 2, "and so did the second key")

    names = {entry.voxel_key: reopened.world.registry_name(entry.voxel_key) for entry in reopened.world.registry}
    check_eq(names.get(1), "stone", "registry name survived")
    check_eq(names.get(2), "grass", "second registry name survived")
    check_eq(len(settings.registry), 2, "opening pulled the registry into the mirror")
    check(abs(reopened.world.units.voxel_size_x - 0.25) < 1e-9, "voxel size survived")


def test_working_set_round_trip() -> None:
    section("checkout, edit, commit")
    settings = bpy.context.scene.bsvx
    current = session.active(bpy.context)
    world = current.world

    before = np.count_nonzero(np.ctypeslib.as_array(world.decode_region(0, binding.api().LAYOUT_REGION_LINEAR)))
    check(before > 0, "the region holds voxels to check out")

    check_eq(bpy.ops.bsvx.checkout_region(region_index=0), {"FINISHED"}, "checkout_region")
    obj = bpy.context.active_object
    check(working_set.is_working_set(obj), "the checked-out object is tagged")
    check_eq(len(obj.data.vertices), before, "one vertex per non-air voxel")

    attribute = obj.data.attributes.get(working_set.KEY_ATTRIBUTE)
    check(attribute is not None, "the key attribute exists")
    keys = np.empty(len(obj.data.vertices), dtype=np.int32)
    attribute.data.foreach_get("value", keys)
    check_eq(sorted(set(keys.tolist())), [1, 2], "both keys came through")

    # A vertex sits at its voxel's centre, so flooring it must return the voxel it came from.
    frame = Frame.from_world(world)
    cells, read_keys = working_set.read_object(obj, frame)
    check_eq(
        world.get_voxels([tuple(int(v) for v in cells[0])])[0],
        int(read_keys[0]),
        "a vertex reads back as its own voxel",
    )

    # Edit the checked-out mesh the way a user would -- delete one vertex, repaint another -- and
    # commit the same object. Rebuilding a mesh by hand here would test commit() against data this
    # test made up rather than against what Blender's own editing leaves behind.
    import bmesh

    victim = tuple(int(v) for v in cells[0])
    survivor = tuple(int(v) for v in cells[1])
    check_eq(world.get_voxels([survivor])[0], 1, "the survivor starts on key 1")

    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    key_layer = bm.verts.layers.int[working_set.KEY_ATTRIBUTE]
    bm.verts[1][key_layer] = 2
    bm.verts.remove(bm.verts[0])
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()

    check_eq(len(obj.data.vertices), before - 1, "the mesh lost a vertex")

    report = working_set.commit(world, obj, frame)
    check_eq(report["written"], before - 1, "every remaining voxel was written")
    check_eq(report["collisions"], 0, "no two vertices shared a cell")
    check_eq(world.get_voxels([victim])[0], 0, "the deleted vertex deleted its voxel")
    check_eq(world.get_voxels([survivor])[0], 2, "the repainted vertex changed its key")

    after = np.count_nonzero(np.ctypeslib.as_array(world.decode_region(0, binding.api().LAYOUT_REGION_LINEAR)))
    check_eq(after, before - 1, "exactly one voxel disappeared")

    bpy.data.objects.remove(obj, do_unlink=True)


def test_save_and_reopen_after_handle_loss() -> None:
    section("dirty save, then reopen")
    settings = bpy.context.scene.bsvx
    current = session.active(bpy.context)
    root = Path(current.path)

    check(current.world.is_dirty, "the committed edits marked the world dirty")
    check_eq(bpy.ops.bsvx.save_world(), {"FINISHED"}, "save_world")
    check(not current.world.is_dirty, "saving cleared the dirty flags")

    # What a .blend reload leaves behind: the recorded path, and no C handle.
    session.close(settings.session_key)
    settings.session_key = ""
    check(session.active(bpy.context) is None, "the handle is gone")
    check_eq(settings.path, str(root), "the path survived")

    check_eq(bpy.ops.bsvx.reopen_world(), {"FINISHED"}, "reopen_world")
    reopened = session.active(bpy.context)
    check(reopened is not None, "reopened from the recorded path")
    check_eq(
        np.count_nonzero(np.ctypeslib.as_array(reopened.world.decode_region(0, binding.api().LAYOUT_REGION_LINEAR))),
        67,
        "the saved file holds what the commit left",
    )


def test_moved_object_commits_moved_voxels() -> None:
    section("object transform is honoured")
    from mathutils import Matrix

    current = session.active(bpy.context)
    world = current.world
    frame = Frame.from_world(world)

    cells = np.array([[20, 0, 20]], dtype=np.int64)
    keys = np.array([1], dtype=np.uint32)
    obj = working_set.build_object("moved", cells, keys, frame, {})
    working_set.tag_bounds(obj, (20, 0, 20), (20, 0, 20))
    bpy.context.collection.objects.link(obj)

    # One voxel along Blender +X, which is BSVX +X in the canonical frame.
    obj.matrix_world = Matrix.Translation((frame.voxel_size[0], 0.0, 0.0))
    moved_cells, _ = working_set.read_object(obj, frame)
    check_eq(tuple(int(v) for v in moved_cells[0]), (21, 0, 20), "the transform moved the voxel one cell")


def test_voxelize_a_cube(tmp: Path) -> None:
    section("voxelize an object")
    settings = bpy.context.scene.bsvx
    current = session.active(bpy.context)

    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(10.0, 0.0, 10.0))
    cube = bpy.context.active_object

    settings.active_key = 1
    check_eq(bpy.ops.bsvx.voxelize_object(surface_only=False), {"FINISHED"}, "voxelize_object")

    frame = Frame.from_world(current.world)
    # The cube spans 2 m on a side at 0.25 m voxels: its centre must be solid, and a point well
    # outside it must not be.
    inside = frame.point_to_cell((10.0, 0.0, 10.0))
    outside = frame.point_to_cell((10.0, 0.0, 14.0))
    check_eq(current.world.get_voxels([inside])[0], 1, "the cube's centre filled")
    check_eq(current.world.get_voxels([outside])[0], 0, "space outside the cube did not")

    bpy.data.objects.remove(cube, do_unlink=True)


def test_validation_and_export(tmp: Path) -> None:
    section("validate and export")
    settings = bpy.context.scene.bsvx
    current = session.active(bpy.context)

    current.world.set_voxels([(1, 1, 1)], 99)
    check_eq(bpy.ops.bsvx.validate(deep=True), {"FINISHED"}, "validate")
    codes = {issue.code for issue in settings.issues}
    check(1 in codes, "an unregistered key is reported (code 1)")
    check(any(issue.message for issue in settings.issues), "issues carry messages")

    current.world.set_voxels([(1, 1, 1)], 0)

    check_eq(bpy.ops.bsvx.compact(), {"FINISHED"}, "compact")

    # export_region is only defined for a single-region world, so it gets one of its own rather
    # than a conditional that quietly skips itself once an earlier test has grown the world.
    settings.chunk_size = (8, 8, 8)
    settings.region_size = (1, 1, 1)
    check_eq(bpy.ops.bsvx.new_world(), {"FINISHED"}, "new_world for the export")
    solo = session.active(bpy.context)
    solo.world.set_registry_entry(1, flags=binding.api().REGISTRY_OPAQUE)
    solo.world.fill_box((0, 0, 0), (3, 3, 3), 1)
    check_eq(solo.world.region_count, 1, "exactly one region")

    target = tmp / "standalone.bvx"
    check_eq(bpy.ops.bsvx.export_region(filepath=str(target)), {"FINISHED"}, "export_region")
    check(target.is_file(), "a standalone .bvx was written")

    api = binding.api()
    with api.World.load_region(target) as reloaded:
        check_eq(reloaded.get_voxels([(0, 0, 0)])[0], 1, "the standalone file holds the voxels")
        check_eq(reloaded.get_voxels([(4, 0, 0)])[0], 0, "and stops where the fill stopped")


def main() -> int:
    bsvx_blender.register()
    print(f"bsvx ABI {binding.api().abi_version()}, Blender {bpy.app.version_string}")

    with tempfile.TemporaryDirectory(prefix="bsvx_blender_") as raw:
        tmp = Path(raw)
        test_axis_mapping()
        test_cell_point_round_trip()
        test_authoring_and_round_trip(tmp)
        test_working_set_round_trip()
        test_save_and_reopen_after_handle_loss()
        test_moved_object_commits_moved_voxels()
        test_voxelize_a_cube(tmp)
        test_validation_and_export(tmp)

    session.close_all()
    print(f"\n{CHECKS} checks, {len(FAILURES)} failures")
    for failure in FAILURES:
        print(f"  - {failure}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

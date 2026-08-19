"""Headless test for the BSVX add-on.

    blender --factory-startup --background --python integrations/blender/tests/test_headless.py

--factory-startup matters once the add-on is also installed: without it Blender enables the
installed copy first, this script registers a second copy of the same classes over it, and the
teardown of whichever one loses raises. The checks still pass, but the noise is real and the
shadowing is not something to rely on.

Exits non-zero on the first failure. Everything here runs through the real operators against a real
world -- the coordinate mapping is checked against the library's own ``convert_cell``, and the mesh
round trip is checked by comparing the region's decoded voxel array before and after, so a mesh
that loses, shifts or hollows anything cannot pass.
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
from bsvx_blender import binding, materials, mesh_read, session, voxel_mesh  # noqa: E402
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


def dense(world, region_index: int = 0) -> np.ndarray:
    """One region as a flat voxel array. Copied: the decode buffer is the library's, not ours."""
    raw = world.decode_region(region_index, binding.api().LAYOUT_REGION_LINEAR)
    return np.ctypeslib.as_array(raw).copy()


def new_world(chunk=(16, 16, 16), region=(2, 2, 2), voxel_size=0.25):
    settings = bpy.context.scene.bsvx
    settings.chunk_size = chunk
    settings.region_size = region
    settings.voxel_size = voxel_size
    bpy.ops.bsvx.new_world()
    return session.active(bpy.context)


def clear_objects() -> None:
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)


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


def test_cube_corners_are_the_cell() -> None:
    section("a cube covers exactly its cell")
    for convention in (AXIS_Y_UP_Z_FORWARD, AXIS_Z_UP_Y_FORWARD, AXIS_Y_UP_Z_BACK):
        frame = Frame(voxel_size=(0.25, 0.5, 0.125), origin=(1.0, -2.0, 0.5), axis_convention=convention)
        cell = np.array([[3, -4, 5]], dtype=np.int64)

        offsets = np.array([[dx, dy, dz] for dx in (0, 1) for dy in (0, 1) for dz in (0, 1)], dtype=np.int64)
        corners = frame.corner_points(cell + offsets)

        low, high = corners.min(axis=0), corners.max(axis=0)
        extent = np.asarray(frame.blender_voxel_size())
        check(np.allclose(high - low, extent), f"convention {convention}: the cube is one voxel across")

        centre = frame.cells_to_points(cell)[0]
        check(np.allclose(0.5 * (low + high), centre), f"convention {convention}: the cube is centred on its cell")


def test_authoring_and_round_trip(tmp: Path) -> None:
    section("author, save, reopen")
    settings = bpy.context.scene.bsvx

    current = new_world()
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


def test_mesh_export_shape() -> None:
    section("export to mesh: what comes out")
    current = session.active(bpy.context)
    world = current.world
    voxels = int(np.count_nonzero(dense(world)))

    check_eq(bpy.ops.bsvx.to_mesh(source="REGION", region_index=0), {"FINISHED"}, "to_mesh")
    obj = bpy.context.active_object
    check(voxel_mesh.is_voxel_mesh(obj), "the object is tagged as a BSVX voxel mesh")
    check(voxel_mesh.bounds_of(obj) is not None, "it records the cell box it came from")

    mesh = obj.data
    check(len(mesh.polygons) > 0, "it has faces")
    check(
        len(mesh.polygons) < 6 * voxels,
        f"interior faces were culled ({len(mesh.polygons)} < {6 * voxels})",
    )
    check(all(len(polygon.vertices) == 4 for polygon in mesh.polygons), "every face is a quad")

    # Welded: two neighbouring cubes name the same lattice corner, so vertices are shared.
    check(len(mesh.vertices) < 4 * len(mesh.polygons), "corners are shared, not one set per face")
    check_eq(mesh.validate(verbose=False), False, "Blender finds nothing wrong with the mesh")

    attribute = mesh.attributes.get(voxel_mesh.KEY_ATTRIBUTE)
    check(attribute is not None, "the key attribute exists")
    check_eq(attribute.domain, "FACE", "the key lives on the face domain")
    keys = np.empty(len(mesh.polygons), dtype=np.int32)
    attribute.data.foreach_get("value", keys)
    check_eq(sorted(set(keys.tolist())), [1, 2], "both keys came through")

    check_eq(len(mesh.materials), 2, "one material slot per key")
    stamped = sorted(material.get(materials.PROP_KEY) for material in mesh.materials)
    check_eq(stamped, [1, 2], "each material is stamped with its key")
    check(mesh.color_attributes.get(voxel_mesh.COLOR_ATTRIBUTE) is not None, "a colour attribute was written")

    # Outward normals: every face of a culled shell points away from its own voxel, so a face's
    # centre stepped along -normal must land on a solid cell and along +normal must not.
    frame = Frame.from_world(world)
    extent = np.asarray(frame.blender_voxel_size())
    matrix = np.array(obj.matrix_world)
    inward, outward = 0, 0
    for polygon in mesh.polygons:
        centre = np.array(polygon.center)
        normal = np.array(polygon.normal)
        cell_in = frame.point_to_cell(centre - normal * 0.25 * extent)
        cell_out = frame.point_to_cell(centre + normal * 0.25 * extent)
        inward += world.get_voxels([cell_in])[0] != 0
        outward += world.get_voxels([cell_out])[0] != 0
    check_eq(inward, len(mesh.polygons), "every face has a solid voxel behind it")
    check_eq(outward, 0, "and air in front of it")


def test_mesh_round_trip_exact() -> None:
    section("import from mesh: exact round trip")
    current = session.active(bpy.context)
    world = current.world
    before = dense(world)

    obj = bpy.context.active_object
    check(voxel_mesh.is_voxel_mesh(obj), "the exported mesh is still the active object")
    check_eq(bpy.ops.bsvx.from_mesh(mode="AUTO", key_source="AUTO"), {"FINISHED"}, "from_mesh")

    after = dense(world)
    check(np.array_equal(before, after), "the region came back voxel for voxel, key for key")


def test_deleting_faces_deletes_voxels() -> None:
    section("deleting geometry deletes voxels")
    import bmesh

    current = session.active(bpy.context)
    world = current.world
    obj = bpy.context.active_object

    # The 2x2x2 patch at y=1 is the only thing above the plate; removing every face whose voxel
    # sits there is what "select it in the viewport and press X" leaves behind.
    frame = Frame.from_world(world)
    extent = np.asarray(frame.blender_voxel_size())

    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    doomed = []
    for face in bm.faces:
        centre = np.array(face.calc_center_median())
        normal = np.array(face.normal)
        cell = frame.point_to_cell(centre - normal * 0.25 * extent)
        if cell[1] == 1:
            doomed.append(face)
    check(len(doomed) > 0, "found the patch's faces")
    bmesh.ops.delete(bm, geom=doomed, context="FACES")
    bm.to_mesh(obj.data)
    bm.free()

    check_eq(bpy.ops.bsvx.from_mesh(mode="GRID", key_source="AUTO"), {"FINISHED"}, "from_mesh after the delete")
    check_eq(world.get_voxels([(3, 1, 3)])[0], 0, "the deleted faces deleted their voxel")
    check_eq(world.get_voxels([(4, 1, 4)])[0], 0, "and the rest of the patch")
    check_eq(world.get_voxels([(0, 0, 0)])[0], 1, "the plate is untouched")
    check_eq(int(np.count_nonzero(dense(world))), 64, "exactly the plate is left")

    bpy.data.objects.remove(obj, do_unlink=True)


def test_dirty_save_and_reopen() -> None:
    section("dirty save, then reopen")
    settings = bpy.context.scene.bsvx
    current = session.active(bpy.context)
    root = Path(current.path)
    expected = int(np.count_nonzero(dense(current.world)))

    check(current.world.is_dirty, "the mesh import marked the world dirty")
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
        int(np.count_nonzero(dense(reopened.world))),
        expected,
        "the saved file holds what the import left",
    )


def _solid_world(size: int = 6):
    clear_objects()
    current = new_world()
    current.world.set_registry_entry(1, flags=binding.api().REGISTRY_OPAQUE)
    current.world.set_registry_color(1, 0x8040C0FF)
    current.world.fill_box((0, 0, 0), (size - 1, size - 1, size - 1), 1)
    bpy.context.scene.bsvx.active_key = 1
    return current


def test_solid_round_trip() -> None:
    section("a solid body survives being a hollow shell")
    size = 6
    current = _solid_world(size)
    world = current.world
    before = dense(world)
    check_eq(int(np.count_nonzero(before)), size**3, "the world is a solid box")

    check_eq(bpy.ops.bsvx.to_mesh(source="REGION", region_index=0), {"FINISHED"}, "to_mesh")
    obj = bpy.context.active_object
    check_eq(len(obj.data.polygons), 6 * size * size, "culling left exactly the shell")

    check_eq(bpy.ops.bsvx.from_mesh(mode="GRID", fill_interior=True), {"FINISHED"}, "from_mesh")
    check(np.array_equal(before, dense(world)), "parity refilled the interior exactly")

    # Without the fill, the same shell is a shell -- the check that the fill is doing the work.
    check_eq(bpy.ops.bsvx.from_mesh(mode="GRID", fill_interior=False), {"FINISHED"}, "from_mesh hollow")
    hollow = int(np.count_nonzero(dense(world)))
    check_eq(hollow, size**3 - (size - 2) ** 3, "without the fill it comes back hollow")


def test_coplanar_merge_round_trip() -> None:
    section("merged coplanar faces still read back")
    import bmesh

    size = 6
    current = _solid_world(size)
    world = current.world
    before = dense(world)

    check_eq(bpy.ops.bsvx.to_mesh(source="REGION", region_index=0), {"FINISHED"}, "to_mesh")
    obj = bpy.context.active_object

    # What Vox Cleaner's decimate and every greedy mesher do: one quad per flat side, not one per
    # voxel face. A reader that assumes "a face is a voxel" loses 97% of this model.
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.dissolve_limit(bm, angle_limit=0.01, verts=bm.verts[:], edges=bm.edges[:])
    bm.to_mesh(obj.data)
    bm.free()
    check_eq(len(obj.data.polygons), 6, "the shell dissolved to one face per side")

    check_eq(
        bpy.ops.bsvx.from_mesh(mode="GRID", key_source="ACTIVE", fill_interior=True),
        {"FINISHED"},
        "from_mesh on the merged mesh",
    )
    check(np.array_equal(before, dense(world)), "six big quads rebuilt the whole solid")


def test_object_transform_moves_voxels() -> None:
    section("object transform is honoured")
    from mathutils import Matrix

    size = 4
    current = _solid_world(size)
    world = current.world
    frame = Frame.from_world(world)

    check_eq(bpy.ops.bsvx.to_mesh(source="REGION", region_index=0), {"FINISHED"}, "to_mesh")
    obj = bpy.context.active_object

    # One voxel along Blender +X, which is BSVX +X in the canonical frame.
    obj.matrix_world = Matrix.Translation((frame.blender_voxel_size()[0], 0.0, 0.0))
    check_eq(
        bpy.ops.bsvx.from_mesh(mode="GRID", key_source="ACTIVE", replace_bounds=True),
        {"FINISHED"},
        "from_mesh after moving the object",
    )

    check_eq(world.get_voxels([(0, 0, 0)])[0], 0, "the near face moved off its old cell")
    check_eq(world.get_voxels([(size, 0, 0)])[0], 1, "and onto the next one")
    check_eq(int(np.count_nonzero(dense(world))), size**3, "the body kept its volume")


def test_keys_from_materials_and_colors() -> None:
    section("keys from materials, and from colours")
    clear_objects()
    settings = bpy.context.scene.bsvx
    current = new_world()
    world = current.world

    settings.registry.clear()
    for key, name, color in ((1, "stone", (0.4, 0.4, 0.4, 1.0)), (2, "grass", (0.25, 0.6, 0.2, 1.0))):
        item = settings.registry.add()
        item.voxel_key = key
        item.name = name
        item.color = color
    bpy.ops.bsvx.registry_push()

    world.fill_box((0, 0, 0), (3, 3, 3), 1)
    world.fill_box((0, 4, 0), (3, 5, 3), 2)
    before = dense(world)

    for source, strip in (("MATERIAL", ("attribute",)), ("COLOR", ("attribute", "materials"))):
        bpy.ops.bsvx.to_mesh(source="REGION", region_index=0)
        obj = bpy.context.active_object
        if "attribute" in strip:
            obj.data.attributes.remove(obj.data.attributes[voxel_mesh.KEY_ATTRIBUTE])
            check(obj.data.attributes.get(voxel_mesh.KEY_ATTRIBUTE) is None, f"{source}: key attribute removed")
        if "materials" in strip:
            obj.data.materials.clear()
            check_eq(len(obj.data.materials), 0, f"{source}: materials removed")

        check_eq(
            bpy.ops.bsvx.from_mesh(mode="GRID", key_source=source, auto_register=False),
            {"FINISHED"},
            f"from_mesh with keys from {source}",
        )
        check(np.array_equal(before, dense(world)), f"{source} recovered every key")
        bpy.data.objects.remove(obj, do_unlink=True)

    # A colour with no registry entry becomes one, which is how a .vox model imported by somebody
    # else's add-on turns into a BSVX world with no manual setup.
    bpy.ops.bsvx.to_mesh(source="REGION", region_index=0)
    obj = bpy.context.active_object
    obj.data.attributes.remove(obj.data.attributes[voxel_mesh.KEY_ATTRIBUTE])
    obj.data.materials.clear()
    colors = obj.data.color_attributes[voxel_mesh.COLOR_ATTRIBUTE]
    for datum in colors.data:
        datum.color = (0.9, 0.1, 0.05, 1.0)

    known = len(list(world.registry))
    check_eq(
        bpy.ops.bsvx.from_mesh(mode="GRID", key_source="COLOR", auto_register=True),
        {"FINISHED"},
        "from_mesh registering a new colour",
    )
    check_eq(len(list(world.registry)), known + 1, "the unknown colour got a registry entry")
    check_eq(int(np.count_nonzero(dense(world))), int(np.count_nonzero(before)), "the body is unchanged")
    check_eq(len(set(np.unique(dense(world)).tolist()) - {0}), 1, "and is now all one new key")


def test_boolean_cut() -> None:
    section("a boolean cut is a voxel edit")
    size = 6
    current = _solid_world(size)
    world = current.world
    frame = Frame.from_world(world)

    check_eq(bpy.ops.bsvx.to_mesh(source="REGION", region_index=0), {"FINISHED"}, "to_mesh")
    obj = bpy.context.active_object

    # A 2x2x2 corner, cut on exact voxel boundaries so the result stays on the grid. The box is
    # built from the cell lattice rather than from metres, because which Blender direction "the
    # far corner" is in depends on the world's axis convention. It is then grown by one voxel on
    # whichever side faces out of the body, so the cutter pokes through and the boolean never has
    # to resolve coplanar faces.
    extent = np.asarray(frame.blender_voxel_size())

    def blender_box(lo, hi):
        low = frame.corner_points(np.array([lo], dtype=np.int64))[0]
        high = frame.corner_points(np.array([hi], dtype=np.int64))[0]
        return np.minimum(low, high), np.maximum(low, high)

    cut_low, cut_high = blender_box((size - 2,) * 3, (size,) * 3)
    body_low, body_high = blender_box((0, 0, 0), (size,) * 3)
    for axis in range(3):
        if abs(cut_low[axis] - body_low[axis]) < abs(cut_high[axis] - body_high[axis]):
            cut_low[axis] -= extent[axis]
        else:
            cut_high[axis] += extent[axis]

    bpy.ops.mesh.primitive_cube_add(size=1.0)
    cutter = bpy.context.active_object
    cutter.scale = tuple(cut_high - cut_low)
    cutter.location = tuple(0.5 * (cut_low + cut_high))

    modifier = obj.modifiers.new("cut", "BOOLEAN")
    modifier.object = cutter
    modifier.operation = "DIFFERENCE"

    bpy.context.view_layer.objects.active = obj
    for other in bpy.context.selected_objects:
        other.select_set(False)
    obj.select_set(True)

    check_eq(
        bpy.ops.bsvx.from_mesh(mode="GRID", key_source="ACTIVE", use_modifiers=True),
        {"FINISHED"},
        "from_mesh through the boolean",
    )
    check_eq(world.get_voxels([(size - 1, size - 1, size - 1)])[0], 0, "the cut corner is air")
    check_eq(world.get_voxels([(0, 0, 0)])[0], 1, "the far corner is untouched")
    check_eq(int(np.count_nonzero(dense(world))), size**3 - 8, "exactly the 2x2x2 corner went")


def test_voxelize_a_cube() -> None:
    section("voxelize an arbitrary mesh")
    clear_objects()
    settings = bpy.context.scene.bsvx
    current = new_world()
    current.world.set_registry_entry(1, flags=binding.api().REGISTRY_OPAQUE)

    bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, location=(10.0, 0.0, 10.0))
    sphere = bpy.context.active_object

    settings.active_key = 1
    check(
        not mesh_read.is_voxel_shaped(sphere, Frame.from_world(current.world)),
        "a sphere is not mistaken for a voxel model",
    )
    check_eq(
        bpy.ops.bsvx.from_mesh(mode="AUTO", key_source="ACTIVE", replace_bounds=False),
        {"FINISHED"},
        "from_mesh voxelizes it",
    )

    frame = Frame.from_world(current.world)
    inside = frame.point_to_cell((10.0, 0.0, 10.0))
    outside = frame.point_to_cell((10.0, 0.0, 13.0))
    check_eq(current.world.get_voxels([inside])[0], 1, "the sphere's centre filled")
    check_eq(current.world.get_voxels([outside])[0], 0, "space outside it did not")

    bpy.data.objects.remove(sphere, do_unlink=True)


def test_voxelize_keeps_materials() -> None:
    section("voxelizing a two-material mesh keeps both keys")
    clear_objects()
    settings = bpy.context.scene.bsvx
    current = new_world()
    world = current.world

    settings.registry.clear()
    for key, name in ((1, "left"), (2, "right")):
        item = settings.registry.add()
        item.voxel_key = key
        item.name = name
    bpy.ops.bsvx.registry_push()

    import bmesh

    # Placed away from the origin so every cell it covers lands in one region -- the checks below
    # read a single region, and a body straddling zero would quietly spread over eight of them.
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(2.0, -2.0, 2.0))
    cube = bpy.context.active_object
    for key, name in ((1, "left"), (2, "right")):
        cube.data.materials.append(materials.ensure_material(key, name, (0.5, 0.5, 0.5, 1.0), False))

    # Subdivided so the face a ray enters through actually varies: an unsubdivided cube has one
    # quad facing -X, and every ray in the world would enter through the same material.
    bm = bmesh.new()
    bm.from_mesh(cube.data)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=3, use_grid_fill=True)
    bm.to_mesh(cube.data)
    bm.free()
    for polygon in cube.data.polygons:
        polygon.material_index = 0 if polygon.center.z <= 0.0 else 1

    settings.active_key = 1
    check_eq(
        bpy.ops.bsvx.from_mesh(mode="VOXELIZE", key_source="MATERIAL", replace_bounds=False),
        {"FINISHED"},
        "from_mesh voxelizing by material",
    )
    used = sorted(set(np.unique(dense(world)).tolist()) - {0})
    check_eq(used, [1, 2], "each solid run took the key of the face the ray entered through")


def test_validation_and_export(tmp: Path) -> None:
    section("validate and export")
    clear_objects()
    settings = bpy.context.scene.bsvx
    current = new_world()
    current.world.fill_box((0, 0, 0), (3, 3, 3), 1)

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
        test_cube_corners_are_the_cell()
        test_authoring_and_round_trip(tmp)
        test_mesh_export_shape()
        test_mesh_round_trip_exact()
        test_deleting_faces_deletes_voxels()
        test_dirty_save_and_reopen()
        test_solid_round_trip()
        test_coplanar_merge_round_trip()
        test_object_transform_moves_voxels()
        test_keys_from_materials_and_colors()
        test_boolean_cut()
        test_voxelize_a_cube()
        test_voxelize_keeps_materials()
        test_validation_and_export(tmp)

    session.close_all()
    print(f"\n{CHECKS} checks, {len(FAILURES)} failures")
    for failure in FAILURES:
        print(f"  - {failure}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

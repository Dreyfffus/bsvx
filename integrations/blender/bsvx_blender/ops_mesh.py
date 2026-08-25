"""The bridge: voxels out to a cube mesh, and any mesh back in.

These two operators are the point of the add-on. Everything between them -- box selects, booleans,
Vox Cleaner's decimate/unwrap/bake, Vox Tools, a MagicaVoxel importer, sculpting a shape by hand --
is somebody else's job and better done by the tools that already do it.
"""

from __future__ import annotations

import numpy as np
import bpy
from bpy.props import BoolProperty, EnumProperty, IntProperty, IntVectorProperty

from . import materials, mesh_read, session, voxel_mesh, voxelize
from .frame import Frame
from .registry_sync import pull_registry


def _fail(operator, exc: Exception) -> set:
    operator.report({"ERROR"}, str(exc))
    return {"CANCELLED"}


def _link(context, obj):
    context.collection.objects.link(obj)
    for other in context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


# ----------------------------------------------------------------------------------------------
# Out
# ----------------------------------------------------------------------------------------------


class BSVX_OT_to_mesh(bpy.types.Operator):
    """Build a cube mesh from the world, ready for Vox Cleaner, Vox Tools or plain Blender"""

    bl_idname = "bsvx.to_mesh"
    bl_label = "Export To Mesh"
    bl_options = {"REGISTER", "UNDO"}

    source: EnumProperty(
        name="Source",
        items=(
            ("WORLD", "Whole World", "Every region the world holds"),
            ("REGION", "Region", "One region, by index"),
            ("BOX", "Box", "An explicit inclusive cell box"),
        ),
        default="WORLD",
    )
    region_index: IntProperty(name="Region", min=0, default=0)
    minimum: IntVectorProperty(name="Minimum", size=3, default=(0, 0, 0))
    maximum: IntVectorProperty(name="Maximum", size=3, default=(31, 31, 31))

    cull_interior: BoolProperty(
        name="Cull Interior Faces",
        description=(
            "Emit only faces that touch air. This is what makes the mesh usable -- a solid world "
            "with every face kept is mostly geometry nothing can ever see -- and Import From Mesh "
            "reconstructs the inside exactly, so nothing is lost by it"
        ),
        default=True,
    )
    use_materials: BoolProperty(
        name="Materials",
        description="One material slot per voxel key, coloured from the registry. What Vox Cleaner bakes",
        default=True,
    )
    use_colors: BoolProperty(
        name="Color Attribute",
        description="Also write per-corner colours, for tools that read vertex colours instead of materials",
        default=True,
    )

    def execute(self, context):
        settings = context.scene.bsvx
        try:
            current = session.require(context)
            world = current.world
            frame = Frame.from_world(world)

            if self.source == "REGION":
                if self.region_index >= world.region_count:
                    raise RuntimeError(f"region {self.region_index} does not exist")
                extent = voxel_mesh.region_extent(world)
                coord = world.region_coord(self.region_index)
                lo = np.asarray(coord, dtype=np.int64) * extent
                hi = lo + extent - 1
                cells, keys = voxel_mesh.collect_region(world, self.region_index)
                name = f"bsvx_region_{coord[0]}_{coord[1]}_{coord[2]}"
                region = self.region_index
            elif self.source == "BOX":
                lo = np.minimum(self.minimum, self.maximum).astype(np.int64)
                hi = np.maximum(self.minimum, self.maximum).astype(np.int64)
                cells, keys = voxel_mesh.collect_box(world, lo, hi)
                name, region = "bsvx_box", None
            else:
                bounds = voxel_mesh.world_bounds(world)
                if bounds is None:
                    raise RuntimeError("this world has no regions yet")
                lo, hi = bounds
                cells, keys = voxel_mesh.collect_box(world, lo, hi)
                name, region = world.name or "bsvx_world", None

            if cells.shape[0] > settings.voxel_budget:
                raise RuntimeError(
                    f"{cells.shape[0]:,} voxels exceeds the budget of {settings.voxel_budget:,}. "
                    "Export a region or a box, or raise the budget if this machine can take it"
                )

            palette = voxel_mesh.palette_lookup(world)
            mesh, per_face_keys = voxel_mesh.build_cube_mesh(
                name,
                cells,
                keys,
                frame,
                palette,
                cull_interior=self.cull_interior,
                with_colors=self.use_colors,
            )
            obj = bpy.data.objects.new(name, mesh)
            obj[voxel_mesh.PROP_SESSION] = settings.session_key
            voxel_mesh.tag_bounds(obj, lo, hi, region)
            if self.use_materials:
                materials.assign(obj, per_face_keys, world, palette)
            _link(context, obj)
        except Exception as exc:
            return _fail(self, exc)

        self.report(
            {"INFO"},
            f"{cells.shape[0]:,} voxel(s) -> {len(mesh.polygons):,} face(s) in '{obj.name}'",
        )
        return {"FINISHED"}


# ----------------------------------------------------------------------------------------------
# In
# ----------------------------------------------------------------------------------------------

_KEY_SOURCES = (
    ("AUTO", "Auto", "The bsvx_key face attribute, then materials, then colours, then the active key"),
    ("ATTRIBUTE", "Face Attribute", "The bsvx_key INT attribute on the FACE domain"),
    ("MATERIAL", "Materials", "Material slots: a stamped bsvx_key, a registry name match, else slot index + 1"),
    ("COLOR", "Colors", "Nearest registry colour to each face's colour attribute"),
    ("ACTIVE", "Active Key", "One key for the whole object"),
)


def _resolve_keys(operator, obj, faces, world, settings):
    """Per-face voxel keys, plus the label to report and any registry entries to add."""
    wanted = operator.key_source
    additions: list = []

    if wanted in {"AUTO", "ATTRIBUTE"}:
        keys = mesh_read.keys_from_attribute(faces)
        if keys is not None:
            return keys, "bsvx_key attribute", additions
        if wanted == "ATTRIBUTE":
            raise RuntimeError(
                f"'{obj.name}' has no {mesh_read.KEY_ATTRIBUTE} INT attribute on the FACE domain"
            )

    if wanted in {"AUTO", "MATERIAL"}:
        mapping = mesh_read.material_key_map(obj, settings.registry)
        if mapping is not None:
            table = np.zeros(max(mapping) + 1, dtype=np.uint32)
            for slot, key in mapping.items():
                table[slot] = key
            index = np.clip(faces.material_index, 0, table.size - 1)
            return table[index], f"{len(mapping)} material slot(s)", additions
        if wanted == "MATERIAL":
            raise RuntimeError(f"'{obj.name}' has no material slots")

    if wanted in {"AUTO", "COLOR"}:
        colors = mesh_read.face_colors(faces)
        if colors is not None:
            keys, additions = mesh_read.keys_from_colors(
                colors, settings.registry, auto_register=operator.auto_register
            )
            return keys, "colour attribute", additions
        if wanted == "COLOR":
            raise RuntimeError(f"'{obj.name}' has no colour attribute")

    return (
        np.full(faces.count, settings.active_key, dtype=np.uint32),
        f"active key {settings.active_key}",
        additions,
    )


class BSVX_OT_from_mesh(bpy.types.Operator):
    """Write a mesh into the world: a voxel model exactly, anything else by voxelization"""

    bl_idname = "bsvx.from_mesh"
    bl_label = "Import From Mesh"
    bl_options = {"REGISTER", "UNDO"}

    mode: EnumProperty(
        name="Mode",
        items=(
            ("AUTO", "Auto", "Read a voxel model cell for cell; voxelize anything else"),
            ("GRID", "Voxel Model", "Axis-aligned faces read straight back into cells"),
            ("VOXELIZE", "Voxelize", "Ray-parity fill of an arbitrary mesh"),
        ),
        default="AUTO",
    )
    key_source: EnumProperty(name="Keys From", items=_KEY_SOURCES, default="AUTO")

    fill_interior: BoolProperty(
        name="Fill Interior",
        description=(
            "Recover the solid inside of a culled shell by parity along one axis. Exact for a "
            "closed model; turn it off to import a shell as a shell"
        ),
        default=True,
    )
    replace_bounds: BoolProperty(
        name="Replace Bounds",
        description=(
            "Clear the cell box the mesh covers before writing. This is what makes deletion mean "
            "something: a voxel the user removed leaves no trace in the mesh, so only replacing "
            "the box reproduces their intent. Off merges into whatever is already there"
        ),
        default=True,
    )
    auto_register: BoolProperty(
        name="Register New Colors",
        description="When keys come from colours, give unmatched colours new registry entries",
        default=True,
    )
    surface_only: BoolProperty(
        name="Surface Only",
        description="Voxelization only: mark the cells the surface passes through, for open meshes",
        default=False,
    )
    use_modifiers: BoolProperty(name="Apply Modifiers", default=True)
    selected_only: BoolProperty(
        name="Selected Only",
        description="Import every selected mesh; otherwise only the active object",
        default=True,
    )

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and context.active_object.type == "MESH"

    def execute(self, context):
        settings = context.scene.bsvx
        totals = {"written": 0, "cleared": 0, "voxelized": 0, "unpaired": 0, "skipped_faces": 0}
        labels: list[str] = []
        try:
            current = session.require(context)
            world = current.world
            frame = Frame.from_world(world)
            depsgraph = context.evaluated_depsgraph_get() if self.use_modifiers else None

            candidates = context.selected_objects if self.selected_only else [context.active_object]
            targets = [obj for obj in candidates if obj is not None and obj.type == "MESH"]
            if not targets:
                raise RuntimeError("no mesh object is selected")

            registered = 0
            for original in targets:
                obj = original.evaluated_get(depsgraph) if depsgraph is not None else original
                faces = mesh_read.Faces(obj)
                if faces.count == 0:
                    continue

                keys, label, additions = _resolve_keys(self, obj, faces, world, settings)
                if additions:
                    registered += _register(world, settings, additions)
                    keys, label, _ = _resolve_keys(self, obj, faces, world, settings)

                grid = self.mode == "GRID" or (
                    self.mode == "AUTO"
                    and float(mesh_read.axis_aligned(faces, frame).mean()) >= 0.999
                )

                if grid:
                    cells, cell_keys, report = mesh_read.read(
                        obj,
                        frame,
                        keys=keys,
                        faces=faces,
                        fill_interior=self.fill_interior,
                    )
                    totals["unpaired"] += report["unpaired"]
                    totals["skipped_faces"] += report["skipped_faces"]
                    labels.append(f"{original.name}: voxel model, keys from {label}")
                else:
                    lo, hi = voxelize.object_cell_bounds(obj, frame)
                    cells, polygons = voxelize.voxelize(
                        obj, frame, lo, hi, surface_only=self.surface_only
                    )
                    fallback = np.uint32(settings.active_key)
                    cell_keys = np.where(polygons >= 0, keys[np.clip(polygons, 0, None)], fallback)
                    totals["voxelized"] += int(cells.shape[0])
                    labels.append(f"{original.name}: voxelized, keys from {label}")

                if cells.shape[0] == 0:
                    continue

                if self.replace_bounds:
                    bounds = voxel_mesh.bounds_of(original)
                    lo, hi = bounds if bounds is not None else (cells.min(axis=0), cells.max(axis=0))
                    totals["cleared"] += world.fill_box(
                        tuple(int(v) for v in lo),
                        tuple(int(v) for v in hi),
                        0,
                        create_missing=False,
                    )

                totals["written"] += world.set_voxels(
                    (tuple(int(v) for v in cell) for cell in cells),
                    [int(k) for k in cell_keys],
                    create_missing=True,
                )
        except Exception as exc:
            return _fail(self, exc)

        message = f"wrote {totals['written']:,} voxel(s) from {len(targets)} object(s)"
        if registered:
            message += f"; registered {registered} new key(s)"
        if totals["cleared"]:
            message += f"; cleared {totals['cleared']:,} first"
        if totals["skipped_faces"]:
            message += f"; ignored {totals['skipped_faces']:,} non-axis-aligned face(s)"
        if totals["unpaired"]:
            message += (
                f"; {totals['unpaired']:,} face(s) did not pair, so the model is not closed there "
                "and its interior is incomplete"
            )
        self.report({"WARNING"} if totals["unpaired"] else {"INFO"}, message)
        for line in labels:
            print(f"[bsvx] {line}")
        return {"FINISHED"}


def _register(world, settings, additions) -> int:
    """Add discovered colours to the world's registry, then refresh the UI mirror from it."""
    from .registry_sync import _rgba8

    for key, rgba in additions:
        world.set_registry_entry(int(key), name=f"color_{int(key)}")
        world.set_registry_color(int(key), _rgba8(rgba))
    pull_registry(settings, world)
    return len(additions)


# ----------------------------------------------------------------------------------------------
# Direct authoring
# ----------------------------------------------------------------------------------------------


class BSVX_OT_fill_box(bpy.types.Operator):
    """Fill or erase an inclusive box of cells directly in the world"""

    bl_idname = "bsvx.fill_box"
    bl_label = "Fill Box"
    bl_options = {"REGISTER", "UNDO"}

    minimum: IntVectorProperty(name="Minimum", size=3, default=(0, 0, 0))
    maximum: IntVectorProperty(name="Maximum", size=3, default=(15, 15, 15))
    erase: BoolProperty(name="Erase", description="Write air instead of the active key", default=False)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        settings = context.scene.bsvx
        try:
            current = session.require(context)
            lo = tuple(int(v) for v in np.minimum(self.minimum, self.maximum))
            hi = tuple(int(v) for v in np.maximum(self.minimum, self.maximum))
            key = 0 if self.erase else settings.active_key
            written = current.world.fill_box(lo, hi, key, create_missing=not self.erase)
        except Exception as exc:
            return _fail(self, exc)

        self.report({"INFO"}, f"{'erased' if self.erase else 'filled'} {written:,} cell(s)")
        return {"FINISHED"}


CLASSES = (
    BSVX_OT_to_mesh,
    BSVX_OT_from_mesh,
    BSVX_OT_fill_box,
)

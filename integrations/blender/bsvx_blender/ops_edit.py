"""Checking voxels out into Blender geometry, editing them, and putting them back."""

from __future__ import annotations

import numpy as np
import bpy
from bpy.props import BoolProperty, IntProperty, IntVectorProperty, StringProperty

from . import binding, session, voxelize, working_set
from .frame import Frame
from .registry_sync import pull_registry, push_registry


def _fail(operator, exc: Exception) -> set:
    operator.report({"ERROR"}, str(exc))
    return {"CANCELLED"}


def _link(context, obj):
    context.collection.objects.link(obj)
    for other in context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


def _checkout(operator, context, lo, hi, name, region_index=None):
    settings = context.scene.bsvx
    current = session.require(context)
    world = current.world
    frame = Frame.from_world(world)

    span = np.asarray(hi, dtype=np.int64) - np.asarray(lo, dtype=np.int64) + 1
    if np.any(span <= 0):
        raise RuntimeError("the checkout box is empty")

    cells, keys = working_set.collect_box(world, lo, hi)
    if cells.shape[0] > settings.vertex_budget:
        raise RuntimeError(
            f"{cells.shape[0]:,} voxels exceeds the vertex budget of {settings.vertex_budget:,}. "
            "Check out a smaller box, or raise the budget if this machine can take it"
        )

    obj = working_set.build_object(name, cells, keys, frame, working_set.palette_lookup(world))
    obj[working_set.PROP_SESSION] = settings.session_key
    working_set.tag_bounds(obj, lo, hi, region_index)
    _link(context, obj)

    operator.report({"INFO"}, f"checked out {cells.shape[0]:,} voxel(s) into '{obj.name}'")
    return {"FINISHED"}


class BSVX_OT_checkout_region(bpy.types.Operator):
    """Materialize one region's voxels as an editable point mesh"""

    bl_idname = "bsvx.checkout_region"
    bl_label = "Check Out Region"
    bl_options = {"REGISTER", "UNDO"}

    region_index: IntProperty(name="Region", min=0, default=0)

    def execute(self, context):
        try:
            world = session.require(context).world
            if self.region_index >= world.region_count:
                raise RuntimeError(f"region {self.region_index} does not exist")

            extent = working_set.region_extent(world)
            origin = np.asarray(world.region_coord(self.region_index), dtype=np.int64) * extent
            coord = world.region_coord(self.region_index)
            return _checkout(
                self,
                context,
                origin,
                origin + extent - 1,
                f"bsvx_region_{coord[0]}_{coord[1]}_{coord[2]}",
                self.region_index,
            )
        except Exception as exc:
            return _fail(self, exc)


class BSVX_OT_checkout_all(bpy.types.Operator):
    """Materialize every region the world holds, subject to the vertex budget"""

    bl_idname = "bsvx.checkout_all"
    bl_label = "Check Out Everything"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            world = session.require(context).world
            bounds = working_set.world_bounds(world)
            if bounds is None:
                raise RuntimeError("this world has no regions yet")
            return _checkout(self, context, bounds[0], bounds[1], "bsvx_world")
        except Exception as exc:
            return _fail(self, exc)


class BSVX_OT_checkout_box(bpy.types.Operator):
    """Materialize an explicit box of voxel cells"""

    bl_idname = "bsvx.checkout_box"
    bl_label = "Check Out Box"
    bl_options = {"REGISTER", "UNDO"}

    minimum: IntVectorProperty(name="Minimum", size=3, default=(0, 0, 0))
    maximum: IntVectorProperty(name="Maximum", size=3, default=(31, 31, 31))

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        try:
            lo = np.minimum(self.minimum, self.maximum)
            hi = np.maximum(self.minimum, self.maximum)
            return _checkout(self, context, lo, hi, "bsvx_box")
        except Exception as exc:
            return _fail(self, exc)


class BSVX_OT_commit(bpy.types.Operator):
    """Write the selected working sets back into the world, replacing the boxes they own"""

    bl_idname = "bsvx.commit"
    bl_label = "Commit Working Set"
    bl_options = {"REGISTER", "UNDO"}

    selected_only: BoolProperty(
        name="Selected Only",
        description="Commit every selected working set; otherwise only the active object",
        default=True,
    )

    def execute(self, context):
        try:
            current = session.require(context)
            frame = Frame.from_world(current.world)

            candidates = context.selected_objects if self.selected_only else [context.active_object]
            targets = [obj for obj in candidates if working_set.is_working_set(obj)]
            if not targets:
                raise RuntimeError("no BSVX working set is selected")

            totals = {"written": 0, "outside": 0, "collisions": 0}
            for obj in targets:
                report = working_set.commit(current.world, obj, frame)
                for key in totals:
                    totals[key] += report[key]
        except Exception as exc:
            return _fail(self, exc)

        message = f"committed {totals['written']:,} voxel(s) from {len(targets)} object(s)"
        if totals["outside"]:
            message += f"; {totals['outside']:,} landed outside the checked-out box"
        if totals["collisions"]:
            message += f"; {totals['collisions']:,} shared a cell and only the last survived"
        self.report({"INFO"}, message)
        return {"FINISHED"}


class BSVX_OT_voxelize_object(bpy.types.Operator):
    """Fill the active mesh object's volume with the active voxel key"""

    bl_idname = "bsvx.voxelize_object"
    bl_label = "Voxelize Object"
    bl_options = {"REGISTER", "UNDO"}

    surface_only: BoolProperty(
        name="Surface Only",
        description=(
            "Mark only the cells the surface passes through. Use this for open or non-manifold "
            "meshes, where the solid fill's ray parity has no closed volume to work from"
        ),
        default=False,
    )
    use_modifiers: BoolProperty(name="Apply Modifiers", default=True)

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and context.active_object.type == "MESH"

    def execute(self, context):
        settings = context.scene.bsvx
        try:
            current = session.require(context)
            frame = Frame.from_world(current.world)

            source = context.active_object
            if self.use_modifiers:
                source = source.evaluated_get(context.evaluated_depsgraph_get())

            lo, hi = voxelize.object_cell_bounds(source, frame)
            cells = voxelize.voxelize(source, frame, lo, hi, surface_only=self.surface_only)
            if cells.shape[0] == 0:
                self.report({"WARNING"}, "the object covered no voxel cells")
                return {"CANCELLED"}

            written = current.world.set_voxels(
                (tuple(int(v) for v in cell) for cell in cells),
                settings.active_key,
                create_missing=True,
            )
        except Exception as exc:
            return _fail(self, exc)

        self.report({"INFO"}, f"voxelized {written:,} cell(s) with key {settings.active_key}")
        return {"FINISHED"}


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


# --- registry ---------------------------------------------------------------------------------


class BSVX_OT_registry_pull(bpy.types.Operator):
    """Replace the registry list with the world's"""

    bl_idname = "bsvx.registry_pull"
    bl_label = "Pull Registry From World"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            count = pull_registry(context.scene.bsvx, session.require(context).world)
        except Exception as exc:
            return _fail(self, exc)
        self.report({"INFO"}, f"pulled {count} registry entr(ies)")
        return {"FINISHED"}


class BSVX_OT_registry_push(bpy.types.Operator):
    """Write the registry list into the world"""

    bl_idname = "bsvx.registry_push"
    bl_label = "Push Registry To World"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            count = push_registry(context.scene.bsvx, session.require(context).world)
        except Exception as exc:
            return _fail(self, exc)
        self.report({"INFO"}, f"pushed {count} registry entr(ies)")
        return {"FINISHED"}


class BSVX_OT_registry_add(bpy.types.Operator):
    """Add a registry row, taking the next free voxel key"""

    bl_idname = "bsvx.registry_add"
    bl_label = "Add Registry Entry"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.bsvx
        used = {item.voxel_key for item in settings.registry}
        key = 1
        while key in used:
            key += 1

        item = settings.registry.add()
        item.voxel_key = key
        item.name = f"voxel_{key}"
        settings.registry_index = len(settings.registry) - 1
        return {"FINISHED"}


class BSVX_OT_registry_remove(bpy.types.Operator):
    """Remove the selected registry row, and its entry in the world"""

    bl_idname = "bsvx.registry_remove"
    bl_label = "Remove Registry Entry"
    bl_options = {"REGISTER", "UNDO"}

    from_world: BoolProperty(
        name="Also Remove From World",
        description="Voxels already carrying this key keep it, and validation will report them",
        default=True,
    )

    def execute(self, context):
        settings = context.scene.bsvx
        if not settings.registry:
            return {"CANCELLED"}
        index = min(settings.registry_index, len(settings.registry) - 1)
        key = settings.registry[index].voxel_key

        if self.from_world:
            current = session.active(context)
            if current is not None:
                try:
                    current.world.remove_registry_entry(key)
                except Exception as exc:
                    self.report({"WARNING"}, str(exc))

        settings.registry.remove(index)
        settings.registry_index = max(0, index - 1)
        return {"FINISHED"}


class BSVX_OT_registry_from_materials(bpy.types.Operator):
    """Build registry rows from the active object's material slots, one key per slot"""

    bl_idname = "bsvx.registry_from_materials"
    bl_label = "Registry From Materials"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and bool(context.active_object.material_slots)

    def execute(self, context):
        settings = context.scene.bsvx
        settings.registry.clear()

        for index, slot in enumerate(context.active_object.material_slots):
            material = slot.material
            item = settings.registry.add()
            item.voxel_key = index + 1
            item.material_id = index
            item.name = material.name if material else f"slot_{index}"
            if material is not None:
                item.color = tuple(material.diffuse_color)

        self.report({"INFO"}, f"built {len(settings.registry)} entr(ies); push to apply")
        return {"FINISHED"}


class BSVX_OT_make_palette(bpy.types.Operator):
    """Build a .btx palette archive from the registry colours and attach it to the world"""

    bl_idname = "bsvx.make_palette"
    bl_label = "Build Palette Texture"
    bl_options = {"REGISTER", "UNDO"}

    texture_id: StringProperty(name="Texture Id", default="palette")

    def execute(self, context):
        settings = context.scene.bsvx
        try:
            current = session.require(context)
            if not settings.registry:
                raise RuntimeError("the registry is empty")

            # make_palette assigns key i+1 to colour i, so the rows have to be in key order and
            # contiguous from 1 or the keys it writes will not be the ones on screen.
            ordered = sorted(settings.registry, key=lambda item: item.voxel_key)
            expected = list(range(1, len(ordered) + 1))
            if [item.voxel_key for item in ordered] != expected:
                raise RuntimeError(
                    "Build Palette needs voxel keys 1..N with no gaps; this registry has "
                    f"{[item.voxel_key for item in ordered]}"
                )

            colors = [
                tuple(int(max(0.0, min(1.0, c)) * 255 + 0.5) for c in item.color) for item in ordered
            ]
            current.world.make_palette(colors, texture_id=self.texture_id)
            pull_registry(settings, current.world)
        except Exception as exc:
            return _fail(self, exc)

        self.report({"INFO"}, f"built a {len(colors)}-colour palette")
        return {"FINISHED"}


# --- housekeeping -----------------------------------------------------------------------------


class BSVX_OT_validate(bpy.types.Operator):
    """Check the world and list what it finds"""

    bl_idname = "bsvx.validate"
    bl_label = "Validate World"
    bl_options = {"REGISTER"}

    deep: BoolProperty(
        name="Deep",
        description="Decode every chunk and check every key actually used, not just the four each summary records",
        default=False,
    )

    def execute(self, context):
        settings = context.scene.bsvx
        try:
            issues = session.require(context).world.validate(deep=self.deep)
        except Exception as exc:
            return _fail(self, exc)

        settings.issues.clear()
        errors = 0
        for issue in issues:
            row = settings.issues.add()
            row.severity = issue.severity
            row.code = issue.code
            row.message = str(issue)
            if issue.is_error:
                errors += 1

        level = {"ERROR"} if errors else {"INFO"}
        self.report(level, f"{len(issues)} issue(s), {errors} error(s)")
        return {"FINISHED"}


class BSVX_OT_compact(bpy.types.Operator):
    """Rewrite each region's section blobs to hold only live data"""

    bl_idname = "bsvx.compact"
    bl_label = "Compact World"
    bl_options = {"REGISTER"}

    def execute(self, context):
        try:
            reclaimed = session.require(context).world.compact()
        except Exception as exc:
            return _fail(self, exc)
        self.report({"INFO"}, f"reclaimed {reclaimed:,} byte(s)")
        return {"FINISHED"}


class BSVX_OT_remove_region(bpy.types.Operator):
    """Remove a region. Saving afterwards needs Prune Orphans or the file comes back"""

    bl_idname = "bsvx.remove_region"
    bl_label = "Remove Region"
    bl_options = {"REGISTER", "UNDO"}

    region_index: IntProperty(name="Region", min=0, default=0)

    def execute(self, context):
        try:
            session.require(context).world.remove_region(self.region_index)
        except Exception as exc:
            return _fail(self, exc)
        self.report({"INFO"}, f"removed region {self.region_index}; every index above it shifted down")
        return {"FINISHED"}


CLASSES = (
    BSVX_OT_checkout_region,
    BSVX_OT_checkout_all,
    BSVX_OT_checkout_box,
    BSVX_OT_commit,
    BSVX_OT_voxelize_object,
    BSVX_OT_fill_box,
    BSVX_OT_registry_pull,
    BSVX_OT_registry_push,
    BSVX_OT_registry_add,
    BSVX_OT_registry_remove,
    BSVX_OT_registry_from_materials,
    BSVX_OT_make_palette,
    BSVX_OT_validate,
    BSVX_OT_compact,
    BSVX_OT_remove_region,
)

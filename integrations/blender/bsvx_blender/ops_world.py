"""World-level operators: the registry, palettes, validation and housekeeping.

Everything here acts on the world itself rather than on Blender geometry. The geometry bridge is
``ops_mesh.py``; reading and writing files is ``ops_io.py``.
"""

from __future__ import annotations

import bpy
from bpy.props import BoolProperty, IntProperty, StringProperty

from . import session
from .registry_sync import pull_registry, push_registry


def _fail(operator, exc: Exception) -> set:
    operator.report({"ERROR"}, str(exc))
    return {"CANCELLED"}


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

        from .materials import PROP_KEY

        for index, slot in enumerate(context.active_object.material_slots):
            material = slot.material
            item = settings.registry.add()
            # A material this add-on made carries its key; slot index + 1 is the fallback, and the
            # convention every .vox importer follows, so a foreign model still lands sensibly.
            stamped = material.get(PROP_KEY) if material is not None else None
            item.voxel_key = int(stamped) if stamped else index + 1
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

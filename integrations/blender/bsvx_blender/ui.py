"""Panels in the 3D viewport's N-panel, under a "BSVX" tab."""

from __future__ import annotations

import bpy

from . import binding, session, voxel_mesh


class BSVX_UL_registry(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_prop, index):
        row = layout.row(align=True)
        row.prop(item, "color", text="")
        row.label(text=str(item.voxel_key))
        row.prop(item, "name", text="", emboss=False)


class BSVX_UL_issues(bpy.types.UIList):
    ICONS = {0: "INFO", 1: "ERROR", 2: "CANCEL"}

    def draw_item(self, context, layout, data, item, icon, active_data, active_prop, index):
        layout.label(text=item.message, icon=self.ICONS.get(item.severity, "INFO"))


class _Panel:
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "BSVX"


class BSVX_PT_world(_Panel, bpy.types.Panel):
    bl_label = "World"
    bl_idname = "BSVX_PT_world"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.bsvx

        ok, message = binding.available()
        if not ok:
            box = layout.box()
            box.alert = True
            for line in message.splitlines():
                box.label(text=line, icon="ERROR")
            return

        current = session.active(context)
        if current is None:
            if settings.path:
                layout.label(text="handle lost on reload", icon="INFO")
                layout.operator("bsvx.reopen_world", icon="FILE_REFRESH")
            row = layout.row(align=True)
            row.operator("bsvx.open_world", icon="FILEBROWSER")
            row.operator("bsvx.new_world", icon="ADD")

            column = layout.column(align=True)
            column.prop(settings, "chunk_size")
            column.prop(settings, "region_size")
            column.prop(settings, "voxel_size")
            return

        world = current.world
        box = layout.box()
        box.label(text=world.name or "(unnamed)", icon="WORLD")
        box.label(text=f"{world.region_count} region(s), {len(world.registry)} registry entr(ies)")
        geometry = world.geometry
        box.label(
            text=f"chunk {geometry.chunk_size_x}x{geometry.chunk_size_y}x{geometry.chunk_size_z}"
            f"  region {geometry.region_size_x}x{geometry.region_size_y}x{geometry.region_size_z}"
        )
        try:
            box.label(text=f"axes: {binding.api().axis_convention_name(world.axis_convention)}")
        except Exception:
            pass
        if current.unsaved:
            box.label(text="never saved", icon="ERROR")
        elif world.is_dirty:
            box.label(text="unsaved changes", icon="FILE_TICK")

        for warning in current.warnings[:3]:
            layout.label(text=warning, icon="ERROR")

        row = layout.row(align=True)
        row.operator("bsvx.save_world", icon="FILE_TICK")
        row.operator("bsvx.save_world_as", text="Save As", icon="FILE_NEW")
        row = layout.row(align=True)
        row.operator("bsvx.export_region", text="Export .bvx", icon="EXPORT")
        row.operator("bsvx.close_world", text="Close", icon="X")


class BSVX_PT_mesh(_Panel, bpy.types.Panel):
    bl_label = "Mesh"
    bl_idname = "BSVX_PT_mesh"
    bl_parent_id = "BSVX_PT_world"

    @classmethod
    def poll(cls, context):
        return session.active(context) is not None

    def draw(self, context):
        layout = self.layout
        settings = context.scene.bsvx

        layout.label(text="A voxel is a cube; its key is the bsvx_key face attribute.")
        layout.label(text="Edit with Vox Cleaner, Vox Tools or plain Blender, then import back.")

        column = layout.column(align=True)
        column.operator("bsvx.to_mesh", icon="EXPORT").source = "WORLD"
        row = column.row(align=True)
        row.operator("bsvx.to_mesh", text="Region", icon="MESH_GRID").source = "REGION"
        row.operator("bsvx.to_mesh", text="Box", icon="MESH_CUBE").source = "BOX"
        layout.prop(settings, "voxel_budget")

        obj = context.active_object
        if voxel_mesh.is_voxel_mesh(obj):
            bounds = voxel_mesh.bounds_of(obj)
            if bounds is not None:
                lo, hi = bounds
                box = layout.box()
                box.label(text=f"owns [{lo[0]}, {lo[1]}, {lo[2]}] .. [{hi[0]}, {hi[1]}, {hi[2]}]")
                box.label(text=f"{len(obj.data.polygons):,} face(s)")

        layout.operator("bsvx.from_mesh", icon="IMPORT")


class BSVX_PT_authoring(_Panel, bpy.types.Panel):
    bl_label = "Authoring"
    bl_idname = "BSVX_PT_authoring"
    bl_parent_id = "BSVX_PT_world"

    @classmethod
    def poll(cls, context):
        return session.active(context) is not None

    def draw(self, context):
        layout = self.layout
        layout.prop(context.scene.bsvx, "active_key")
        layout.operator("bsvx.fill_box", icon="CUBE")


class BSVX_PT_registry(_Panel, bpy.types.Panel):
    bl_label = "Registry"
    bl_idname = "BSVX_PT_registry"
    bl_parent_id = "BSVX_PT_world"

    @classmethod
    def poll(cls, context):
        return session.active(context) is not None

    def draw(self, context):
        layout = self.layout
        settings = context.scene.bsvx

        row = layout.row()
        row.template_list("BSVX_UL_registry", "", settings, "registry", settings, "registry_index")
        column = row.column(align=True)
        column.operator("bsvx.registry_add", text="", icon="ADD")
        column.operator("bsvx.registry_remove", text="", icon="REMOVE")

        if 0 <= settings.registry_index < len(settings.registry):
            item = settings.registry[settings.registry_index]
            box = layout.box()
            box.prop(item, "voxel_key")
            box.prop(item, "name")
            box.prop(item, "color")
            box.prop(item, "material_id")
            flags = box.row(align=True)
            flags.prop(item, "opaque", toggle=True)
            flags.prop(item, "emissive", toggle=True)
            flags.prop(item, "special", toggle=True)
            flags.prop(item, "collidable", toggle=True)

        row = layout.row(align=True)
        row.operator("bsvx.registry_pull", text="Pull", icon="IMPORT")
        row.operator("bsvx.registry_push", text="Push", icon="EXPORT")
        layout.operator("bsvx.registry_from_materials", icon="MATERIAL")
        layout.operator("bsvx.make_palette", icon="COLOR")


class BSVX_PT_validation(_Panel, bpy.types.Panel):
    bl_label = "Validation"
    bl_idname = "BSVX_PT_validation"
    bl_parent_id = "BSVX_PT_world"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return session.active(context) is not None

    def draw(self, context):
        layout = self.layout
        settings = context.scene.bsvx

        row = layout.row(align=True)
        row.operator("bsvx.validate", text="Validate").deep = False
        row.operator("bsvx.validate", text="Deep").deep = True
        layout.operator("bsvx.compact", icon="FILE_REFRESH")

        if settings.issues:
            layout.template_list("BSVX_UL_issues", "", settings, "issues", settings, "issues_index")


CLASSES = (
    BSVX_UL_registry,
    BSVX_UL_issues,
    BSVX_PT_world,
    BSVX_PT_mesh,
    BSVX_PT_authoring,
    BSVX_PT_registry,
    BSVX_PT_validation,
)

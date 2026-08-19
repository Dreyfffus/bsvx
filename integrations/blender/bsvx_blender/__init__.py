"""BSVX for Blender -- author, export and save .bsvx worlds.

Registration only. What a voxel *is* in Blender is settled in ``voxel_mesh.py`` (a cube, in the
mesh shape the rest of the voxel ecosystem speaks); reading one back is ``mesh_read.py``; the
coordinate mapping is settled in ``frame.py``. Everything else is operators over those.

Editing is deliberately not here. Vox Cleaner, Vox Tools and Blender's own tools already do it
better than a format add-on ever would, so this one's job is the round trip they sit inside.
"""

from __future__ import annotations

import bpy

from . import ops_io, ops_mesh, ops_world, props, session, ui

bl_info = {
    "name": "BSVX (Basil Voxel)",
    "author": "bsvx",
    "version": (4, 0, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > BSVX",
    "description": "Author, export and write BSVX voxel worlds",
    "category": "Import-Export",
}

_OPERATOR_CLASSES = ops_io.CLASSES + ops_mesh.CLASSES + ops_world.CLASSES


def _menu_import(self, context):
    self.layout.operator("bsvx.open_world", text="BSVX World (.toml/.bvx)")


def _menu_export(self, context):
    self.layout.operator("bsvx.save_world_as", text="BSVX World (directory)")
    self.layout.operator("bsvx.export_region", text="BSVX Region (.bvx)")


def register() -> None:
    props.register()
    for cls in _OPERATOR_CLASSES:
        bpy.utils.register_class(cls)
    for cls in ui.CLASSES:
        bpy.utils.register_class(cls)

    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister() -> None:
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)

    for cls in reversed(ui.CLASSES):
        bpy.utils.unregister_class(cls)
    for cls in reversed(_OPERATOR_CLASSES):
        bpy.utils.unregister_class(cls)
    props.unregister()

    # The C handles outlive Blender's own teardown otherwise, and a reload of the add-on would
    # leak one world per cycle.
    session.close_all()

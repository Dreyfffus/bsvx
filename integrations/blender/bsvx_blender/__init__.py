"""BSVX for Blender -- save, author, modify and export .bsvx worlds.

Registration only. What a voxel *is* in Blender is settled in ``working_set.py``; the coordinate
mapping is settled in ``frame.py``. Everything else is operators over those two.
"""

from __future__ import annotations

import bpy

from . import ops_edit, ops_io, props, session, ui

bl_info = {
    "name": "BSVX (Basil Voxel)",
    "author": "bsvx",
    "version": (4, 0, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > BSVX",
    "description": "Read, author and write BSVX voxel worlds",
    "category": "Import-Export",
}

_OPERATOR_CLASSES = ops_io.CLASSES + ops_edit.CLASSES


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

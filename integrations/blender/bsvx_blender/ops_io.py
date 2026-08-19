"""Opening, creating, saving and exporting worlds."""

from __future__ import annotations

import os

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ImportHelper

from . import binding, session
from .registry_sync import pull_registry


def _fail(operator, exc: Exception) -> set:
    operator.report({"ERROR"}, str(exc))
    return {"CANCELLED"}


class BSVX_OT_new_world(bpy.types.Operator):
    """Create an empty world in memory from the geometry settings"""

    bl_idname = "bsvx.new_world"
    bl_label = "New BSVX World"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.bsvx
        try:
            api = binding.api()
            world = api.World.create(
                chunk_size=tuple(settings.chunk_size),
                region_size=tuple(settings.region_size),
            )
            world.set_units(voxel_size=settings.voxel_size)
            world.name = "untitled"
        except Exception as exc:
            return _fail(self, exc)

        session.close(settings.session_key)
        settings.session_key = session.open_session(world, path="", unsaved=True)
        settings.path = ""
        settings.registry.clear()
        self.report({"INFO"}, "created an empty world")
        return {"FINISHED"}


class BSVX_OT_open_world(bpy.types.Operator, ImportHelper):
    """Open a .bsvx world directory, its manifest.toml, or a standalone .bvx region"""

    bl_idname = "bsvx.open_world"
    bl_label = "Open BSVX World"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".toml"
    filter_glob: StringProperty(default="*.toml;*.bvx", options={"HIDDEN"})

    ignore_hash_mismatch: BoolProperty(
        name="Ignore Hash Mismatch",
        description="Open regions whose stamped manifest hash does not match, so they can be repaired",
        default=True,
    )

    def execute(self, context):
        settings = context.scene.bsvx
        path = self.filepath
        # The library takes a directory, a manifest, or a .bvx; a file browser can only return a
        # file, so a manifest.toml selection is normalized to the directory it names.
        if os.path.basename(path).lower() == "manifest.toml":
            path = os.path.dirname(path)

        try:
            api = binding.api()
            if path.lower().endswith(".bvx"):
                world = api.World.load_region(path)
            else:
                world = api.World.load(path, ignore_hash_mismatch=self.ignore_hash_mismatch)
        except Exception as exc:
            return _fail(self, exc)

        warnings = world.warnings
        session.close(settings.session_key)
        settings.session_key = session.open_session(world, path=path, warnings=warnings)
        settings.path = path
        settings.ignore_hash_mismatch = self.ignore_hash_mismatch

        pull_registry(settings, world)

        for message in warnings:
            self.report({"WARNING"}, message)
        self.report({"INFO"}, f"opened {path} ({world.region_count} region(s))")
        return {"FINISHED"}


class BSVX_OT_reopen_world(bpy.types.Operator):
    """Reopen the world this scene records, after a .blend reload dropped the handle"""

    bl_idname = "bsvx.reopen_world"
    bl_label = "Reopen BSVX World"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        settings = getattr(context.scene, "bsvx", None)
        return settings is not None and bool(settings.path) and session.active(context) is None

    def execute(self, context):
        try:
            reopened = session.reopen(context)
        except Exception as exc:
            return _fail(self, exc)
        pull_registry(context.scene.bsvx, reopened.world)
        self.report({"INFO"}, f"reopened {reopened.path}")
        return {"FINISHED"}


class BSVX_OT_close_world(bpy.types.Operator):
    """Close the open world. Uncommitted working sets are not written"""

    bl_idname = "bsvx.close_world"
    bl_label = "Close BSVX World"
    bl_options = {"REGISTER"}

    def execute(self, context):
        settings = context.scene.bsvx
        session.close(settings.session_key)
        settings.session_key = ""
        self.report({"INFO"}, "closed")
        return {"FINISHED"}


class BSVX_OT_save_world(bpy.types.Operator):
    """Write changed regions back to the path this world came from"""

    bl_idname = "bsvx.save_world"
    bl_label = "Save BSVX World"
    bl_options = {"REGISTER"}

    prune_orphans: BoolProperty(
        name="Prune Orphans",
        description=(
            "Delete .bvx and .btx files the manifest no longer references. Required after removing "
            "a region, or auto-discovery finds the orphan on the next load and undoes the deletion"
        ),
        default=False,
    )
    backup: BoolProperty(name="Keep Backup", description="Keep the previous contents as <file>.bak", default=False)

    def execute(self, context):
        try:
            current = session.require(context)
            if current.unsaved or not current.path:
                self.report({"ERROR"}, "this world has never been saved -- use Save As")
                return {"CANCELLED"}
            report = current.world.save(
                current.path,
                dirty_only=True,
                prune_orphans=self.prune_orphans,
                backup=self.backup,
            )
        except Exception as exc:
            return _fail(self, exc)

        note = " (widened to a full rewrite)" if report.full_rewrite else ""
        self.report(
            {"INFO"},
            f"wrote {report.files_written} file(s), skipped {report.files_skipped}, "
            f"{report.bytes_written} bytes{note}",
        )
        return {"FINISHED"}


class BSVX_OT_save_world_as(bpy.types.Operator, ImportHelper):
    """Write the whole world to a directory, creating it if needed"""

    bl_idname = "bsvx.save_world_as"
    bl_label = "Save BSVX World As"
    bl_options = {"REGISTER"}

    filename_ext = ""
    use_filter_folder = True
    filter_glob: StringProperty(default="*.toml", options={"HIDDEN"})

    compact_first: BoolProperty(
        name="Compact First",
        description="Rewrite each region's section blobs to hold only live data before writing",
        default=True,
    )

    def execute(self, context):
        try:
            current = session.require(context)
            root = self.filepath
            if os.path.basename(root).lower() == "manifest.toml":
                root = os.path.dirname(root)
            elif os.path.splitext(root)[1]:
                root = os.path.dirname(root)
            report = current.world.save(root, compact_first=self.compact_first)
        except Exception as exc:
            return _fail(self, exc)

        settings = context.scene.bsvx
        settings.path = root
        current.path = root
        current.unsaved = False
        self.report({"INFO"}, f"wrote {report.files_written} file(s) to {root}")
        return {"FINISHED"}


class BSVX_OT_export_region(bpy.types.Operator, ImportHelper):
    """Write the world's single region as a standalone .bvx"""

    bl_idname = "bsvx.export_region"
    bl_label = "Export Standalone .bvx"
    bl_options = {"REGISTER"}

    filename_ext = ".bvx"
    filter_glob: StringProperty(default="*.bvx", options={"HIDDEN"})

    def execute(self, context):
        try:
            current = session.require(context)
            if current.world.region_count != 1:
                self.report(
                    {"ERROR"},
                    f"a standalone .bvx holds exactly one region; this world has "
                    f"{current.world.region_count}. Use Save As for a multi-region world",
                )
                return {"CANCELLED"}
            current.world.save_region(self.filepath)
        except Exception as exc:
            return _fail(self, exc)

        self.report({"INFO"}, f"wrote {self.filepath}")
        return {"FINISHED"}


CLASSES = (
    BSVX_OT_new_world,
    BSVX_OT_open_world,
    BSVX_OT_reopen_world,
    BSVX_OT_close_world,
    BSVX_OT_save_world,
    BSVX_OT_save_world_as,
    BSVX_OT_export_region,
)

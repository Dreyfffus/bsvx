"""Scene-side state: the registry mirror, and the settings the operators read.

The registry lives in two places on purpose. The world's own registry is authoritative and is what
gets written to disk; this ``CollectionProperty`` is the editable mirror, because Blender's UI can
only bind to Blender properties. ``bsvx.registry_pull`` and ``bsvx.registry_push`` move between
them explicitly rather than syncing on every property change, so an accidental keystroke cannot
rewrite a world's registry -- which is the datum that, if lost, orphans every voxel in the file.
"""

from __future__ import annotations

import bpy


class BsvxRegistryEntry(bpy.types.PropertyGroup):
    voxel_key: bpy.props.IntProperty(
        name="Key",
        description="The uint32 written into every voxel using this entry. 0 is air and cannot be registered",
        min=1,
        default=1,
    )
    name: bpy.props.StringProperty(
        name="Name",
        description="Round-trips through the manifest; the format also stores its hash on the entry",
    )
    color: bpy.props.FloatVectorProperty(
        name="Color",
        description="Display colour, stored on the registry so any reader of the format finds it",
        subtype="COLOR",
        size=4,
        min=0.0,
        max=1.0,
        default=(0.8, 0.8, 0.8, 1.0),
    )
    material_id: bpy.props.IntProperty(
        name="Material",
        description="Index into an attached .btx archive's material table",
        min=0,
        default=0,
    )
    opaque: bpy.props.BoolProperty(name="Opaque", default=True)
    emissive: bpy.props.BoolProperty(name="Emissive", default=False)
    special: bpy.props.BoolProperty(name="Special", default=False)
    collidable: bpy.props.BoolProperty(name="Collidable", default=True)


class BsvxValidationIssue(bpy.types.PropertyGroup):
    severity: bpy.props.IntProperty(name="Severity", default=0)
    code: bpy.props.IntProperty(name="Code", default=0)
    message: bpy.props.StringProperty(name="Message")


class BsvxSceneSettings(bpy.types.PropertyGroup):
    #: Key into the module-level session table. Empty means no world is open in this session --
    #: which is also the state a freshly loaded .blend is in, since a C handle cannot be saved.
    session_key: bpy.props.StringProperty(name="Session", default="")
    path: bpy.props.StringProperty(name="Path", subtype="DIR_PATH", default="")

    ignore_hash_mismatch: bpy.props.BoolProperty(
        name="Ignore Hash Mismatch",
        description=(
            "Open a world whose regions were baked against a different manifest. Every .bvx stores "
            "the hash of the manifest text it was written against, so a hand edit or a CRLF "
            "checkout makes the whole world refuse to load; an editor has to open it to repair it"
        ),
        default=True,
    )

    chunk_size: bpy.props.IntVectorProperty(
        name="Chunk Size",
        description="Voxels per chunk. Fixed once any region holds a chunk",
        size=3,
        min=1,
        max=0xFFFF,
        default=(32, 32, 32),
    )
    region_size: bpy.props.IntVectorProperty(
        name="Region Size",
        description="Chunks per region, on each axis",
        size=3,
        min=1,
        max=0xFFFF,
        default=(4, 4, 4),
    )
    voxel_size: bpy.props.FloatProperty(
        name="Voxel Size",
        description="Metres per voxel. Travels with the file, so importers do not have to guess",
        min=1e-6,
        default=0.25,
    )

    active_key: bpy.props.IntProperty(
        name="Active Key",
        description="The voxel key authoring operators write",
        min=1,
        default=1,
    )

    vertex_budget: bpy.props.IntProperty(
        name="Vertex Budget",
        description=(
            "Refuse to check out more voxels than this. A 512-cubed world is 134 million voxels; "
            "materializing one is not an operation Blender survives"
        ),
        min=1,
        default=2_000_000,
    )

    registry: bpy.props.CollectionProperty(type=BsvxRegistryEntry)
    registry_index: bpy.props.IntProperty(default=0)

    issues: bpy.props.CollectionProperty(type=BsvxValidationIssue)
    issues_index: bpy.props.IntProperty(default=0)


CLASSES = (BsvxRegistryEntry, BsvxValidationIssue, BsvxSceneSettings)


def register() -> None:
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.bsvx = bpy.props.PointerProperty(type=BsvxSceneSettings)


def unregister() -> None:
    del bpy.types.Scene.bsvx
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)

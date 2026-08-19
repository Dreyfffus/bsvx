"""One Blender material per voxel key.

This is the interchange the rest of the ecosystem runs on. Vox Cleaner bakes material properties --
base colour, roughness, metallic, emission -- into a texture, and the MagicaVoxel importers it is
built on produce one material per palette entry. Matching that convention is the difference between
a BSVX world those tools can clean and a mesh they refuse.

The key is stamped on the material as a custom property rather than parsed back out of its name.
Blender renames on collision (``bsvx_001_stone.001``) and users rename on purpose; a stamped
integer survives both, and a mesh that lost it still falls back to slot index + 1, which is the
convention every voxel importer already follows.
"""

from __future__ import annotations

import numpy as np

PROP_KEY = "bsvx_key"


def material_name(key: int, name: str) -> str:
    clean = "".join(c for c in (name or "") if c.isalnum() or c in "_-")
    return f"bsvx_{key:03d}_{clean}" if clean else f"bsvx_{key:03d}"


def _configure(material, key: int, rgba, emissive: bool) -> None:
    material[PROP_KEY] = int(key)
    material.diffuse_color = rgba
    material.use_nodes = True

    node = next((n for n in material.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if node is None:
        return
    for socket, value in (("Base Color", rgba), ("Roughness", 1.0), ("Metallic", 0.0)):
        if socket in node.inputs:
            node.inputs[socket].default_value = value
    # Named "Emission" before 4.0 and "Emission Color" after; neither is worth failing over.
    for socket in ("Emission Color", "Emission"):
        if socket in node.inputs:
            node.inputs[socket].default_value = rgba
            break
    if "Emission Strength" in node.inputs:
        node.inputs["Emission Strength"].default_value = 1.0 if emissive else 0.0


def ensure_material(key: int, name: str, rgba, emissive: bool):
    """The material for one voxel key, reused when one of the right name already carries the key."""
    import bpy

    wanted = material_name(key, name)
    existing = bpy.data.materials.get(wanted)
    if existing is not None and existing.get(PROP_KEY) == int(key):
        _configure(existing, key, rgba, emissive)
        return existing

    material = bpy.data.materials.new(wanted)
    _configure(material, key, rgba, emissive)
    return material


def assign(obj, per_face_keys: np.ndarray, world, palette: dict) -> dict[int, int]:
    """Gives the object one material slot per key its faces use. Returns key -> slot index."""
    from . import binding
    from .voxel_mesh import colors_for

    api = binding.api()
    emissive_keys = set()
    for entry in world.registry:
        if entry.flags & api.REGISTRY_EMISSIVE:
            emissive_keys.add(int(entry.voxel_key))

    keys = np.unique(np.asarray(per_face_keys, dtype=np.uint32))
    colors = colors_for(keys, palette)

    obj.data.materials.clear()
    slots: dict[int, int] = {}
    for index, (key, rgba) in enumerate(zip(keys.tolist(), colors.tolist())):
        try:
            name = world.registry_name(key)
        except Exception:
            name = ""
        obj.data.materials.append(ensure_material(key, name, tuple(rgba), key in emissive_keys))
        slots[int(key)] = index

    if per_face_keys.size:
        # np.unique returned `keys` sorted, so the slot of a face's key is where it lands in it.
        indices = np.searchsorted(keys, per_face_keys).astype(np.int32)
        obj.data.polygons.foreach_set("material_index", indices)
        obj.data.update()
    return slots

"""Moving the registry between the world and the Blender-side mirror.

Kept out of the operator modules because both directions are needed from several places -- opening
a world, committing an edit, building a palette from materials -- and because the flag packing is
the sort of thing that goes subtly wrong when it is written twice.
"""

from __future__ import annotations


def _flags(api, entry) -> int:
    flags = 0
    if entry.opaque:
        flags |= api.REGISTRY_OPAQUE
    if entry.emissive:
        flags |= api.REGISTRY_EMISSIVE
    if entry.special:
        flags |= api.REGISTRY_SPECIAL
    if entry.collidable:
        flags |= api.REGISTRY_COLLIDABLE
    return flags


def _rgba8(color) -> int:
    r, g, b, a = (max(0.0, min(1.0, float(c))) for c in color)
    return (
        (int(r * 255 + 0.5) << 24)
        | (int(g * 255 + 0.5) << 16)
        | (int(b * 255 + 0.5) << 8)
        | int(a * 255 + 0.5)
    )


def pull_registry(settings, world) -> int:
    """World -> Blender. Replaces the mirror wholesale; the world is the source of truth."""
    from . import binding

    api = binding.api()
    settings.registry.clear()

    for entry in world.registry:
        item = settings.registry.add()
        item.voxel_key = entry.voxel_key
        item.material_id = entry.material_id
        item.opaque = bool(entry.flags & api.REGISTRY_OPAQUE)
        item.emissive = bool(entry.flags & api.REGISTRY_EMISSIVE)
        item.special = bool(entry.flags & api.REGISTRY_SPECIAL)
        item.collidable = bool(entry.flags & api.REGISTRY_COLLIDABLE)

        try:
            item.name = world.registry_name(entry.voxel_key)
        except Exception:
            item.name = ""

        rgba = 0
        try:
            rgba = world.registry_color(entry.voxel_key)
        except Exception:
            rgba = 0
        if rgba == 0 and world.texture_count > 0:
            try:
                rgba = world.material(0, entry.material_id).tint_rgba8
            except Exception:
                rgba = 0
        if rgba:
            item.color = (
                ((rgba >> 24) & 0xFF) / 255.0,
                ((rgba >> 16) & 0xFF) / 255.0,
                ((rgba >> 8) & 0xFF) / 255.0,
                (rgba & 0xFF) / 255.0,
            )

    return len(settings.registry)


def push_registry(settings, world) -> int:
    """Blender -> world. Upserts what the mirror holds; entries only in the world are left alone.

    Deliberately additive: removing a row here should not silently orphan every voxel already
    carrying that key. Removal is its own operator, so it is a decision rather than a side effect.
    """
    from . import binding

    api = binding.api()
    seen: set[int] = set()

    for item in settings.registry:
        if item.voxel_key == 0:
            continue  # 0 is air; the format refuses to register it
        if item.voxel_key in seen:
            continue
        seen.add(item.voxel_key)

        world.set_registry_entry(
            item.voxel_key,
            material_id=item.material_id,
            flags=_flags(api, item),
            name=item.name or None,
        )
        world.set_registry_color(item.voxel_key, _rgba8(item.color))

    return len(seen)

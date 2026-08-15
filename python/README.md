# bsvx — Python binding

ctypes over the flat C ABI in [BSVX (Basil Voxel)](https://github.com/Dreyfffus/bsvx), an asset
baker, packer, loader and decoder for voxel worlds.

Deliberately **not** a compiled extension: one build of the shared library works with whatever
Python a host application ships, so this imports cleanly into Blender's bundled interpreter without
a build step. The only dependency is the shared library itself.

## Install

```bash
pip install ./python
```

Or run straight out of a checkout after building the library — `cmake --build build` stages it into
`bsvx/bin/<platform>/`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
PYTHONPATH=python python3 -c "import bsvx; print(bsvx.build_info())"
```

The loader looks in `$BSVX_LIBRARY`, then `bsvx/bin/<platform>/`, then the repository's `build/`.

## Use

```python
from bsvx import World, REGISTRY_OPAQUE

world = World.create(chunk_size=(16, 16, 16), region_size=(16, 16, 16))
world.name = "Quarry"
world.set_units(voxel_size=0.25)
world.set_registry_entry(1, material_id=0, flags=REGISTRY_OPAQUE, name="stone")

world.fill_box((0, 0, 0), (63, 3, 63), 1)      # regions and chunks appear as needed
world.set_voxels([(4, 4, 4), (5, 4, 4)], 1)    # world-space, batched by chunk inside the library

for issue in world.validate(deep=True):
    print(issue)

print(world.save("quarry"))
```

Editing an existing world in place, without rewriting the parts you did not touch:

```python
with World.load("quarry", ignore_hash_mismatch=True) as world:
    world.set_voxels([(4, 4, 4)], 0)           # erase one voxel
    report = world.save_dirty()                # writes one .bvx, skips the rest
```

Never loop per voxel across the FFI boundary — use `set_voxels`, `get_voxels`, `fill_box`,
`chunk_infos` and `decode_region`, which each cross once.

## Safety checks at import

The binding verifies the library before using it: the ABI version first, then the size of every
struct it mirrors, via `bsvx_struct_size()`. A mismatch raises immediately rather than silently
reading garbled fields for the rest of the session.

## Tests

Stdlib-only (`unittest`), so they run inside a host application's Python:

```bash
PYTHONPATH=python python3 -m unittest discover -s python/tests
```

## License

Public domain (Unlicense) — see the `LICENSE` file at the repository root.

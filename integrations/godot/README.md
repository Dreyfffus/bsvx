# bsvx for Godot

A GDExtension that reads, authors and writes `.bsvx` worlds and standalone `.bvx` regions from
Godot 4.4+.

This is the **format layer, and only the format layer**. It gives you voxel keys, a registry, units
and metadata. It does not mesh, raymarch, bake a distance field or put anything on screen — that is
a renderer's decision, and keeping it out of here is the point: a world authored through this
extension is readable by any renderer, by Blender, and by the library's own tools. A renderer that
wants baked geometry builds it on top of this, and several can coexist.

## Building

The only submodule is `godot-cpp`. bsvx itself is the repository this directory lives in, consumed
through its own `CMakeLists.txt` and linked statically, so the extension ships as one file.

```sh
git submodule update --init --recursive integrations/godot/extern/godot-cpp

# The editor and debug templates load this one.
cmake -S integrations/godot -B integrations/godot/build-debug \
      -DCMAKE_BUILD_TYPE=Debug -DGODOTCPP_TARGET=template_debug
cmake --build integrations/godot/build-debug -j

# Release templates load this one.
cmake -S integrations/godot -B integrations/godot/build \
      -DCMAKE_BUILD_TYPE=Release -DGODOTCPP_TARGET=template_release
cmake --build integrations/godot/build -j
```

Both land in `integrations/godot/project/addons/bsvx/bin/` under the names
`bsvx.gdextension` expects. To use the extension in your own project, copy
`integrations/godot/project/addons/bsvx/` into your project's `addons/`.

## Verifying

`project/` is a minimal Godot project whose only content is a headless smoke test that authors a
world, saves it, reads it back and checks that what came out is what went in:

```sh
godot --headless --path integrations/godot/project --import          # once, to register the extension
godot --headless --path integrations/godot/project --script res://smoke_test.gd
```

It exits non-zero on the first failure and prints a check count.

## Using it

Everything hangs off one class, `BsvxWorld`, a `Resource`.

```gdscript
var world := BsvxWorld.new()
world.create(Vector3i(32, 32, 32), Vector3i(4, 4, 4))   # chunk size, region size in chunks
world.set_world_name("cavern")
world.set_voxel_size(Vector3(0.25, 0.25, 0.25))

# Colour lives on the registry, where every other reader of the format can find it.
world.make_palette(PackedColorArray([Color8(110, 100, 90), Color8(90, 130, 70)]), "palette")
world.set_voxel_name(1, "stone")
world.set_voxel_name(2, "grass")

world.fill_box(Vector3i(-16, -4, -16), Vector3i(15, 0, 15), 1)
world.set_voxel(Vector3i(0, 1, 0), 2)

world.save_region("user://cavern.bvx")
```

Reading it back, and getting at the voxels a renderer would consume:

```gdscript
var world := BsvxWorld.new()
if world.load_region("res://worlds/cavern.bvx") != OK:
    push_error(world.get_last_error())
    return

var palette := world.get_palette()          # indexed by voxel key; index 0 is air
var chunk_size := world.get_chunk_size()

for region in world.get_region_count():
    for coord in world.get_chunk_coords(region):
        var voxels := world.get_chunk(region, coord)   # dense, x + sx * (y + sy * z)
        ...
```

### Loading from `res://`

`load_world()` and `load_region()` both read through Godot's `FileAccess`, so `res://` works in an
exported project where there is no filesystem path at all. A manifest world goes through the
library's VFS hook; a standalone `.bvx` is read into memory and parsed from there.

Saving is the other way round: it needs a real path, because an atomic save is a temp file plus a
rename and no pack file offers that. `user://` globalizes fine; `res://` only in the editor.

### Coordinates

Godot's frame — X right, Y up, Z forward, right-handed — is the format's canonical one, so a world
authored here needs no conversion. A world authored in Blender is Z-up and reports
`AXIS_X_RIGHT_Z_UP_Y_FORWARD` from `get_axis_convention()`; `convert_axis_convention()` rewrites it,
which moves voxels across region boundaries and drops any baked payloads, so it is an explicit call
rather than something that happens on load.

Voxel coordinates are integers and can be negative. `locate_voxel()` exposes the
region/chunk/local decomposition rather than making you re-derive it — the division floors where
C's truncates, and getting that wrong is invisible until content crosses the origin.

### Chunk order

`get_chunk()` and `set_chunk()` speak the format's own dense order, `index = x + sx * (y + sy * z)`,
with no reordering in between. `decode_region()` gives a whole region as one array with unauthored
chunks reading as air.

Voxel keys are `uint32` carried in a `PackedInt32Array`. The reinterpretation is bit-exact, so a key
above 2³¹ reads back negative rather than wrong; registries that large do not occur in practice, and
the alternative doubles the memory of every chunk read.

## What is deliberately absent

- **Meshing, raymarching, LOD, materials on screen.** See the note at the top.
- **Generation.** Noise, terrain, procedural fill: that is authoring, and it belongs in whatever
  tool writes the world, not in the thing that reads the format.
- **`.btx` writing beyond `make_palette()`.** Texture archives can be introspected here; building a
  full one is the library's own API.

## API reference

`BsvxWorld` groups its methods as:

| Group | Methods |
| --- | --- |
| Lifecycle | `create` `load_world` `load_region` `load_region_bytes` `save_world` `save_region` `save_region_bytes` `is_open` `close` `get_last_error` `get_warnings` |
| Description | `get_chunk_size` `get_region_size` `get_region_extent` `get_world_name` `set_world_name` `get_uuid` `set_uuid` `get_axis_convention` `convert_axis_convention` `get_voxel_size` `set_voxel_size` `get_origin` `set_origin` |
| Regions | `get_region_count` `get_region_coord` `find_region` `add_region` `remove_region` `prune_empty_regions` |
| Chunks | `get_chunk_count` `get_chunk_coords` `has_chunk` `get_chunk_info` `get_chunk` `set_chunk` `clear_chunk` `remove_chunk` `decode_region` |
| Voxels | `get_voxel` `set_voxel` `get_voxels` `set_voxels` `fill_box` `locate_voxel` |
| Registry | `get_registry_keys` `get_registry_entry` `set_registry_entry` `remove_registry_entry` `get_voxel_name` `set_voxel_name` `get_voxel_color` `set_voxel_color` `get_palette` `make_palette` |
| Textures | `get_texture_count` `get_texture_id` `get_texture_path` |
| Metadata | `get_metadata_keys` `get_metadata` `set_metadata` `remove_metadata` |
| Housekeeping | `validate` `is_dirty` `clear_dirty` `compact` |
| Library | `get_abi_version` `get_build_info` `axis_convention_name` (static) |

Enums: `LoadFlags`, `SaveFlags`, `RegistryFlags`, `AxisConvention`, `Severity`. They mirror the C
ABI's, and `static_assert`s in `bsvx_world_resource.cpp` fail the build if the two ever drift.

Anything that can fail returns `Error`; `get_last_error()` carries the library's own message.

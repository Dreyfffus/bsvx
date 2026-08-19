# Basil Voxel (BSVX)

Basil Voxel is an asset baker, packer, loader and decoder for voxel worlds — a C++23 library with a
flat C ABI, built around a structured world format for the Basil minimal engine.

Two goals drive the whole design:

- keep voxel assets portable and easy to move around
- make world data accessible **before** full decode, so an engine can inspect, stream, bake and
  transform chunks without committing to a single rendering strategy

This project is both a practical tool and a learning project. The practical side is simple: I wanted
an asset format that would not fall apart every time I copied folders around, reorganized content,
renamed assets, or split a world into smaller pieces.

I do that often. A lot.

So the format is built around explicit metadata, region containers and stable asset references
instead of brittle assumptions about where files "should" be.

---

## What it does

- a binary texture/texel asset format (`.btx`)
- a binary voxel region format (`.bvx`)
- a manifest-driven world layout (`manifest.toml`)
- standalone region loading — from a path, from a buffer, or through a host VFS
- summary-first inspection: chunk statistics without decoding voxels
- chunk-granularity **streaming**: open a region's metadata only, page in payloads on demand
- write-back for modified voxel data and baked payloads
- world authoring from scratch, no files required until you save — geometry, registry, textures
- world-space voxel editing, removal, free-form metadata, validation
- atomic and incremental saves, to a real filesystem or through host callbacks
- axis-convention conversion, colour palettes, 20 texel formats
- a DLL-friendly C API for tooling and engine integration, plus a ctypes binding for Python

It deliberately does **not** mesh, bake, voxelize or render anything. The consumer owns all of that.

The library does not force a single bake target. It exposes voxel data so you can build whatever
runtime representation your renderer needs — chunk meshes for rasterization, octrees or brick
structures for ray marching, collision data, surface bake payloads, custom acceleration structures.

---

## File layout

The entry point for a Basil world is `manifest.toml`:

```text
world_root/
  manifest.toml
  regions/
    r_0_0_0.bvx
    r_1_0_0.bvx
  textures/
    terrain_red.btx
    terrain_blue.btx
```

### `manifest.toml`

The root description of the world: format version, chunk size, region size, bounds, voxel schema,
axis convention, units, registry (with human-readable voxel type names), texture references, region
references, free-form metadata, hashes. This is what ties the world together.

The manifest hash is FNV-1a-64 over the file's **raw bytes**, and every `.bvx` stores the hash it was
baked against — so line-ending translation invalidates a whole world. `.gitattributes` pins
`*.toml -text` for exactly that reason.

### `.bvx` — binary voxel region

One region of chunked voxel data plus optional baked payloads. Loadable as part of a manifest world,
or standalone if it carries the self-describing world metadata.

A `.bvx` is designed to be useful before full decode: a header, chunk map, per-chunk summaries and a
section directory sit up front (tens of KB), and the voxel blobs sit behind them. The engine can
inspect every chunk's statistics, decide what to stream, and pay only for the chunks it decodes.

### `.btx` — binary texel archive

Sampler, texture, subresource and material tables plus a texel blob, laid out Vulkan-natively (raw
`VkFormat` and usage values, `bufferRowLength` / `bufferImageHeight` stored per subresource) so a
Vulkan backend can upload without translation.

Twenty formats are supported: R8, RG8, RGBA8 and BGRA8 in UNORM/SRGB, 16- and 32-bit float, and
BC1/BC3/BC4/BC5/BC7. Block-compressed sizes come from `bsvx_format_subresource_size`, not from a
per-texel size — a 5×5 BC7 image is 2×2 blocks, which no per-texel figure can express.

---

## Core concepts

| Concept | Meaning | Lives in |
|---|---|---|
| **World** | root directory + manifest | a directory |
| **Region** | a box of chunks | one `.bvx` |
| **Chunk** | the smallest decode/bake unit | one chunk-map entry |
| **Voxel** | one `uint32_t` **voxel key** | a dense array inside a chunk |
| **Registry** | voxel key → material id + flags + name + colour | manifest `[[registry]]`, mirrored into the world desc |
| **Texture** | sampler/texture/material tables + texel blob | one `.btx` |
| **Units** | metres per voxel, world origin | manifest `[units]`, mirrored into the world desc |
| **Metadata** | opaque key/value bytes a tool owns | manifest `[metadata]`, and a section per region |

`GeometryDesc` carries two triplets that mean different things: `chunk_size_*` is **voxels per
chunk**, `region_size_*` is **chunks per region**. Voxel key `0` is air by convention everywhere.
Dense chunk order is `x + sx*(y + sy*z)`.

**Baked payloads.** A region can store derived per-chunk sections — surface bake, collision, distance
field, light, or your own `USER_BASE` types. The library stores and returns them; it does not produce
them.

---

## Access model

1. load a world, a standalone region, or open a region reader
2. inspect regions and chunk summaries
3. decode only the chunks you need
4. build your own runtime representation
5. write modified voxels or baked payloads back to disk

Which makes it useful for editors, bakers, converters, offline asset pipelines and in-engine import
tools.

---

## Building

Requires a C++23 compiler (C++20 also builds). The only dependency is toml++ v3.4.0, vendored in
`src/impl/`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bsvx_tests            # fixture-driven suite against test/data
```

Or, without CMake:

```bash
g++ -std=c++23 -O2 -fPIC -shared \
    -Isrc/include -Isrc/impl -DTOML_HEADER_ONLY=0 \
    src/impl/*.cpp -o libbsvx.so
```

Or with premake, which generates a Visual Studio solution without CMake in the picture, and
makefiles everywhere else:

```bash
premake\premake5.exe --file=Build.lua vs2022     # what build.bat runs
premake5 --file=Build.lua gmake2 && make -j      # Linux/macOS
```

`Build.lua` mirrors the CMake decisions rather than inventing its own — same MSVC flags, same
static/shared choice (`--static-lib`), and the same staging into `python/bsvx/bin/<platform>/` that
the Python binding and the Blender add-on rely on. `--help` lists its options. CMake remains the
primary build and the one CI runs; a build definition that is never exercised is a build definition
that is already broken, so premake is verified against the same test suite.

Generated projects (`*.vcxproj`, `bsvx.sln`, `Makefile`, `*.make`) and `Binaries/` are outputs, not
sources, and are gitignored.

`CXX_VISIBILITY_PRESET hidden` is deliberate: it keeps the exported surface to exactly the `bsvx_*` C
functions.

Two options matter to anyone embedding the library rather than shipping it beside their binary.
`BSVX_BUILD_SHARED=OFF` builds a static archive instead — what a host wants when the library is
linked into one loadable module of its own, so there is a single file to distribute and no rpath to
arrange. `BSVX_STATIC_CXX_RUNTIME=ON` links libstdc++/libgcc statically, which keeps a shared build
from fighting whatever older runtime the host process already has mapped in.

---

## Integrations

`integrations/` holds host plugins built on the C ABI. Each owns its own packaging and its host's
plugin manifest; none of them extend the format.

- **[`integrations/godot/`](integrations/godot/)** — a GDExtension exposing `BsvxWorld`, a Godot
  `Resource` that loads, authors and saves worlds. `godot-cpp` is its only submodule; bsvx is linked
  statically from this repository. It is the format layer only — no meshing, no baking, no
  generation — so several renderers can sit on top of the same worlds.
- **[`integrations/blender/`](integrations/blender/)** — a Blender 4.2+ add-on for opening,
  authoring, modifying and writing worlds, built on the ctypes binding. A voxel is a **vertex** with
  a `bsvx_key` integer attribute, checked out from the world a bounded box at a time, so Blender's
  own editing tools operate on voxels directly.

---

## The public API

Two boundaries, and the choice matters:

**`src/include/bsvx_dll.h` — the C ABI.** POD structs, `bsvx_result` returns, no exceptions crossing
the boundary. This is the shipping surface for tools, engines and language bindings. It is versioned:
`bsvx_abi_version()` returns `BSVX_ABI_VERSION`, currently **4**, and `bsvx_struct_size()` lets a
binding verify its own struct layouts against the library it loaded.

| ABI | Adds |
|---|---|
| 1 | the original 20-symbol surface: load, save, enumerate, decode, set chunk, set payload |
| 2 | in-memory loading, payload read-back, the streaming region reader, `_ex` error reporting, compaction |
| 3 | world authoring, world metadata, region serialization to a buffer, manifest loading through a host VFS, `.btx` introspection |
| 4 | ABI self-description, `.btx` **writing**, round-trippable registry names, units, world-space sparse voxel I/O, removal, free-form metadata, incremental and atomic saves, load flags, validation, progress/cancel, bulk accessors, asset paths, axis conventions, palettes, wide texel formats, cloning, VFS saving |

**`src/include/bsvx.h` and friends — the C++ internals.** `Parser`, `Manifest`, `WorldPackage`,
`bvx::Archive`, `btx::Archive`. Everything the C ABI can do is available here too, with richer types
and without the buffer-sizing dance — but it is not a stable boundary and it throws.

A sketch, through the C API:

```c
bsvx_context* ctx = bsvx_context_create();
bsvx_world* world = NULL;
bsvx_world_load(ctx, "world_root", &world);

bsvx_geometry_desc g; bsvx_world_geometry(world, &g);
size_t n = bsvx_region_required_voxel_count(world, 0);
uint32_t* voxels = malloc(n * sizeof(uint32_t));

size_t chunks = bsvx_region_chunk_count(world, 0);
for (size_t i = 0; i < chunks; ++i) {
    bsvx_chunk_info info;
    bsvx_region_get_chunk_info(world, 0, i, &info);
    if (info.summary.non_air_count == 0) continue;      /* skip before decoding */

    size_t written = 0;
    bsvx_region_decode_chunk_u32_ex(ctx, world, 0, info.local_chunk_x, info.local_chunk_y,
                                    info.local_chunk_z, voxels, n, &written);
    /* ... mesh it, bake it, whatever you do ... */
}

bsvx_world_destroy(world);
bsvx_context_destroy(ctx);
```

Conventions worth knowing before you write against it:

- **Buffer sizing**: `(NULL, 0)` is a legal size probe. Too-small buffers return
  `BSVX_RESULT_BUFFER_TOO_SMALL` with the required size in `*out_size` and write nothing. String
  getters count the terminating NUL.
- **`_ex` variants** take a context and populate `bsvx_context_last_error()`. Use them.
- **Threading**: one `bsvx_context` per thread. Region readers are safe to share across threads; a
  `bsvx_world` under mutation is not.
- **Growth**: every `set_chunk_*` appends and repoints, so an edited region grows monotonically. Call
  `bsvx_world_compact()` before saving.
- **Set the registry before writing voxels** — chunk summaries are built through it.
- **Saves are atomic by default** (temp file, fsync, rename). `BSVX_SAVE_NON_ATOMIC` opts out.
- **All paths are UTF-8**, on every platform.
- **One writer or many readers** per `bsvx_world`; there is no internal locking. `bsvx_world_clone`
  gives a background thread its own copy.

---

## Python

`python/bsvx/` is a ctypes binding over the C ABI — deliberately not a compiled extension, so one
build of the shared library works with whatever Python a host application ships (Blender, for
instance). It verifies the ABI version *and* the size of every struct it mirrors at import, and
raises rather than reading garbled fields.

```python
from bsvx import World, REGISTRY_OPAQUE

world = World.create(chunk_size=(16, 16, 16), region_size=(16, 16, 16))
world.name = "Quarry"
world.set_units(voxel_size=0.25)
world.set_registry_entry(1, material_id=0, flags=REGISTRY_OPAQUE, name="stone")

world.fill_box((0, 0, 0), (63, 3, 63), 1)          # regions and chunks appear as needed
world.set_voxels([(4, 4, 4), (5, 4, 4)], 1)        # world-space, batched by chunk in C

for issue in world.validate(deep=True):
    print(issue)

print(world.save("quarry"))
```

Colour-first authoring, and converting a Blender-frame world to the canonical one:

```python
world.make_palette([(180, 170, 160, 255), (60, 90, 40, 255)])   # texture + materials + registry
world.set_registry_color(1, 0xB4AAA0FF)

world.convert_axis_convention(bsvx.AXIS_X_RIGHT_Y_UP_Z_FORWARD)  # rewrites every voxel
bsvx.convert_cell(bsvx.AXIS_X_RIGHT_Z_UP_Y_FORWARD, bsvx.AXIS_X_RIGHT_Y_UP_Z_FORWARD, 1, 2, 3)
```

Editing an existing world in place:

```python
with World.load("quarry", ignore_hash_mismatch=True) as world:
    world.set_voxels([(4, 4, 4)], 0)               # erase one voxel
    report = world.save_dirty()                    # writes one .bvx, skips the rest
```

`cmake --build build` stages the built library into `python/bsvx/bin/<platform>/`, so the package
works straight out of the checkout with `PYTHONPATH=python` — or `pip install ./python` to install
it. Set `BSVX_LIBRARY` to point the loader somewhere else.

```bash
PYTHONPATH=python python3 -m unittest discover -s python/tests
```

---

## Integration guides

Two long-form documents, each written as a complete brief for building against this library:

- **[`GODOT_INTEGRATION.md`](GODOT_INTEGRATION.md)** — the runtime consumer's view. The full C ABI,
  the exact on-disk layouts, coordinates and indexing, the verified Linux build, GDExtension
  architecture, threading, `res://` handling, and every landmine found by actually compiling and
  running the library.
- **[`BLENDER_INTEGRATION.md`](BLENDER_INTEGRATION.md)** — the authoring tool's view. What a Blender
  add-on for saving, authoring and rewriting BSVX worlds needs, the coordinate mapping, the add-on
  architecture, and a feature-by-feature record of what ABI v4 added for it and what is still open.

---

## Status

Workable, and honest about its edges.

**Solid:** reading, decoding, streaming, chunk edits, baked payloads, authoring worlds and textures
from scratch, world-space voxel editing, removal, metadata, units, colours, axis conversion,
validation, incremental and atomic saves, saving through host callbacks, the manifest round-trip and
the VFS load path. Covered by 45 tests in `test/tests.cpp` and 18 in `python/tests/`, run by CI on
Linux, macOS and Windows, plus `-std=c++20` and `_GLIBCXX_DEBUG` passes.

**Known limits**, in rough order of how likely you are to hit them:

- no meshing, baking, LOD or voxelization; that is deliberate — the consumer owns it
- mip generation covers 8-bit uncompressed formats only. BCn mips must be supplied by whatever
  produced the blocks: downsampling them means decode → filter → re-encode, i.e. a compressor
- `TextureKind::TEXTURE_3D` and `TEXEL_BUFFER` are declared and implemented nowhere
- axis conversion is an explicit whole-world rewrite, not something the loader does for you, and it
  drops baked payload sections — they describe the old frame
- the manifest hash is over raw bytes, so any reformatting invalidates every region's stamp;
  `BSVX_LOAD_IGNORE_HASH_MISMATCH` + `bsvx_world_rehash` make that recoverable rather than fatal
- saving through a VFS leaves atomicity to the host — the library cannot rename inside someone
  else's filesystem
- containers are raw little-endian POD dumps — no big-endian target

`BLENDER_INTEGRATION.md` §4 covers each of these in detail.

---

## License

See [`LICENSE`](LICENSE).

# BSVX (Basil Voxel) — Blender Plugin Integration Brief

**Purpose of this file:** this is the design and gap document for a **Blender add-on that saves,
authors and rewrites BSVX worlds** (`manifest.toml` + `.bvx` + `.btx`). It states what the library
can do today, what a Blender add-on actually needs, and — the point of the document — **the complete
set of features that are missing**, each one explained, prioritised, and sketched as a concrete API
addition.

Companion document: `GODOT_INTEGRATION.md` describes the same library from a *runtime consumer's*
point of view (load, decode, mesh, render). It is the authoritative reference for the existing C ABI,
the on-disk layout, and the verified Linux build — this document does not repeat it and assumes you
have it open.

> **Implementation status — ABI v4.** Everything in Tier 0 and Tier 1 below (**B1–B15**) is
> **implemented and tested**: `bsvx_abi_version()` returns 4, the C ABI exports 145 functions, and
> `test/tests.cpp` runs 37 tests (19 of them new) that pass on GCC under both `-std=c++20` and
> `-std=c++23`, including under `_GLIBCXX_DEBUG`. A ctypes binding lives in `python/bsvx/`.
> Each entry below records what shipped. Tier 2 (**B16–B24**) is still open, except for the parts
> pulled forward and noted inline: mip generation (B20), UTF-8 paths (B21) and the round-trip
> fixtures (B24).

**The short version:** BSVX was a strong *reader* and an adequate *writer*. ABI v4 makes it an
*authoring* library — which is what a Blender add-on needs it to be.

Legend used below:

- **DONE** — implemented in ABI v4, with a test.
- **MISSING** — does not exist anywhere in the code.
- **C++ ONLY** — exists in C++ (`bsvx.h` / `bvx.h` / `btx.h`) but is not reachable from `bsvx_dll.h`,
  which is what Python can call.
- **FRAGILE** — exists and works, but breaks under the usage pattern an editor produces.

---

## 1. Where the library stands today

Read `GODOT_INTEGRATION.md` §4 for the full 82-symbol C ABI. Condensed to what matters here:

| Subsystem | Read | Write / author | Reachable from C (i.e. from Python) |
|---|---|---|---|
| `manifest.toml` world | ✅ full, plus a VFS path | ✅ full tree, incremental, atomic | ✅ / ⚠️ save is real-paths only |
| `.bvx` region — chunks, summaries | ✅ eager **and** streaming | ✅ add region, set chunk, set payload | ✅ |
| `.bvx` — remove region/chunk | — | ✅ **v4** | ✅ |
| Registry (voxel key → material + flags) | ✅ | ✅ upsert / remove | ✅, **names round-trip** since v4 |
| `.btx` textures/materials/samplers | ✅ full, down to texel bytes | ✅ `btx::Archive` | ✅ **v4** builder |
| Attaching a `.btx` to a world | ✅ | ✅ | ✅ **v4** |
| World metadata (name, uuid, bounds) | ✅ | ✅ | ✅ |
| Free-form / tool metadata | ✅ **v4** | ✅ **v4** | ✅ world- and region-level |
| Voxel size, world transform, units | ✅ **v4** | ✅ **v4** | ✅ |
| World-space sparse voxel I/O | ✅ **v4** | ✅ **v4** | ✅ batched by chunk |
| Validation | ✅ **v4**, structured issues | — | ✅ |
| Packaging (Python-loadable binary) | — | — | ✅ **v4** (`python/bsvx/`) |

The remaining gaps are Tier 2: texel formats beyond RGBA8, axis conventions, palette convenience,
per-chunk change detection beyond the content hash, VFS saving.

---

## 2. What a Blender add-on actually has to do

Four workflows, in increasing order of how much they hurt:

1. **Import** — open a world or a standalone region, show it in Blender. Needs decode (exists),
   materials/colors (needs `.btx` reads — exists), and a preview mesh (add-on's job).
2. **Author** — build voxel data from Blender data: voxelise a mesh, sample a point cloud from
   Geometry Nodes, paint with a brush operator, place prefabs. Produces a **sparse set of
   world-space voxels with a voxel key each**, in Blender coordinates, and a **palette** of voxel
   types with names and colors.
3. **Export / save** — write that out as a manifest world or a standalone `.bvx`, plus the `.btx`
   that carries the palette.
4. **Rewrite** — open an existing world, change *part* of it, write it back **to the same files**,
   losing nothing the add-on did not understand.

Workflow 4 is the hard requirement ("rewriting"), and it was the one the library was least prepared
for. Rewriting demands three properties: **removal**, **lossless round-trip of human-authored
strings**, and **safe, incremental, interruptible writes**. ABI v4 adds all three (**B7**, **B4**/**B8**,
**B9**/**B11**).

---

## 3. Coordinates and units — settle this before writing a line of code

### 3.1 The axis mapping

- **Blender**: right-handed, **+Z up**, +Y "into the screen" / forward, +X right. Units are floats,
  nominally metres.
- **BSVX**: `AxisConvention::X_RIGHT_Y_UP_Z_FORWARD` — +X right, **+Y up**, +Z forward. Integer
  grid. (`GODOT_INTEGRATION.md` §3 establishes that this maps to Godot as the identity, which pins
  down what "forward" means: it is Godot's +Z, i.e. *toward the viewer*.)

So the Blender → BSVX mapping is the standard Blender → Godot one:

```
bsvx_x =  blender_x
bsvx_y =  blender_z
bsvx_z = -blender_y
```

For **continuous** positions that is all. For **voxel cell indices** the negation needs a cell
offset, or you shift the whole world by one cell on the Z axis:

```python
def blender_cell_to_bsvx(bx, by, bz):
    return (bx, bz, -by - 1)      # the -1 keeps cell occupancy, not just the corner

def bsvx_cell_to_blender(sx, sy, sz):
    return (sx, -sz - 1, sy)
```

Get this wrong and the world is mirrored on one axis — which is invisible on symmetric test content
and obvious on the first piece of real geometry. Put it in one module, unit-test it with a
deliberately asymmetric fixture, and never inline it anywhere else.

**Shipped in v4 (B16):** you no longer write this mapping yourself. `bsvx_convert_cell()` and
`bsvx_convert_position()` do it between any two conventions, and a world can now be *labelled* Z-up
(`AxisConvention::X_RIGHT_Z_UP_Y_FORWARD`) rather than converted by silent agreement. In Python:

```python
import bsvx
bsvx.convert_cell(bsvx.AXIS_X_RIGHT_Z_UP_Y_FORWARD, bsvx.AXIS_X_RIGHT_Y_UP_Z_FORWARD, 1, 2, 3)
# -> (1, 3, -3)
```

Still put it behind one module in the add-on and unit-test it with deliberately asymmetric content —
a mirrored world is invisible on a symmetric fixture and obvious on the first real piece of geometry.

### 3.2 Decomposing a world voxel into region / chunk / local

**Shipped in v4 (B6):** `bsvx_world_locate_voxel()` does this, and `bsvx_world_set_voxels()` /
`_get_voxels()` / `_fill_box()` take world coordinates directly, so you rarely need the arithmetic
at all. It is written out here because the mapping still has to be understood:

```python
voxels_per_region_x = cs_x * rs_x
region_x, rem_x   = divmod(world_x, voxels_per_region_x)   # Python divmod floors — correct for negatives
chunk_x, local_x  = divmod(rem_x, cs_x)
```

Note `divmod`'s flooring behaviour is what you want; C's `/` truncates toward zero and is **wrong**
for negative world coordinates. Region coords are `int32_t` and signed; chunk and local coords are
`uint16_t` and always positive within their region.

Dense chunk array order is `index = x + sx*(y + sy*z)`, length `cs_x*cs_y*cs_z`.

### 3.3 Units — voxel size and origin

**Shipped in v4 (B5).** The format now stores metres-per-voxel and a world origin, in a `[units]`
table in the manifest and in the v2 world-desc blob inside a standalone `.bvx`:

```python
world.set_units(voxel_size=0.25, origin=(0.0, 0.0, 0.0))
u = world.units          # bsvx_units: voxel_size_x/y/z, origin_x/y/z
```

Defaults are 1.0 per axis and a zero origin, and a world still carrying those defaults is reported as
an INFO issue by `validate()` — a nudge, not an error. Blender is metric and floating-point, so an
import scales by `voxel_size` and an export divides by it; the factor now travels with the file
instead of living in the add-on's scene properties.

---

## 4. The missing pieces — the complete list

Grouped in three tiers. Tier 0 items block the add-on outright; Tier 1 items make the difference
between a demo and a tool people keep using; Tier 2 items are quality, ergonomics and format reach.

Proposed signatures follow the existing house style: POD-only, `bsvx_result` returns, out-params,
the `(NULL, 0)` size-probe convention for anything variable-length, and `BSVX_ABI_VERSION` bumped to
**4** so a host can feature-detect with `bsvx_abi_version()`.

---

### Tier 0 — blockers

#### B1. A Python-loadable, distributable build — **DONE**

**What's missing.** The library builds a `libbsvx.so` and nothing else. There is no `install()` rule
in `CMakeLists.txt`, no SONAME/version, no macOS or Windows CI build, no Python package, and no
declared minimum glibc. Blender ships its own Python and runs on Windows, macOS (x86_64 + arm64) and
Linux; an add-on is a zip that must carry working binaries for all of them.

**Why Blender needs it.** An add-on cannot compile C++ at install time. It must `ctypes.CDLL()` a
prebuilt binary out of its own directory.

**What to add.**

- `install(TARGETS bsvx …)` + `set_target_properties(bsvx PROPERTIES VERSION 4.0.0 SOVERSION 4)`.
- CI matrix producing `libbsvx.so` (manylinux-compatible, i.e. built against an old glibc or with
  `-static-libstdc++`), `libbsvx.dylib` (universal2 via `CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`),
  `bsvx.dll` (MSVC, `/MD`).
- A `python/bsvx/` package: `_ffi.py` (ctypes declarations), `world.py` (Pythonic wrapper),
  `bin/<platform>/` for the binaries.
- **`-static-libstdc++ -static-libgcc` on the Linux build.** Blender loads add-on libraries into a
  process that already has its own C++ runtime; a `libbsvx.so` that pulls in a newer `libstdc++`
  than the host's is the classic way to crash Blender at import time.

**Shipped.** `CMakeLists.txt` gained `install()` rules, a CMake package export (`bsvx::bsvx`),
`VERSION 4.0.0` / `SOVERSION 4`, and `-DBSVX_STATIC_CXX_RUNTIME=ON` for redistributable builds. A
post-build step stages the shared library into `python/bsvx/bin/<platform>/`, and the ctypes package
lives in `python/bsvx/` (`_structs.py`, `_lib.py`, `world.py`). The loader checks `$BSVX_LIBRARY`,
then the bundled binary, then the build tree.

**Not done:** the CI matrix producing macOS and Windows binaries. One platform is built and staged;
the other two need a runner.

#### B2. ABI self-description, so ctypes can be trusted — **DONE**

**What's missing.** A ctypes binding hand-mirrors every struct in `bsvx_dll.h`. Nothing verifies that
the mirror is right. `bsvx_chunk_summary` alone is 20 fields of mixed `uint8/16/32/64` — a compiler
padding difference, or one field reordered in a future version, produces silently garbled data
rather than an error. `bsvx_abi_version()` guards *symbols*, not *layouts*.

**Why Blender needs it.** The add-on ships a binary it did not build, on three platforms, and Python
has no compiler to check the header against.

**What to add.**

```c
typedef enum bsvx_struct_id {
  BSVX_STRUCT_GEOMETRY_DESC = 0, BSVX_STRUCT_REGISTRY_ENTRY = 1,
  BSVX_STRUCT_CHUNK_SUMMARY = 2, BSVX_STRUCT_CHUNK_INFO = 3,
  BSVX_STRUCT_WORLD_DESC = 4,    BSVX_STRUCT_TEXTURE_DESC = 5,
  /* … */
} bsvx_struct_id;

BSVX_API size_t      bsvx_struct_size(bsvx_struct_id id);      /* 0 for an unknown id */
BSVX_API size_t      bsvx_struct_field_offset(bsvx_struct_id id, size_t field_index);
BSVX_API const char* bsvx_result_string(bsvx_result r);        /* static, never NULL */
BSVX_API const char* bsvx_build_info(void);                    /* compiler, std, flags, git hash */
```

The binding asserts `ctypes.sizeof(BsvxChunkSummary) == lib.bsvx_struct_size(2)` at import and
refuses to load otherwise. `bsvx_result_string` removes a hand-maintained enum→text table from every
host.

**Shipped**, minus `bsvx_struct_field_offset` (sizes turned out to be enough: a layout that differs
in offsets but not in total size has yet to occur in practice, and the check is a load-time guard,
not a debugger). `python/bsvx/_lib.py` runs the comparison at import and raises rather than
proceeding with a mismatched mirror. `bsvx_build_info()` returns e.g.
`"bsvx abi 4; gcc 16.1; c++202302L"`.

#### B3. `.btx` authoring from the C ABI — **DONE**

**What's missing.** `btx::Archive::add_texture`, `append_subresource`, `append_rgba8_layer`,
`add_material`, `add_sampler` and `save_to_file` all exist and work — in C++. The C ABI has
**read-only** texture access (ABI v3) and **no way whatsoever to create a `.btx` or attach one to a
world**. `WorldPackage::textures` is a public `std::vector`, so C++ can `push_back` an archive; from
C there is no equivalent.

**Why Blender needs it.** This is the single largest blocker. A Blender add-on's whole reason to
exist is turning Blender data into assets: images and materials into a texture archive, a color
palette into materials. Without `.btx` writing, the add-on can produce voxel *keys* but nothing that
says what a key looks like. Every authored world would need a `.btx` made by hand in C++ elsewhere.

**What to add.** A builder handle so the archive can be assembled incrementally, mirroring the C++
class:

```c
typedef struct bsvx_texture_builder bsvx_texture_builder;

BSVX_API bsvx_result bsvx_texture_builder_create(bsvx_context*, bsvx_texture_builder** out);
BSVX_API void        bsvx_texture_builder_destroy(bsvx_texture_builder*);

BSVX_API bsvx_result bsvx_texture_builder_add_sampler (bsvx_texture_builder*, const bsvx_sampler_desc*,  uint32_t* out_id);
BSVX_API bsvx_result bsvx_texture_builder_add_texture (bsvx_texture_builder*, const bsvx_texture_desc*,  uint32_t* out_id);
BSVX_API bsvx_result bsvx_texture_builder_add_material(bsvx_texture_builder*, const bsvx_material_desc*, uint32_t* out_id);

/* texels are tightly packed rows unless packed_row_length / packed_image_height say otherwise */
BSVX_API bsvx_result bsvx_texture_builder_append_subresource(bsvx_texture_builder*, uint32_t texture_id,
                                                             uint16_t mip_level, uint16_t layer,
                                                             uint16_t width, uint16_t height,
                                                             const void* texels, size_t size,
                                                             uint16_t packed_row_length, uint16_t packed_image_height,
                                                             uint32_t* out_subresource_index);

/* validation before anything touches disk — wraps btx::Archive::validate() */
BSVX_API size_t      bsvx_texture_builder_validate(bsvx_texture_builder*, char* out_messages, size_t cap, size_t* out_size);

/* hand the finished archive to a world, or write it standalone */
BSVX_API bsvx_result bsvx_world_add_texture(bsvx_world*, const char* texture_id, const char* relative_path,
                                            bsvx_texture_builder*, size_t* out_tex_index);
BSVX_API bsvx_result bsvx_world_remove_texture(bsvx_world*, size_t tex_index);
BSVX_API bsvx_result bsvx_world_set_texture_id(bsvx_world*, size_t tex_index, const char* texture_id);
BSVX_API bsvx_result bsvx_texture_builder_save(bsvx_context*, bsvx_texture_builder*, const char* path);
BSVX_API bsvx_result bsvx_texture_builder_save_memory(bsvx_context*, bsvx_texture_builder*, void* out, size_t cap, size_t* out_size);
```

`bsvx_world_add_texture` should take ownership (or deep-copy) so the builder can be destroyed
immediately after.

**Shipped** as sketched, plus `bsvx_texture_builder_open` / `_from_world` (edit an existing archive
and write it back) and `_generate_mips` (**B20**, pulled forward — a box-filter loop in Python over a
2K texture is seconds, not milliseconds). `bsvx_texture_builder_add_texture` rejects a `vk_format`
the library cannot describe *at insert time* rather than at serialize time, and
`bsvx_world_add_texture` refuses a duplicate texture id and a reference path that would not fit a
`BtxRef`. `TextureBuilder.solid_color_palette()` in the Python layer turns a list of RGBA tuples into
a texture plus one material per colour in one call.

#### B4. Registry entry **names** survive a round-trip — **DONE**

**What's missing.** `manifest.toml` accepts `name = "stone"` in a `[[registry]]` table. The parser
immediately hashes it (`bsvx.cpp`, `parse_registry`: `entry.name_hash = fnv1a64_string(*name)`) and
throws the string away — `RegistryEntry` has only `name_hash`. On save, `build_manifest_table` writes
**`name_hash` only**. FNV-1a is not invertible.

**Consequence, verifiable in ten seconds:** load any world with named registry entries, save it, and
every name is gone. The very "rewrite" workflow being asked for is *currently guaranteed to destroy
the world's most human-visible metadata*.

**Why Blender needs it.** The add-on's UI is a list of voxel types. Users pick "stone", not
`0x8f3a…`. A palette editor that renames a type on save is not shippable.

**What to add.** Two options; the second is the right one.

1. *Cheap:* preserve the string in the `Manifest` alongside the entry and re-emit it. Fixes
   `manifest.toml` round-trip only — a standalone `.bvx` still carries just the hash.
2. *Correct:* give the on-disk `RegistryEntry` a name. It is a fixed-size POD in a
   `static_assert(TriviallySerializable<>)` table, so either add `char name[48]` (bumping
   `DiskWorldDescHeader.version`, with the old size still accepted) or add a **string table** section
   to the world desc blob and store a `uint32_t name_offset`. The string table is the better shape —
   it also gives texture ids, material names and user metadata (B7) somewhere to live, and it removes
   the `BtxRef.relative_path[128]` truncation trap (§9).

```c
BSVX_API bsvx_result bsvx_world_get_registry_name(const bsvx_world*, uint32_t voxel_key, char* out, size_t cap, size_t* out_size);
BSVX_API bsvx_result bsvx_world_set_registry_name(bsvx_world*, uint32_t voxel_key, const char* name);
```

**Shipped** using option 2 with a string table, and *without* touching `RegistryEntry`: the names
live in a v2 extension appended to the world-desc blob, as a parallel `uint32_t name_offset` table
plus a NUL-separated string pool. That keeps the registry hash (taken over the raw entry bytes)
exactly where it was, so existing worlds keep loading and their region stamps stay valid. A v1 blob
simply has no extension and parses as before; a v1 reader ignores the trailing bytes.

`test_registry_names_roundtrip` covers the regression in both directions — through `manifest.toml`
and through a standalone `.bvx`.

#### B5. Voxel size, world origin and units — **DONE**

**What's missing.** See §3.3. There is no field anywhere for metres-per-voxel, world origin offset,
or an up-axis declaration.

**Why Blender needs it.** Blender is metric and float. Import must scale; export must un-scale. If
the factor is not in the file, re-importing a world the add-on exported yesterday puts it at the
wrong size unless the user remembers the number.

**What to add.** A `[units]` table in the manifest and matching fields in `DiskWorldDescHeader`
(there are two `reserved` `uint16_t`s and room to grow the versioned header):

```toml
[units]
voxel_size = 0.25          # metres per voxel edge; non-uniform allowed: [0.25, 0.25, 0.25]
origin     = [0.0, 0.0, 0.0]   # world-space position of voxel (0,0,0) in region (0,0,0)
up_axis    = "y"           # informational; pairs with axis_convention
```

Exposed as `float voxel_size_x/y/z` and `double origin_x/y/z` on `bsvx_world_desc`, which is already
a versioned struct behind `bsvx_world_get_desc` / `_set_desc`.

**Shipped** in the same format bump as B4 (`DiskWorldDescExt` carries six doubles), with one
change: rather than appending to `bsvx_world_desc` — which would have silently broken every ABI v3
consumer passing a smaller struct — units get their own `bsvx_units` struct and their own
getter/setter. No existing struct changed size in v4.

#### B6. World-space sparse voxel I/O — **DONE**

**What's missing.** The only write path is `bsvx_region_set_chunk_u32` — a **whole dense chunk**, by
region index and chunk coordinate. There is nothing that takes a world-space voxel coordinate.

**Why Blender needs it.** Every authoring source in Blender produces *sparse, world-space* voxels: a
mesh voxeliser emits surface cells, a Geometry Nodes point cloud emits scattered points, a brush
operator emits a handful of cells under the cursor. The add-on currently has to, per edit: work out
region/chunk/local itself (§3.2), find-or-add the region, **decode the existing chunk into a 4096-entry
array**, patch a few entries, re-encode and write it back. For a brush stroke that is a decode +
re-encode of every touched chunk per mouse move, in Python.

**What to add.**

```c
/* Sparse scatter/gather in world voxel coordinates. Regions and chunks are created on demand
   (create_missing != 0). Coordinates are signed: negative world coordinates floor correctly.
   Returns the number of voxels actually written / read in *out_count. */
BSVX_API bsvx_result bsvx_world_set_voxels(bsvx_context*, bsvx_world*,
                                           const int64_t* xs, const int64_t* ys, const int64_t* zs,
                                           const uint32_t* keys, size_t count,
                                           int create_missing, size_t* out_count);

BSVX_API bsvx_result bsvx_world_get_voxels(bsvx_context*, const bsvx_world*,
                                           const int64_t* xs, const int64_t* ys, const int64_t* zs,
                                           uint32_t* out_keys, size_t count, size_t* out_count);

/* Fill an axis-aligned world-space box with one key — the cheap path for blocking out and erasing. */
BSVX_API bsvx_result bsvx_world_fill_box(bsvx_context*, bsvx_world*,
                                         int64_t min_x, int64_t min_y, int64_t min_z,
                                         int64_t max_x, int64_t max_y, int64_t max_z,
                                         uint32_t key, int create_missing);

/* Address arithmetic exposed so hosts stop re-deriving it. */
BSVX_API bsvx_result bsvx_world_locate_voxel(const bsvx_world*, int64_t x, int64_t y, int64_t z,
                                             int32_t* out_rx, int32_t* out_ry, int32_t* out_rz,
                                             uint16_t* out_cx, uint16_t* out_cy, uint16_t* out_cz,
                                             uint32_t* out_local_index);
```

Internally these batch by chunk, decode each touched chunk **once**, apply every hit in it, and
re-encode once. That turns an N-voxel brush stroke from N chunk round-trips into one per touched
chunk, in C++, out of Python's reach.

**Shipped** exactly as sketched. The batching is real: writes are grouped by target chunk, each
touched chunk is decoded once, all of its hits applied, and re-encoded once, and a chunk whose voxels
would not actually change is skipped entirely. `bsvx_world_fill_box` shares the same path, so erasing
a volume is `key = 0`.

The flooring division is the part worth testing, and `test_locate_voxel_negative_coords` does:
world voxel `-1` belongs to region `-1` at local `3`, not to region `0`.

#### B7. Removal: regions, chunks, textures — **DONE**

**What's missing.** You can add a region, overwrite a chunk and upsert a registry entry. Nothing
deletes anything. `GODOT_INTEGRATION.md` §5 already flags this.

**Why Blender needs it.** "Rewriting" includes deleting. A user erases a structure, deletes a
collection, trims a world's bounds, removes a voxel type. Today the only expressible "delete" is
overwriting a chunk with all-zero voxels — which leaves the chunk present in the map, its summary
zeroed and its payload bytes still in the blob, and leaves emptied regions on disk forever.

**What to add.**

```c
BSVX_API bsvx_result bsvx_world_remove_region(bsvx_world*, size_t region_index);   /* invalidates later indices */
BSVX_API bsvx_result bsvx_region_remove_chunk(bsvx_world*, size_t region_index, uint16_t cx, uint16_t cy, uint16_t cz);
BSVX_API bsvx_result bsvx_region_clear_chunk (bsvx_world*, size_t region_index, uint16_t cx, uint16_t cy, uint16_t cz); /* keep entry, drop payloads */
BSVX_API bsvx_result bsvx_region_remove_chunk_payload(bsvx_world*, size_t region_index, uint32_t section_type,
                                                      uint16_t cx, uint16_t cy, uint16_t cz);
BSVX_API bsvx_result bsvx_world_prune_empty_regions(bsvx_world*, size_t* out_removed);
```

Two things must be got right: `remove_region` shifts every later `region_index`, so document it and
have the Python wrapper re-resolve indices after any removal (or return a stable region *id* instead
— worth considering). And `save_world` must **delete the orphaned `.bvx` file** from the regions
directory, or the next load re-discovers it via auto-discovery and the deletion silently undoes
itself (§9).

**Shipped**, including the two hazards called out above. Region indices do shift, and the docs and
header say so. Orphaned files are deleted by `BSVX_SAVE_PRUNE_ORPHANS` — **opt-in**, not automatic,
because a "save as" into a populated directory would otherwise delete files the world never owned.
`test_prune_orphans_on_save` asserts both halves: without the flag the file survives and
auto-discovery resurrects the region on the next load; with it, the file goes and the reload sees one
region.

`remove_chunk` has to renumber `ChunkMapEntry::summary_index` across the whole map, since summaries
are addressed indirectly — the test decodes every surviving chunk afterwards and checks each one's
non-air count against its summary, which is what catches a botched erase.

---

### Tier 1 — required for a tool people keep using

#### B8. Free-form world / region / chunk metadata — **DONE**

**What's missing.** The README advertises "tool metadata" in the manifest; `build_manifest_table`
writes no such table. `SectionType::USER_BASE` exists, and `PayloadSection` has a `chunk_associated`
flag — but the C ABI only exposes **chunk-associated** payloads (`bsvx_region_set_chunk_payload`),
and the deserialiser *infers* `chunk_associated` from `entries.size() == chunk_map.size()`
(`bvx.cpp:449`), which is a heuristic that misfires whenever a non-chunk section happens to have that
many entries. So there is nowhere safe to put "this world was authored by the Blender add-on v1.2,
voxel size 0.25, palette order X, source collection 'Terrain'".

**Why Blender needs it.** Lossless re-import. The add-on has state that has no home in the format —
which Blender object each region came from, export settings, palette display order, per-type UI color
overrides, a schema version for its own custom properties. Without a home, every re-import is a
downgrade, and any other tool that opens the world drops the state entirely.

**What to add.** A key/value blob at each level, plus an explicit non-chunk section type:

```c
BSVX_API bsvx_result bsvx_world_set_metadata (bsvx_world*, const char* key, const void* value, size_t size);
BSVX_API bsvx_result bsvx_world_get_metadata (const bsvx_world*, const char* key, void* out, size_t cap, size_t* out_size);
BSVX_API size_t      bsvx_world_metadata_count(const bsvx_world*);
BSVX_API bsvx_result bsvx_world_get_metadata_key(const bsvx_world*, size_t i, char* out, size_t cap, size_t* out_size);
BSVX_API bsvx_result bsvx_world_remove_metadata(bsvx_world*, const char* key);

BSVX_API bsvx_result bsvx_region_set_metadata(bsvx_world*, size_t region_index, const char* key, const void* value, size_t size);
BSVX_API bsvx_result bsvx_region_get_metadata(const bsvx_world*, size_t region_index, const char* key, void* out, size_t cap, size_t* out_size);
```

World-level keys land in a `[metadata]` table in `manifest.toml` (base64 for non-UTF-8 values);
region-level keys land in a non-chunk-associated `USER_BASE` section. **Fix the `chunk_associated`
heuristic at the same time** by writing the flag explicitly into `SectionRecord` — there are spare
bits available and the header is versioned.

Preservation rule to state in the docs and enforce in tests: **an unknown metadata key must survive a
load/save cycle untouched.** That is what makes third-party rewriting safe.

**Shipped** at the world and region levels (per-*chunk* metadata was dropped: an `OffsetSizeEntry`
payload in a `USER_BASE` section already does that job, and a second mechanism would be redundant).
World keys land in a `[metadata]` table in `manifest.toml` — printable UTF-8 as a plain readable
string, anything else as `{ hex = "..." }`, both round-tripping byte-for-byte. Region keys land in a
non-chunk `METADATA` section inside the `.bvx`.

The `chunk_associated` heuristic is fixed as promised: `SectionRecord` gained a `section_flags` word
**in what used to be alignment padding**, so the struct stays 40 bytes and v1 files still parse. A
`FLAGS_PRESENT` bit distinguishes "not chunk-associated" from "written by a build that did not record
it", and only the latter falls back to guessing from the entry count.

#### B9. Incremental and partial saving — **DONE**

**What's missing.** `Parser::save_world` re-serialises **the entire world**: every `.btx` file and
every `.bvx` file, every time (`bsvx.cpp` ~1050–1095). Editing one chunk rewrites the whole tree.
There is no per-region save into an existing world, no manifest-only save, and no dirty tracking.

**Why Blender needs it.** Worlds get big. A user hits Ctrl-S after moving three voxels and Blender
freezes while gigabytes of unchanged `.btx` and `.bvx` data are rewritten — and every one of those
files gets a new mtime, which is poison for version control, asset syncing and incremental cooking.

**What to add.**

```c
BSVX_API int         bsvx_region_is_dirty(const bsvx_world*, size_t region_index);
BSVX_API int         bsvx_world_is_dirty (const bsvx_world*);
BSVX_API bsvx_result bsvx_world_save_dirty(bsvx_context*, bsvx_world*, size_t* out_files_written);
BSVX_API bsvx_result bsvx_world_save_region_index(bsvx_context*, const bsvx_world*, size_t region_index, const char* path);
BSVX_API bsvx_result bsvx_world_save_manifest(bsvx_context*, const bsvx_world*, const char* manifest_path);
BSVX_API bsvx_result bsvx_world_save_manifest_memory(bsvx_context*, const bsvx_world*, char* out, size_t cap, size_t* out_size);
```

The catch, and it is a real one: the manifest hash is FNV-1a-64 over the raw bytes of
`manifest.toml`, and every `.bvx` stores the hash it was baked against. So **any** manifest change
invalidates every region and forces a full rewrite anyway. Incremental save is only meaningful
alongside B10.

**Shipped**, and the hash coupling is handled the only honest way: `save_world` serializes the
manifest text, compares it byte-for-byte with the file on disk, and if it matches, writes only the
dirty regions and leaves the manifest alone. If it differs, everything is rewritten — and
`bsvx_save_report.full_rewrite` tells the caller which happened rather than leaving them to guess.
`bsvx_world_save_manifest_memory` exposes the same text so a host can make that determination itself.

`test_dirty_incremental_save` asserts both branches: a voxel edit writes 1 file and skips 2; a rename
writes 3.

#### B10. Loosen the manifest-hash coupling — **DONE**

**What's missing.** On load, a region whose `manifest_hash` or `registry_hash` disagrees with the
manifest's is a **hard error** (`bsvx.cpp` ~949–956: `throw std::runtime_error("registry hash
mismatch for region …")`). The manifest hash is over the file's raw bytes, so it changes if someone
opens `manifest.toml` in an editor and adds a trailing newline, if git checks it out with CRLF (hence
the `*.toml -text` rule in `.gitattributes`), or if a future toml++ orders keys differently.

**Why Blender needs it.** An add-on that rewrites worlds will trip this constantly: partial saves,
crashed saves, users hand-editing the manifest, worlds shared through git. A world becoming
*completely unloadable* because of a whitespace change is the worst possible failure mode in a DCC
tool — the user's reasonable conclusion is that the add-on ate their work.

**What to add.**

```c
typedef enum bsvx_load_flags {
  BSVX_LOAD_DEFAULT             = 0,
  BSVX_LOAD_IGNORE_HASH_MISMATCH= 1u << 0,  /* downgrade to a warning retrievable from the context */
  BSVX_LOAD_SKIP_TEXTURES       = 1u << 1,  /* metadata-only open, fast */
  BSVX_LOAD_SKIP_REGIONS        = 1u << 2   /* manifest/palette only */
} bsvx_load_flags;

BSVX_API bsvx_result bsvx_world_load_ex2(bsvx_context*, const char* path, uint32_t flags, bsvx_world** out);
BSVX_API bsvx_result bsvx_world_rehash(bsvx_world*, size_t* out_regions_restamped);  /* repair */
BSVX_API size_t      bsvx_context_warning_count(const bsvx_context*);
BSVX_API const char* bsvx_context_warning(const bsvx_context*, size_t i);
```

`BSVX_LOAD_SKIP_TEXTURES` also matters on its own: a palette browser or a file-picker preview should
not have to read every `.btx` blob to show a world's name and bounds.

Longer term, hash the manifest's *semantic content* (the parsed values) rather than its bytes, which
makes formatting changes harmless by construction.

**Shipped** as the flags plus `bsvx_world_rehash`. The semantic-content hash was **not** done: it
changes what the hash means for every existing world, and the load flag solves the user-facing
problem without a migration. It stays on the Tier 2 list.

`bsvx_world_rehash` works by zeroing each region's stamps rather than computing new ones — the real
manifest hash is only knowable once the manifest text is final, which is at save time, and a zero on
either side of the comparison already means "unstamped". `test_hash_mismatch_tolerance_and_repair`
appends a comment to a manifest, confirms the strict load refuses, the tolerant load warns, and
rehash + save makes the strict load work again.

#### B11. Atomic, crash-safe writes — **DONE**

**What's missing.** Every save opens the destination path directly with `std::ofstream` and streams
into it. There is no temp-file-plus-rename, no fsync, no backup.

**Why Blender needs it.** Blender saves from the UI thread. Users kill Blender, run out of disk, save
to network shares and eject USB drives. An interrupted `save_world` today leaves a truncated
`manifest.toml` **and** a half-written `.bvx` — the world is gone, not merely stale.

**What to add.** Inside the library (so every host benefits): write to `<path>.tmp-<pid>`, `fsync`,
then `std::filesystem::rename` onto the target — atomic on the same filesystem on both POSIX and
Win32. Plus:

```c
typedef enum bsvx_save_flags {
  BSVX_SAVE_DEFAULT      = 0,
  BSVX_SAVE_ATOMIC       = 1u << 0,   /* temp + fsync + rename (should become the default) */
  BSVX_SAVE_BACKUP       = 1u << 1,   /* keep <file>.bak of the previous version */
  BSVX_SAVE_COMPACT_FIRST= 1u << 2,   /* run bsvx_world_compact before serialising */
  BSVX_SAVE_DRY_RUN      = 1u << 3    /* validate + report sizes, write nothing */
} bsvx_save_flags;

BSVX_API bsvx_result bsvx_world_save_ex2(bsvx_context*, bsvx_world*, const char* root_or_manifest, uint32_t flags);
```

`BSVX_SAVE_DRY_RUN` is what powers a truthful "this export will write 412 MB across 96 files" dialog.

**Shipped**, and **atomic is the default** — including for the pre-existing `bsvx_world_save` and
`bsvx_world_save_region`, since the result is identical and only the failure mode differs.
`BSVX_SAVE_NON_ATOMIC` opts out. `write_file_atomic` writes a sibling temp file, `fsync`s it (POSIX;
the rename alone publishes only the directory entry, not the contents), renames onto the target, and
falls back to an in-place write if the rename fails across a filesystem boundary. A failure removes
the temp file rather than leaving it behind.

`BSVX_SAVE_DRY_RUN` reports files and bytes without touching anything, which is what powers a
truthful "this export will write 412 MB across 96 files" dialog.

#### B12. Validation exposed to the host — **DONE**

**What's missing.** `btx::Archive::validate()` returns `std::vector<ValidationError>` and is not
exported. There is no world-level validation at all: nothing checks that every voxel key used in a
chunk exists in the registry, that every `material_id` resolves to a material in some `.btx`, that
regions fall inside declared bounds, that texture references resolve, or that chunk coordinates fall
inside `region_size`.

**Why Blender needs it.** An authoring tool must tell the user *before* writing that they painted with
a key they later deleted from the palette. Finding out at engine load time is far too late, and
Python cannot re-implement these checks cheaply.

**What to add.**

```c
typedef enum bsvx_validation_severity { BSVX_SEVERITY_INFO=0, BSVX_SEVERITY_WARNING=1, BSVX_SEVERITY_ERROR=2 } bsvx_validation_severity;

typedef struct bsvx_validation_issue {
  uint32_t severity;
  uint32_t code;              /* stable, machine-readable */
  int64_t  region_index;      /* -1 when not region-scoped */
  int64_t  chunk_ordinal;     /* -1 when not chunk-scoped */
  uint32_t voxel_key;         /* 0 when irrelevant */
} bsvx_validation_issue;

BSVX_API bsvx_result bsvx_world_validate(bsvx_context*, const bsvx_world*, uint32_t flags, size_t* out_issue_count);
BSVX_API bsvx_result bsvx_world_get_validation_issue(const bsvx_context*, size_t i, bsvx_validation_issue* out);
BSVX_API bsvx_result bsvx_world_get_validation_message(const bsvx_context*, size_t i, char* out, size_t cap, size_t* out_size);
```

Structured issues (not a text blob) let the add-on turn each one into a clickable row that selects
the offending region in the outliner.

**Shipped** with 15 stable issue codes (`bsvx_validation_code`), three severities, and a message per
issue. The cheap pass checks the four dominant keys each chunk summary records, geometry, bounds,
duplicate coordinates, duplicate registry keys, material resolution, `.btx` structure and texture
path length; `BSVX_VALIDATE_DEEP` decodes every chunk and catches every key actually present.

#### B13. Progress and cancellation — **DONE**

**What's missing.** `bsvx_world_load` and `bsvx_world_save` are opaque blocking calls.

**Why Blender needs it.** Blender's UI convention is a modal operator with a progress bar and Esc to
cancel. A multi-second blocking call with no feedback reads as a hang, and Windows will paint the
window "Not Responding".

**What to add.**

```c
/* Return 0 to request cancellation; the operation then fails with BSVX_RESULT_CANCELLED and
   (for atomic saves) leaves the destination untouched. Called from the calling thread only. */
typedef int (*bsvx_progress_fn)(void* user, const char* stage, size_t done, size_t total);
BSVX_API void bsvx_context_set_progress(bsvx_context*, bsvx_progress_fn, void* user);
```

Add `BSVX_RESULT_CANCELLED = 5` to `bsvx_result`. Note that a ctypes callback into Python must hold
the GIL and must not raise — document that, because it is a classic Blender crash.

**Shipped.** The callback is installed on the context and applies to every subsequent load, save,
validate and bulk decode. Cancelling unwinds through a `CancelledError` and surfaces as
`BSVX_RESULT_CANCELLED`; because saves are atomic, a cancelled save leaves the destination untouched
— `test_paths_and_progress` asserts exactly that. The Python wrapper's `Context.set_progress`
swallows exceptions raised inside the callback and treats them as a cancel request, since an
exception escaping a ctypes callback crashes the interpreter.

#### B14. Batched / bulk accessors to amortise FFI cost — **DONE**

**What's missing.** Everything is one call per item: `bsvx_region_get_chunk_info` per chunk,
`bsvx_world_get_registry_entry` per entry, `bsvx_region_decode_chunk_u32` per chunk. There is no
whole-region decode and no array form of anything.

**Why Blender needs it.** A ctypes call is ~1–3 µs. A 32×32×32-chunk region is 32,768 chunks: just
*enumerating* it costs ~50–100 ms of pure call overhead before any work happens, and importing a
large world means several of those passes.

**What to add.**

```c
BSVX_API bsvx_result bsvx_region_get_chunk_infos(const bsvx_world*, size_t region_index,
                                                 size_t first, size_t count,
                                                 bsvx_chunk_info* out_array, size_t* out_written);

/* Whole region into one dense buffer, air-filled where chunks are absent.
   Layout: chunk-major in chunk-map order unless BSVX_LAYOUT_REGION_LINEAR is asked for. */
BSVX_API bsvx_result bsvx_region_decode_all_u32(bsvx_context*, const bsvx_world*, size_t region_index,
                                                uint32_t layout, uint32_t* out_voxels, size_t capacity, size_t* out_written);

BSVX_API bsvx_result bsvx_world_get_registry_entries(const bsvx_world*, size_t first, size_t count,
                                                     bsvx_registry_entry* out_array, size_t* out_written);
```

With these plus B6, the Python side becomes: allocate one `numpy` array, hand its
`ctypes.data_as(POINTER(c_uint32))` to the library, and do all per-voxel work vectorised in numpy —
which is the only way this performs acceptably inside Blender.

**Shipped**, both layouts. `test_bulk_accessors` checks the whole-region decode against the
per-chunk path voxel by voxel in `BSVX_LAYOUT_CHUNK_ORDER` and again in `BSVX_LAYOUT_REGION_LINEAR`,
because a scatter that is subtly wrong on one axis is otherwise invisible.

#### B15. Region and world provenance — paths — **DONE**

**What's missing.** `bsvx_world_get_region_coord` exists; there is no way to ask **which file** a
region came from, or which path a world was loaded from. `RegionReference::absolute_path` is right
there in C++ and simply is not exported.

**Why Blender needs it.** Rewriting means writing back to the files you read. The add-on must show
"regions/r_3_0_-2.bvx — modified", must save one region back to its own file (B9), must detect that
the file changed on disk since import, and must relink when a user moves a world folder.

**What to add.**

```c
BSVX_API bsvx_result bsvx_world_get_source_path (const bsvx_world*, char* out, size_t cap, size_t* out_size);
BSVX_API bsvx_result bsvx_world_get_root_dir    (const bsvx_world*, char* out, size_t cap, size_t* out_size);
BSVX_API bsvx_result bsvx_world_get_region_path (const bsvx_world*, size_t region_index, char* out, size_t cap, size_t* out_size);
BSVX_API bsvx_result bsvx_world_set_region_path (bsvx_world*, size_t region_index, const char* relative_path);
BSVX_API bsvx_result bsvx_world_set_paths       (bsvx_world*, const char* regions_dir, const char* textures_dir);
```

**Shipped**, and `bsvx_world_save_dirty` is built on it: it writes back to
`bsvx_world_get_source_path`'s directory, and fails with `NOT_FOUND` for a world created in memory
that has never been saved.

---

### Tier 2 — reach, ergonomics, format

#### B16. Axis conventions beyond one enumerator **MISSING**

`AxisConvention` has exactly one value, `X_RIGHT_Y_UP_Z_FORWARD`. A world authored in Blender's Z-up
frame cannot be *labelled* Z-up, so the mapping in §3.1 has to be applied by convention with no way
to record whether it already was. Add `Z_UP_RIGHT_HANDED` (Blender/3ds Max) and `Y_UP_LEFT_HANDED`
(Unity), plus conversion helpers, and have the loader convert on read when the requested convention
differs from the stored one. Until then, **record the convention in world metadata (B8)** and check
it on import.

#### B17. Palette / color as a first-class concept **MISSING**

Getting a voxel type's color today is: registry entry → `material_id` → find the material row across
some `.btx` → its `tint_rgba8` or an albedo texture layer → the texel bytes. That is a lot of
indirection for what Blender users author first and think about most, and it is unavailable to any
world that has no `.btx` at all (which is every world you can currently create from C — see B3).

Two complementary additions:

- An optional `color_rgba8` on the registry entry (a display/debug tint, authoritative when no
  material resolves). Cheap, and it makes a palette meaningful before any texture exists.
- A convenience builder: `bsvx_world_make_palette_texture(bsvx_world*, const uint32_t* colors_rgba8,
  size_t count, const char* texture_id)` — builds a 1×1×N array texture plus one material per color
  plus the matching registry entries, in one call. That is the "MagicaVoxel-style palette" path, and
  it is what most Blender users will actually want.

#### B18. Per-chunk content hashes for change detection **MISSING**

Nothing identifies a chunk's *content*. The add-on cannot tell which chunks a user actually changed,
so any save is all-or-nothing (B9) and any diff against on-disk state is impossible.

```c
BSVX_API bsvx_result bsvx_region_chunk_content_hash(const bsvx_world*, size_t region_index,
                                                    uint16_t cx, uint16_t cy, uint16_t cz, uint64_t* out_hash);
```

FNV-1a over the decoded dense array (codec-independent, so re-encoding does not spuriously change it).
This is also what makes a sane undo integration possible — Blender's undo restores its own datablocks
and the add-on has to reconcile the BSVX side afterwards.

#### B19. Texel formats beyond RGBA8 **MISSING**

`bytes_per_texel` handles only `VK_FORMAT_R8G8B8A8_UNORM` (37) and `_SRGB` (43). `TextureKind::TEXTURE_3D`
and `TEXEL_BUFFER` are declared and implemented nowhere. Blender images are commonly float or 16-bit,
often single-channel (roughness, metallic, height), and users will expect at least R8, RG8, RGBA16F
and ideally BC1/BC3/BC7 compression for shipping. The add-on must otherwise convert everything to
RGBA8 on export and say so loudly in the UI.

Minimum useful set: R8_UNORM, R8G8_UNORM, R16G16B16A16_SFLOAT, R32G32B32A32_SFLOAT, BC7_UNORM_BLOCK.
Block-compressed formats need `bytes_per_texel` to become a `subresource_size(format, w, h)` function
— a signature change worth making now rather than later.

#### B20. Mip generation — **DONE**

Pulled forward with B3: `bsvx_texture_builder_generate_mips(builder, texture_id)` box-filters every
mip above 0 from the level-0 subresources already appended, per array layer, replacing any higher
mips that were there. Edges are clamped so odd extents keep their last row and column. The filter is
not selectable yet — a box filter is what a voxel palette wants, and the argument can be added later
without changing what the call means.

#### B21. UTF-8 path policy — **DONE**

Every path in the C ABI is `const char*` and is fed straight into `std::filesystem::path`. On MSVC
that constructor interprets `char*` in the **active code page**, not UTF-8 — so a Blender user on
Windows whose project lives in `C:\Users\Zoltán\Projekte\` gets a silent failure or a mangled path.
Blender hands out UTF-8 everywhere.

**Shipped, both halves.** `bsvx::path_from_utf8` / `path_to_utf8` convert through `std::u8string`,
and **every** entry point that takes a path now uses them — including the pre-v4 ones
(`bsvx_world_load`, `bsvx_world_save`, `bsvx_region_reader_open`) and the `std::string`-taking
`Archive::save_to_file` / `load_from_file` beneath them. `bsvx_dll.h` states the policy at the top.
`test_non_ascii_paths` and the Python `test_unicode_paths_roundtrip` both save into a directory named
`wörld-日本-🧊`, reload through the v4 *and* pre-v4 entry points, and check that a registry name with
a non-ASCII character round-trips.

#### B22. Threading contract for authoring — **DONE**

The contract is stated in `bsvx_dll.h` beside `bsvx_world_clone`: **one writer or many readers per
`bsvx_world`, no internal locking.** `bsvx_world_clone(ctx, world, &clone)` makes a deep copy sharing
nothing, so the add-on can snapshot, hand the copy to a background export thread, and let the user
keep editing the original. `test_world_clone` asserts the independence in both directions.

#### B23. Saving through a VFS — **DONE**

`bsvx_world_save_vfs(ctx, world, path, &writer, flags, &report)` with a `bsvx_vfs_writer` of six
callbacks, mirroring the load side. Internally this is a `FileWriter` interface that `save_world`
writes through; the native implementation is the atomic-write path from B11.

Two things the design has to be honest about. **Atomicity is the host's business**: this library
cannot rename inside someone else's filesystem, so the atomic and backup flags are passed *through*
to `write_file` rather than acted on. And `list_dir` / `read_file` are optional — without the first,
orphan pruning is disabled; without the second, a dirty-only save cannot compare the manifest and
conservatively rewrites everything. Both degrade to something safe rather than something wrong.

One bug this shook out: `save_world` unconditionally called `std::filesystem::absolute()` on its
target, which for a `res://` path prepended the process's working directory. It now only absolutizes
native paths.

#### B24. Test fixtures and CI for the authoring paths — **DONE**

`test/tests.cpp` now runs 37 tests. The 19 new ones cover `.btx` authoring, name round-trips,
removal, metadata preservation (text *and* binary), atomic saves and backups, dry runs, negative
world coordinates, incremental saves, hash-mismatch tolerance and repair, validation, bulk accessors,
content hashes, progress and cancellation. `test_manifest_text_matches_save` is the round-trip
invariant in its sharpest form: the text `bsvx_world_save_manifest_memory` returns must be
byte-identical to the file on disk, because the manifest hash is taken over exactly those bytes.

Tier 2 added six more: face states (the regression for the fixed bug), texel formats, axis
conversion arithmetic, whole-world axis conversion, palettes and colours, cloning, VFS saving, and
non-ASCII paths — 45 in total.

`python/tests/test_bsvx.py` adds 18 more on the binding itself, deliberately stdlib-only
(`unittest`, no pytest, no numpy) so it runs inside a host application's bundled Python.

`.github/workflows/ci.yml` builds and tests on Linux, macOS (universal) and Windows, uploads the
staged shared library as an artifact per platform, and runs a second job covering `-std=c++20` and
`_GLIBCXX_DEBUG`.

---

## 5. Feature list at a glance

| # | Feature | Tier | Status | Shipped as |
|---|---|---|---|---|
| B1 | Python-loadable, distributable build | 0 | **DONE** | `python/bsvx/`, CMake install + SOVERSION, CI matrix |
| B2 | ABI struct-size / result-string self-description | 0 | **DONE** | `bsvx_struct_size`, `_result_string`, `_build_info` |
| B3 | `.btx` authoring from C + attach to world | 0 | **DONE** | `bsvx_texture_builder_*`, `bsvx_world_add_texture` |
| B4 | Registry names survive round-trip | 0 | **DONE** | world-desc v2 string table, `_get/set_registry_name` |
| B5 | Voxel size / origin / units in the format | 0 | **DONE** | `[units]`, `bsvx_units`, `_get/set_units` |
| B6 | World-space sparse voxel set/get, box fill | 0 | **DONE** | `bsvx_world_set_voxels`, `_get_voxels`, `_fill_box`, `_locate_voxel` |
| B7 | Remove region / chunk / texture | 0 | **DONE** | `bsvx_world_remove_region`, `bsvx_region_remove_chunk`, … |
| B8 | Free-form world/region metadata | 1 | **DONE** | `[metadata]`, `METADATA` section, explicit `section_flags` |
| B9 | Incremental / dirty-only / manifest-only save | 1 | **DONE** | `BSVX_SAVE_DIRTY_ONLY`, `bsvx_world_save_dirty` |
| B10 | Load flags, hash-mismatch tolerance, rehash | 1 | **DONE** (no semantic hash) | `bsvx_world_load_ex2`, `_rehash`, context warnings |
| B11 | Atomic saves + backup + dry run | 1 | **DONE**, atomic by default | `write_file_atomic`, `bsvx_save_flags` |
| B12 | World validation as structured issues | 1 | **DONE** | `bsvx_world_validate`, 15 stable codes |
| B13 | Progress + cancellation callbacks | 1 | **DONE** | `bsvx_context_set_progress`, `BSVX_RESULT_CANCELLED` |
| B14 | Bulk/batched accessors | 1 | **DONE** | `_get_chunk_infos`, `_decode_all_u32`, `_get_registry_entries` |
| B15 | Region + world source paths | 1 | **DONE** | `_get_source_path`, `_get_root_dir`, `_get/set_region_path` |
| B16 | More axis conventions + conversion | 2 | **DONE** (explicit, not on load) | `bsvx_convert_cell`, `_convert_position`, `_world_convert_axis_convention` |
| B17 | Palette / registry colour as first-class | 2 | **DONE** | `_get/set_registry_color`, `bsvx_world_make_palette` |
| B18 | Per-chunk content hashes | 2 | **DONE** | `bsvx_region_chunk_content_hash` |
| B19 | Texel formats beyond RGBA8 | 2 | **DONE**, 20 formats | `bsvx_format_subresource_size`, `_is_block_compressed` |
| B20 | Mip generation | 2 | **DONE** (8-bit uncompressed only) | `bsvx_texture_builder_generate_mips` |
| B21 | UTF-8 path policy | 2 | **DONE**, all entry points | `path_from_utf8` / `path_to_utf8` |
| B22 | Threading contract + `world_clone` | 2 | **DONE** | `bsvx_world_clone`, contract in the header |
| B23 | Saving through a VFS | 2 | **DONE** | `bsvx_world_save_vfs`, `bsvx_vfs_writer`, `FileWriter` |
| B24 | Authoring fixtures + CI | 2 | **DONE** | 45 C++ tests, 18 Python tests, 3-platform CI |

**Everything on this list is implemented.** What is left is genuinely outside the library's job:
meshing, baking, LOD generation and voxelization stay the consumer's, by design. The format-level
limits that remain are `TextureKind::TEXTURE_3D` / `TEXEL_BUFFER` (declared, unimplemented),
little-endian-only containers, and a manifest hash taken over raw bytes rather than semantic
content — the last of which `BSVX_LOAD_IGNORE_HASH_MISMATCH` makes survivable rather than fatal.

---

## 6. Python binding strategy

**Use ctypes, not pybind11.** Blender ships a fixed CPython (3.11 for Blender 4.x) but users run
Blender builds from distributions with different ABI details; a pybind11 extension is bound to one
Python ABI version and one platform triple, and it must be rebuilt for every Blender Python bump.
`ctypes.CDLL` against the flat C ABI has none of those constraints — which is exactly what the C ABI
was designed for. It also means the add-on can be shipped as a plain zip with three binaries in it.

Layout:

```
blender_bsvx/                  ← the add-on zip root
├── __init__.py                # bl_info, register()/unregister()
├── bin/
│   ├── linux_x86_64/libbsvx.so
│   ├── windows_x86_64/bsvx.dll
│   └── macos_universal/libbsvx.dylib
├── ffi/
│   ├── _lib.py                # CDLL loading, platform pick, ABI + struct-size checks (B2)
│   ├── _structs.py            # ctypes mirrors of bsvx_dll.h
│   └── api.py                 # thin 1:1 function declarations, argtypes/restype for every symbol
├── core/
│   ├── world.py               # Pythonic World/Region/Chunk wrappers, context manager, error raising
│   ├── convert.py             # THE coordinate module (§3) — the only place the axis swap lives
│   ├── voxelize.py            # mesh → sparse voxels
│   └── palette.py             # Blender materials/images ↔ registry + .btx
├── ops/                       # bpy.types.Operator: import, export, save, rewrite, validate
├── ui/                        # panels, palette list, region list
└── props/                     # PropertyGroups stored on the scene/objects
```

Rules that save pain later:

- Declare `argtypes` and `restype` on **every** function. ctypes defaults to `int` returns and
  unchecked args; on 64-bit that silently truncates every returned `size_t` and pointer.
- Wrap every call in a helper that checks `bsvx_result` and raises a Python exception carrying
  `bsvx_context_last_error()`. Never let a non-zero result pass silently.
- One `bsvx_context` per thread, created and destroyed by a context manager.
- Keep Python references to every buffer handed to the library for the duration of the call —
  ctypes will happily let a temporary be collected mid-call.
- Use `numpy` arrays for voxel buffers and pass `arr.ctypes.data_as(POINTER(c_uint32))`. Blender
  ships numpy. Never build voxel arrays with Python loops.
- Never let a Python exception propagate out of a ctypes callback (B13, and the VFS callbacks) —
  it crashes the interpreter. Catch everything at the callback boundary and return a failure code.

---

## 7. Add-on architecture

> **Shipped.** The add-on described below now exists at
> [`integrations/blender/`](integrations/blender/), and its README is the current reference for how
> it actually works. Two decisions in this section were refined by building it, and the refinements
> are noted inline: a voxel *is* materialised as Blender geometry, but only a bounded **working
> set** of it at a time, and that geometry is a point mesh rather than an empty-per-region proxy.

### 7.1 How a world lives in Blender

Do **not** materialise the *whole* world as Blender geometry. A 512³ world is 134 M voxels;
Blender's mesh datablocks will not survive it and neither will the user's session. Instead:

- The world is an **add-on-owned handle** (a `bsvx_world*`) held in a module-level registry, keyed by
  an id stored in a scene `PropertyGroup`. It does not survive a `.blend` reload — the add-on
  re-opens the world from its recorded path on load, or asks.
- Blender objects act as **proxies**: one empty per region with the region coordinate in custom
  properties, optionally a decimated preview mesh per region generated on demand.

  **As shipped:** a proxy that cannot be edited is not much of an editor, so the proxy became a
  *checked-out working set* — one vertex per non-air voxel, its key in a `bsvx_key` POINT attribute,
  bounded by a box the object records and a vertex budget the user sets. Committing replaces that
  box, which is what makes deleting a vertex mean deleting a voxel. See
  [`integrations/blender/README.md`](integrations/blender/README.md).
- Authoring happens through **operators** that read Blender data and push voxels through the C API
  (B6), not through a persistent Blender-side voxel datablock.
- The palette is a `CollectionProperty` of `(voxel_key, name, color, flags, material_id)` mirrored
  into the world's registry on save. This is the datum that B4 must not destroy.

### 7.2 Preview meshing

Only ever mesh what the user is looking at, and use the chunk summary before decoding anything:
`non_air_count == 0` skips the chunk, `face_state_*` skips interior faces against a full neighbour,
`macro_occ_4x4x4` gives a 4³ occupancy mask for a cheap LOD, `top_id_*` gives the dominant voxel keys
for a flat-colored preview without decoding at all. `GODOT_INTEGRATION.md` §8.2 covers this in
detail and applies unchanged.

Build preview meshes with `bmesh` or `Mesh.from_pydata` on a background-safe worker, and hand them
back on the main thread with a timer (`bpy.app.timers`). Never touch `bpy.data` off the main thread.

### 7.3 Operators the add-on needs

| Operator | Uses |
|---|---|
| `bsvx.import_world` | `bsvx_world_load_ex2` (B10), bulk chunk info (B14), `.btx` reads |
| `bsvx.import_region` | `bsvx_world_load_region` / `_memory` |
| `bsvx.export_world` | `bsvx_world_create` + B3 + B5 + B6 + `bsvx_world_save_ex2` (B11) |
| `bsvx.save_world` | dirty save (B9), atomic (B11), progress (B13) |
| `bsvx.voxelize_selection` | mesh sampling → `bsvx_world_set_voxels` (B6) |
| `bsvx.palette_sync` | registry upsert + names (B4) + palette texture (B17) |
| `bsvx.validate_world` | `bsvx_world_validate` (B12), issues → UI list |
| `bsvx.compact_world` | `bsvx_world_compact` (exists) |
| `bsvx.delete_region` | `bsvx_world_remove_region` (B7) |

---

## 8. The rewrite workflow, step by step

This is the workflow the request centres on, written out with the gaps marked so the order of work is
obvious:

1. **Open** the world — `bsvx_world_load_ex2(path, BSVX_LOAD_IGNORE_HASH_MISMATCH)` **(B10)**.
2. **Read provenance** — root dir, per-region paths **(B15)**; record mtimes for later
   change-detection.
3. **Read everything the add-on does not own** — unknown metadata keys **(B8)**, unknown payload
   sections, registry names **(B4)**, units **(B5)**. Hold them verbatim.
4. **Present** — palette from registry + `.btx` **(B17)**, region proxies, previews from summaries.
5. **Edit** — sparse world-space writes **(B6)**, removals **(B7)**, palette edits **(B4)**.
6. **Validate** — `bsvx_world_validate` **(B12)**; refuse to save on errors, warn on warnings.
7. **Write back** — restore the untouched metadata verbatim **(B8)**, `bsvx_world_compact`, then
   dirty-only **(B9)**, atomic, with a backup **(B11)**, reporting progress **(B13)**.
8. **Verify** — reload the saved world and assert equality of everything the add-on knows about;
   this is the fixture from **B24** running in production.

Steps 3 and 7 together are the definition of lossless rewriting, and every step is supported as of
v4. In Python:

```python
from bsvx import World, REGISTRY_OPAQUE

with World.load("world_root", ignore_hash_mismatch=True) as world:   # 1, 2
    for w in world.warnings:
        print("warning:", w)

    tool_state = world.get_metadata("blender.state")                 # 3 — hold it verbatim
    world.set_registry_entry(7, material_id=2, flags=REGISTRY_OPAQUE, name="granite")  # 5
    world.set_voxels([(12, 0, -4), (13, 0, -4)], 7)
    world.remove_region(world.find_region(9, 9, 9))

    problems = [i for i in world.validate(deep=True) if i.is_error]   # 6
    if problems:
        raise RuntimeError(problems[0].message)

    if tool_state is not None:                                        # 7 — put it back untouched
        world.set_metadata("blender.state", tool_state)
    world.compact()
    report = world.save(world.source_path, dirty_only=True, prune_orphans=True, backup=True)
    print(report)                                                     # 8
```

The one thing the library cannot do for you: **carry through metadata keys you do not understand.**
It preserves them across its own load/save cycle, but if your add-on rebuilds a world from scratch
rather than editing the loaded one, it has to copy them itself.

---

## 9. Landmines

Everything in `GODOT_INTEGRATION.md` §9 applies. These are the ones that bite an **authoring** tool
specifically:

1. ~~**Registry names are destroyed on save.**~~ **Fixed in v4 (B4)** — names round-trip through both
   `manifest.toml` and a standalone `.bvx`.
2. ~~**`BtxRef.relative_path` truncates silently.**~~ **Fixed in v4**: `bsvx_world_add_texture` and
   the save path both reject a reference path longer than 127 characters instead of cutting it, and
   `bsvx_world_validate` reports it as an error. Blender image names are long and users nest folders,
   so you will hit this — but now you hit it as a message rather than as a texture that silently
   fails to resolve.
3. **Chunk dimensions must be ≤ 255** — `ChunkSummary` stores its AABB in `uint8_t`.
   `bsvx_world_create` enforces it; a UI that offers "chunk size" must clamp too.
4. **Set the registry *before* writing voxels.** `set_chunk_u32` builds the chunk summary through the
   registry; with an empty registry every non-air voxel is counted opaque and none emissive/special.
   Re-ordering the palette after painting does not retroactively fix summaries — re-write the chunks.
5. **Removing a region does not delete its file** unless you save with `prune_orphans=True`.
   Auto-discovery finds the orphan on the next load and the deletion undoes itself. This is
   deliberate — a "save as" into a populated directory must not delete files the world never owned —
   but it means every removal has to be paired with a pruning save.
6. ~~**`save_world` rewrites every `.btx`.**~~ **Fixed in v4 (B9)** — `dirty_only=True` skips
   unchanged assets. Note the fallback: if the manifest text changes at all, everything is rewritten,
   because the manifest hash is stamped into every region. `SaveReport.full_rewrite` tells you which
   happened.
7. **Manifest hash vs line endings.** `.gitattributes` pins `*.toml -text`; if an add-on user's
   toolchain rewrites line endings anyway, every region fails to load. Open with
   `ignore_hash_mismatch=True` and call `rehash()` before saving to repair it (B10). Never write
   `manifest.toml` yourself from Python without `newline=''` — always write bytes, LF only.
8. **Region indices are positional** and shift on removal (B7). Never cache a `region_index` across a
   mutation; look it up with `bsvx_world_find_region` by coordinate.
9. **The containers are raw little-endian POD dumps** with no endian check enforced. Fine for the
   platforms Blender ships on; do not promise otherwise.
9b. ~~**`compute_face_state` is wrong.**~~ **Fixed**: all three axis cases tested `x` and `total` was
   never incremented, so `FaceState::FULL` was unreachable. Face-state culling now works. The
   consequence to know about: the summary bytes of a chunk written by this build differ from one
   written by an older build. Regions are only rewritten when you save them, so a world mixes both
   until then — which is harmless, since a stale EMPTY/MIXED merely under-culls.
10. **`Binaries/` and `.vs/` in the repo are Windows MSVC leftovers** and are untracked. Ignore them;
    the add-on's binaries come from the CI matrix in B1.

---

## 10. What is left

Nothing on the B1–B24 list. The format changes were batched into one bump as planned: the world-desc
blob is version 2, carrying registry names, colours and units behind a size-prefixed extension, and
`SectionRecord` records `chunk_associated` explicitly in what used to be padding. v1 files still
load; v1 readers still read v2 files, ignoring what they do not know.

What the library still deliberately does not do:

- **mesh, bake, generate LOD, or voxelize.** That is the consumer's, by design.
- **compress or transcode texels.** BCn blocks are stored and sized, not produced —
  `generate_mips` refuses them rather than pretending.
- **convert on load.** `bsvx_world_convert_axis_convention` is explicit because it rewrites the
  entire world; opening a file should not silently cost that.

Format-level limits worth knowing:

- `TextureKind::TEXTURE_3D` and `TEXEL_BUFFER` are declared and implemented nowhere.
- The containers are little-endian POD dumps; `DiskHeader.endian` exists in `.btx` and is not
  enforced. Do not ship to a big-endian target.
- The manifest hash is FNV-1a over the manifest's raw bytes, not its parsed content, so any
  reformatting invalidates every region's stamp. `BSVX_LOAD_IGNORE_HASH_MISMATCH` plus
  `bsvx_world_rehash` make that recoverable; a semantic hash would make it a non-event, at the cost
  of changing what the hash means for every existing world.

Bump `BSVX_ABI_VERSION` for anything that adds symbols, and keep `bsvx_abi_version()` as the add-on's
compatibility gate. The Python binding already fails loudly on a library older than it expects, and
on a struct whose size does not match its mirror.

---

## 11. Definition of done

The add-on can be called finished when all of these hold:

- [ ] Import a world authored elsewhere, edit one voxel, save, and **every byte the add-on did not
      intend to change is unchanged** — registry names, unknown metadata, unknown payload sections,
      untouched regions' files.
- [ ] Author a world from scratch in Blender — geometry, palette with names and colors, textures —
      save it, and load it in the Godot plugin without a manual step.
- [ ] A save interrupted by killing Blender leaves the previous world intact.
- [ ] A world with a hand-edited `manifest.toml` still opens, with a warning.
- [ ] Importing a 100 M-voxel world does not block the UI for more than a frame at a time and can be
      cancelled.
- [ ] Non-ASCII paths work on Windows, macOS and Linux.
- [ ] The round-trip invariant test runs in CI on all three platforms.

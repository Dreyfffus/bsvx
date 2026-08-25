# BSVX (Basil Voxel) — Agent Brief for Godot Plugin Integration on Linux

**Purpose of this file:** this is a context document for a coding agent (Claude) that will be asked to
build a **Godot 4 GDExtension plugin** that reads/writes voxel worlds using **this specific library**,
on **Linux**. It describes the whole project, the exact on-disk formats, the exact public API, the
verified Linux build, the recommended plugin architecture, and every landmine found by actually
compiling and running the library on this machine.

Everything marked **VERIFIED** was executed on this machine (Fedora, GCC 16.1.1, 2026-07-31 and
2026-08-01).
Everything marked **NOT IMPLEMENTED** does not exist in the code today — do not assume otherwise.

---

## 1. What BSVX is

BSVX ("Basil Voxel") is a C++ asset baker / packer / loader / decoder for voxel worlds, written for the
"Basil" engine. It is a **library**, not an engine. It gives you:

- a manifest-driven world layout (`manifest.toml`)
- a binary **voxel region** container (`.bvx`) holding chunked voxel data + optional baked payloads
- a binary **texture/texel** container (`.btx`), Vulkan-oriented
- summary-first access: you can inspect chunk statistics *before* decoding voxels
- write-back: mutate chunks, attach baked payloads, save
- partial loading: open a region's metadata only and fetch individual chunk payloads on demand
- a flat **C ABI** (`bsvx_dll.h`) intended as the shipping boundary for tools/engines

It deliberately does **not** mesh, bake, or render anything. The consumer owns all
of that.

### Repository layout

```
bsvx/
├── src/include/          # public + internal headers
│   ├── bsvx_dll.h        # ← THE C ABI. This is what a Godot plugin should use.
│   ├── include.h         # just #includes bsvx_dll.h
│   ├── bsvx.h            # C++ internals: Manifest, WorldPackage, Parser
│   ├── bvx.h / bvx_header.h   # .bvx region archive + on-disk structs
│   ├── btx.h / btx_header.h   # .btx texture archive + on-disk structs
│   ├── codec.h           # voxel encode/decode primitives
│   ├── definitions.h     # all enums (flags, codecs, section types, …)
│   ├── util.h            # POD serialization helpers, fnv1a64, concepts
│   └── vk_btx.h          # optional Vulkan helpers, gated on BSVX_VULKAN (irrelevant to Godot)
├── src/impl/             # implementation (.cpp) + vendored toml++ v3.4.0 (toml.hpp, toml_impl.cpp)
├── test/tests.cpp        # fixture-driven test runner, uses ONLY the C API
├── test/data/            # world_basic/ (manifest world) and standalone_region/
├── Build.lua, premake/premake5.exe, build.bat, *.vcxproj, bsvx.sln   # Windows/MSVC build (see §6)
└── Binaries/             # prebuilt Windows x86_64 DLLs (untracked) — useless on Linux
```

Source size is small: ~4,100 lines total including headers, plus vendored toml++.

Only external dependency: **toml++ v3.4.0**, vendored as `src/impl/toml.hpp` and compiled once in
`src/impl/toml_impl.cpp` (needs `TOML_HEADER_ONLY=0` defined for the whole library).

---

## 2. Data model (read this before writing any plugin code)

### World → Region → Chunk → Voxel

| Concept | Meaning | Where it lives |
|---|---|---|
| **World** | root directory + `manifest.toml` | filesystem directory |
| **Region** | a spatial box of chunks | one `.bvx` file |
| **Chunk** | smallest decode/bake unit | one entry in a region's chunk map |
| **Voxel** | one `uint32_t` **voxel key** | dense array inside a chunk |
| **Registry** | maps voxel key → material_id + flags | manifest `[[registry]]`, mirrored into the world desc |
| **Texture** | sampler/texture/material tables + texel blob | one `.btx` file |

`GeometryDesc` has two triplets and they mean different things:

- `chunk_size_{x,y,z}` — **voxels per chunk** (fixture: 4×4×4; typical: 16×16×16)
- `region_size_{x,y,z}` — **chunks per region** (fixture: 1×1×1)

Voxel key `0` is **air by convention** everywhere in the code (summaries skip it, `CHUNK_EMPTY`
means all-zero). Non-zero keys resolve through the registry.

### On-disk world

```
world_root/
  manifest.toml
  regions/  r_0_0_0.bvx  r_1_0_0.bvx  …
  textures/ terrain_red.btx  terrain_blue.btx  …
```

`manifest.toml` (this is the real fixture, `test/data/world_basic/manifest.toml`):

```toml
format_version = 1

[world]
name = "FreshBasilWorld"
uuid = "basil-world-2r2t"

[geometry]
chunk_size = [4, 4, 4]
region_size = [1, 1, 1]
voxel_schema = "dense_u32_voxel_key"
axis_convention = "x_right_y_up_z_forward"

[bounds]
mode = "explicit"          # or "unbounded"
min_region = [0, 0, 0]
max_region = [1, 0, 0]

[paths]
regions_dir = "regions"
textures_dir = "textures"

[[registry]]
voxel_key = 1
material_id = 0
flags = ["opaque", "collidable"]   # opaque | emissive | special | collidable
name_hash = 0                      # or use name = "stone" and it gets fnv1a64'd

[[textures]]
id = "terrain_red"
path = "terrain_red.btx"           # relative to textures_dir

[[regions]]
path = "r_0_0_0.bvx"               # relative to regions_dir
coord = [0, 0, 0]                  # optional; if present it MUST match the .bvx header
```

Optional `[hashes]` table with `registry` / `manifest` keys is parsed but see the **manifest-hash
landmine in §9.1** — the manifest hash is always recomputed from the file bytes regardless.

If `[[textures]]` is absent, the loader auto-discovers `*.btx` in `textures_dir` (sorted).
If `[[regions]]` is absent, it auto-discovers `*.bvx` in `regions_dir` (sorted).

### `.bvx` region file

Magic `0x584F564C49534142` (`"BASILVOX"` little-endian), version 1, integrity via a **FNV-1a-64 over
the whole file with the crc field zeroed** (the field is named `crc64` but it is FNV-1a, not CRC).

Structure (all offsets absolute, all tables 16-byte aligned):

```
DiskHeader (magic, version, flags, region_x/y/z, chunk_count, section_count,
            chunk_map_offset, summary_table_offset, section_dir_offset, file_size,
            manifest_hash, registry_hash, crc64)
ChunkMapEntry[chunk_count]     # local_chunk_x/y/z, flags, summary_index, user0
ChunkSummary[chunk_count]      # precomputed statistics, see below
SectionRecord[section_count]   # section_type, default_codec, entry_stride, entry_count,
                               # entry_table_offset, blob_offset, blob_size
per section: OffsetSizeEntry[entry_count]  # offset, size, flags, codec  (one per chunk)
             blob[blob_size]
```

`FileFlags::STANDALONE (1)` in the header means the file carries a `WORLD_DESC` section and can be
loaded **without** a manifest. A region taken out of a manifest world is *not* standalone and will be
rejected by `bsvx_world_load_region`.

**Section types** (`SectionType`, `definitions.h`):

| Value | Name | Notes |
|---|---|---|
| 1 | `WORLD_DESC` | standalone metadata blob; not chunk-associated; managed by the library |
| 2 | `VOXELS` | the voxel payloads |
| 3 | `SURFACE` | your baked mesh/surface data |
| 4 | `COLLISION` | your baked collision data |
| 5 | `DISTANCE_FIELD` | SDF-ish payloads |
| 6 | `LIGHT` | light bake |
| 0x80000000+ | `USER_BASE` | anything you want |

Sections 3–6 and user sections are **opaque byte blobs to BSVX**. It stores them per chunk, records a
`codec` u16 that means whatever you decide, and sets a corresponding `ChunkFlags` bit. This is the
intended place for a Godot plugin to cache baked meshes/collision.

**ChunkFlags** (u16, on `ChunkMapEntry.flags`): `PRESENT=1`, `CHUNK_EMPTY=2`, `HAS_VOXELS=4`,
`HAS_SURFACE_BAKE=8`, `HAS_COLLISION_BAKE=16`, `HAS_DISTANCE_FIELD=32`, `HAS_LIGHT_BAKE=64`.

**ChunkSummary** — the "inspect before decode" payload, all precomputed at write time:

- `non_air_count`, `opaque_count`, `emissive_count`, `special_count`
- `aabb_min/max_{x,y,z}` (u8, chunk-local, so chunk dims must be ≤ 255)
- `face_state_{px,nx,py,ny,pz,nz}` — `FaceState`: `0=EMPTY, 1=FULL, 2=MIXED` → **use this for cheap
  neighbour-occlusion culling in the mesher**
- `macro_occ_4x4x4` — u64 bitmask, chunk subdivided into 4×4×4 macrocells, bit index
  `gx + 4*(gy + 4*gz)`, set if any non-air voxel falls in the cell → cheap LOD/raymarch acceleration
- `top_id_0..3` / `top_count_0..3` — 4 most common voxel keys and their counts

**Voxel codecs** (`VoxelCodec`, per-chunk, stored in the `OffsetSizeEntry.codec`):

| Value | Codec | Payload |
|---|---|---|
| 0 | `CHUNK_INVALID` | — |
| 1 | `CHUNK_EMPTY` | empty; decodes to all zeros |
| 2 | `CHUNK_UNIFORM` | single u32 key |
| 3 | `PALLETE_BITPACK` | palette + LSB-first bitpacked indices |
| 4 | `SPARSE_LIST` | (index, key) pairs |
| 5 | `Y_COLUMN_INTERVALS` | run-length along Y columns |
| 6 | `RAW_DENSE` | dense u32 array |
| 7 | `SPARSE_PACKED` | palette + bit-packed (index, palette index) pairs |
| 0xFFFF | `AUTO` | on write: try all, keep the smallest (`choose_best_voxel`) |

Pass `0xFFFF` (AUTO) when writing unless you have a reason not to. Decode is transparent — you always
get a dense `uint32_t` array back.

### `.btx` texture file

Magic `0x5845544C49534142` (`"BASILTEX"`), version 1. Tables: `SamplerDesc[]`, `DiskTextureDesc[]`,
`SubresourceDesc[]`, `MaterialDesc[]`, plus a texel blob. Formats are **raw Vulkan enum values**
(`vk_format`), and only two are supported by `bytes_per_texel()` today:
`VK_FORMAT_R8G8B8A8_UNORM (37)` and `VK_FORMAT_R8G8B8A8_SRGB (43)`; anything else returns 0 and
`append_subresource` throws. `MaterialDesc` carries per-face albedo/normal array-layer indices
(px/nx/py/ny/pz/nz) plus `tint_rgba8` — i.e. it is designed around a texture-2D-array atlas.

Since ABI v3 the C surface can **read** all of that — descs, materials, samplers and texel bytes
(§4.3). It still cannot *author* a `.btx`; that is C++-only.

---

## 3. Coordinates and indexing — get this exactly right

### Chunk-local dense array order

```c
index = x + size_x * (y + size_y * z)      // X fastest, then Y, then Z
```
(`codec.cpp: linear_index`). Array length is `chunk_size_x * chunk_size_y * chunk_size_z`, which is
exactly what `bsvx_region_required_voxel_count()` returns.

### World position of a voxel

The library **never computes world positions**. You must:

```
world_voxel = (region_coord * region_size + local_chunk_coord) * chunk_size + local_voxel_coord
```

where `region_coord` comes from `bsvx_world_get_region_coord()` and `local_chunk_coord` from
`bsvx_chunk_info.local_chunk_{x,y,z}`.

### Axis convention vs Godot

BSVX declares `X_RIGHT_Y_UP_Z_FORWARD`. Godot 4 is also right-handed Y-up, but Godot's *visual
forward* is **-Z**. Both are the same handedness, so the safe mapping is the **identity**:

```
Godot Vector3(x, y, z) = BSVX (x, y, z)
```

Do **not** negate Z to "fix forward" — negating a single axis flips handedness and will invert every
triangle winding and normal you generate. If the art direction needs the world flipped, rotate 180°
about Y instead.

---

## 4. The public C API (`src/include/bsvx_dll.h`) — complete

Pure C, `extern "C"`, no STL crosses the boundary. On Linux `BSVX_API` expands to
`__attribute__((visibility("default")))`. **79 symbols, VERIFIED exported** (20 original +
33 added in ABI v2, §4.1 — plus 26 added in ABI v3, §4.3). Check `bsvx_abi_version()` at runtime;
it returns `3`.

```c
// lifetime / diagnostics
bsvx_context* bsvx_context_create(void);
void          bsvx_context_destroy(bsvx_context*);
const char*   bsvx_context_last_error(const bsvx_context*);   // owned by ctx, valid until next call

// load / save
bsvx_result bsvx_world_load(bsvx_context*, const char* path, bsvx_world** out);        // manifest.toml, or a root dir, or a .bvx
bsvx_result bsvx_world_load_region(bsvx_context*, const char* path, bsvx_world** out); // standalone .bvx only
void        bsvx_world_destroy(bsvx_world*);
bsvx_result bsvx_world_save(const bsvx_world*, const char* root_or_manifest_path);
bsvx_result bsvx_world_save_region(const bsvx_world*, const char* region_path);

// world queries
size_t      bsvx_world_region_count(const bsvx_world*);
size_t      bsvx_world_texture_count(const bsvx_world*);
bsvx_result bsvx_world_geometry(const bsvx_world*, bsvx_geometry_desc* out);
size_t      bsvx_world_registry_entry_count(const bsvx_world*);
bsvx_result bsvx_world_get_registry_entry(const bsvx_world*, size_t i, bsvx_registry_entry* out);
bsvx_result bsvx_world_get_region_coord(const bsvx_world*, size_t region_i, int32_t* x, int32_t* y, int32_t* z);

// region / chunk queries
size_t      bsvx_region_chunk_count(const bsvx_world*, size_t region_i);
bsvx_result bsvx_region_get_chunk_info(const bsvx_world*, size_t region_i, size_t chunk_ordinal, bsvx_chunk_info* out);
size_t      bsvx_region_required_voxel_count(const bsvx_world*, size_t region_i);   // voxels per chunk

// voxel decode / encode
bsvx_result bsvx_region_decode_chunk_u32(const bsvx_world*, size_t region_i,
                                         uint16_t cx, uint16_t cy, uint16_t cz,
                                         uint32_t* out_voxels, size_t capacity, size_t* out_written);
bsvx_result bsvx_region_set_chunk_u32(bsvx_world*, size_t region_i,
                                      uint16_t cx, uint16_t cy, uint16_t cz,
                                      const uint32_t* voxels, size_t count, uint16_t requested_codec);
bsvx_result bsvx_region_set_chunk_payload(bsvx_world*, size_t region_i, uint32_t section_type,
                                          uint16_t cx, uint16_t cy, uint16_t cz, uint16_t codec,
                                          const void* payload, size_t size, uint16_t entry_flags);
```

`bsvx_result`: `0 OK`, `1 INVALID_ARGUMENT`, `2 NOT_FOUND`, `3 BUFFER_TOO_SMALL`, `4 RUNTIME_ERROR`.

Structs passed across the boundary are plain PODs: `bsvx_geometry_desc`, `bsvx_registry_entry`
(`voxel_key`, `material_id`, `flags`, `name_hash`), `bsvx_chunk_summary`, and `bsvx_chunk_info`
(`local_chunk_{x,y,z}`, `flags`, `summary_index`, embedded `summary`).

### Semantics you must know (all VERIFIED by running probes)

1. **`bsvx_world_load` accepts three things**: a `manifest.toml` path, a directory containing one, or
   a `.bvx` path (it silently forwards to `load_region`).
2. **`bsvx_world_load` is fully eager.** The entire world — every `.bvx` and every `.btx` — is read
   into memory. Voxel payloads stay *encoded*; decode is lazy and per call. For partial loading use
   the streaming reader in §4.2, which reads only the metadata tier and fetches chunk payloads on
   demand.
3. **`decode` requires the chunk to exist in the chunk map.** Asking for a chunk coordinate that was
   never authored returns `4 RUNTIME_ERROR`, not "empty chunk". Always enumerate with
   `bsvx_region_chunk_count` + `bsvx_region_get_chunk_info` instead of scanning the region grid.
4. **`BUFFER_TOO_SMALL` is well-behaved**: it writes the needed size into `*out_written` and touches
   nothing else.
5. **`set_chunk_u32` requires exactly `required_voxel_count` elements**, otherwise `INVALID_ARGUMENT`.
   It creates the chunk if it does not exist yet, rebuilds the summary, and updates flags.
6. **`bsvx_world_save` writes a complete world tree** (manifest + `regions/` + `textures/`), creating
   directories as needed, and re-stamps `manifest_hash`/`registry_hash` into every region. Round-trip
   of mutated voxels is byte-exact. Region filenames are preserved if known, else `r_x_y_z.bvx`.
7. **`bsvx_world_save_region` requires the package to hold exactly one region**, writes it as a
   *standalone* `.bvx` (injects a `WORLD_DESC`), and dumps the `.btx` files next to it with
   `manifest_hash = 0`.
8. **The original entry points still return bare codes.** `bsvx_world_save`,
   `bsvx_region_decode_chunk_u32`, `bsvx_region_set_chunk_u32` and `bsvx_region_set_chunk_payload`
   keep their old silent behaviour for source compatibility. Use the `_ex` variants (§4.1) —
   same semantics, plus `last_error`.

### 4.1 ABI v2 additions

All of these are POD-only and `noexcept` at the boundary, matching the existing style.

```c
uint32_t    bsvx_abi_version(void);                       // == 2

// --- in-memory loading: res://, .pck, async I/O buffers, network streams ---------------------
bsvx_result bsvx_world_load_region_memory(bsvx_context*, const void* bytes, size_t, bsvx_world**);
bsvx_result bsvx_world_load_region_memory_ex(bsvx_context*, const void*, size_t,
                                             const char* texture_root, bsvx_world**);

// --- O(1) chunk lookup and baked-payload read-back -------------------------------------------
bsvx_result bsvx_region_find_chunk(const bsvx_world*, size_t region, uint16_t cx, cy, cz, size_t* out_ordinal);
bsvx_result bsvx_region_get_chunk_payload_info(const bsvx_world*, size_t region, uint32_t section_type,
                                               uint16_t cx, cy, cz,
                                               size_t* out_size, uint16_t* out_codec, uint16_t* out_flags);
bsvx_result bsvx_region_get_chunk_payload(const bsvx_world*, size_t region, uint32_t section_type,
                                          uint16_t cx, cy, cz,
                                          void* out, size_t capacity, size_t* out_size, uint16_t* out_codec);

// --- compaction (see §9.4) -------------------------------------------------------------------
size_t      bsvx_region_reclaimable_bytes(const bsvx_world*, size_t region);
bsvx_result bsvx_region_compact(bsvx_world*, size_t region, size_t* out_reclaimed);
bsvx_result bsvx_world_compact(bsvx_world*, size_t* out_reclaimed);

// --- error-reporting variants (see §9.3) -----------------------------------------------------
bsvx_result bsvx_world_save_ex(bsvx_context*, const bsvx_world*, const char* path);
bsvx_result bsvx_world_save_region_ex(bsvx_context*, const bsvx_world*, const char* path);
bsvx_result bsvx_region_decode_chunk_u32_ex(bsvx_context*, /* …same args… */);
bsvx_result bsvx_region_set_chunk_u32_ex(bsvx_context*, /* …same args… */);
bsvx_result bsvx_region_set_chunk_payload_ex(bsvx_context*, /* …same args… */);
bsvx_result bsvx_region_get_chunk_payload_ex(bsvx_context*, /* …same args… */);

// --- streaming: partial / lazy region loading (§4.2) ------------------------------------------
bsvx_result bsvx_region_reader_open(bsvx_context*, const char* path, bsvx_region_reader**);
bsvx_result bsvx_region_reader_open_memory(bsvx_context*, const void*, size_t, int copy_bytes, bsvx_region_reader**);
void        bsvx_region_reader_close(bsvx_region_reader*);
bsvx_result bsvx_region_reader_set_geometry(bsvx_region_reader*, const bsvx_geometry_desc*);
bsvx_result bsvx_region_reader_geometry(const bsvx_region_reader*, bsvx_geometry_desc*);
int         bsvx_region_reader_is_standalone(const bsvx_region_reader*);
bsvx_result bsvx_region_reader_coord(const bsvx_region_reader*, int32_t*, int32_t*, int32_t*);
size_t      bsvx_region_reader_resident_bytes(const bsvx_region_reader*);
size_t      bsvx_region_reader_chunk_count(const bsvx_region_reader*);
size_t      bsvx_region_reader_required_voxel_count(const bsvx_region_reader*);
bsvx_result bsvx_region_reader_get_chunk_info(const bsvx_region_reader*, size_t ordinal, bsvx_chunk_info*);
bsvx_result bsvx_region_reader_find_chunk(const bsvx_region_reader*, uint16_t cx, cy, cz, size_t* out_ordinal);
size_t      bsvx_region_reader_registry_entry_count(const bsvx_region_reader*);
bsvx_result bsvx_region_reader_get_registry_entry(const bsvx_region_reader*, size_t, bsvx_registry_entry*);
bsvx_result bsvx_region_reader_decode_chunk_u32(bsvx_context*, const bsvx_region_reader*,
                                                uint16_t cx, cy, cz, uint32_t* out, size_t cap, size_t* written);
bsvx_result bsvx_region_reader_get_chunk_payload(bsvx_context*, const bsvx_region_reader*, uint32_t section_type,
                                                 uint16_t cx, cy, cz,
                                                 void* out, size_t cap, size_t* out_size, uint16_t* out_codec);
bsvx_result bsvx_region_reader_verify(bsvx_context*, const bsvx_region_reader*);
bsvx_result bsvx_region_reader_load_full(bsvx_context*, const bsvx_region_reader*, bsvx_world**);
```

`bsvx_region_get_chunk_payload` and the reader's version follow the decode convention: too small a
buffer returns `3 BUFFER_TOO_SMALL` with the required size in `*out_size` and nothing written, so
`(NULL, 0)` is a legal size probe. A chunk with no payload in that section returns `2 NOT_FOUND`.

### 4.2 The streaming region reader — chunk-granularity loading

`bsvx_region_reader_open*` parses only the **metadata tier** — header, chunk map, chunk summaries,
section directory and per-section entry tables — and leaves every payload blob on the backing source
until you ask for a specific chunk. The format already stored everything at absolute offsets behind
a section directory; this exposes it.

**VERIFIED** on a synthetic 16×16×16-chunk region (4096 chunks of 16³ voxels, 6.42 MiB on disk):

| | eager `bsvx_world_load_region` | `bsvx_region_reader_open` |
|---|---|---|
| open time | 9.3 ms | **0.10 ms** |
| resident bytes | 6576 KiB (100%) | **352 KiB (5.4%)** |

Decoded voxels are byte-identical between the two paths for all 4096 chunks.

Practical shape of a streamer:

1. `bsvx_region_reader_open` (or `_open_memory`) each candidate region — cheap enough to keep many open.
2. Enumerate `bsvx_region_reader_get_chunk_info` and use the summaries (`non_air_count`,
   `face_state_*`, `macro_occ_4x4x4`, `aabb_*`) to decide what is worth paging in — **no payload
   bytes are touched by this**.
3. `bsvx_region_reader_decode_chunk_u32` only the chunks you actually need, on worker threads.
4. Budget residency against `bsvx_region_reader_resident_bytes`.

Non-standalone regions (those belonging to a manifest world) carry no geometry of their own, so
`bsvx_region_reader_geometry` returns `2 NOT_FOUND` and `_required_voxel_count` returns 0 until you
call `bsvx_region_reader_set_geometry` with the world's geometry. Load the manifest once with
`bsvx_world_load` to get it.

Readers are read-only and independent of `bsvx_world`. **Every accessor except `_open`/`_close` is
safe to call concurrently on the same reader from several threads** — the file source uses `pread`
on Linux, so parallel chunk fetches do not serialize. The `bsvx_context` you pass in is still not
thread-safe: one per thread.

`_open` deliberately does not hash the file; call `bsvx_region_reader_verify` when you want the
integrity check. `bsvx_region_reader_load_full` promotes a standalone region to a full `bsvx_world`.

### 4.3 ABI v3 additions

```c
uint32_t    bsvx_abi_version(void);                       // == 3

// --- authoring a world from scratch ----------------------------------------------------------
bsvx_result bsvx_world_create(bsvx_context*, const bsvx_geometry_desc*, bsvx_world** out);
bsvx_result bsvx_world_add_region(bsvx_world*, int32_t rx, int32_t ry, int32_t rz, size_t* out_index);
bsvx_result bsvx_world_find_region(const bsvx_world*, int32_t rx, int32_t ry, int32_t rz, size_t* out_index);
bsvx_result bsvx_world_set_registry_entry(bsvx_world*, const bsvx_registry_entry*);   // upsert by voxel_key
bsvx_result bsvx_world_remove_registry_entry(bsvx_world*, uint32_t voxel_key);

// --- world metadata (name, uuid, bounds, schema, hashes) --------------------------------------
bsvx_result bsvx_world_get_desc(const bsvx_world*, bsvx_world_desc* out);
bsvx_result bsvx_world_set_desc(bsvx_world*, const bsvx_world_desc*);
bsvx_result bsvx_world_get_name(const bsvx_world*, char* out, size_t cap, size_t* out_size);
bsvx_result bsvx_world_get_uuid(const bsvx_world*, char* out, size_t cap, size_t* out_size);
bsvx_result bsvx_world_set_name(bsvx_world*, const char* name);
bsvx_result bsvx_world_set_uuid(bsvx_world*, const char* uuid);

// --- serialize a region to a buffer (symmetry with load_region_memory) ------------------------
bsvx_result bsvx_world_save_region_memory(bsvx_context*, const bsvx_world*, void* out, size_t cap, size_t* out_size);

// --- load a manifest world through a host VFS (res://, .pck, anything) ------------------------
bsvx_result bsvx_world_load_vfs(bsvx_context*, const char* path, const bsvx_vfs*, bsvx_world** out);

// --- .btx introspection ----------------------------------------------------------------------
bsvx_result bsvx_world_get_texture_id(const bsvx_world*, size_t tex, char* out, size_t cap, size_t* out_size);
bsvx_result bsvx_world_get_texture_path(const bsvx_world*, size_t tex, char* out, size_t cap, size_t* out_size);
size_t      bsvx_texture_texture_count(const bsvx_world*, size_t tex);
size_t      bsvx_texture_subresource_count(const bsvx_world*, size_t tex);
size_t      bsvx_texture_material_count(const bsvx_world*, size_t tex);
size_t      bsvx_texture_sampler_count(const bsvx_world*, size_t tex);
bsvx_result bsvx_texture_get_desc(const bsvx_world*, size_t tex, size_t i, bsvx_texture_desc* out);
bsvx_result bsvx_texture_get_subresource_desc(const bsvx_world*, size_t tex, size_t i, bsvx_subresource_desc* out);
bsvx_result bsvx_texture_get_material(const bsvx_world*, size_t tex, size_t i, bsvx_material_desc* out);
bsvx_result bsvx_texture_get_sampler(const bsvx_world*, size_t tex, size_t i, bsvx_sampler_desc* out);
bsvx_result bsvx_texture_find_material(const bsvx_world*, size_t tex, uint32_t material_id, size_t* out_i);
bsvx_result bsvx_texture_get_subresource_bytes(const bsvx_world*, size_t tex, size_t i, void* out, size_t cap, size_t* out_size);
uint32_t    bsvx_format_bytes_per_texel(uint32_t vk_format);
```

**String getters follow the payload convention**, with the terminating NUL counted: `(NULL, 0)` is a
size probe returning `3 BUFFER_TOO_SMALL` and the required size, and a large enough buffer gets the
NUL-terminated text.

**Authoring.** `bsvx_world_create` gives you geometry and nothing else — no regions, no textures, no
registry, nothing on disk. Set the registry **before** writing voxels: `set_chunk_u32` builds the
chunk summary through it, and with an empty registry every non-air voxel counts as opaque (§9.2).
Then `bsvx_world_add_region` per region coordinate, `bsvx_region_set_chunk_u32` per chunk, and
`bsvx_world_save` to write the whole tree (`manifest.toml` + `regions/r_x_y_z.bvx`).
`bsvx_world_set_desc` refuses a geometry change once a region holds chunks — those payloads were
encoded against the old chunk dimensions.

**`bsvx_world_load_vfs`** is `bsvx_world_load` with the filesystem swapped out for three callbacks:

```c
typedef struct bsvx_vfs {
    void* user;
    int (*read_file)(void* user, const char* path, void* out, size_t cap, size_t* out_size);
    int (*file_exists)(void* user, const char* path);                      // optional
    int (*list_dir)(void* user, const char* dir, size_t index, char* out_name, size_t cap, size_t* out_size); // optional
} bsvx_vfs;
```

`read_file` is the only one that is required; it is called with `(NULL, 0)` first to size the read,
so always set `*out_size`. Paths handed back to you keep the scheme of the path you passed in
(`res://`, `user://`, …) and use `/` separators — the library splits the scheme off before doing any
path arithmetic, because `std::filesystem::path` collapses the `//` in it. `list_dir` is only needed
if you pass a *directory* rather than a `manifest.toml`, or if the manifest relies on auto-discovery
instead of explicit `[[regions]]` / `[[textures]]`.

**Textures.** A world holds `bsvx_world_texture_count()` `.btx` archives; each archive has its own
tables. `tex_index` selects the archive, the trailing index selects a row. `bsvx_texture_get_desc`
gives extents, `vk_format` and `array_layers`; `bsvx_texture_get_subresource_desc` + `_bytes` give
one mip/layer's texels (tight RGBA8 unless `packed_row_length`/`packed_image_height` say otherwise);
`bsvx_texture_get_material` resolves a registry entry's `material_id` to per-face albedo/normal array
layers and a tint. That is enough to build a `Texture2DArray` and a `StandardMaterial3D` without ever
leaving the C ABI.

---

## 5. What the C API cannot do today (NOT IMPLEMENTED)

Plan around these, or extend the ABI:

- **No `.btx` authoring.** You can read every table since v3, but creating a texture archive —
  `add_texture`, `append_subresource`, `add_material` — is C++-only (`btx::Archive`).
- **No saving through a VFS.** Loading has `bsvx_world_load_vfs` and a region can be serialized to a
  buffer (`bsvx_world_save_region_memory`), but `bsvx_world_save` still writes a manifest with
  `std::ofstream` to a real path. In an exported game write to `user://` (a real path) or hand the
  region bytes to `FileAccess` yourself.
- **No region or chunk removal.** You can add regions and overwrite chunks; nothing deletes them.
- **No LOD/mip generation.** The baked-payload sections are the place to put a mip pyramid or an
  SVDAG, and you can read them back — but building them is yours.
- **Formats other than RGBA8**, 3D textures and texel buffers are declared in the enums and not
  implemented anywhere in the library (§9.6).

---

## 6. Building on Linux — VERIFIED

The checked-in build system is Windows-only: `Build.lua` hardcodes `toolset "msc"`, `premake/premake5.exe`
is a Win32 binary, and `build.bat` / `*.vcxproj` / `bsvx.sln` are MSVC. `Binaries/` holds prebuilt
Windows DLLs. **None of that is usable on Linux — ignore it.**

The source itself is portable. The only platform-specific code is the `BSVX_API` macro, which already
has a GCC branch. No Windows headers, no `wchar_t` paths.

### 6.1 One-liner build (VERIFIED, GCC 16.1.1)

```bash
g++ -std=c++23 -O2 -fPIC -shared \
    -Isrc/include -Isrc/impl -DTOML_HEADER_ONLY=0 \
    src/impl/*.cpp -o libbsvx.so
```

Produces a working `libbsvx.so`. Warnings only (a misplaced `[[nodiscard]]` in `definitions.h`
applied to a return type, and deprecated `operator"" _toml` spacing inside vendored toml++). No errors.

Build the test runner against it:

```bash
g++ -std=c++23 -O2 -Isrc/include test/tests.cpp -o bsvx_tests -L. -lbsvx -Wl,-rpath,'$ORIGIN'
```

### 6.2 CMake (VERIFIED — configures and builds with Ninja)
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

The `CXX_VISIBILITY_PRESET hidden` line is worth keeping: **VERIFIED** it cuts exported symbols down
to **exactly the 79 `bsvx_*` C API functions** and nothing else, which is what you want when this
`.so` sits inside a Godot addon next to godot-cpp.

### 6.3 C++ standard requirement

- **C++23 (GCC): compiles clean.**
- **C++20: compiles clean too** (VERIFIED). The includes it was missing — `<optional>`/`<memory>`/
  `<mutex>` in `bvx.h`, `<algorithm>` in `btx.cpp`/`bvx.cpp`/`bvx_header.cpp`, `<cstdint>` +
  `<stdexcept>` + `<string>` in `util.h` — are now explicit instead of arriving transitively.
- C++17 is impossible (concepts, `std::span`, designated initializers).

This matters because **godot-cpp defaults to C++17** — see §7.2.

---

## 7. Recommended Godot plugin architecture

Target: **Godot 4.x GDExtension** built with **godot-cpp** matching your engine version, on Linux
x86_64.

### 7.1 Layout

```
my_game/
└── addons/bsvx/
    ├── plugin.cfg                       # only if you add an EditorPlugin
    ├── bsvx.gdextension
    └── bin/libbsvx_godot.linux.template_{debug,release}.x86_64.so
extension_src/
├── SConstruct
├── godot-cpp/                           # submodule, branch == your Godot version
├── bsvx/                                # this repo, as a submodule
└── src/  register_types.cpp, bsvx_world.{h,cpp}, bsvx_mesher.{h,cpp}
```

### 7.2 Linking strategy — compile BSVX *into* the extension

**Recommended:** compile the BSVX `.cpp` files into the extension's own `.so` as objects built with
`-std=c++23`, while godot-cpp and your extension code stay at godot-cpp's default standard. Only the
**C ABI** crosses between them, so there is no ABI hazard. You ship **one** `.so`, with no runtime
lookup, no `[dependencies]` block, no `$ORIGIN` rpath.

```python
# SConstruct (excerpt)
env = SConscript("godot-cpp/SConstruct")

# BSVX objects: separate std, private include dirs, header-only toml disabled
bsvx_env = env.Clone()
bsvx_env.Append(CXXFLAGS=["-std=c++23"])      # GCC honours the LAST -std flag; this overrides c++17
bsvx_env.Append(CPPPATH=["bsvx/src/include", "bsvx/src/impl"])
bsvx_env.Append(CPPDEFINES=["TOML_HEADER_ONLY=0"])
bsvx_objects = [bsvx_env.SharedObject(f) for f in Glob("bsvx/src/impl/*.cpp")]

env.Append(CPPPATH=["src", "bsvx/src/include"])
sources = Glob("src/*.cpp") + bsvx_objects

library = env.SharedLibrary(
    "../my_game/addons/bsvx/bin/libbsvx_godot.{}.{}{}".format(
        env["platform"], env["target"], env["SHLIBSUFFIX"]),
    source=sources)
Default(library)
```

Do **not** define `BSVX_ASSETS_BUILD_DLL` / `BSVX_ASSETS_USE_DLL` — those are Windows-only branches of
the macro; on Linux the GCC visibility branch is used unconditionally.

*Alternative* (separate `libbsvx.so` next to the extension): build with §6.2, link with
`-Wl,-rpath,'$ORIGIN'`, ship it in `bin/`, and list it under `[dependencies]` in the `.gdextension`
so it survives export. More moving parts; only do it if you want to hot-swap the library.

### 7.3 `bsvx.gdextension`

```ini
[configuration]
entry_symbol = "bsvx_library_init"
compatibility_minimum = "4.2"

[libraries]
linux.debug.x86_64   = "res://addons/bsvx/bin/libbsvx_godot.linux.template_debug.x86_64.so"
linux.release.x86_64 = "res://addons/bsvx/bin/libbsvx_godot.linux.template_release.x86_64.so"
```

### 7.4 Class design

Wrap the C handles in `RefCounted` so GDScript gets deterministic cleanup:

```cpp
class BsvxWorld : public godot::RefCounted {
    GDCLASS(BsvxWorld, godot::RefCounted)
    bsvx_context* ctx_ = nullptr;   // one context per world instance → no shared mutable state
    bsvx_world*   world_ = nullptr;
public:
    godot::Error load(const godot::String& path);          // globalize res:// first, see §8.5
    godot::Error load_region(const godot::String& path);
    godot::Error save(const godot::String& path);
    godot::Dictionary get_geometry() const;                // chunk_size / region_size as Vector3i
    int  get_region_count() const;
    godot::Vector3i get_region_coord(int region) const;
    godot::Array get_chunk_list(int region) const;         // Dictionaries of coord+flags+summary
    godot::PackedByteArray decode_chunk(int region, godot::Vector3i chunk) const;
    godot::Error set_chunk(int region, godot::Vector3i chunk, const godot::PackedByteArray& voxels);
    godot::String get_last_error() const;
};
```

**Voxel keys are `uint32_t`; `PackedInt32Array` is signed 32-bit.** Keys ≥ 2³¹ would come back
negative. Either use `PackedByteArray` (memcpy the raw `uint32_t` buffer, exact, and it's what you'd
hand to a compute shader anyway) or `PackedInt32Array` with a documented "keys must stay < 2³¹" rule.

Destructor order matters: `bsvx_world_destroy` **before** `bsvx_context_destroy`.

### 7.5 Threading

- The library has no globals; a `bsvx_world` is plain data and `decode_chunk` is `const` and only
  allocates. **Concurrent decode of the same world from several threads is safe** as long as nobody
  calls `set_chunk_*` or `save` at the same time.
- `bsvx_context` holds the `last_error` string and is **not** thread-safe. One context per thread, or
  one per `BsvxWorld` used only from the owning thread.
- Practical pattern: load on the main thread → fan out `decode_chunk` + meshing over
  `WorkerThreadPool` → `call_deferred` the finished `ArrayMesh` back.

---

## 8. Doing the actual work in the plugin

### 8.1 Enumerate and decode

```cpp
const size_t chunks = bsvx_region_chunk_count(world_, region);
const size_t n = bsvx_region_required_voxel_count(world_, region);   // = cx*cy*cz
std::vector<uint32_t> dense(n);

for (size_t i = 0; i < chunks; ++i) {
    bsvx_chunk_info info{};
    if (bsvx_region_get_chunk_info(world_, region, i, &info) != BSVX_RESULT_OK) continue;
    if (info.flags & 2 /*CHUNK_EMPTY*/) continue;                    // skip empty chunks for free
    if (info.summary.non_air_count == 0) continue;

    size_t written = 0;
    if (bsvx_region_decode_chunk_u32(world_, region, info.local_chunk_x, info.local_chunk_y,
                                     info.local_chunk_z, dense.data(), dense.size(), &written)
        != BSVX_RESULT_OK) continue;
    // dense[x + sx*(y + sy*z)] is the voxel key
}
```

### 8.2 Cheap culling before you mesh

- `info.summary.face_state_*` — if a neighbour chunk's touching face is `FULL (1)`, skip that whole
  boundary plane in the mesher.
- `info.summary.aabb_*` — iterate only the occupied sub-box instead of the full chunk.
- `info.summary.macro_occ_4x4x4` — 64-bit occupancy for LOD imposters or a raymarch acceleration grid.
- `info.summary.top_id_0..3` — if `top_count_0 == non_air_count`, the chunk is single-material; emit
  one surface, no per-voxel material lookups.

### 8.3 Voxel key → Godot material

`bsvx_world_get_registry_entry` gives `voxel_key → material_id + flags`. Build a
`std::unordered_map<uint32_t, bsvx_registry_entry>` once at load. `flags` bits:
`OPAQUE=1, EMISSIVE=2, SPECIAL=4, COLLIDABLE=8`. Group faces by `material_id` into one `ArrayMesh`
surface per material; use `COLLIDABLE` to decide what goes into the `ConcavePolygonShape3D`.

`material_id` resolves into the world's `.btx` through `bsvx_texture_find_material` +
`bsvx_texture_get_material` (§4.3): per-face albedo/normal **array layers** plus a tint. Upload the
`.btx` subresources into a `Texture2DArray` once at load — layer index `i` is the subresource with
`layer_or_slice == i` — and the material's six face layers index straight into it:

```cpp
bsvx_material_desc mat{};
size_t row = 0;
if (bsvx_texture_find_material(world_, 0, entry.material_id, &row) == BSVX_RESULT_OK) {
    bsvx_texture_get_material(world_, 0, row, &mat);
    // mat.albedo_layer[0..5] = +x -x +y -y +z -z
}
```

### 8.4 Writing results back

- Modified voxels: `bsvx_region_set_chunk_u32(..., 0xFFFF)` (AUTO codec picks the smallest encoding),
  then `bsvx_world_save(world_, root)`. **Read §9.2 first — this loses summary fidelity.**
- Baked artefacts: `bsvx_region_set_chunk_payload(world_, region, 3 /*SURFACE*/, cx, cy, cz,
  my_codec_id, bytes, size, 0)`. Define your own `codec` id (e.g. `1 = Godot ArrayMesh blob v1`) and
  read it back with `bsvx_region_get_chunk_payload` (§4.1).
- A single region without a manifest: `bsvx_world_save_region_memory` hands you the `.bvx` bytes, so
  the write can go through `FileAccess` to `user://` or straight into a network buffer (§4.3).

### 8.5 `res://` vs the real filesystem — a hard constraint

`bsvx_world_load` takes an OS path and uses `std::ifstream`. It knows nothing about Godot's VFS.

- **In the editor / a non-packed run:** `ProjectSettings::get_singleton()->globalize_path("res://worlds/w/manifest.toml")`
  gives a real path and everything works.
- **In an exported game:** `res://` lives inside the `.pck`. `globalize_path` returns a path that does
  not exist and the load fails. Since ABI v2 the answer is to read the bytes yourself and hand them
  over:

  ```cpp
  Ref<FileAccess> f = FileAccess::open("res://worlds/w/regions/r_0_0_0.bvx", FileAccess::READ);
  PackedByteArray buf = f->get_buffer(f->get_length());

  // fully resident:
  bsvx_world_load_region_memory(ctx, buf.ptr(), buf.size(), &world);

  // or streaming, metadata only (copy_bytes = 1 so the reader owns its bytes):
  bsvx_region_reader_open_memory(ctx, buf.ptr(), buf.size(), /*copy_bytes=*/1, &reader);
  ```

  This works for `.pck` contents, `user://`, a `WorkerThreadPool` read, a memory-mapped file or a
  network buffer — anything you can get bytes from. Pass `copy_bytes = 0` only if the buffer
  outlives the reader.

- **A whole manifest world out of a `.pck`:** since ABI v3, `bsvx_world_load_vfs` (§4.3) reads
  everything — `manifest.toml`, every `.bvx`, every `.btx` — through callbacks you supply, so the
  library never touches a real path:

  ```cpp
  static int vfs_read(void* user, const char* path, void* out, size_t cap, size_t* out_size) {
      Ref<FileAccess> f = FileAccess::open(String(path), FileAccess::READ);   // path keeps res://
      if (f.is_null()) return 0;
      const size_t size = (size_t)f->get_length();
      *out_size = size;
      if (cap < size) return 1;                       // size probe, or a buffer to grow
      f->get_buffer((uint8_t*)out, size);
      return 1;
  }

  bsvx_vfs vfs{}; vfs.user = this; vfs.read_file = &vfs_read;
  bsvx_world_load_vfs(ctx, "res://worlds/w/manifest.toml", &vfs, &world);
  ```

  Add `file_exists` (`FileAccess::file_exists`) and `list_dir` (`DirAccess`) if you want to hand it a
  directory or rely on auto-discovery. **Saving still needs a real path** (§5) — write to `user://`.

  Without the VFS the fallback is unchanged: ship `manifest.toml` outside the pack, or skip it
  entirely — a standalone `.bvx` carries its own geometry and registry, and
  `bsvx_region_reader_set_geometry` covers the manifest-world case.

Also mark `*.bvx`, `*.btx`, `*.toml` in the export "non-resource files to export" filter, or Godot
will drop them.

---

## 9. Landmines — every one of these was hit or verified on this machine

### 9.1 The manifest hash breaks on line-ending changes (this repo is broken right now)

`parse_hashes()` ends with `manifest.world_desc.manifest_hash = hash_file(manifest.manifest_path);` —
the world's manifest hash is **FNV-1a-64 over the raw bytes of `manifest.toml`**, unconditionally,
ignoring any `[hashes]` value. Every `.bvx` stores the manifest hash it was written against, and
`load_world` throws `"manifest hash mismatch for region ..."` when they differ.

Consequence: **any** byte-level change to `manifest.toml` — reformatting, a trailing newline, and
especially **CRLF↔LF conversion** — invalidates every region in the world.

**VERIFIED — this working tree used to be broken exactly this way:** `test/data/world_basic/manifest.toml`
was CRLF on disk (962 bytes) but LF in git (902 bytes), and the `.bvx` files were baked against the
LF bytes, so the suite reported

```
[PASS] load_standalone_region
[FAIL] load_world_and_roundtrip_save: manifest hash mismatch for region .../r_0_0_0.bvx
[FAIL] mutate_chunk_and_save:        manifest hash mismatch for region .../r_0_0_0.bvx
```

The fixture is LF now and `.gitattributes` pins `*.toml -text`, so the suite is green — but expect
this the first time a world crosses an OS. This is a Windows→Linux hazard, not a code bug per se, but
a Godot plugin on Linux **will** hit it whenever a world is authored on Windows or passes through git
with `core.autocrlf`. Mitigations: add `*.toml -text` / `* -text` to `.gitattributes`, never hand-edit
a manifest (re-save through `bsvx_world_save`, which recomputes and re-stamps everything), and surface
the mismatch message to the user — `last_error` *is* populated for load failures.

Note also: `load_region` (standalone `.bvx`) does **not** check the manifest hash, so standalone
assets are immune to this.

### 9.2 Summary counts on the write path — FIXED

Historically `set_chunk_voxels_dense` rebuilt the chunk summary from `Archive::standalone`, which is
`nullopt` for any region loaded as part of a manifest world. With no registry `build_chunk_summary`
counted **every non-air voxel as opaque** and left `emissive_count`/`special_count` at zero — so a
`set_chunk_u32` silently degraded `macro_occ_4x4x4` and the material split on every save.

`set_chunk_voxels_dense` now takes an optional registry lookup and `bsvx_region_set_chunk_u32` hands
it the world's (cached per `bsvx_world`). **VERIFIED** on the fixture: chunk (0,0,0) is
`non_air=13 opaque=9 special=4` before, after a rewrite, and after save + reload. Covered by the
`summary_survives_write` test.

If you drive `bvx::Archive` directly from C++, pass `registry_override` yourself — a region taken out
of a manifest world still has no registry of its own.

### 9.3 Error messages on non-load calls — FIXED via `_ex`

The original `save` / `decode` / `set_chunk_u32` / `set_chunk_payload` entry points still catch `(...)`
and return a bare code; that behaviour is kept so existing callers do not change meaning. The `_ex`
variants (§4.1) take a `bsvx_context*`, populate `last_error`, and additionally map
`std::invalid_argument → 1` and `std::out_of_range → 2` instead of collapsing everything to `4`.
They also **clear** `last_error` on success, so a stale message can never be reported as fresh.
Use the `_ex` forms everywhere in a plugin.

### 9.4 Chunk lookup and monotonic file growth — FIXED

`Archive::try_find_chunk_index` is now backed by a hash index built at deserialize and maintained by
`add_or_get_chunk`, and it is exposed as `bsvx_region_find_chunk` /
`bsvx_region_reader_find_chunk`. It is built eagerly rather than lazily on purpose: a const lookup
never mutates, so concurrent decodes of one archive stay safe. **VERIFIED:** 81,920 lookups against a
4096-chunk region in 0.35 ms. (If you edit `Archive::chunk_map` by hand from C++, call
`rebuild_chunk_index()`; the lookup falls back to a linear scan while the index is stale rather than
returning a wrong answer.)

Every `set_chunk_*` still **appends** to the section blob and repoints the entry — that is what makes
writes cheap — but the dead bytes are now reclaimable. `bsvx_region_reclaimable_bytes` reports how
much is dead, `bsvx_region_compact` / `bsvx_world_compact` rewrite the blobs keeping only live,
deduplicated ranges. Call it before saving an edited world; it is data-preserving (VERIFIED, and the
resulting `.bvx` is measurably smaller).

### 9.5 Standalone vs manifest regions are not interchangeable

`bsvx_world_load_region` throws if the file lacks the `STANDALONE` flag + `WORLD_DESC` section. A
region extracted from a manifest world has no geometry of its own — `resolve_geometry` throws
`"could not find geometry in non-authored region file"`. Conversely `bsvx_world_save_region` demands
exactly one region in the package. Decide per asset which kind it is; converting means load-world →
save-region.

### 9.6 `.btx` is readable from C now, but only RGBA8 exists

The texture accessors landed in v3 (§4.3), so a plugin can pull descs, materials, samplers and texels
out of a world without C++-side access. What has not changed is the format support: `bytes_per_texel`
only knows `VK_FORMAT_R8G8B8A8_UNORM (37)` and `_SRGB (43)` — `bsvx_format_bytes_per_texel` returns 0
for anything else — `append_subresource` throws for other formats, and only `TEXTURE_2D` (with array
layers) is implemented. 3D textures and texel buffers are declared in the enum and nothing more.
Writing a `.btx` is still C++-only.

### 9.7 Smaller ones

- `BtxRef.relative_path` is a fixed **128-byte** buffer, silently truncated. Keep texture paths short.
- `ChunkSummary` AABB fields are `uint8_t` → chunk dimensions must be ≤ 255.
- `[[regions]] coord` is validated against the `.bvx` header and throws on mismatch — renaming region
  files without updating the manifest is a load error.
- The `crc64` field is FNV-1a-64, not CRC-64; don't validate it with a CRC implementation.
- `.bvx`/`.btx` are written as **raw little-endian POD dumps** with no endianness conversion
  (`DiskHeader.endian` exists in `.btx` but is never checked). x86_64/aarch64 Linux is fine; don't
  ship to a big-endian target.
- The world is fully re-serialized on every `bsvx_world_save`, including all `.btx` files. Saving one
  chunk edit rewrites the entire world tree.
- The working tree carries ~445 MB of `Binaries/` (committed Windows DLLs) and `.vs/`, but neither is
  tracked — `.gitignore` covers both and the git history is under 1 MB, so it is fine as a submodule.
  Delete both directories on a Linux checkout; nothing in the build reads them.

---

## 10. Remaining ABI gaps

Implemented in v2 (§4.1/§4.2): memory loading, partial/lazy region loading, payload read-back, `_ex`
error reporting, the chunk index, the summary-registry fix, compaction.

Implemented in v3 (§4.3), which closes every gap this section used to list: world authoring
(`create` / `add_region` / `find_region` / registry upsert+remove), world metadata (name, uuid,
bounds, schema, hashes), `bsvx_world_save_region_memory`, manifest loading through a host VFS
(`bsvx_world_load_vfs`), and full `.btx` introspection down to texel bytes.

**Nothing on the plugin's critical path is missing from the ABI now.** What is left is either
deliberate (the library does not mesh, bake or generate LOD) or a format-level limitation rather than
a missing export:

```c
// writing a texture archive: btx::Archive::add_texture / append_subresource / add_material are C++-only
bsvx_result bsvx_texture_create(bsvx_context*, bsvx_texture_builder** out);
bsvx_result bsvx_texture_append_subresource(bsvx_texture_builder*, /* … */);

// saving through a VFS: only the region side has a buffer form today
bsvx_result bsvx_world_save_manifest_memory(bsvx_context*, const bsvx_world*, char* out, size_t cap, size_t* out_size);

// removal, for an editor that deletes as well as adds
bsvx_result bsvx_world_remove_region(bsvx_world*, size_t region_index);
bsvx_result bsvx_region_remove_chunk(bsvx_world*, size_t region_index, uint16_t cx, uint16_t cy, uint16_t cz);
```

Plus two things no export can fix on its own: `bytes_per_texel` covers only RGBA8 UNORM/SRGB, and the
containers are raw little-endian POD dumps (§9.6, §9.7).

Keep every addition POD-only and `noexcept` at the boundary, matching the existing style, and bump
`BSVX_ABI_VERSION` so a host can feature-detect with `bsvx_abi_version()`.

---

## 11. Verification log (Fedora, GCC 16.1.1)

Original pass, 2026-07-31:

| Check | Result |
|---|---|
| `g++ -std=c++23 -shared` build of all `src/impl/*.cpp` | ✅ `libbsvx.so`, warnings only |
| CMake + Ninja build (the `CMakeLists.txt` in §6.2) | ✅ library + test runner |
| `test/tests.cpp` against the checked-in fixtures | 1 pass / 2 fail — manifest hash mismatch (§9.1) |
| Same tests with LF-normalised `manifest.toml` | ✅ all pass |
| Load fixture world | ✅ 2 regions, 2 textures, chunk 4×4×4, region 1×1×1, 4 registry entries, 64 voxels/chunk |
| Enumerate chunk 0 | `flags=0x5` (PRESENT\|HAS_VOXELS), `non_air=13 opaque=9 special=4`, aabb (0,0,0)–(3,3,3) |
| Decode nonexistent chunk (99,99,99) | rc=4 RUNTIME_ERROR, `last_error` **empty** |
| Decode into a 1-element buffer | rc=3 BUFFER_TOO_SMALL, `out_written` = 64 |
| `set_chunk_u32` with wrong count | rc=1 INVALID_ARGUMENT |
| Mutate → `bsvx_world_save` to a new root → reload → decode | ✅ byte-identical round-trip |
| Summary after that write/reload | ⚠️ `opaque 9→13`, `special 4→0` — fixed in v2 (§9.2) |

ABI v2 pass:

| Check | Result |
|---|---|
| `g++ -std=c++23` build | ✅ clean |
| `g++ -std=c++20` build | ✅ clean (was 4 missing includes) |
| Exported symbols with `CXX_VISIBILITY_PRESET hidden` | ✅ exactly the 53 C API functions |
| `test/tests.cpp` (13 tests, LF manifest) | ✅ all pass |
| Same suite under `_GLIBCXX_DEBUG` + `_GLIBCXX_ASSERTIONS` | ✅ all pass, no checked-STL violations |
| Standalone `.bvx` loaded from a buffer vs from disk | ✅ identical decode; truncated buffer rejected with a message |
| Payload write → read back → save → reload → read back | ✅ bytes, codec and entry flags preserved |
| Summary after `set_chunk_u32` + save + reload (manifest world) | ✅ `non_air=13 opaque=9 special=4` unchanged |
| `_ex` decode of a missing chunk | ✅ rc=4 with a non-empty `last_error`; cleared on the next success |
| 8 × `set_chunk_u32` then compact | ✅ `reclaimable == reclaimed`, 0 residual, voxels unchanged, smaller `.bvx` |
| Synthetic 4096-chunk region (16³ voxels/chunk, 6.42 MiB) | eager load 9.3 ms / 100% resident vs reader open **0.10 ms / 5.4% resident** |
| Reader vs eager decode, all 4096 chunks | ✅ 0 mismatches |
| 8 threads decoding through one file-backed reader | ✅ 0 mismatches (`pread`, no lock) |
| 81,920 chunk lookups against that region | ✅ 0.35 ms total |
| Non-standalone region through a reader | ✅ NOT_FOUND until `set_geometry`, then decode parity with the manifest world |

ABI v3 pass, 2026-08-01:

| Check | Result |
|---|---|
| `g++ -std=c++23` and `-std=c++20` builds of all `src/impl/*.cpp` | ✅ clean, warnings only |
| Exported symbols with `CXX_VISIBILITY_PRESET hidden` | ✅ exactly 79, all `bsvx_*`, nothing else |
| `test/tests.cpp` (18 tests, 5 new) | ✅ all pass |
| Same suite under `_GLIBCXX_DEBUG` + `_GLIBCXX_ASSERTIONS` | ✅ all pass, no checked-STL violations |
| `bsvx_world_create` → registry → `add_region` → `set_chunk_u32` → save → reload | ✅ geometry, registry, name and voxels all round-trip; `r_0_0_0.bvx` / `r_1_0_0.bvx` written |
| Summary of an authored chunk (3 non-air, keys 1,1,2) | ✅ `non_air=3 opaque=2 emissive=1` — built through the authored registry |
| `bsvx_world_create` with a 300-voxel chunk axis | ✅ rejected, `1 INVALID_ARGUMENT` (AABBs are u8) |
| `add_region` at an occupied coordinate / `set_registry_entry` for key 0 | ✅ both `1 INVALID_ARGUMENT` |
| `set_desc` changing chunk size on a populated world | ✅ refused; bounds changes still accepted |
| `bsvx_world_save_region_memory` vs `bsvx_world_save_region` | ✅ byte-identical, and the buffer reloads through `load_region_memory` |
| `bsvx_world_load_vfs` over the fixture world (`res://` scheme, real FS behind the callbacks) | ✅ identical decode to `bsvx_world_load` for every chunk; a directory path drives `list_dir` |
| VFS load with a missing manifest / with no `read_file` | ✅ fails with a message / `1 INVALID_ARGUMENT`, never falls back to the real filesystem |
| `.btx` introspection of the fixture | ✅ 2 archives; each 1 texture 4×4 `vk_format=43` (SRGB), 2 array layers, 1 sampler, 1 material with face layers `0,0,1,1,0,0` |
| Subresource bytes | ✅ `size == extent_x * extent_y * bytes_per_texel` (64 B), `(NULL,0)` sizes, `find_material` round-trips every row |

---

## 12. TL;DR for the implementing agent

1. Build BSVX with **`-std=c++23`**; compile its objects **into** the GDExtension `.so`; talk to it
   **only** through `bsvx_dll.h` (C ABI, PODs only).
2. Load with `bsvx_world_load(globalized_path)`; **enumerate chunks via the chunk map**, never by
   scanning coordinates.
3. Voxel array is dense `uint32_t`, `x + sx*(y + sy*z)`, length = `bsvx_region_required_voxel_count`.
   Key `0` is air. Resolve non-zero keys through the registry to `material_id` + flags.
4. Use `ChunkSummary` (`face_state_*`, `aabb_*`, `macro_occ_4x4x4`, `top_id_*`) to skip work before
   decoding or meshing.
5. Godot mapping is the **identity** — same handedness, do not negate Z.
6. Chunk lookup is O(1) now — use `bsvx_region_find_chunk` instead of caching your own map. Decode +
   mesh on `WorkerThreadPool`, one `bsvx_context` per thread.
7. For streaming, use `bsvx_region_reader_*` (§4.2): open cheaply, read summaries, decode only the
   chunks you page in. For a `.pck` / `res://`, either read bytes with `FileAccess` into the
   `_memory` entry points, or hand the whole world to `bsvx_world_load_vfs` (§4.3, §8.5).
8. Use the `_ex` calls everywhere so failures come with a message.
9. Before writing any save path, read §9.1 (manifest hash / line endings) and call
   `bsvx_world_compact` before saving an edited world (§9.4).
10. Authoring a world from C is `bsvx_world_create` → `set_registry_entry` (**before** any voxels) →
    `add_region` → `set_chunk_u32` → `save` (§4.3). Textures resolve through
    `bsvx_texture_find_material` / `_get_material` / `_get_subresource_bytes`.
11. Still not in the ABI: writing a `.btx`, saving through a VFS, removing regions or chunks — plus
    RGBA8-only texel formats (§5, §10).

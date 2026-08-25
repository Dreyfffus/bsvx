# bsvx for Blender

A Blender 4.2+ add-on that **authors, exports and writes** `.bsvx` worlds and standalone `.bvx`
regions, through the ctypes binding in `python/bsvx`.

It is deliberately not a voxel editor. Blender already has several good ones —
[Vox Cleaner](https://github.com/TheStrokeForge/Vox-Cleaner-V3), Vox Tools, the MagicaVoxel
importers, and Blender's own mesh tools — and a format add-on that grew its own would be a worse
version of all of them. What it does instead is put BSVX worlds into the shape those tools already
speak, and take them back afterwards.

## What a voxel is in Blender

**A voxel is a cube in a mesh.** One material per voxel key, interior faces culled, corners welded.

That is not the cheapest encoding. A vertex per voxel would be a quarter of the data and a
bijection with the cell, and an earlier version of this add-on did exactly that. It is the wrong
choice anyway, because it is a representation only this add-on understands. Vox Cleaner imports,
decimates, UV-unwraps and bakes a cube mesh with one material per palette entry; the MagicaVoxel
importers it is built on produce exactly that; every modifier, boolean and select tool in Blender
operates on it. Matching that convention is the difference between a world those tools can work on
and a point cloud they refuse.

| | |
| --- | --- |
| Geometry | one axis-aligned cube per non-air voxel, interior faces culled, lattice corners shared |
| Key | integer attribute `bsvx_key` on the **FACE** domain — authoritative |
| Materials | one slot per key, named `bsvx_<key>_<name>`, base colour from the registry, stamped with a `bsvx_key` custom property |
| Colour | `bsvx_color`, byte colour on the CORNER domain, and the mesh's active colour attribute |

The key is stamped on the material rather than parsed out of its name, because Blender renames on
collision and users rename on purpose. A mesh that has lost the stamp still falls back to slot
index + 1 — the convention every `.vox` importer follows — so a freshly imported MagicaVoxel model
exports to BSVX with no setup at all.

## The round trip, and what survives it

**Import From Mesh** does not assume a face is a voxel. It rasterizes every axis-aligned face into
the cells its rectangle covers, so coplanar merging — greedy meshing on the way in, Decimate on the
way out — changes nothing. Six big quads rebuild a solid box exactly as well as 216 small ones.

It also reconstructs the **interior**. A culled shell has lost its inside, and a naive read would
give back a hollow world: silent loss, the kind that surfaces three exports later. It is recovered
by parity along one axis — a cell owning a face that points down the axis opens a solid run, the
next cell owning a face that points up it closes one. For a closed model that is exact, and it
costs O(faces) where a flood fill would cost O(volume).

So these round-trip losslessly:

- deleting geometry (a voxel with no faces left is air — this is what makes deletion mean something)
- moving, rotating by right angles, or scaling the whole object
- booleans against grid-aligned cutters
- greedy/coplanar merging, limited dissolve, Decimate's planar mode
- anything Vox Tools does that leaves the model on the grid

And these do **not**, by their own design:

- **Vox Cleaner's clean is a terminal step.** Once it has decimated to a marching-cubes-ish surface
  and baked colour into a texture, the result is a game-ready mesh, not a voxel model. That is the
  point of it. Export to it, not back through it.
- anything that leaves the grid — sculpting, subdivision, non-right-angle rotation. Those still
  import, but through the ray-parity voxelizer rather than cell for cell, and the operator says so.

`Fill Interior` reports how many faces failed to pair; that count is how an open or self-
intersecting model announces itself, rather than quietly filling to infinity.

## The working set, and why it is not the whole world

A 512³ world is 134 million voxels. Blender does not survive materializing that, so **Export To
Mesh** takes the whole world, one region, or an explicit cell box, and refuses to exceed a voxel
budget (default 2 million, in the panel).

The object records the box it came from (`bsvx_bounds_min` / `bsvx_bounds_max`), and importing
**replaces that box** by default. That is what gives deletion meaning: a voxel the user removed
leaves no trace in the mesh, so only "everything in this box is what the mesh now says" reproduces
their intent. Turn *Replace Bounds* off to merge instead.

The world handle cannot be saved into a `.blend`. What the scene records is the world's *path*;
after a reload the panel offers **Reopen**, and the file on disk is the source of truth. A copy of
the world inside Blender's data would be a second source of truth that drifts.

## Coordinates

Blender is +Z up; BSVX's canonical convention is +Y up. `frame.py` holds the mapping — one signed
axis permutation per convention, with cell indices derived by flooring the continuous mapping rather
than by hand-written offsets. A negated axis needs a one-cell shift (cell `c` covers `[c, c+1)`, its
mirror is `-c-1`), and writing that by hand is the classic way to move a world by exactly one voxel:
invisible on symmetric test content, obvious on the first real geometry.

Cube winding is *measured* against that permutation rather than derived by hand, for the same
reason: two of the three conventions flip handedness, and a mesh with inverted normals looks right
until something renders it with backface culling.

The add-on reads the world's declared convention and adapts, so a Z-up world authored here and a
Y-up world authored for Godot both display correctly without either being silently rewritten. Voxel
size and origin come from the file's `[units]`, so nothing has to be guessed on import.

## Installing

First build the library once. That stages `libbsvx.so` into `python/bsvx/bin/<platform>/`, which is
where the binding looks:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

**From a checkout** — symlink the add-on into the user extensions repository, under the `id` its
manifest declares (`bsvx`, not the directory name). Edits then take effect on the next Blender
restart, with no repackaging. From 4.2 on this is `extensions/user_default/`, *not* the legacy
`scripts/addons/`:

```sh
# Linux; macOS uses ~/Library/Application Support/Blender/<ver>/,
# Windows %APPDATA%\Blender Foundation\Blender\<ver>\
mkdir -p ~/.config/blender/<ver>/extensions/user_default
ln -s "$PWD/integrations/blender/bsvx_blender" ~/.config/blender/<ver>/extensions/user_default/bsvx
```

Then enable **BSVX (Basil Voxel)** in *Edit ▸ Preferences ▸ Add-ons*. It registers as
`bl_ext.user_default.bsvx`.

**As an extension zip**:

```sh
python3 integrations/blender/package.py            # -> integrations/blender/dist/bsvx_blender.zip
```

`package.py` vendors the binding into `_vendor/` and includes whatever shared libraries are staged
under `python/bsvx/bin/<platform>/`. Build the library on each platform you want to ship before
packaging; a zip built on one machine carries only that machine's binary. Install through
*Edit ▸ Preferences ▸ Get Extensions ▸ Install from Disk*.

If the library cannot be found at all, the panel says so instead of throwing — and `$BSVX_LIBRARY`
overrides the search.

## Verifying

```sh
blender --factory-startup --background --python integrations/blender/tests/test_headless.py
```

`--factory-startup` is not optional once you have also installed the add-on: otherwise Blender
enables the installed copy, the script registers a second copy of the same classes over it, and
the teardown of whichever one loses raises on exit.

148 checks. The axis mapping is checked against the library's own `convert_cell`; the cube is
checked to cover exactly its cell under every convention; the mesh round trip is checked by
comparing the region's decoded voxel array *before and after*, so a mesh that loses, shifts or
hollows anything cannot pass. Also covered: face culling and outward normals, deleting faces
deleting voxels, a solid body surviving as a hollow shell, six dissolved quads rebuilding it, a
boolean cut, keys recovered from materials and from colours, auto-registering an unknown colour,
object transforms, voxelizing an arbitrary mesh, dirty save and reopen, validation, and standalone
`.bvx` export. It exits non-zero on the first failure.

## The workflow

**Author from nothing**

1. *BSVX ▸ World*: set chunk size, region size and voxel size, then **New**.
2. *Registry*: add entries, or **Registry From Materials** to take one key per material slot of the
   active object. Set colours. **Push** to write them into the world.
3. *Authoring*: **Fill Box** for blocks, or model a shape and **Import From Mesh** to voxelize it —
   keys come from its materials, so a two-material mesh gives two voxel types.
4. **Save As** to a directory.

**Bring in a MagicaVoxel model**

1. Import the `.vox` with whichever add-on you already use.
2. **New** a world with a matching voxel size.
3. **Import From Mesh**. Keys come from the material slots; unknown colours get registry entries.
4. **Save As**.

**Edit an existing world**

1. **Open** a world directory, its `manifest.toml`, or a `.bvx`.
2. *Mesh*: **Export To Mesh** — the whole world, one region, or a box.
3. Edit it with Vox Tools, Blender's mesh tools, booleans, whatever keeps it on the grid.
4. **Import From Mesh**, then **Save** (writes only what changed) or **Save As**.

**Ship it as an asset**

**Export To Mesh**, then run Vox Cleaner on the result. That path ends there — the cleaned mesh is
an asset, not a world.

## Operators

| Operator | What it does |
| --- | --- |
| `bsvx.new_world` | empty world from the geometry settings |
| `bsvx.open_world` | directory, `manifest.toml`, or `.bvx` |
| `bsvx.reopen_world` | reopen the recorded path after a `.blend` reload |
| `bsvx.close_world` | drop the handle |
| `bsvx.save_world` | dirty-only write back to the source path |
| `bsvx.save_world_as` | full write to a directory |
| `bsvx.export_region` | single region as a standalone `.bvx` |
| `bsvx.to_mesh` | world, region or box → cube mesh with materials |
| `bsvx.from_mesh` | mesh → voxels, cell for cell or by voxelization |
| `bsvx.fill_box` | fill or erase a cell box directly |
| `bsvx.registry_pull` / `registry_push` | move the registry between world and UI |
| `bsvx.registry_add` / `registry_remove` / `registry_from_materials` | edit the registry list |
| `bsvx.make_palette` | build a `.btx` palette archive from the registry colours |
| `bsvx.validate` | run the library's checks into a UI list |
| `bsvx.compact` | reclaim dead bytes in each region |
| `bsvx.remove_region` | drop a region |

`bsvx.from_mesh` picks its mode from the mesh unless told: essentially-axis-aligned reads cell for
cell, anything else voxelizes. Keys come from `bsvx_key`, then materials, then colours, then the
active key — again unless told.

## Limits, stated plainly

- **A merged face is read as its bounding rectangle.** That is exact for the rectangles coplanar
  merging actually produces, and wrong for a concave n-gon — an L-shaped dissolved face fills the
  notch. Limited Dissolve on a rectangular slab is fine; on an L-shaped one, keep the quads.
- **Voxelization needs a closed mesh.** It is ray parity along Blender's +X, which makes the cost
  proportional to cross-section rather than volume — but an open surface has an unpaired crossing
  and the fill leaks along that line. `Surface Only` sidesteps the question.
- **Colours are 8-bit sRGB in a colour attribute**, so key-from-colour matches by nearest rather
  than by equality. The `bsvx_key` attribute is the lossless path and is preferred automatically.
- **One world per scene.** Editing two at once multiplies every operator's "which world?" question
  and nothing in the format needs it.
- **Keys above 2³¹** read back negative in the `bsvx_key` attribute, which is `int32`. The
  reinterpretation is bit-exact, so they round-trip correctly; they just look odd in the UI.
- **No threading.** Everything runs on the main thread. A large export blocks Blender.
- **`.btx` authoring is limited to `make_palette`.** Full texture archives are the library's API.

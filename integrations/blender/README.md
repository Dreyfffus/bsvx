# bsvx for Blender

A Blender 4.2+ add-on that opens, authors, modifies and writes `.bsvx` worlds and standalone `.bvx`
regions, through the ctypes binding in `python/bsvx`.

## What a voxel is in Blender

This is the question the whole design turns on, so it is answered first and answered once.

Blender has **no datablock that is a `uint32` voxel grid**. Volume objects are OpenVDB float grids
and are effectively read-only from Python; meshes are surfaces; geometry attributes are the only
general-purpose per-element storage. So a representation has to be chosen, and it has to be one
that Blender's own editing tools operate on — otherwise "modify" means "type numbers into a panel",
which is not why anyone opens Blender.

**A voxel is a vertex.**

| | |
| --- | --- |
| Position | the voxel cell's **centre**, in Blender-space metres |
| Key | integer attribute `bsvx_key` on the POINT domain |
| Colour | byte-colour attribute `bsvx_color` on POINT — *display only*, rebuilt from the registry on every checkout |

Everything follows from that. Box-select, move (with grid snap), delete, duplicate, mask, and
Geometry Nodes all work on vertices, so they all work on voxels. Instancing a cube on each point
gives you the blocky preview without the data ever being cubes.

The obvious alternative — eight vertices and six faces per voxel — was rejected on both counts that
matter. It is twenty-four times the data for the same information, and it is *ambiguous on
read-back*: once a user has merged, extruded or dissolved anything, no rule recovers "which voxels
did they mean". A vertex is a bijection with a cell, and flooring its position is the entire inverse.

### Consequences worth knowing

- **A vertex anywhere inside a cell counts as that cell.** You do not have to land exactly on the
  centre; the commit floors. Two vertices in one cell is a collision — the last one wins and the
  operator reports how many.
- **The object transform is honoured.** Moving, rotating or scaling the whole object moves the
  voxels, because that is the only reading of "I moved it" that does not silently discard the edit.
- **Air is not a vertex.** A cell with no vertex is air. That is what makes deletion work.

## The working set, and why it is not the whole world

A 512³ world is 134 million voxels. Blender does not survive materializing that, and neither does
the session. So the world itself lives as a C handle owned by the add-on, and Blender geometry is a
**checked-out working set**: a bounded box of voxels, materialized for editing.

The object records the box it owns (`bsvx_bounds_min` / `bsvx_bounds_max` custom properties), and
**committing replaces that box**. That is precisely what gives deletion meaning — a vertex the user
removed leaves no trace in the mesh, so only "everything in this box is what the mesh now says"
reproduces their intent. Voxels dragged *outside* the box are still written, and counted separately
in the report, because dropping them would silently undo a deliberate move.

Checkout refuses to exceed a vertex budget (default 2 million, in the panel).

The handle cannot be saved into a `.blend`. What the scene records is the world's *path*; after a
reload the panel offers **Reopen**, and the file on disk is the source of truth. This is deliberate:
a copy of the world inside Blender's data would be a second source of truth that drifts.

## Coordinates

Blender is +Z up; BSVX's canonical convention is +Y up. `frame.py` holds the mapping — one signed
axis permutation per convention, with cell indices derived by flooring the continuous mapping rather
than by hand-written offsets. A negated axis needs a one-cell shift (cell `c` covers `[c, c+1)`, its
mirror is `-c-1`), and writing that by hand is the classic way to move a world by exactly one voxel:
invisible on symmetric test content, obvious on the first real geometry.

The add-on reads the world's declared convention and adapts, so a Z-up world authored here and a
Y-up world authored for Godot both display correctly without either being silently rewritten.

Voxel size and origin come from the file's `[units]`, so nothing has to be guessed on import or
remembered on export.

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
blender --background --python integrations/blender/tests/test_headless.py
```

98 checks covering the axis mapping against the library's own `convert_cell`, the cell/point round
trip, authoring and reopening a world from disk, checkout → edit in bmesh → commit (including that
deleting a vertex deletes the voxel), object transforms, voxelization, dirty save, reopen after
handle loss, validation, and standalone `.bvx` export. It exits non-zero on the first failure.

## The workflow

**Author from nothing**

1. *BSVX ▸ World*: set chunk size, region size and voxel size, then **New**.
2. *Registry*: add entries, or **Registry From Materials** to take one key per material slot of the
   active object. Set colours. **Push** to write them into the world.
3. *Authoring*: set the active key, then **Voxelize Object** on a mesh, or **Fill Box**.
4. **Save As** to a directory.

**Modify an existing world**

1. **Open** a world directory, its `manifest.toml`, or a `.bvx`.
2. *Working Set*: **Check Out Everything**, a region, or an explicit box.
3. Edit the point mesh with Blender's normal tools — delete vertices, move them, duplicate them,
   change `bsvx_key` in the spreadsheet editor or through Geometry Nodes.
4. **Commit Working Set**.
5. **Save** (writes only what changed) or **Save As**.

**Export**

**Save As** writes a full world directory. **Export .bvx** writes a single-region world as one
standalone file, which is what a runtime that streams individual regions wants.

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
| `bsvx.checkout_all` / `checkout_region` / `checkout_box` | materialize voxels as a point mesh |
| `bsvx.commit` | write working sets back, replacing the boxes they own |
| `bsvx.voxelize_object` | mesh volume → voxels, solid or surface-only |
| `bsvx.fill_box` | fill or erase a cell box directly |
| `bsvx.registry_pull` / `registry_push` | move the registry between world and UI |
| `bsvx.registry_add` / `registry_remove` / `registry_from_materials` | edit the registry list |
| `bsvx.make_palette` | build a `.btx` palette archive from the registry colours |
| `bsvx.validate` | run the library's checks into a UI list |
| `bsvx.compact` | reclaim dead bytes in each region |
| `bsvx.remove_region` | drop a region |

## Limits, stated plainly

- **Voxelization needs a closed mesh.** It is ray parity along Blender's +X, which makes the cost
  proportional to cross-section rather than volume — but an open surface has an unpaired crossing
  and the fill leaks along that line. `Surface Only` sidesteps the question entirely.
- **No preview meshing.** Points are drawn as points. Instancing a cube per point is a Geometry
  Nodes modifier away and is a display choice, not the add-on's to make.
- **One world per scene.** Editing two at once multiplies every operator's "which world?" question
  and nothing in the format needs it.
- **Keys above 2³¹** read back negative in the `bsvx_key` attribute, which is `int32`. The
  reinterpretation is bit-exact, so they round-trip correctly; they just look odd in the UI.
- **No threading.** Everything runs on the main thread. A large checkout blocks Blender.
- **`.btx` authoring is limited to `make_palette`.** Full texture archives are the library's API.

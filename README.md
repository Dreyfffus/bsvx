# Basil Voxel

Basil Voxel is an asset baker, packer, loader, and decoder for voxel worlds.

It is built around a structured world format designed for the Basil minimal engine, with two main goals:

- keep voxel assets portable and easy to move around
- make world data accessible before full decode, so the engine can inspect, bake, stream, and transform chunks without committing to a single rendering strategy

This project is both a practical tool and a learning project. The practical side is simple: I wanted an asset format that would not fall apart every time I copied folders around, reorganized content, renamed assets, or split a world into smaller pieces.

---

## What it does

Basil Voxel provides:

- a binary texture/texel asset format
- a binary voxel region format
- a manifest-driven world layout
- standalone region loading
- chunk-level decode access
- write-back support for modified voxel data
- a DLL-friendly public API for tooling and engine integration

The library does **not** force a single bake target.

It lets you access voxel data so you can build whatever runtime representation your renderer needs, for example:

- chunk meshes for rasterization
- octrees or brick structures for ray marching
- collision data
- surface bake payloads
- custom acceleration structures

---

## File structure

The main entry point for a Basil world is `manifest.toml`.

A typical world looks like this:

```text
world_root/
  manifest.toml
  regions/
    r_0_0_0.bvx
    r_1_0_0.bvx
    ...
  textures/
    terrain_red.btx
    terrain_blue.btx
    ...
```

### `manifest.toml`
This is the root description of the world.

It contains the metadata needed to interpret the world consistently, including things such as:

- format version
- chunk size
- region size
- bounds
- voxel schema
- axis convention
- registry data
- texture references
- region references
- tool metadata

The manifest is what ties the world together.

### `.bvx`
A `.bvx` file is a **binary voxel region** file.

It stores one region of chunked voxel data, plus optional baked payloads. A `.bvx` can be loaded either:

- as part of a larger manifest-driven world
- or standalone, if it contains the required self-describing world metadata

A `.bvx` is designed to be useful **before full voxel decode**. That means the engine can inspect chunk summaries, decide what to stream, and choose which chunks to bake without immediately expanding the whole region into dense voxel buffers.

### `.btx`
A `.btx` file is a **binary texel** file.

It stores texture and texel data referenced by materials, regions, or the manifest. It is meant to be consumed cleanly by Vulkan-oriented pipelines and similar rendering backends.

---

## Design goals

### 1. Stable asset layout
The format is meant to survive being copied, moved, renamed, or reorganized without turning into a dependency nightmare.

### 2. Chunk-first access
Voxel worlds are large. The format is built around regions and chunks so the engine can work incrementally.

### 3. Decode only when needed
The library is not just a serializer. It is meant to expose the data in a way that supports:

- summary-first inspection
- selective chunk decode
- bake-then-save workflows

### 4. Rendering-strategy agnostic
The format does not assume a raster-only engine.

The same source data can be used to generate:
- raster meshes
- ray-marching structures
- collision volumes
- baked surface data
- future runtime-specific formats

### 5. Tool-friendly and DLL-friendly
The public boundary is designed so the implementation can stay internal while external tools use a stable API.

---

## Why this exists

Developing Basil Voxel is partly for learning purposes, but also because I wanted an asset format that would not destroy itself if I copied files from one unit to another or renamed assets.

I do that often.

A lot.

So this format is intentionally built around explicit metadata, region containers, and stable asset references instead of brittle assumptions about where files “should” be.

---

## Core concepts

### World
A world is the root directory plus its manifest.

### Region
A region is a spatial container of chunks stored in one `.bvx` file.

### Chunk
A chunk is the smallest practical decode and bake unit.

### Registry
Voxel keys resolve through a registry. This allows compact dense chunk storage using integer voxel identifiers while still preserving material and semantic meaning.

### Baked payloads
A region may store derived chunk payloads such as:
- surface bake data
- collision bake data
- distance-field-related payloads
- light data
- custom user sections

The library exposes the source voxel data and the baked data, but does not force a particular baking algorithm.

---

## Access model

The intended workflow is:

1. load a world or standalone region
2. inspect regions and chunk summaries
3. decode only the chunks you need
4. build your own runtime representation
5. optionally write modified data or baked payloads back to disk

That means the library is useful for:

- editors
- bakers
- converters
- offline asset pipelines
- in-engine import tools

---

## Engine integration

Basil Voxel is built so an engine can:

- load a manifest-described world
- load a standalone region as an asset
- decode chunks to canonical dense voxel-key buffers
- mutate chunk data
- attach baked payload sections
- save the result back to disk

For shipping or integration through the DLL boundary, the intended public surface is the exported API, not the full internal implementation headers. (bsvx_dll.h)

---

## Typical use cases

### Raster engine
Decode a chunk, generate faces or greedy meshes, attach a baked surface section, save the region.

### Ray-marching engine
Decode a chunk, build an octree or brick hierarchy, keep the source voxels and optionally attach baked acceleration data.

### World tools
Load a world, scan summaries, modify chunks, repack regions, save.

---

## Project status

Workable.
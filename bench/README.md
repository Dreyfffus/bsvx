# `.bvx` against `.vox`

A measurement harness, not a test. It generates voxel scenes covering the range of content the two
formats are actually asked to hold, writes each one through both formats, and reports what each
costs in bytes and in load time.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBSVX_BUILD_BENCH=ON
cmake --build build
./build/bench_vox --out /var/tmp/bsvx_bench
```

`--out` must be on a **real filesystem**. On `tmpfs` the cold-cache column measures nothing, because
there is no page cache to evict — the file is already RAM.

| flag | effect |
|---|---|
| `--out DIR` | where fixtures are written (default: the system temp dir) |
| `--runs N` | timed iterations per measurement; the median is reported (default 5) |
| `--chunk N` | chunk edge in voxels (default 16) |
| `--vox FILE` | also run a real MagicaVoxel file through the same pipeline; repeatable |
| `--sweep` | re-run two scenes at chunk 4/8/16/32/64 |

## What it measures

Every scene starts as one dense `uint32` grid. Both formats are written from it and both are read
back into that same grid, so the timed work is identical on both sides.

- **warm / cold decode** — the full dense grid. `.bvx` through `bsvx_region_decode_all_u32`, `.vox`
  through a single-pass parse and scatter. Cold evicts the file with `posix_fadvise(DONTNEED)` before
  each iteration.
- **open** — what each format costs before any voxel is available. For `.bvx` that is the streaming
  reader with header, chunk map and summaries resident and no payload touched; for `.vox` it is the
  whole file, because the format offers nothing cheaper.
- **byte anatomy** — where a `.bvx`'s bytes go: header, chunk map, summary table, section and entry
  tables, voxel payload, and which codec each chunk chose. Read through the library's own layout
  headers, so it follows the format rather than duplicating it.
- **the packed-sparse what-if** — what the payloads would come to if `SPARSE_LIST` bit-packed its
  index and palette-indexed its key instead of spending a full `uint32` on each.

Both round-trips are verified against the source grid before anything is timed. A benchmark whose
two sides disagree about the data is measuring nothing.

## Reading the results honestly

- `.vox` caps a model at **256 voxels per axis** and **255 colours**. Every scene is inside both
  limits so the comparison is possible at all; a world larger than that is not one `.vox` file's
  worth of data, and the harness refuses rather than quietly splitting it.
- Both formats are compared **raw and gzipped**. A format that merely defers its redundancy to the
  packer should not get credit for it.
- Warm-cache numbers are the CPU cost; cold-cache numbers are what an asset load actually costs the
  first time. They tell different stories and both are reported.
- `bench/vox.hpp` is a straight single-pass parser over a fully-buffered file with one bulk `memcpy`
  for the voxel list. It is deliberately not a strawman.

## Files

| file | what it is |
|---|---|
| `bench_vox.cpp` | scene generation, both format sides, timing, reporting |
| `vox.hpp` | minimal MagicaVoxel `.vox` reader/writer; knows nothing about bsvx |
| `bvx_anatomy.hpp` | walks a `.bvx`'s on-disk structures to attribute its bytes |

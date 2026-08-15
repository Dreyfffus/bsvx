"""Test suite for the bsvx Python binding.

Deliberately stdlib-only (``unittest``, no pytest, no numpy) so it runs inside a host application's
bundled Python — Blender's, for instance — without installing anything.

    PYTHONPATH=python python3 -m unittest discover -s python/tests
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import bsvx
from bsvx import BsvxError, Context, TextureBuilder, VfsWriter, World


class TempWorld(unittest.TestCase):
    """Base class handing out a scratch directory that is cleaned up afterwards."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bsvx_py_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def authoring_world(self) -> World:
        """4x4x4 voxels per chunk, 2x2x2 chunks per region — a region spans 8 voxels per axis."""
        world = World.create((4, 4, 4), (2, 2, 2))
        self.addCleanup(world.close)
        world.set_registry_entry(1, material_id=0, flags=bsvx.REGISTRY_OPAQUE, name="stone")
        return world


class TestBinding(unittest.TestCase):
    def test_abi_and_struct_layout(self) -> None:
        # Importing the package already ran the size checks; this asserts they were meaningful.
        self.assertGreaterEqual(bsvx.abi_version(), 4)
        self.assertIn("bsvx abi", bsvx.build_info())

    def test_error_carries_library_message(self) -> None:
        with self.assertRaises(BsvxError) as caught:
            World.load("/nonexistent/definitely/not/a/world")
        self.assertTrue(str(caught.exception))


class TestAuthoring(TempWorld):
    def test_create_save_reload(self) -> None:
        root = self.tmp / "world"

        world = self.authoring_world()
        world.name = "PyWorld"
        world.set_units(0.25, (1.0, 2.0, 3.0))
        world.set_metadata("tool.name", "python")
        world.set_metadata("tool.blob", bytes([0, 1, 2, 255]))
        world.fill_box((0, 0, 0), (3, 3, 3), 1)

        report = world.save(root)
        self.assertTrue(report.manifest_written)
        self.assertGreater(report.bytes_written, 0)

        with World.load(root) as reloaded:
            self.assertEqual(reloaded.name, "PyWorld")
            self.assertEqual(reloaded.units.voxel_size_x, 0.25)
            self.assertEqual(reloaded.units.origin_y, 2.0)
            self.assertEqual(reloaded.registry_name(1), "stone")
            self.assertEqual(reloaded.get_metadata("tool.name"), b"python")
            self.assertEqual(reloaded.get_metadata("tool.blob"), bytes([0, 1, 2, 255]))
            self.assertIsNone(reloaded.get_metadata("absent"))
            self.assertEqual(sorted(reloaded.metadata_keys()), ["tool.blob", "tool.name"])

    def test_world_space_voxels_including_negative(self) -> None:
        world = self.authoring_world()

        coords = [(0, 0, 0), (5, 1, 2), (-1, -1, -1), (100, 100, 100)]
        self.assertEqual(world.set_voxels(coords, 1), 4)
        # (0,0,0) and (5,1,2) share a region; the other two get their own.
        self.assertEqual(world.region_count, 3)
        self.assertEqual(world.get_voxels(coords), [1, 1, 1, 1])
        self.assertEqual(world.get_voxels([(3, 3, 3)]), [0])

        # Flooring, not truncation: -1 belongs to region -1.
        address = world.locate_voxel(-1, -1, -1)
        self.assertEqual(address.region_x, -1)
        self.assertEqual(address.local_x, 3)

    def test_set_voxels_rejects_mismatched_lengths(self) -> None:
        world = self.authoring_world()
        with self.assertRaises(BsvxError):
            world.set_voxels([(0, 0, 0), (1, 1, 1)], [1])

    def test_removal_and_pruning(self) -> None:
        root = self.tmp / "world"

        world = self.authoring_world()
        world.fill_box((0, 0, 0), (0, 0, 0), 1)
        world.fill_box((16, 16, 16), (16, 16, 16), 1)
        world.save(root)

        on_disk = lambda: sorted(p.name for p in (root / "regions").glob("*.bvx"))  # noqa: E731
        self.assertEqual(len(on_disk()), 2)

        with World.load(root) as reopened:
            victim = reopened.find_region(2, 2, 2)
            self.assertIsNotNone(victim)
            reopened.remove_region(victim)

            # Without pruning the orphaned file survives and auto-discovery resurrects the region.
            reopened.save(reopened.source_path)
            self.assertEqual(len(on_disk()), 2)

            report = reopened.save(reopened.source_path, prune_orphans=True)
            self.assertEqual(report.files_removed, 1)

        self.assertEqual(len(on_disk()), 1)
        with World.load(root) as final:
            self.assertEqual(final.region_count, 1)

    def test_dirty_incremental_save(self) -> None:
        root = self.tmp / "world"

        world = self.authoring_world()
        world.fill_box((0, 0, 0), (0, 0, 0), 1)
        world.fill_box((16, 16, 16), (16, 16, 16), 1)
        world.save(root)

        with World.load(root) as reopened:
            self.assertFalse(reopened.is_dirty)

            region = reopened.find_region(0, 0, 0)
            reopened.set_voxels([(1, 1, 1)], 1, create_missing=False)
            self.assertTrue(reopened.is_dirty)
            self.assertTrue(reopened.region_is_dirty(region))

            report = reopened.save_dirty()
            self.assertFalse(report.manifest_written)
            self.assertEqual(report.files_written, 1)
            self.assertEqual(report.files_skipped, 2)
            self.assertFalse(report.full_rewrite)
            self.assertFalse(reopened.is_dirty)

            # A manifest-level change forces a full rewrite: the hash is stamped into every region.
            reopened.name = "Renamed"
            report = reopened.save_dirty()
            self.assertTrue(report.manifest_written)
            self.assertTrue(report.full_rewrite)

    def test_hash_mismatch_tolerance_and_repair(self) -> None:
        root = self.tmp / "world"

        world = self.authoring_world()
        world.fill_box((0, 0, 0), (0, 0, 0), 1)
        world.save(root)

        with open(root / "manifest.toml", "ab") as handle:
            handle.write(b"\n# a human was here\n")

        with self.assertRaises(BsvxError):
            World.load(root)

        with World.load(root, ignore_hash_mismatch=True) as tolerant:
            self.assertEqual(len(tolerant.warnings), 1)
            self.assertIn("hash mismatch", tolerant.warnings[0])
            tolerant.rehash()
            tolerant.save(root)

        with World.load(root) as repaired:
            self.assertEqual(repaired.region_count, 1)

    def test_validation_reports_orphan_key(self) -> None:
        world = self.authoring_world()
        world.fill_box((0, 0, 0), (1, 1, 1), 1)
        world.set_voxels([(2, 2, 2)], 77)

        issues = world.validate(deep=True)
        orphans = [i for i in issues if i.code == 1 and i.voxel_key == 77]
        self.assertEqual(len(orphans), 1)
        self.assertIn("77", orphans[0].message)
        self.assertGreaterEqual(orphans[0].region_index, 0)

    def test_progress_and_cancel(self) -> None:
        stages: list[str] = []

        ctx = Context()
        self.addCleanup(ctx.close)
        # `stages.append` returns None, so this always keeps going.
        ctx.set_progress(lambda stage, done, total: stages.append(stage) is None)

        world = World.create((4, 4, 4), (2, 2, 2), ctx=ctx)
        self.addCleanup(world.close)
        world.set_registry_entry(1, flags=bsvx.REGISTRY_OPAQUE)
        world.fill_box((0, 0, 0), (3, 3, 3), 1)
        world.save(self.tmp / "progress")
        self.assertIn("regions", stages)

        # Cancelling leaves nothing behind, because saves are atomic.
        ctx.set_progress(lambda stage, done, total: False)
        cancelled = self.tmp / "cancelled"
        with self.assertRaises(BsvxError) as caught:
            world.save(cancelled)
        self.assertEqual(caught.exception.result, bsvx.RESULT_CANCELLED)
        self.assertFalse((cancelled / "manifest.toml").exists())

    def test_callback_exception_is_treated_as_cancel(self) -> None:
        ctx = Context()
        self.addCleanup(ctx.close)

        world = World.create((4, 4, 4), (2, 2, 2), ctx=ctx)
        self.addCleanup(world.close)
        world.set_registry_entry(1, flags=bsvx.REGISTRY_OPAQUE)
        world.fill_box((0, 0, 0), (0, 0, 0), 1)

        # Installed only now: the callback applies to every operation on the context, and a raising
        # one would have cancelled the fill above too.
        def explode(stage: str, done: int, total: int) -> bool:
            raise ValueError("this must not reach the interpreter")

        ctx.set_progress(explode)

        with self.assertRaises(BsvxError) as caught:
            world.save(self.tmp / "explode")
        self.assertEqual(caught.exception.result, bsvx.RESULT_CANCELLED)


class TestTextures(TempWorld):
    def test_palette_authoring(self) -> None:
        root = self.tmp / "world"
        colors = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)]

        world = World.create((4, 4, 4), (2, 2, 2))
        self.addCleanup(world.close)
        world.make_palette(colors, texture_id="palette")

        # One registry entry per colour, keyed from 1 because 0 is air.
        self.assertEqual(len(world.registry), len(colors))
        self.assertEqual(world.registry_color(1), 0xFF0000FF)
        self.assertEqual(world.registry_color(2), 0x00FF00FF)

        world.fill_box((0, 0, 0), (1, 1, 1), 2)
        world.save(root)

        with World.load(root) as reloaded:
            self.assertEqual(reloaded.texture_count, 1)
            self.assertEqual(reloaded.texture_id(0), "palette")
            self.assertEqual(reloaded.registry_color(3), 0x0000FFFF)
            self.assertEqual(reloaded.material(0, 1).tint_rgba8, 0x00FF00FF)

    def test_builder_and_formats(self) -> None:
        ctx = Context()
        self.addCleanup(ctx.close)

        builder = TextureBuilder(ctx)
        self.addCleanup(builder.close)
        builder.add_sampler()

        texture = builder.add_texture(4, 4, vk_format=bsvx.VK_FORMAT_R8G8B8A8_SRGB, array_layers=2, mip_levels=3)
        for layer in range(2):
            builder.append_layer(texture, layer, bytes([0xC0]) * (4 * 4 * 4), 4, 4)
        builder.generate_mips(texture)
        self.assertEqual(builder.validate(), [])

        # Single-channel and block-compressed formats size differently; both must be accepted.
        self.assertTrue(bsvx.format_is_supported(bsvx.VK_FORMAT_R8_UNORM))
        self.assertTrue(bsvx.format_is_supported(bsvx.VK_FORMAT_BC7_UNORM))
        self.assertFalse(bsvx.format_is_supported(9999))
        self.assertEqual(bsvx.format_subresource_size(bsvx.VK_FORMAT_R8G8B8A8_SRGB, 4, 4), 64)
        self.assertEqual(bsvx.format_subresource_size(bsvx.VK_FORMAT_BC7_UNORM, 4, 4), 16)
        # Rounded up to whole blocks, never down.
        self.assertEqual(bsvx.format_subresource_size(bsvx.VK_FORMAT_BC7_UNORM, 5, 5), 64)

        bc7 = builder.add_texture(8, 8, vk_format=bsvx.VK_FORMAT_BC7_UNORM)
        builder.append_layer(bc7, 0, bytes(64), 8, 8)
        with self.assertRaises(BsvxError):
            builder.append_layer(bc7, 0, bytes(32), 8, 8)   # half the blocks

        path = self.tmp / "textures.btx"
        builder.save(path)
        self.assertTrue(path.exists())

        with TextureBuilder.open(ctx, path) as reopened:
            self.assertEqual(reopened.validate(), [])


class TestAxisConventions(TempWorld):
    def test_cell_conversion_roundtrip(self) -> None:
        blender = bsvx.AXIS_X_RIGHT_Z_UP_Y_FORWARD
        canonical = bsvx.AXIS_X_RIGHT_Y_UP_Z_FORWARD

        # Blender (x, y, z) -> canonical (x, z, -y), with the extra -1 for cells.
        self.assertEqual(bsvx.convert_cell(blender, canonical, 1, 2, 3), (1, 3, -3))
        self.assertEqual(bsvx.convert_position(blender, canonical, 1.0, 2.0, 3.0), (1.0, 3.0, -2.0))

        for x in range(-3, 4):
            for y in range(-3, 4):
                for z in range(-3, 4):
                    there = bsvx.convert_cell(blender, canonical, x, y, z)
                    back = bsvx.convert_cell(canonical, blender, *there)
                    self.assertEqual(back, (x, y, z))

    def test_world_conversion_moves_every_voxel(self) -> None:
        world = self.authoring_world()

        # Asymmetric on purpose: a mirrored result is otherwise indistinguishable.
        points = [(0, 0, 0), (1, 0, 0), (0, 2, 0), (0, 0, 3), (-4, 5, -6)]
        world.set_voxels(points, 1)

        moved = world.convert_axis_convention(bsvx.AXIS_X_RIGHT_Z_UP_Y_FORWARD)
        self.assertEqual(moved, len(points))
        self.assertEqual(world.axis_convention, bsvx.AXIS_X_RIGHT_Z_UP_Y_FORWARD)

        converted = [
            bsvx.convert_cell(bsvx.AXIS_X_RIGHT_Y_UP_Z_FORWARD, bsvx.AXIS_X_RIGHT_Z_UP_Y_FORWARD, *p)
            for p in points
        ]
        self.assertEqual(world.get_voxels(converted), [1] * len(points))

        world.convert_axis_convention(bsvx.AXIS_X_RIGHT_Y_UP_Z_FORWARD)
        self.assertEqual(world.get_voxels(points), [1] * len(points))


class TestCloneAndVfs(TempWorld):
    def test_clone_is_deep(self) -> None:
        world = self.authoring_world()
        world.fill_box((0, 0, 0), (1, 1, 1), 1)
        world.name = "Original"

        clone = world.clone()
        self.addCleanup(clone.close)
        self.assertEqual(clone.name, "Original")

        clone.name = "Copy"
        clone.set_voxels([(0, 0, 0)], 0, create_missing=False)

        self.assertEqual(world.name, "Original")
        self.assertEqual(world.get_voxels([(0, 0, 0)]), [1])
        self.assertEqual(clone.get_voxels([(0, 0, 0)]), [0])

    def test_save_through_vfs(self) -> None:
        class MemoryWriter(VfsWriter):
            def __init__(self) -> None:
                super().__init__()
                self.files: dict[str, bytes] = {}
                self.directories: list[str] = []

            def write_file(self, path: str, data: bytes, atomic: bool, backup: bool) -> None:
                self.files[path] = data

            def make_directories(self, path: str) -> None:
                self.directories.append(path)

            def file_exists(self, path: str) -> bool:
                return path in self.files

            def remove_file(self, path: str) -> None:
                self.files.pop(path, None)

            def read_file(self, path: str) -> bytes | None:
                return self.files.get(path)

        writer = MemoryWriter()

        world = self.authoring_world()
        world.fill_box((0, 0, 0), (0, 0, 0), 1)

        report = world.save_vfs("res://worlds/demo", writer)
        self.assertEqual(report.files_written, 2)
        self.assertIn("res://worlds/demo/manifest.toml", writer.files)
        self.assertIn("res://worlds/demo/regions/r_0_0_0.bvx", writer.files)
        # The scheme has to survive: std::filesystem::path would collapse the "//".
        self.assertTrue(all(p.startswith("res://") for p in writer.files))

        # The captured bytes must load as a real world.
        root = self.tmp / "from_vfs"
        for path, data in writer.files.items():
            target = root / path[len("res://worlds/demo/"):]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)

        with World.load(root) as reloaded:
            self.assertEqual(reloaded.region_count, 1)
            self.assertEqual(reloaded.registry_name(1), "stone")


class TestNonAsciiPaths(TempWorld):
    def test_unicode_paths_roundtrip(self) -> None:
        # Every path crossing the C boundary is UTF-8, on every platform.
        root = self.tmp / "wörld-日本-🧊"

        world = self.authoring_world()
        world.set_registry_name(1, "pierre grisâtre")
        world.fill_box((0, 0, 0), (1, 1, 1), 1)
        world.save(root)

        self.assertTrue((root / "manifest.toml").exists())

        with World.load(root) as reloaded:
            self.assertEqual(reloaded.region_count, 1)
            self.assertEqual(reloaded.registry_name(1), "pierre grisâtre")
            self.assertIn("wörld", reloaded.source_path)


if __name__ == "__main__":
    unittest.main()

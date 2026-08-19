#!/usr/bin/env python3
"""Builds a shippable Blender extension zip.

    python3 integrations/blender/package.py [--output DIR] [--platform linux_x86_64 ...]

The add-on's source tree deliberately does not contain the ctypes binding: it lives once, in the
repository's ``python/`` directory, and is copied into ``_vendor/`` here. That keeps a checkout from
having two copies that drift, and keeps the shipped zip self-contained -- which it has to be,
because a Blender extension cannot reach outside its own directory.

Whatever shared libraries are staged under ``python/bsvx/bin/<platform>/`` come along. Build the
library for each platform first (``cmake --build build`` stages the host's automatically); a zip
built on one machine only carries that machine's binary.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ADDON = HERE / "bsvx_blender"
BINDING = REPO / "python" / "bsvx"

#: Not shipped: caches, and the tests, which need the repository around them anyway.
EXCLUDE_DIRS = {"__pycache__", "_vendor", ".pytest_cache"}
EXCLUDE_SUFFIXES = {".pyc", ".pyo"}


def _iter_files(root: Path):
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if any(part in EXCLUDE_DIRS for part in path.relative_to(root).parts):
            continue
        if path.suffix in EXCLUDE_SUFFIXES:
            continue
        yield path


def build(output_dir: Path, platforms: list[str] | None) -> Path:
    if not (ADDON / "blender_manifest.toml").is_file():
        raise SystemExit(f"no blender_manifest.toml under {ADDON}")
    if not (BINDING / "__init__.py").is_file():
        raise SystemExit(f"the bsvx binding is not at {BINDING}")

    staged_binaries: list[str] = []
    bin_root = BINDING / "bin"
    if bin_root.is_dir():
        for entry in sorted(bin_root.iterdir()):
            if entry.is_dir() and (platforms is None or entry.name in platforms):
                staged_binaries.extend(f"{entry.name}/{f.name}" for f in sorted(entry.iterdir()) if f.is_file())

    if not staged_binaries:
        print(
            "warning: no shared library is staged under python/bsvx/bin/<platform>/.\n"
            "         The extension will install but will not load until one is present\n"
            "         or $BSVX_LIBRARY points at it.",
            file=sys.stderr,
        )

    output_dir.mkdir(parents=True, exist_ok=True)
    target = output_dir / "bsvx_blender.zip"

    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in _iter_files(ADDON):
            archive.write(path, Path("bsvx_blender") / path.relative_to(ADDON))

        for path in _iter_files(BINDING):
            relative = path.relative_to(BINDING)
            # bin/<platform>/ is filtered by --platform; everything else in the package ships.
            if relative.parts and relative.parts[0] == "bin":
                if len(relative.parts) < 2:
                    continue
                if platforms is not None and relative.parts[1] not in platforms:
                    continue
            archive.write(path, Path("bsvx_blender") / "_vendor" / "bsvx" / relative)

    print(f"wrote {target}")
    print(f"  binaries: {', '.join(staged_binaries) if staged_binaries else '(none)'}")
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE / "dist", help="where to write the zip")
    parser.add_argument(
        "--platform",
        action="append",
        dest="platforms",
        help="only include this staged binary directory (repeatable); default is all of them",
    )
    parser.add_argument("--clean", action="store_true", help="remove the output directory first")
    args = parser.parse_args()

    if args.clean and args.output.exists():
        shutil.rmtree(args.output)

    build(args.output, args.platforms)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

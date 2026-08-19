"""Locates the ``bsvx`` Python package and the shared library behind it.

The package is deliberately not vendored into this add-on's source tree. It lives once, in the
repository's ``python/`` directory, and ``package.py`` copies it into ``_vendor/`` when building a
shippable zip. Running from a checkout and running from an installed extension therefore differ in
exactly one place -- this module -- instead of everywhere an import appears.
"""

from __future__ import annotations

import sys
from pathlib import Path

_ADDON_DIR = Path(__file__).resolve().parent

#: Where a packaged build puts the binding, and where a source checkout keeps it.
_SEARCH = (
    _ADDON_DIR / "_vendor",
    _ADDON_DIR.parent.parent.parent / "python",
)


class BindingUnavailable(RuntimeError):
    """The bsvx package or its shared library could not be loaded."""


def _import_bsvx():
    for root in _SEARCH:
        if not (root / "bsvx" / "__init__.py").is_file():
            continue
        if str(root) not in sys.path:
            sys.path.insert(0, str(root))
        break

    try:
        import bsvx  # noqa: PLC0415  -- deliberately late; sys.path was just adjusted
    except ImportError as exc:
        searched = "\n  ".join(str(p) for p in _SEARCH)
        raise BindingUnavailable(
            f"the bsvx Python package was not found. Searched:\n  {searched}\n"
            f"({exc})"
        ) from exc
    except OSError as exc:
        # _lib raises OSError when the package imports but libbsvx does not load, which is the
        # common case: the add-on was installed without a binary for this platform.
        raise BindingUnavailable(
            f"the bsvx package loaded but its shared library did not: {exc}\n"
            "Build the library, or set $BSVX_LIBRARY to it."
        ) from exc

    return bsvx


_cached = None


def api():
    """The ``bsvx`` module, imported once. Raises BindingUnavailable with a legible message."""
    global _cached
    if _cached is None:
        _cached = _import_bsvx()
    return _cached


def available() -> tuple[bool, str]:
    """(ok, message) -- for drawing a panel instead of throwing inside a draw callback."""
    try:
        module = api()
    except BindingUnavailable as exc:
        return False, str(exc)
    return True, f"bsvx ABI {module.abi_version()}"

"""The open world, and why it is not stored in the .blend.

A ``bsvx_world*`` is a C handle. It cannot be serialized into a .blend, and a copy of it inside
Blender's data would be a second source of truth that drifts from the file on disk. So the world
lives here, in module state, and the .blend records only the *path* it came from -- enough to
reopen it after a restart, and honest about the fact that the file on disk is what matters.

One world per scene. Editing two worlds at once is a real workflow, but it multiplies every
operator's "which world?" question, and nothing in the format needs it.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field

from . import binding


@dataclass
class Session:
    world: object
    path: str = ""
    #: Set when the world was created in memory and has never been written anywhere.
    unsaved: bool = False
    warnings: list[str] = field(default_factory=list)

    def close(self) -> None:
        try:
            self.world.close()
        except Exception:
            pass


_sessions: dict[str, Session] = {}


def open_session(world, path: str = "", unsaved: bool = False, warnings=None) -> str:
    key = uuid.uuid4().hex
    _sessions[key] = Session(world=world, path=path, unsaved=unsaved, warnings=list(warnings or ()))
    return key


def get(key: str) -> Session | None:
    return _sessions.get(key) if key else None


def close(key: str) -> None:
    session = _sessions.pop(key, None)
    if session is not None:
        session.close()


def close_all() -> None:
    for key in list(_sessions):
        close(key)


# --- convenience over the scene property ------------------------------------------------------


def active(context) -> Session | None:
    settings = getattr(context.scene, "bsvx", None)
    if settings is None:
        return None
    return get(settings.session_key)


def active_world(context):
    session = active(context)
    return session.world if session else None


def require(context):
    """The open world, or a RuntimeError whose message an operator can report verbatim."""
    session = active(context)
    if session is None:
        raise RuntimeError("no BSVX world is open -- use Open or New first")
    return session


def reopen(context) -> Session:
    """Reopens the recorded path after a .blend reload dropped the handle."""
    settings = context.scene.bsvx
    if not settings.path:
        raise RuntimeError("this scene has no recorded world path to reopen")

    api = binding.api()
    world = api.World.load(settings.path, ignore_hash_mismatch=settings.ignore_hash_mismatch)
    settings.session_key = open_session(world, path=settings.path, warnings=world.warnings)
    return get(settings.session_key)

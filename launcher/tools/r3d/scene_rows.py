"""Shared SCNE row layouts and codecs, independent of the scene writer."""

import pathlib
import struct

from asset.asset_pack import NAME_BYTES
from build.c_header import read_define

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/scene/scene_format.h"
COLOUR_SET_OWN = read_define(OWNER, "SCENE_ASSET_COLOUR_SET_OWN")
U16_MAX = (1 << 16) - 1
NAME = struct.Struct(f"<{NAME_BYTES}s")
TRANSFORM_AXES = 3
TRANSFORM = struct.Struct(f"<{TRANSFORM_AXES * (TRANSFORM_AXES + 1)}f")
RENDERER = struct.Struct(f"<HH{NAME_BYTES}s")


class SceneError(ValueError):
    """An authored scene or mapped row violates the scene format."""


def field(value, what):
    raw = value.encode("ascii")
    if len(raw) >= NAME_BYTES:
        raise SceneError(f"{what} {value!r} does not fit the entry's {NAME_BYTES - 1}-byte field")
    return raw


def text_at(raw, what):
    value, nul, rest = raw.partition(b"\0")
    if not value or not nul or any(rest):
        raise SceneError(f"{what} is not NUL terminated and padded")
    return value.decode("ascii")


def decode_transform(values):
    """The matrix and position fields of one SCNE transform row."""
    end = TRANSFORM_AXES * TRANSFORM_AXES
    return {"matrix": [list(values[i:i + TRANSFORM_AXES]) for i in range(0, end, TRANSFORM_AXES)],
            "position": list(values[end:])}

"""The scene pack entry (SCNE): what a scene reads at run time, baked from its
NAME.scene.toml, and read back. The one writer of the entry, and its reader
on the host; main/scene/scene_asset.c is the firmware's.

Each object that is a mesh renderer or the camera is an entity, in file
order, found by name at run time. A renderer names its mesh by pack id and
the camera its path by clip id (the stem of the .anim.toml the scene names)
and node, all opened from the scene's bundle at scene_load(). Lights, the
camera region and the tone map are bake settings and stay offline. A
transform is baked as a 3x3 (rotation times scale) and a position, so the
device does no trigonometry. The layout is in docs/render/Scene-Files.md,
"The scene entry". Standard library only.
"""

import math
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import SettingsError, load_scene  # noqa: E402

TYPE = b"SCNE"
VERSION = 1
SUFFIX = ".scene.toml"
# version, entity, renderer and camera counts, then where the names, transforms, renderers and cameras start
HEADER = struct.Struct("<HHHHIIII")
NAME = struct.Struct("<32s")
TRANSFORM = struct.Struct("<12f")
RENDERER = struct.Struct("<HH32s")
CAMERA = struct.Struct("<HHffI32s32s")
NAME_BYTES = 32


class SceneError(ValueError):
    """A scene that cannot be baked, or bytes that are not the entry."""


def scene_id(path):
    """The pack id of a .scene.toml, and the name of its bundle: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def entities(scene):
    """The scene's objects that exist at run time, in file order: a camera or
    a mesh renderer. Lights are baked offline and have no entity."""
    return [obj for obj in scene.objects if obj.kind in ("mesh_renderer", "camera")]


def mesh_ids(scene):
    """The pack id of each renderer's mesh, in the entry's order: what scene_load() opens."""
    return [item.asset_name for item in scene.renderers]


def field(value, what):
    raw = value.encode("ascii")
    if len(raw) >= NAME_BYTES:
        raise SceneError(f"{what} {value!r} does not fit the entry's {NAME_BYTES - 1}-byte field")
    return raw


def encode(scene):
    """The entry's bytes for a scene as load_scene() returns it."""
    objects = entities(scene)
    index = {obj.name: i for i, obj in enumerate(objects)}
    cameras = [scene.camera] if scene.camera else []
    names = b"".join(NAME.pack(field(obj.name, "entity")) for obj in objects)
    transforms = b"".join(TRANSFORM.pack(*[x for row in obj.matrix for x in row], *obj.position) for obj in objects)
    renderers = b"".join(RENDERER.pack(index[item.object.name], 0, field(mesh, "mesh id"))
                         for item, mesh in zip(scene.renderers, mesh_ids(scene)))
    rows = []
    for camera in cameras:
        lens, path = camera.component, camera.component.path
        rows.append(CAMERA.pack(index[camera.name], 0, lens.half_fov_short_tan, lens.near_z, lens.background,
                                field(path.clip, "clip id") if path else b"", field(path.node, "node") if path else b""))
    parts = [names, transforms, renderers, b"".join(rows)]
    offsets, at = [], HEADER.size
    for part in parts:
        offsets.append(at)
        at += len(part)
    if any(len(items) > 0xFFFF for items in (objects, scene.renderers)):
        raise SceneError("a scene holds at most 65535 entities")
    return HEADER.pack(VERSION, len(objects), len(scene.renderers), len(cameras), *offsets) + b"".join(parts)


def bake(path):
    """The entry's bytes for a .scene.toml."""
    try:
        return encode(load_scene(path))
    except (SettingsError, OSError) as error:
        raise SceneError(f"{path}: {error}") from error


def text_at(raw, what):
    """A NUL-padded field: some text, then nothing but NULs."""
    value, nul, rest = raw.partition(b"\0")
    if not nul or any(rest):
        raise SceneError(f"{what} is not NUL terminated and padded")
    return value.decode("ascii")


def section(entry, offset, count, row, table_end, what):
    if offset % 4 or offset < table_end or offset + count * row.size > len(entry):
        raise SceneError(f"the {what} leave the entry or are misaligned")
    return [row.unpack_from(entry, offset + i * row.size) for i in range(count)]


def decode(entry):
    """The entry's contents after the checks scene_asset_open() makes: a
    dict of `entities` (name, matrix, position), `renderers` (entity,
    mesh id) and `cameras` (entity, half_fov_short_tan, near_z, clear_rgb,
    clip, node)."""
    if len(entry) < HEADER.size:
        raise SceneError("shorter than a header")
    version, entity_count, renderer_count, camera_count, *offsets = HEADER.unpack_from(entry)
    if version != VERSION:
        raise SceneError(f"version {version}, this reads {VERSION}")
    names, transforms, renderers, cameras = (
        section(entry, at, count, row, HEADER.size, what) for at, count, row, what in zip(
            offsets, (entity_count, entity_count, renderer_count, camera_count), (NAME, TRANSFORM, RENDERER, CAMERA),
            ("names", "transforms", "renderers", "cameras")))
    out = {"entities": [], "renderers": [], "cameras": []}
    for (raw,), values in zip(names, transforms):
        name = text_at(raw, "an entity name")
        if not name:
            raise SceneError("an entity has no name")
        out["entities"].append({"name": name, "matrix": [list(values[i:i + 3]) for i in range(0, 9, 3)],
                                "position": list(values[9:])})
    for entity, pad, raw in renderers:
        mesh = text_at(raw, "a mesh id")
        if entity >= entity_count or pad or not mesh:
            raise SceneError(f"renderer {mesh!r}: its entity, padding or mesh id is out of range")
        out["renderers"].append({"entity": entity, "mesh": mesh})
    for entity, pad, tan, near, clear, clip, node in cameras:
        clip, node = text_at(clip, "a clip id"), text_at(node, "a node")
        if entity >= entity_count or pad or not (math.isfinite(tan) and tan > 0 and math.isfinite(near) and near > 0) \
                or clear > 0xFFFFFF or bool(clip) != bool(node):
            raise SceneError(f"camera {entity}: a field holds a value the entry does not allow")
        out["cameras"].append({"entity": entity, "half_fov_short_tan": tan, "near_z": near, "clear_rgb": clear,
                               "clip": clip, "node": node})
    return out

"""SCNE v2 settings, lights, renderers and animators, validated against the mapped C reader."""

import math
import pathlib
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.asset_pack import NAME_BYTES
from build.c_header import read_define
from r3d.import_settings import ANIM_WRAPS, SettingsError, color_fits, load_scene
from r3d import skin_pack

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/scene/scene_internal.h"
ANIM_OWNER = OWNER.parents[1] / "anim/anim_track.h"
TYPE = b"SCNE"
VERSION = read_define(OWNER, "SCENE_ASSET_VERSION")
DIRECTIONAL = read_define(OWNER, "SCENE_LIGHT_KIND_DIRECTIONAL")
LIGHT_KINDS = {"directional": DIRECTIONAL}
DEFAULT_COLOR = read_define(OWNER, "SCENE_LIGHT_COLOR_DEFAULT")
AMBIENT_NONE = read_define(OWNER, "SCENE_AMBIENT_INTENSITY_NONE")
TONEMAP_NONE = read_define(OWNER, "SCENE_TONEMAP_WHITE_NONE")
DEFAULT_CLIP_NONE = read_define(OWNER, "SCENE_ASSET_DEFAULT_CLIP_NONE")
WRAPS = {name: read_define(ANIM_OWNER, "ANIM_WRAP_" + name.upper()) for name in ANIM_WRAPS}
SUFFIX = ".scene.toml"
PARTS = ("names", "transforms", "renderers", "cameras", "lights", "skinned_renderers", "animators", "clips", "settings")
HEADER = struct.Struct(f"<{1 + len(PARTS)}H{len(PARTS)}I")
U16_MAX = (1 << 16) - 1
NAME = struct.Struct(f"<{NAME_BYTES}s")
TRANSFORM = struct.Struct("<12f")
RENDERER = struct.Struct(f"<HH{NAME_BYTES}s")
CAMERA = struct.Struct("<HHffI")
LIGHT = struct.Struct("<HBB4f")
SKINNED_RENDERER = struct.Struct(f"<HH{NAME_BYTES}s{NAME_BYTES}s{NAME_BYTES}s")
ANIMATOR = struct.Struct("<HHIHBB")
SETTINGS = struct.Struct("<5f")
ROWS = (NAME, TRANSFORM, RENDERER, CAMERA, LIGHT, SKINNED_RENDERER, ANIMATOR, NAME, SETTINGS)


class SceneError(ValueError):
    """An authored scene or mapped row violates the scene format."""


if HEADER.size != read_define(OWNER, "SCENE_ASSET_HEADER_SIZE"):
    raise SceneError("SCNE header size disagrees with its C owner")


def scene_id(path):
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def mesh_ids(scene):
    return [item.asset_name for item in scene.renderers]


def field(value, what):
    raw = value.encode("ascii")
    if len(raw) >= NAME_BYTES:
        raise SceneError(f"{what} {value!r} does not fit the entry's {NAME_BYTES - 1}-byte field")
    return raw


def encode(scene, cache=None, offline=False):
    """Encode authored rows; resolve skin and skeleton ids through the pack's rig owner."""
    objects = scene.objects
    index = {obj.name: i for i, obj in enumerate(objects)}
    rows = {part: [] for part in PARTS}
    rows["names"] = [(field(obj.name, "entity"),) for obj in objects]
    rows["transforms"] = [(*[x for row in obj.matrix for x in row], *obj.position) for obj in objects]
    for item in scene.renderers:
        obj, mesh = item.object, item.asset_name
        entity = index[obj.name]
        if obj.kind == "skinned_renderer":
            skin, skeleton = skin_pack.rig_ids(mesh, item.settings, cache, offline)
            rows["skinned_renderers"].append((entity, 0, field(mesh, "mesh id"),
                field(skin, "skin id"), field(skeleton, "skeleton id")))
        else:
            rows["renderers"].append((entity, 0, field(mesh, "mesh id")))
    for obj in objects:
        entity = index[obj.name]
        animator = getattr(obj, "animator", None)
        if obj.kind == "light":
            light = obj.component
            if light["type"] not in LIGHT_KINDS:
                raise SceneError(f"unknown light kind {light['type']!r}")
            rows["lights"].append((entity, LIGHT_KINDS[light["type"]], 0, *light["color"], light["intensity"]))
        if obj.kind == "camera":
            lens = obj.component
            rows["cameras"].append((entity, 0, lens.half_fov_short_tan, lens.near_z, lens.background))
            if lens.path:
                animator = SimpleNamespace(clips=[lens.path.clip], default_clip=lens.path.clip, wrap="loop")
        if animator:
            clips = animator.clips
            default = DEFAULT_CLIP_NONE if animator.default_clip is None else clips.index(animator.default_clip)
            rows["animators"].append((entity, len(clips), len(rows["clips"]), default, WRAPS[animator.wrap], 0))
            rows["clips"].extend((field(clip, "clip id"),) for clip in clips)
    ambient = next((light for light in getattr(scene, "lights", []) if light["type"] == "ambient"), None)
    rows["settings"] = [(*(ambient["color"] if ambient else DEFAULT_COLOR),
                         ambient["intensity"] if ambient else AMBIENT_NONE,
                         TONEMAP_NONE if getattr(scene, "tonemap_white", None) is None else scene.tonemap_white)]
    counts = [len(rows[part]) for part in PARTS]
    if any(count > U16_MAX for count in counts):
        raise SceneError("a scene holds at most 65535 rows per part")
    parts = [b"".join(row.pack(*values) for values in rows[part]) for part, row in zip(PARTS, ROWS)]
    offsets, at = [], HEADER.size
    for part in parts:
        offsets.append(at)
        at += len(part)
    entry = HEADER.pack(VERSION, *counts, *offsets) + b"".join(parts)
    decode(entry)
    return entry


def bake(path, cache=None, offline=False):
    try:
        return encode(load_scene(path), cache, offline)
    except (SettingsError, OSError) as error:
        raise SceneError(f"{path}: {error}") from error


def text_at(raw, what):
    value, nul, rest = raw.partition(b"\0")
    if not value or not nul or any(rest):
        raise SceneError(f"{what} is not NUL terminated and padded")
    return value.decode("ascii")


def section(entry, offset, count, row, table_end, what):
    if offset % 4 or offset < table_end or offset + count * row.size > len(entry):
        raise SceneError(f"the {what} leave the entry or are misaligned")
    return [row.unpack_from(entry, offset + i * row.size) for i in range(count)]


def nonnegative(value):
    return math.isfinite(value) and value >= 0


def decode(entry):
    if len(entry) >= 2 and struct.unpack_from("<H", entry)[0] != VERSION:
        raise SceneError(f"version {struct.unpack_from('<H', entry)[0]}, this reads {VERSION}")
    if len(entry) < HEADER.size:
        raise SceneError("shorter than a header")
    _, *header = HEADER.unpack_from(entry)
    counts, offsets = header[:len(PARTS)], header[len(PARTS):]
    if counts[0] != counts[1] or counts[-1] != 1:
        raise SceneError("transforms must match names and settings must have exactly one row")
    rows = {part: section(entry, at, count, row, HEADER.size, part)
            for part, at, count, row in zip(PARTS, offsets, counts, ROWS)}
    out = {part: [] for part in PARTS if part not in ("names", "transforms")}
    out["entities"] = [{"name": text_at(raw, "entity name"),
                        "matrix": [list(values[i:i + 3]) for i in range(0, 9, 3)], "position": list(values[9:])}
                       for (raw,), values in zip(rows["names"], rows["transforms"])]
    for part in ("renderers", "skinned_renderers", "cameras", "lights", "animators"):
        seen = set()
        for row in rows[part]:
            entity = row[0]
            if entity >= counts[0] or (entity in seen and part in ("lights", "animators", "skinned_renderers")):
                raise SceneError(f"{part}: entity out of range or duplicate")
            seen.add(entity)
            if part == "animators":
                _, count, first, default, wrap, pad = row
                if pad or wrap not in WRAPS.values() or first + count > counts[7] \
                        or (default != DEFAULT_CLIP_NONE and default >= count):
                    raise SceneError("animator: invalid clip range, default_clip, wrap or padding")
                out[part].append(dict(entity=entity, clip_count=count, first_clip=first, default_clip=default, wrap=wrap))
                continue
            if part == "lights":
                _, kind, pad, *values = row
                if pad or kind != DIRECTIONAL or not color_fits(values[:3]) or not nonnegative(values[3]):
                    raise SceneError("light: invalid kind, colour, intensity or padding")
                out[part].append(dict(entity=entity, kind=kind, color=values[:3], intensity=values[3]))
                continue
            if row[1]:
                raise SceneError(f"{part}: padding out of range")
            if part == "cameras":
                _, _, tan, near, clear = row
                if not (math.isfinite(tan) and tan > 0 and math.isfinite(near) and near > 0) or clear > 0xFFFFFF:
                    raise SceneError(f"camera {entity}: a field holds a value the entry does not allow")
                out[part].append(dict(entity=entity, half_fov_short_tan=tan, near_z=near, clear_rgb=clear))
            else:
                names = ("mesh",) if part == "renderers" else ("mesh", "skin", "skeleton")
                out[part].append(dict(entity=entity, **{key: text_at(raw, key + " id") for key, raw in zip(names, row[2:])}))
    out["clips"] = [text_at(raw, "clip id") for (raw,) in rows["clips"]]
    color, intensity, white = rows["settings"][0][:3], *rows["settings"][0][3:]
    if not color_fits(color) or not nonnegative(intensity) or not nonnegative(white):
        raise SceneError("settings: invalid ambient or tonemap_white")
    out["settings"] = dict(ambient_color=list(color), ambient_intensity=intensity, tonemap_white=white)
    return out

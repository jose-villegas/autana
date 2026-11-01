"""SCNE v5 entities, parents, settings, lights, renderers and animators, validated against the mapped C reader."""

import math
import pathlib
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.asset_pack import NAME_BYTES
from build.c_header import read_define
from build.name_hash import NONE as HASH_NONE, name_hash
from r3d.import_settings import ANIM_WRAPS, SettingsError, color_fits, load_scene
from r3d import skin_pack
from r3d.scene_rows import (CLIP, COLOUR_SET_OWN, NAME, RENDERER, TRANSFORM, TRANSFORM_AXES, U16_MAX,
                            SceneError, decode_transform, field, text_at)

OWNER = pathlib.Path(__file__).resolve().parents[2] / "main/scene/scene_format.h"
INTERNAL = OWNER.with_name("scene_internal.h")
ANIM_OWNER = OWNER.parents[1] / "anim/anim_track.h"
TYPE = b"SCNE"
VERSION = read_define(OWNER, "SCENE_ASSET_VERSION")
DIRECTIONAL = read_define(OWNER, "SCENE_LIGHT_KIND_DIRECTIONAL")
LIGHT_KINDS = {"directional": DIRECTIONAL}
DEFAULT_COLOR = read_define(INTERNAL, "SCENE_LIGHT_COLOR_DEFAULT")
AMBIENT_NONE = read_define(INTERNAL, "SCENE_AMBIENT_INTENSITY_NONE")
TONEMAP_NONE = read_define(INTERNAL, "SCENE_TONEMAP_WHITE_NONE")
DEFAULT_CLIP_NONE = read_define(OWNER, "SCENE_ASSET_DEFAULT_CLIP_NONE")
PARENT_ROOT = read_define(OWNER, "SCENE_ASSET_PARENT_ROOT")
SCALE_TOLERANCE = read_define(OWNER.parents[2] / "packages/math/include/math/linear/transformf.h",
                              "TRANSFORMF_MATRIX_TOLERANCE")
WRAPS = {name: read_define(ANIM_OWNER, "ANIM_WRAP_" + name.upper()) for name in ANIM_WRAPS}
PARTS = ("names", "transforms", "renderers", "cameras", "lights", "skinned_renderers", "animators", "clips", "settings",
         "parents")
ALIGN = 4
HEADER = struct.Struct(f"<{1 + len(PARTS)}H{-2 * (1 + len(PARTS)) % ALIGN}x{len(PARTS)}I")
CAMERA = struct.Struct("<HHffI")
LIGHT = struct.Struct("<HBB4f")
SKINNED_RENDERER = struct.Struct(f"<HH{NAME_BYTES}s{NAME_BYTES}s{NAME_BYTES}s")
ANIMATOR = struct.Struct("<HHIHBB")
SETTINGS = struct.Struct("<5f")
PARENT = struct.Struct("<H")
ROWS = (NAME, TRANSFORM, RENDERER, CAMERA, LIGHT, SKINNED_RENDERER, ANIMATOR, CLIP, SETTINGS, PARENT)


def check_header(owner_size):
    if HEADER.size != owner_size:
        raise SceneError(f"scene header size {HEADER.size} differs from {owner_size} in {OWNER}")


check_header(read_define(OWNER, "SCENE_ASSET_HEADER_SIZE"))


def mesh_ids(scene):
    return [item.asset_name for item in scene.renderers]


def encode(scene, cache=None, offline=False):
    """Encode authored rows; resolve skin and skeleton ids through the pack's rig owner."""
    objects = scene.objects
    index = {obj.name: i for i, obj in enumerate(objects)}
    rows = {part: [] for part in PARTS}
    rows["names"] = name_rows(objects)
    rows["parents"] = [(parent_index(obj, index),) for obj in objects]
    for obj in objects:
        if obj.kind in PLAIN_ROOT_KINDS and mirrors(obj.matrix):
            raise SceneError(f"{obj.name}: a {obj.kind} cannot mirror")
    drawn = {item.object.name for item in scene.renderers}
    placements = fold_parent_scales(objects, [parent for (parent,) in rows["parents"]], drawn)
    rows["transforms"] = [(*[x for row in matrix for x in row], *position) for matrix, position in placements]
    for item in scene.renderers:
        obj, mesh = item.object, item.asset_name
        entity = index[obj.name]
        if obj.kind == "skinned_renderer":
            skin, skeleton = skin_pack.rig_ids(mesh, item.settings, cache, offline)
            rows["skinned_renderers"].append((entity, 0, field(mesh, "mesh id"),
                field(skin, "skin id"), field(skeleton, "skeleton id")))
        else:
            colour_set = getattr(item.renderer, "colour_set", None)
            rows["renderers"].append((entity, COLOUR_SET_OWN if colour_set is None else colour_set, field(mesh, "mesh id")))
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
    if any(count > U16_MAX for count in counts) or len(objects) >= PARENT_ROOT:
        raise SceneError(f"a scene holds at most {U16_MAX} rows per part and {PARENT_ROOT - 1} entities")
    parts = [b"".join(row.pack(*values) for values in rows[part]) for part, row in zip(PARTS, ROWS)]
    parts = [part + bytes(-len(part) % ALIGN) for part in parts]
    offsets, at = [], HEADER.size
    for part in parts:
        offsets.append(at)
        at += len(part)
    entry = HEADER.pack(VERSION, *counts, *offsets) + b"".join(parts)
    decode(entry)
    return entry


def name_rows(objects):
    """Each entity's (name_hash, name); two names that share a hash, or one that hashes to NONE, are refused."""
    rows, seen = [], {}
    for obj in objects:
        hashed = name_hash(obj.name)
        if hashed == HASH_NONE:
            raise SceneError(f"entity {obj.name!r} hashes to NONE; rename it")
        if seen.setdefault(hashed, obj.name) != obj.name:
            raise SceneError(f"entities {seen[hashed]!r} and {obj.name!r} share a name hash; rename one")
        rows.append((hashed, field(obj.name, "entity")))
    return rows


def parent_index(obj, index):
    """The parent's entity index, PARENT_ROOT for none; a parent comes before its child."""
    parent = getattr(obj, "parent", None)
    if parent is None:
        return PARENT_ROOT
    if parent not in index or index[parent] >= index[obj.name]:
        raise SceneError(f"{obj.name}: parent {parent!r} must be an entity listed before it")
    if obj.kind in PLAIN_ROOT_KINDS:
        raise SceneError(f"{obj.name}: a {obj.kind} has no parent yet")
    return index[parent]


# A light's direction, a camera's view and a skin's normals read the entity's own rotation: a root that does not mirror.
PLAIN_ROOT_KINDS = ("camera", "light", "skinned_renderer")


def mirrors(matrix):
    """True when the 3x3 matrix has a negative determinant: the device draws it mirrored."""
    (a, b, c), (d, e, f), (g, h, i) = matrix
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g) < 0


def column_scales(matrix):
    return [math.sqrt(sum(matrix[r][c] ** 2 for r in range(3))) for c in range(3)]


def scale_is_even(scales):
    return max(scales) - min(scales) <= SCALE_TOLERANCE * max(scales)


def fold_parent_scales(objects, parents, drawn):
    """Each object's (matrix, position), with a parent's uneven scale moved down into its children.

    A parent keeps an even scale so every world below it is exact; the device refuses one that does not.
    Folding scale S out of a parent and into each child (S x matrix, S x position) draws the same, while
    each child stays rotation times scale. A parent that draws, or a child the fold would shear, is refused.
    """
    placements = [([list(row) for row in obj.matrix], list(obj.position)) for obj in objects]
    has_children = {parent for parent in parents if parent != PARENT_ROOT}
    for entity, obj in enumerate(objects):
        matrix, _ = placements[entity]
        scales = column_scales(matrix)
        if entity in has_children and mirrors(matrix):
            raise SceneError(f"{obj.name}: a parent cannot mirror; mirror its children instead")
        if entity not in has_children or scale_is_even(scales):
            continue
        if obj.name in drawn:
            raise SceneError(f"{obj.name}: a parent that draws needs an even scale, the same on every axis")
        placements[entity] = ([[matrix[r][c] / scales[c] for c in range(3)] for r in range(3)], placements[entity][1])
        for child, parent in enumerate(parents):
            if parent != entity:
                continue
            child_matrix, child_position = placements[child]
            child_matrix = [[scales[r] * child_matrix[r][c] for c in range(3)] for r in range(3)]
            columns = [[child_matrix[r][c] for r in range(3)] for c in range(3)]
            lengths = column_scales(child_matrix)
            for a, b in ((0, 1), (0, 2), (1, 2)):
                if abs(sum(x * y for x, y in zip(columns[a], columns[b]))) > SCALE_TOLERANCE * lengths[a] * lengths[b]:
                    raise SceneError(f"{obj.name}: its uneven scale would shear its turned child "
                                     f"{objects[child].name}; scale the child's mesh instead")
            placements[child] = (child_matrix, [scales[r] * child_position[r] for r in range(3)])
    return placements


def bake(path, cache=None, offline=False):
    try:
        return encode(load_scene(path), cache, offline)
    except (SettingsError, OSError) as error:
        raise SceneError(f"{path}: {error}") from error


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
    if any(entry[2 * (1 + len(PARTS)):HEADER.size - 4 * len(PARTS)]):
        raise SceneError("the header's padding is not zero")
    _, *header = HEADER.unpack_from(entry)
    counts, offsets = header[:len(PARTS)], header[len(PARTS):]
    settings, parents = PARTS.index("settings"), PARTS.index("parents")
    if counts[0] != counts[1] or counts[0] != counts[parents] or counts[settings] != 1 or counts[0] >= PARENT_ROOT:
        raise SceneError("transforms and parents must match names and settings must have exactly one row")
    rows = {part: section(entry, at, count, row, HEADER.size, part)
            for part, at, count, row in zip(PARTS, offsets, counts, ROWS)}
    parents_end = offsets[parents] + counts[parents] * PARENT.size
    if any(entry[parents_end:parents_end + (-parents_end % ALIGN)]):
        raise SceneError("the padding after the parents is not zero")
    out = {part: [] for part in PARTS if part not in ("names", "transforms")}
    out["entities"] = [{"name": text_at(raw, "entity name"), "name_hash": hashed, **decode_transform(values),
                        "parent": None if parent == PARENT_ROOT else parent}
                       for (hashed, raw), values, (parent,) in zip(rows["names"], rows["transforms"], rows["parents"])]
    for entity, row in enumerate(out["entities"]):
        if row["name_hash"] == HASH_NONE or row["name_hash"] != name_hash(row["name"]):
            raise SceneError(f"entity {entity}: the stored name hash is NONE or not the name's")
        if row["parent"] is not None and row["parent"] >= entity:
            raise SceneError(f"entity {entity}: parent {row['parent']} is not listed before it")
        parent_matrix = None if row["parent"] is None else out["entities"][row["parent"]]["matrix"]
        if parent_matrix is not None and (not scale_is_even(column_scales(parent_matrix)) or mirrors(parent_matrix)):
            raise SceneError(f"entity {row['parent']}: a parent's scale is not even or mirrors")
    for part in ("renderers", "skinned_renderers", "cameras", "lights", "animators"):
        seen = set()
        for row in rows[part]:
            entity = row[0]
            if entity >= counts[0] or (entity in seen and part in ("lights", "animators", "skinned_renderers")):
                raise SceneError(f"{part}: entity out of range or duplicate")
            if part in ("cameras", "lights", "skinned_renderers") and (out["entities"][entity]["parent"] is not None
                                                                       or mirrors(out["entities"][entity]["matrix"])):
                raise SceneError(f"{part}: entity {entity} has a parent or mirrors, which this part does not allow")
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
            if part != "renderers" and row[1]:
                raise SceneError(f"{part}: padding out of range")
            if part == "cameras":
                _, _, tan, near, clear = row
                if not (math.isfinite(tan) and tan > 0 and math.isfinite(near) and near > 0) or clear > 0xFFFFFF:
                    raise SceneError(f"camera {entity}: a field holds a value the entry does not allow")
                out[part].append(dict(entity=entity, half_fov_short_tan=tan, near_z=near, clear_rgb=clear))
            else:
                names = ("mesh",) if part == "renderers" else ("mesh", "skin", "skeleton")
                out[part].append(dict(entity=entity, **{key: text_at(raw, key + " id") for key, raw in zip(names, row[2:])}))
                if part == "renderers":
                    out[part][-1]["colour_set"] = row[1]
    out["clips"] = [text_at(raw, "clip id") for (raw,) in rows["clips"]]
    color, intensity, white = rows["settings"][0][:3], *rows["settings"][0][3:]
    if not color_fits(color) or not nonnegative(intensity) or not nonnegative(white):
        raise SceneError("settings: invalid ambient or tonemap_white")
    out["settings"] = dict(ambient_color=list(color), ambient_intensity=intensity, tonemap_white=white)
    return out

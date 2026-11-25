"""Write and validate scene entries against the mapped SCNE reader."""

import math
import pathlib
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.asset_pack import NAME_BYTES
from anim import tracks_asset
from build.c_header import read_define
from build.name_hash import NONE as HASH_NONE, name_hash
from r3d.import_settings import (ANIM_WRAPS, BINDING_MODES, CONSTRAINT_SOURCES_MAX, WORLD_UP_OBJECT_TYPES,
                                 WORLD_UP_TYPES, SettingsError, color_fits, load_scene, rotation_matrix)
from r3d import skin_pack
from r3d.float32 import f32
from r3d.mesh_asset import checked_struct
from r3d.batch import copy_batches
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
         "parents", "batches", "chunks", "places", "turns", "timeline", "position_constraints", "aim_constraints",
         "sources", "hinges")
ALIGN = 4
HEADER = struct.Struct(f"<{1 + len(PARTS)}H{-2 * (1 + len(PARTS)) % ALIGN}x{len(PARTS)}I")
CAMERA = struct.Struct("<HHffI")
LIGHT = struct.Struct("<HBB4f")
SKINNED_RENDERER = struct.Struct(f"<HH{NAME_BYTES}s{NAME_BYTES}s{NAME_BYTES}s")
ANIMATOR = struct.Struct("<HHIHBB")
SETTINGS = struct.Struct("<5f")
PARENT = struct.Struct("<H")
BINDS = {name: read_define(OWNER, "SCENE_ASSET_BIND_" + name.upper())
         for name in BINDING_MODES}
WORLD_UP = {name: read_define(OWNER, "SCENE_ASSET_WORLD_UP_" + name.upper()) for name in WORLD_UP_TYPES}
WORLD_UP_OBJECT_NONE = read_define(OWNER, "SCENE_ASSET_WORLD_UP_OBJECT_NONE")
AIM_ROTATION_TOLERANCE = read_define(OWNER, "SCENE_ASSET_AIM_ROTATION_TOLERANCE")
HINGE_AXIS_TOLERANCE = read_define(OWNER, "SCENE_ASSET_HINGE_AXIS_TOLERANCE")


VECTOR = f"{TRANSFORM_AXES}f"
TIMELINE_PAD_BYTES = read_define(OWNER, "SCENE_ASSET_TIMELINE_PAD_BYTES")
TIMELINE = checked_struct(OWNER, "SCENE_ASSET_TIMELINE",
                          (("CLIP", f"{NAME_BYTES}s"), ("WRAP", "B"), ("PAD", f"{TIMELINE_PAD_BYTES}s")),
                          size_suffix="SIZE")
POSITION_CONSTRAINT = checked_struct(
    OWNER, "SCENE_ASSET_POSITION_CONSTRAINT",
    (("ENTITY", "H"), ("BINDING_MODE", "B"), ("SOURCE_COUNT", "B"), ("SOURCE_FIRST", "H"), ("PAD", "H"),
     ("WEIGHT", "f"), ("TRANSLATION_OFFSET", VECTOR)), size_suffix="SIZE")
AIM_CONSTRAINT = checked_struct(
    OWNER, "SCENE_ASSET_AIM_CONSTRAINT",
    (("ENTITY", "H"), ("WORLD_UP_TYPE", "B"), ("SOURCE_COUNT", "B"), ("SOURCE_FIRST", "H"),
     ("WORLD_UP_OBJECT", "H"), ("WEIGHT", "f"), ("AIM_VECTOR", VECTOR), ("UP_VECTOR", VECTOR),
     ("WORLD_UP_VECTOR", VECTOR), ("ROTATION_OFFSET", f"{copy_batches.QUATERNION}f")), size_suffix="SIZE")
CONSTRAINT_SOURCE = checked_struct(OWNER, "SCENE_ASSET_CONSTRAINT_SOURCE", (("TARGET", "I"), ("WEIGHT", "f")),
                                  size_suffix="SIZE")
HINGE = checked_struct(OWNER, "SCENE_ASSET_HINGE",
                       (("ENTITY", "H"), ("PAD", "H"), ("AXIS", VECTOR), ("MIN", "f"), ("MAX", "f"), ("ANGLE", "f")),
                       size_suffix="SIZE")

ROWS = (NAME, TRANSFORM, RENDERER, CAMERA, LIGHT, SKINNED_RENDERER, ANIMATOR, CLIP, SETTINGS, PARENT,
        copy_batches.BATCH, copy_batches.CHUNK, copy_batches.PLACE, copy_batches.TURN, TIMELINE,
        POSITION_CONSTRAINT, AIM_CONSTRAINT, CONSTRAINT_SOURCE, HINGE)
# A batch's renderer row opens its mesh at this colour set; each copy names its own.
BATCH_COLOUR_SET = 0


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
    timeline = getattr(scene, "timeline", None)
    if timeline:
        rows["timeline"].append((field(timeline.clip, "timeline clip"), WRAPS[timeline.wrap], b""))
    rows["parents"] = [(parent_index(obj, index),) for obj in objects]
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
        elif getattr(item.renderer, "copies", None):
            chunks, places, turns = copy_batches.pack(item.renderer.copies)
            rows["batches"].append((len(rows["renderers"]), len(places), len(rows["places"]), len(rows["chunks"])))
            rows["renderers"].append((entity, BATCH_COLOUR_SET, field(mesh, "mesh id")))
            rows["chunks"].extend(chunks)
            rows["places"].extend(places)
            rows["turns"].extend(turns)
        else:
            colour_set = getattr(item.renderer, "colour_set", None)
            rows["renderers"].append((entity, COLOUR_SET_OWN if colour_set is None else colour_set, field(mesh, "mesh id")))
    for obj in objects:
        entity = index[obj.name]
        animator = getattr(obj, "animator", None)
        position = getattr(obj, "position_constraint", None)
        if position is not None:
            count, first = append_constraint_sources(rows["sources"], position, index, f"{obj.name}.position_constraint")
            rows["position_constraints"].append((entity, BINDS[position.binding_mode], count, first, 0,
                                                 position.weight, *position.translation_offset))
        aim = getattr(obj, "aim_constraint", None)
        if aim is not None:
            where = f"{obj.name}.aim_constraint"
            count, first = append_constraint_sources(rows["sources"], aim, index, where)
            up_object = WORLD_UP_OBJECT_NONE
            if aim.world_up_object is not None:
                if aim.world_up_object not in index:
                    raise SceneError(f"{where}.world_up_object: unknown object {aim.world_up_object!r}")
                up_object = index[aim.world_up_object]
            rotation = copy_batches.quaternion(rotation_matrix(aim.rotation_offset))
            rows["aim_constraints"].append((entity, WORLD_UP[aim.world_up_type], count, first, up_object,
                                            aim.weight, *aim.aim_vector, *aim.up_vector, *aim.world_up_vector, *rotation))
        hinge = getattr(obj, "hinge", None)
        if hinge is not None:
            rows["hinges"].append((entity, 0, *hinge.axis, hinge.min, hinge.max, hinge.angle))
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
    parts = []
    for part, row in zip(PARTS, ROWS):
        try:
            parts.append(b"".join(row.pack(*values) for values in rows[part]))
        except (OverflowError, struct.error) as error:
            raise SceneError(f"{part}: value does not fit its field") from error
    parts = [part + bytes(-len(part) % ALIGN) for part in parts]
    offsets, at = [], HEADER.size
    for part in parts:
        offsets.append(at)
        at += len(part)
    entry = HEADER.pack(VERSION, *counts, *offsets) + b"".join(parts)
    decode(entry)
    return entry


def append_constraint_sources(rows, constraint, index, where):
    first = len(rows)
    for source in constraint.sources:
        if source.target not in index:
            raise SceneError(f"{where}.sources.target: unknown object {source.target!r}")
        rows.append((name_hash(source.target), source.weight))
    return len(constraint.sources), first


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
    if obj.kind in ("camera", "light", "skinned_renderer"):
        raise SceneError(f"{obj.name}: a {obj.kind} has no parent yet")
    return index[parent]


def world_placement(scene, obj):
    """The authored object-to-world transform, composed in source axes."""
    import numpy as np

    index = {item.name: i for i, item in enumerate(scene.objects)}
    matrix, position = np.asarray(obj.matrix), np.asarray(obj.position)
    parent = parent_index(obj, index)
    while parent != PARENT_ROOT:
        obj = scene.objects[parent]
        position = obj.matrix @ position + obj.position
        matrix = obj.matrix @ matrix
        parent = parent_index(obj, index)
    return SimpleNamespace(matrix=matrix, position=position)


def column_scales(matrix):
    return [math.sqrt(sum(matrix[r][c] ** 2 for r in range(3))) for c in range(3)]


def scale_is_even(scales):
    return max(scales) - min(scales) <= SCALE_TOLERANCE * max(abs(s) for s in scales)


def check_clip_scales(scene, clips):
    """Refuse uneven parent scale curves: they would shear turned children, whose worlds hold TRS.

    Clips absent from this pack are skipped.
    """
    entities = scene["entities"]
    parents = {row["parent"] for row in entities if row["parent"] is not None}
    parent_hashes = {entities[index]["name_hash"] for index in parents}
    owners = [(row["clip"], None) for row in scene.get("timeline", ())]
    for animator in scene["animators"]:
        first = animator["first_clip"]
        owners.extend((clip, entities[animator["entity"]]["name"])
                      for clip in scene["clips"][first:first + animator["clip_count"]])
    for clip, owner in owners:
        for track in clips.get(clip, []):
            if (track["root"] != tracks_asset.ROOT_SCENE or track["component"] != tracks_asset.TRANSFORM
                    or track["field"] != "scale"):
                continue
            target = owner if track["path"] == tracks_asset.TARGET_SELF else track["path"]
            if target is None or name_hash(target) not in parent_hashes:
                continue
            runs = tracks_asset.CUBIC_RUNS if track["interpolation"] == "CUBICSPLINE" else 1
            for index, value in enumerate(track["values"]):
                if not scale_is_even(value):
                    time = track["times"][index // runs]
                    raise SceneError(f"clip {clip}: entity {target}: scale is not even at {time:g}s")


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
    if offset % ALIGN or offset < table_end or offset + count * row.size > len(entry):
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
    ranges = sorted((at, at + count * row.size + (-(count * row.size) % ALIGN), part)
                    for part, at, count, row in zip(PARTS, offsets, counts, ROWS) if count)
    for (_, end, left), (start, _, right) in zip(ranges, ranges[1:]):
        if end > start:
            raise SceneError(f"{left} and {right}: sections overlap")
    timeline = PARTS.index("timeline")
    if counts[timeline] > 1:
        raise SceneError("timeline: at most one row")
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
        if row["parent"] is not None and not scale_is_even(column_scales(out["entities"][row["parent"]]["matrix"])):
            raise SceneError(f"entity {row['parent']}: a parent's scale is not even")
    for part in ("renderers", "skinned_renderers", "cameras", "lights", "animators"):
        seen = set()
        for row in rows[part]:
            entity = row[0]
            if entity >= counts[0] or (entity in seen and part in ("lights", "animators", "skinned_renderers")):
                raise SceneError(f"{part}: entity out of range or duplicate")
            if part in ("cameras", "lights", "skinned_renderers") and out["entities"][entity]["parent"] is not None:
                raise SceneError(f"{part}: entity {entity} has a parent, which this part does not allow yet")
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
    decode_motion_rows(rows, out)
    out["batches"] = decode_batches(rows, out["renderers"])
    for part in ("chunks", "places", "turns"):
        del out[part]
    out["clips"] = [text_at(raw, "clip id") for (raw,) in rows["clips"]]
    color, intensity, white = rows["settings"][0][:3], *rows["settings"][0][3:]
    if not color_fits(color) or not nonnegative(intensity) or not nonnegative(white):
        raise SceneError("settings: invalid ambient or tonemap_white")
    out["settings"] = dict(ambient_color=list(color), ambient_intensity=intensity, tonemap_white=white)
    return out


def decode_motion_rows(rows, out):
    for clip, wrap, pad in rows["timeline"]:
        if any(pad):
            raise SceneError("timeline: invalid padding")
        if wrap not in WRAPS.values():
            raise SceneError("timeline: invalid wrap")
        out["timeline"].append(dict(clip=text_at(clip, "timeline clip"), wrap=wrap))
    entities = out["entities"]
    targets = {row["name_hash"] for row in entities}
    for target, weight in rows["sources"]:
        if target not in targets or not 0 <= weight <= 1:
            raise SceneError("constraint source: invalid target or weight")
        out["sources"].append(dict(target=target, weight=weight))
    for part in ("position_constraints", "aim_constraints", "hinges"):
        seen = set()
        for entity, *row in rows[part]:
            if entity >= len(entities) or entity in seen:
                raise SceneError(f"{part}: entity out of range or duplicate")
            seen.add(entity)
            if part == "hinges":
                decoded = decode_hinge(row)
            else:
                decoded = decode_constraint(part, row, len(entities), len(rows["sources"]))
            out[part].append(dict(entity=entity, **decoded))


def decode_constraint(part, row, entity_count, total_sources):
    mode, count, first, reference, weight, *values = row
    if not 1 <= count <= CONSTRAINT_SOURCES_MAX or first + count > total_sources:
        raise SceneError(f"{part}: invalid sources range")
    if not 0 <= weight <= 1:
        raise SceneError(f"{part}: invalid weight or nonfinite vector or rotation")
    fields = dict(source_count=count, source_first=first, weight=weight)
    if part == "position_constraints":
        if not all(map(math.isfinite, values)):
            raise SceneError(f"{part}: invalid weight or nonfinite vector or rotation")
        if mode not in BINDS.values() or reference:
            raise SceneError(f"{part}: invalid binding_mode or padding")
        return dict(**fields, binding_mode=mode, translation_offset=tuple(values))
    if mode not in WORLD_UP.values():
        raise SceneError(f"{part}: invalid world_up_type")
    object_up = mode in (WORLD_UP[name] for name in WORLD_UP_OBJECT_TYPES)
    if not (reference < entity_count if object_up else reference == WORLD_UP_OBJECT_NONE):
        raise SceneError(f"{part}: invalid world_up_object")
    aim = tuple(values[:TRANSFORM_AXES])
    up = tuple(values[TRANSFORM_AXES:2 * TRANSFORM_AXES])
    world_up = tuple(values[2 * TRANSFORM_AXES:3 * TRANSFORM_AXES])
    rotation = tuple(values[3 * TRANSFORM_AXES:])
    directions = [("aim_vector", aim), ("up_vector", up)]
    if mode in (WORLD_UP["vector"], WORLD_UP["object_rotation_up"]):
        directions.append(("world_up_vector", world_up))
    for field, direction in directions:
        if not all(map(math.isfinite, direction)) or not any(direction):
            raise SceneError(f"{part}: {field} must be finite and nonzero")
    if not all(map(math.isfinite, rotation)):
        raise SceneError(f"{part}: invalid weight or nonfinite vector or rotation")
    length_squared = 0.0
    for value in rotation:
        length_squared = f32(length_squared + f32(value * value))
    if abs(f32(length_squared - 1)) > f32(AIM_ROTATION_TOLERANCE):
        raise SceneError(f"{part}: rotation_offset must be a unit quaternion")
    return dict(**fields, world_up_type=mode, world_up_object=reference, aim_vector=aim, up_vector=up,
                world_up_vector=world_up, rotation_offset=rotation)


def decode_hinge(row):
    pad, *values = row
    axis = values[:TRANSFORM_AXES]
    low, high, angle = values[TRANSFORM_AXES:]
    x, y, z = (f32(value * value) for value in axis)
    length_sq = f32(f32(x + y) + z)
    if pad or not abs(f32(length_sq - 1)) <= f32(HINGE_AXIS_TOLERANCE):
        raise SceneError("hinge: invalid unit axis or padding")
    if not all(map(math.isfinite, (low, high, angle))) or low > high:
        raise SceneError("hinge: invalid min, max or angle")
    return dict(axis=tuple(axis), min=low, max=high, angle=angle)


def decode_batches(rows, renderers):
    """Each batch's renderer and copies, (matrix rows, position, colour_set), checked as scene_asset_open() does."""
    if len(rows["turns"]) != len(rows["places"]):
        raise SceneError("batches: places and turns differ in count")
    out, copies, chunks, seen = [], 0, 0, set()
    for renderer, count, first_copy, first_chunk in rows["batches"]:
        if renderer >= len(renderers) or renderer in seen or count == 0:
            raise SceneError("batch: renderer out of range or repeated, or no copies")
        if first_copy != copies or first_chunk != chunks:
            raise SceneError("batch: copies or chunks out of turn")
        if renderers[renderer]["colour_set"] != BATCH_COLOUR_SET:
            raise SceneError(f"batch: renderer {renderer} must open colour set {BATCH_COLOUR_SET}")
        seen.add(renderer)
        runs = copy_batches.chunk_count(count)
        chunk_rows = rows["chunks"][chunks:chunks + runs]
        for *centre, reach in chunk_rows:
            if not all(math.isfinite(v) for v in centre) or not nonnegative(reach):
                raise SceneError("chunk: a centre is not finite or a reach is negative")
        turn_rows = rows["turns"][copies:copies + count]
        if any(turn[-1] >= copy_batches.QUATERNION for turn in turn_rows):
            raise SceneError("turn: largest names no quaternion component")
        out.append(dict(renderer=renderer, copies=copy_batches.unpack(
            chunk_rows, rows["places"][copies:copies + count], turn_rows)))
        copies += count
        chunks += runs
    if copies != len(rows["places"]) or chunks != len(rows["chunks"]):
        raise SceneError("batches: copies or chunks left over")
    return out

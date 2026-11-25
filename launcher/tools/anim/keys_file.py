"""Build camera shorthand and scene track keys (NAME.keys.toml) into an in-memory glTF binary."""
import math
import pathlib
import struct
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import scene_fields
from anim.tracks_asset import (BLEND_FIELD, CLIP_FIELD, DURATION_EXTRA, DURATION_MAX, SPEED_FIELD,
                               check_block_timing, seconds_to_ms)
from build.c_enum import ASSET_TAG_WIDTH, read_define, tag_text, tag_value
from build.name_hash import NONE, name_hash
from geometry.vectors import qmul, qrot, unit
from gltf.gltf_write import build_glb

INTERPOLATIONS = {"step": "STEP", "linear": "LINEAR", "cubic": "CUBICSPLINE"}
QUAT_UNIT_TOLERANCE = 1e-5
IDENTITY_ROTATION = (0, 0, 0, 1)
ZERO_TRANSLATION = (0, 0, 0)
F32 = struct.Struct("<f")
CUBIC_ROWS = 3
CUBIC_VALUE = 1
ANIMATOR_LIMITS = {
    SPEED_FIELD: read_define(scene_fields.MAIN / "anim/playback/anim_playhead.h", "ANIM_PLAYHEAD_SPEED_MAX"),
    BLEND_FIELD: read_define(scene_fields.ANIMATOR_HEADER, "ANIMATOR_BLEND_MS_MAX"),
}



def rotation(eye, target):
    """glTF xyzw rotation with local -Z forward and world +Y up."""
    forward = [b - a for a, b in zip(eye, target)]
    if sum(v * v for v in forward) == 0 or forward[0] == forward[2] == 0:
        raise ValueError("look_at must differ from eye and not lie on its vertical axis")
    yaw = math.atan2(-forward[0], -forward[2])
    pitch = math.atan2(forward[1], math.hypot(forward[0], forward[2]))
    sy, cy = math.sin(yaw / 2), math.cos(yaw / 2)
    sp, cp = math.sin(pitch / 2), math.cos(pitch / 2)
    return [cy * sp, sy * cp, -sy * sp, cy * cp]


def camera_glb(values):
    if set(values) - {"duration"} != {"node", "animation", "keys"}:
        raise ValueError("expected node, animation and keys")
    if any(not isinstance(values[name], str) or not values[name] for name in ("node", "animation")):
        raise ValueError("node and animation must be nonempty names")
    keys = values["keys"]
    if not isinstance(keys, list) or len(keys) < 2:
        raise ValueError("at least two keys are required")
    eyes, rotations = [], []
    for key in keys:
        if not isinstance(key, dict) or set(key) != {"t", "eye", "look_at"}:
            raise ValueError("each key needs t, eye and look_at")
        numbers = []
        for name in ("eye", "look_at"):
            if not isinstance(key[name], list) or len(key[name]) != 3:
                raise ValueError(f"{name} must have three coordinates")
            numbers.extend(key[name])
        if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in numbers):
            raise ValueError("key coordinates and times must be finite numbers")
        q = rotation(key["eye"], key["look_at"])
        if rotations and opposite_sign(q, rotations[-1]):
            q = [-v for v in q]
        eyes.append(key["eye"])
        rotations.append(q)
    times = key_times(keys, "camera")
    loop = eyes[-1] == eyes[0] and keys[-1]["look_at"] == keys[0]["look_at"]
    translations = catmull_rom(times, eyes, loop)
    channels = [dict(node=0, path="translation", interpolation="CUBICSPLINE", times=times, values=translations),
                dict(node=0, path="rotation", interpolation="LINEAR", times=times, values=rotations)]
    return build_glb([dict(name=values["node"], camera=0)],
                     [animation_glb(values, channels)],
                     [dict(type="perspective", perspective=dict(yfov=1.0, znear=0.1))])


def animation_glb(values, channels):
    """Carry the authored clip end through GLB without adding synthetic keys."""
    animation = dict(name=values["animation"], channels=channels)
    if "duration" in values:
        seconds = values["duration"]
        float32(seconds, "duration", "seconds")
        end = seconds_to_ms(seconds)
        if not 0 < end <= DURATION_MAX:
            raise ValueError("duration must be positive and fit the track duration in milliseconds")
        if any(channel["times"][-1] > seconds for channel in channels):
            raise ValueError("a track key is past the duration")
        animation["extras"] = {DURATION_EXTRA: end}
    return animation


def opposite_sign(a, b):
    return sum(x * y for x, y in zip(a, b)) < 0


def catmull_rom(times, values, loop=False):
    # Catmull-Rom (Catmull and Rom 1974) with time-scaled finite differences
    # and wrapped neighbors for a closed loop.
    if len(times) == 1:
        return [[0] * len(values[0]), values[0], [0] * len(values[0])]
    rows = []
    for i, value in enumerate(values):
        before, after = max(0, i - 1), min(len(times) - 1, i + 1)
        dt = times[after] - times[before]
        if loop and i in (0, len(times) - 1):
            before, after = len(times) - 2, 1
            dt = times[-1] - times[before] + times[after]
        tangent = [(b - a) / dt for a, b in zip(values[before], values[after])]
        rows.extend((tangent, value, tangent))
    return rows


def float32(value, where, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}: {name} must contain finite numbers")
    try:
        rounded, = F32.unpack(F32.pack(value))
    except (OverflowError, struct.error) as error:
        raise ValueError(f"{where}: {name} must fit in float32") from error
    if not math.isfinite(rounded):
        raise ValueError(f"{where}: {name} must contain finite numbers")
    return rounded


def key_times(keys, where, start_at_zero=True):
    """Validate the timeline at stored precision and return the authored times."""
    times = []
    previous = None
    for i, key in enumerate(keys):
        time = key["t"]
        label = f"{where} key {i}"
        rounded = float32(time, label, "t")
        if previous is None:
            if time < 0 or (start_at_zero and time != 0):
                rule = "start at zero" if start_at_zero else "be nonnegative"
                raise ValueError(f"{label}: times must {rule}")
        elif rounded <= previous:
            raise ValueError(f"{label}: times must increase strictly in float32")
        times.append(time)
        previous = rounded
    return times


def vector(value, width, where, name, quaternion=False):
    row = value if isinstance(value, (list, tuple)) else [value] if width == 1 else ()
    if len(row) != width:
        raise ValueError(f"{where}: {name} needs {width} components")
    for number in row:
        float32(number, where, name)
    if quaternion and not math.isclose(math.hypot(*row), 1, rel_tol=0, abs_tol=QUAT_UNIT_TOLERANCE):
        raise ValueError(f"{where}: {name} must be a unit quaternion (xyzw)")
    return tuple(row)


def offset_rows(field, offset, rows, cubic, where):
    if field not in ("position", "rotation"):
        raise ValueError(f"{where}: offset is only allowed on position and rotation")
    if not isinstance(offset, dict) or set(offset) - {"position", "rotation"}:
        raise ValueError(f"{where}: offset needs a position and/or rotation")
    position = vector(offset.get("position", ZERO_TRANSLATION), len(ZERO_TRANSLATION), where, "offset position")
    rotation = unit(vector(offset.get("rotation", IDENTITY_ROTATION), len(IDENTITY_ROTATION), where,
                           "offset rotation", quaternion=True))
    if field == "rotation":
        return [qmul(rotation, row) for row in rows]
    rotated = [qrot(rotation, row) for row in rows]
    for i in range(CUBIC_VALUE if cubic else 0, len(rows), CUBIC_ROWS if cubic else 1):
        rotated[i] = tuple(x + t for x, t in zip(rotated[i], position))
    return rotated


def curve_keys(track, label):
    """Validate key tables before reading or merging them."""

    keys = track["keys"]
    if not isinstance(keys, list) or not keys:
        raise ValueError(f"{label} key 0: keys must be a nonempty list")
    for i, key in enumerate(keys):
        if not isinstance(key, dict) or not {"t", "v"} <= key.keys() or key.keys() - {"t", "v", "in", "out"}:
            raise ValueError(f"{label} key {i}: each key needs t and v, with optional in and out")
    return keys


def track_curve(track, label, value_type, interp, transform_field, start_at_zero=True):
    keys = curve_keys(track, label)
    times = key_times(keys, label, start_at_zero=start_at_zero)
    values, rows = [], []
    width = scene_fields.WIDTHS[value_type]
    cubic = interp == INTERPOLATIONS["cubic"]
    quaternion = value_type == scene_fields.VALUE_QUAT
    explicit = None
    for i, key in enumerate(keys):
        where = f"{label} key {i}"
        authored = key["v"]
        if value_type == scene_fields.VALUE_NAME:
            if not isinstance(authored, str) or not authored or "\0" in authored:
                raise ValueError(f"{where}: v must be a nonempty name without NUL")
            hashed = name_hash(authored)
            if hashed == NONE:
                raise ValueError(f"{where}: name hashes to NONE")
            authored = scene_fields.encode_name(hashed)
        value = vector(authored, width, where, "v", quaternion=quaternion)
        has_tangents = "in" in key
        if has_tangents != ("out" in key):
            raise ValueError(f"{where}: supply both in and out tangents")
        if has_tangents and not cubic:
            raise ValueError(f"{where}: tangents require cubic interpolation")
        if explicit is not None and has_tangents != explicit:
            raise ValueError(f"{where}: supply tangents for every key or for none")
        explicit = has_tangents
        flip = quaternion and i > 0 and opposite_sign(value, values[-1])
        if flip:
            value = tuple(-x for x in value)
        values.append(value)
        if explicit:
            incoming = vector(key["in"], width, where, "in")
            outgoing = vector(key["out"], width, where, "out")
            if flip:
                incoming, outgoing = tuple(-x for x in incoming), tuple(-x for x in outgoing)
            rows.extend((incoming, value, outgoing))
    if not explicit:
        rows = catmull_rom(times, values) if cubic else values
    if "offset" in track:
        rows = offset_rows(transform_field, track["offset"], rows, cubic, f"{label} key 0")
    for i, row in enumerate(rows):
        where = f"{label} key {i // CUBIC_ROWS if cubic else i}"
        for number in row:
            float32(number, where, "baked value or tangent")
    return times, rows


def resolve_field(name, fields, where):
    """Resolve the authored field name to its component and member."""
    if not isinstance(name, str):
        raise ValueError(f"{where}: unknown field")
    tag, dot, member = name.partition(".")
    if dot:
        if len(tag) == ASSET_TAG_WIDTH and tag.isascii():
            binding = (tag_value(tag), member)
            if binding in fields:
                return binding
        raise ValueError(f"{where}: unknown field")
    matches = [(component, field) for component, field in fields if field == name]
    if not matches:
        raise ValueError(f"{where}: unknown field")
    if len(matches) > 1:
        options = [f'field = "{tag}.{field}"' for tag, field in
                   sorted((tag_text(component), field) for component, field in matches)]
        raise ValueError(f"{where}: ambiguous field; use " + " or ".join(options))
    return matches[0]


def block_tracks(track, label):
    """Expand a clip block table into aligned step curves."""
    blocks = track["blocks"]
    if not isinstance(blocks, list) or not blocks:
        raise ValueError(f"{label} key 0: blocks must be a nonempty list")
    columns = {CLIP_FIELD: [], SPEED_FIELD: [], BLEND_FIELD: []}
    for i, block in enumerate(blocks):
        if not isinstance(block, dict) or not {"t", "clip"} <= block.keys() or block.keys() - {
            "t", "clip", "speed", "blend"
        }:
            raise ValueError(f"{label} key {i}: block needs t and clip, with optional speed and blend")
        for field, value in zip(columns, (block["clip"], block.get("speed", 1), block.get("blend", 0))):
            columns[field].append(dict(t=block["t"], v=value))
    key_times(blocks, label, start_at_zero=False)
    return {field: dict(interp="step", keys=keys) for field, keys in columns.items()}


def merge_speed(blocks, extra, label):
    """Combine two strictly ordered lists, rejecting coincident stored times."""
    keys = curve_keys(extra, label)
    key_times(keys, label, start_at_zero=False)
    merged = sorted([*blocks["keys"], *keys], key=lambda key: key["t"])
    times = [float32(key["t"], label, "t") for key in merged]
    for i, (a, b) in enumerate(zip(times, times[1:]), start=1):
        if a == b:
            raise ValueError(f"{label} key {i}: duplicate speed key time")
    return dict(extra, keys=merged)


def tracks_glb(values):
    if set(values) - {"duration"} != {"animation", "tracks"}:
        raise ValueError("expected animation and tracks")
    animation, tracks = values["animation"], values["tracks"]
    if not isinstance(animation, str) or not animation:
        raise ValueError("animation must be a nonempty name")
    if not isinstance(tracks, list) or not tracks:
        raise ValueError("tracks must be a nonempty list")

    nodes, indices, channels = [], {}, []
    authored, expanded, animator_curves = {}, {}, {}
    required = {"object", "field", "interp", "keys"}
    extension = scene_fields.SCENE_FIELDS_EXTENSION
    fields = scene_fields.fields()
    for i, track in enumerate(tracks):
        if not isinstance(track, dict):
            raise ValueError(f"track {i} key 0: expected a table")
        obj, field = track.get("object", "<object>"), track.get("field", "<field>")
        label = f"{obj}.{field}"
        where = f"{label} key 0"
        blocks = "blocks" in track
        if (blocks and track.keys() != {"object", "field", "blocks"}
                or not blocks and (not required <= track.keys() or track.keys() - (required | {"offset"}))):
            raise ValueError(f"{where}: missing or unexpected track entries")
        if not isinstance(obj, str) or not obj or "\0" in obj:
            raise ValueError(f"{where}: object must be a nonempty name without NUL")
        component, field = resolve_field(field, fields, where)
        binding = (obj, component, field)
        if blocks:
            if (component, field) != (scene_fields.ANIMATOR, CLIP_FIELD):
                raise ValueError(f"{where}: blocks require the animator clip field")
            if binding in expanded:
                raise ValueError(f"{where}: duplicate track")
            for member, curve in block_tracks(track, label).items():
                expanded[obj, component, member] = curve
        else:
            if binding in authored:
                raise ValueError(f"{where}: duplicate track")
            authored[binding] = track

    for binding in dict.fromkeys([*expanded, *authored]):
        obj, component, field = binding
        track = authored.get(binding, expanded.get(binding))
        label = f"{obj}.{track.get('field', field)}"
        where = f"{label} key 0"
        interp = track["interp"]
        if not isinstance(interp, str) or interp not in INTERPOLATIONS:
            raise ValueError(f"{where}: interp must be step, linear or cubic")
        value_type, step_only = fields[component, field]
        if step_only and interp != "step":
            raise ValueError(f"{where}: field requires step interpolation")
        if binding in expanded and binding in authored:
            if field != SPEED_FIELD:
                raise ValueError(f"{where}: duplicate track")
            track = merge_speed(expanded[binding], track, label)
        transform = scene_fields.TRANSFORM_FIELDS.get(field) if component == scene_fields.TRANSFORM else None
        interp = INTERPOLATIONS[interp]
        times, rows = track_curve(track, label, value_type, interp, field if transform else None,
                                  start_at_zero=component != scene_fields.ANIMATOR)
        if component == scene_fields.ANIMATOR and field in ANIMATOR_LIMITS:
            for i, (value,) in enumerate(rows):
                if not 0 <= value <= ANIMATOR_LIMITS[field] or float32(value, where, field) > ANIMATOR_LIMITS[field]:
                    raise ValueError(f"{label} key {i}: {field} is out of range")
        if component == scene_fields.ANIMATOR:
            animator_curves[obj, field] = label, times, rows
        if obj not in indices:
            indices[obj] = len(nodes)
            nodes.append(dict(name=obj))
        node = indices[obj]
        channel = dict(interpolation=interp, times=times, values=rows)
        if transform:
            channel.update(node=node, path=transform)
        else:
            tag = tag_text(component)
            channel.update(path="pointer", pointer=f"/nodes/{node}/extensions/{extension}/{tag}/{field}")
            first = rows[CUBIC_VALUE if interp == INTERPOLATIONS["cubic"] else 0]
            properties = nodes[node].setdefault("extensions", {}).setdefault(extension, {}).setdefault(tag, {})
            properties[field] = first[0] if len(first) == 1 else first
        channels.append(channel)
    glb_animation = animation_glb(values, channels)
    clip_curves = [obj for obj, field in animator_curves if field == CLIP_FIELD]
    if clip_curves and len(animator_curves) == len(channels) and "duration" not in values:
        raise ValueError("a block-only clip requires a duration")
    end = glb_animation.get("extras", {}).get(DURATION_EXTRA)
    if end is None:
        last = max(channel["times"][-1] for channel in channels)
        end = seconds_to_ms(last)
    check_block_timing(animator_curves, end)
    return build_glb(nodes, [glb_animation])


def build(values):
    """GLB bytes from the tables of either keys-file form."""
    return tracks_glb(values) if "tracks" in values else camera_glb(values)


def load_glb_bytes(path):
    """GLB bytes of a keys file; every fault is a ValueError."""
    try:
        return build(tomllib.loads(pathlib.Path(path).read_text(encoding="utf-8")))
    except (OSError, OverflowError) as error:
        raise ValueError(str(error)) from error

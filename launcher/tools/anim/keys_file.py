"""Build camera shorthand and scene track keys (NAME.keys.toml) into an in-memory glTF binary."""
import math
import pathlib
import struct
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import scene_fields
from geometry.vectors import qmul, qrot, unit
from gltf.gltf_write import build_glb

INTERPOLATIONS = {"step": "STEP", "linear": "LINEAR", "cubic": "CUBICSPLINE"}
QUAT_UNIT_TOLERANCE = 1e-5
IDENTITY_ROTATION = (0, 0, 0, 1)
ZERO_TRANSLATION = (0, 0, 0)
F32 = struct.Struct("<f")
CUBIC_ROWS = 3
CUBIC_VALUE = 1


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
    if set(values) != {"node", "animation", "keys"}:
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
                     [dict(name=values["animation"], channels=channels)],
                     [dict(type="perspective", perspective=dict(yfov=1.0, znear=0.1))])


def opposite_sign(a, b):
    return sum(x * y for x, y in zip(a, b)) < 0


def catmull_rom(times, values, loop=False):
    # Catmull and Rom, 1974, A Class of Local Interpolating Splines.
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


def key_times(keys, where):
    """Validate the timeline at stored precision and return the authored times."""
    times = []
    previous = None
    for i, key in enumerate(keys):
        time = key["t"]
        label = f"{where} key {i}"
        rounded = float32(time, label, "t")
        if (time != 0 if previous is None else rounded <= previous):
            raise ValueError(f"{label}: times must start at zero and increase strictly in float32")
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


def track_curve(track, label, value_type, interp, transform_field):
    keys = track["keys"]
    if not isinstance(keys, list) or not keys:
        raise ValueError(f"{label} key 0: keys must be a nonempty list")
    for i, key in enumerate(keys):
        if not isinstance(key, dict) or not {"t", "v"} <= key.keys() or key.keys() - {"t", "v", "in", "out"}:
            raise ValueError(f"{label} key {i}: each key needs t and v, with optional in and out")
    times = key_times(keys, label)
    values, rows = [], []
    width = scene_fields.WIDTHS[value_type]
    cubic = interp == INTERPOLATIONS["cubic"]
    quaternion = value_type == scene_fields.VALUE_QUAT
    explicit = None
    for i, key in enumerate(keys):
        where = f"{label} key {i}"
        value = vector(key["v"], width, where, "v", quaternion=quaternion)
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
        if len(tag) == 4 and tag.isascii():
            binding = (int.from_bytes(tag.encode("ascii"), "little"), member)
            if binding in fields:
                return binding
        raise ValueError(f"{where}: unknown field")
    matches = [(component, field) for component, field in fields if field == name]
    if not matches:
        raise ValueError(f"{where}: unknown field")
    if len(matches) > 1:
        options = [f'field = "{tag}.{field}"' for tag, field in
                   sorted((component.to_bytes(4, "little").decode("ascii"), field) for component, field in matches)]
        raise ValueError(f"{where}: ambiguous field; use " + " or ".join(options))
    return matches[0]


def tracks_glb(values):
    if set(values) != {"animation", "tracks"}:
        raise ValueError("expected animation and tracks")
    animation, tracks = values["animation"], values["tracks"]
    if not isinstance(animation, str) or not animation:
        raise ValueError("animation must be a nonempty name")
    if not isinstance(tracks, list) or not tracks:
        raise ValueError("tracks must be a nonempty list")

    nodes, indices, channels, seen = [], {}, [], set()
    required = {"object", "field", "interp", "keys"}
    extension = scene_fields.SCENE_FIELDS_EXTENSION
    fields = scene_fields.fields()
    for i, track in enumerate(tracks):
        if not isinstance(track, dict):
            raise ValueError(f"track {i} key 0: expected a table")
        obj, field = track.get("object", "<object>"), track.get("field", "<field>")
        label = f"{obj}.{field}"
        where = f"{label} key 0"
        if not required <= track.keys() or track.keys() - (required | {"offset"}):
            raise ValueError(f"{where}: missing or unexpected track entries")
        if not isinstance(obj, str) or not obj or "\0" in obj:
            raise ValueError(f"{where}: object must be a nonempty name without NUL")
        component, field = resolve_field(field, fields, where)
        interp = track["interp"]
        if not isinstance(interp, str) or interp not in INTERPOLATIONS:
            raise ValueError(f"{where}: interp must be step, linear or cubic")
        binding = (obj, component, field)
        if binding in seen:
            raise ValueError(f"{where}: duplicate track")
        seen.add(binding)
        value_type, _ = fields[component, field]
        transform = scene_fields.TRANSFORM_FIELDS.get(field) if component == scene_fields.TRANSFORM else None
        interp = INTERPOLATIONS[interp]
        times, rows = track_curve(track, label, value_type, interp, field if transform else None)
        if obj not in indices:
            indices[obj] = len(nodes)
            nodes.append(dict(name=obj))
        node = indices[obj]
        channel = dict(interpolation=interp, times=times, values=rows)
        if transform:
            channel.update(node=node, path=transform[0])
        else:
            tag = component.to_bytes(4, "little").decode("ascii")
            channel.update(path="pointer", pointer=f"/nodes/{node}/extensions/{extension}/{tag}/{field}")
            first = rows[CUBIC_VALUE if interp == INTERPOLATIONS["cubic"] else 0]
            properties = nodes[node].setdefault("extensions", {}).setdefault(extension, {}).setdefault(tag, {})
            properties[field] = first[0] if len(first) == 1 else first
        channels.append(channel)
    return build_glb(nodes, [dict(name=animation, channels=channels)])


def build(values):
    """GLB bytes from the tables of either keys-file form."""
    return tracks_glb(values) if "tracks" in values else camera_glb(values)


def load_glb_bytes(path):
    """GLB bytes of a keys file; every fault is a ValueError."""
    try:
        return build(tomllib.loads(pathlib.Path(path).read_text(encoding="utf-8")))
    except (OSError, OverflowError) as error:
        raise ValueError(str(error)) from error

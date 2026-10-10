"""The animation tracks pack entry (TRCK): one glTF animation, baked from the
NAME.anim.toml beside its source, and read back. The one writer of the entry,
and its reader on the host; main/anim/anim_tracks.c is the firmware's.

A NAME.anim.toml names its source, a .glb, .fbx or camera .keys.toml in its
own folder, and the animation in it; the pack id is NAME. The keys and the
entry's layout are in docs/Animation-Tracks.md, "The pack entry".

Each curve is stored in its target field's units (a camera's yfov
becomes `half_fov_short_tan`), and a channel that never changes
collapses to one key. Standard library only.
"""

import math
import pathlib
import re
import struct
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf import gltf_read  # noqa: E402

TYPE = b"TRCK"
VERSION = 2
SUFFIX = ".anim.toml"
HEADER = struct.Struct("<HHIIIB3x")
ROW = struct.Struct("<HHIIIHBB4x")
HEADER_SIZE = 20
ROW_SIZE = 24
AT_VERSION, AT_ROOT, AT_PAD = 0, 16, 17
HEADER_PAD_SIZE = 3
ROW_PATH, ROW_FIELD, ROW_COMPONENT = 0, 2, 4
ROW_TIMES, ROW_VALUES, ROW_KEYS = 8, 12, 16
ROW_TYPE, ROW_INTERP, ROW_PAD = 18, 19, 20
ROW_PAD_SIZE = 4
ALIGNMENT, CUBIC_RUNS = 4, 3
STRING_MAX, STRINGS_MAX, COUNT_MAX, DURATION_MAX = 255, 65535, 65535, 0xFFFFFFFF
ROOT_SCENE, ROOT_SKELETON = 0, 1
VALUE_FLOAT, VALUE_VEC2, VALUE_VEC3, VALUE_QUAT, VALUE_COLOUR = range(5)
WIDTHS = (1, 2, 3, 4, 3)
TRANSFORM, CAMERA = (int.from_bytes(tag, "little") for tag in (b"TRNS", b"CAMR"))
FIELDS = {"translation": ("position", VALUE_VEC3), "rotation": ("rotation", VALUE_QUAT),
          "scale": ("scale", VALUE_VEC3)}
assert HEADER.size == HEADER_SIZE == AT_PAD + HEADER_PAD_SIZE
assert ROW.size == ROW_SIZE == ROW_PAD + ROW_PAD_SIZE
# glTF's interpolation names, in anim_interp_t's order.
INTERPOLATIONS = ("STEP", "LINEAR", "CUBICSPLINE")
SOURCE_SUFFIXES = (*gltf_read.ASSET_SUFFIXES, gltf_read.KEYS_SUFFIX)
SOURCE = re.compile(r"[^/\\:]+(?:%s)" % "|".join(re.escape(s) for s in SOURCE_SUFFIXES), re.I)  # a file name: no folder, drive or path separator of either system


class TracksError(ValueError):
    """An animation that cannot be baked, or bytes that are not the entry."""


def object_name(document, collection, index):
    items = document.get(collection, [])
    if index >= len(items):
        raise TracksError("a pointer names /%s/%d, which the file does not have" % (collection, index))
    return items[index].get("name") or "%s%d" % (collection, index)


def channel_binding(document, channel, joint_paths=None):
    pointer = channel["pointer"]
    if pointer:
        match = re.fullmatch(r"/cameras/(\d+)/perspective/yfov", pointer)
        if not match:
            raise TracksError("channel %s: unsupported animation pointer" % pointer)
        camera = int(match.group(1))
        holders = [i for i, node in enumerate(document.get("nodes", [])) if node.get("camera") == camera]
        if camera >= len(document.get("cameras", [])) or len(holders) != 1:
            raise TracksError("channel %s: needs one node holding the camera" % pointer)
        return object_name(document, "nodes", holders[0]), CAMERA, "half_fov_short_tan", VALUE_FLOAT, camera
    node, path = channel["node"], channel["path"]
    if path not in FIELDS or node is None:
        raise TracksError("channel node %s/%s: unsupported target" % (node, path))
    field, value_type = FIELDS[path]
    return (joint_paths[node] if joint_paths else object_name(document, "nodes", node)), TRANSFORM, field, value_type, None


def channel_name(document, channel):
    path, component, field, _, _ = channel_binding(document, channel)
    return binding_name(path, component, field)


def binding_name(path, component, field):
    return "%s:%s.%s" % (path, component.to_bytes(4, "little").decode("latin1"), field)


def check_curve(name, channel):
    times, values = channel["times"], channel["values"]
    keys = len(times)
    per_key = 3 if channel["interpolation"] == "CUBICSPLINE" else 1
    if channel["interpolation"] not in INTERPOLATIONS:
        raise TracksError("%s: interpolation %s is not STEP, LINEAR or CUBICSPLINE" % (name, channel["interpolation"]))
    if keys < 1 or keys > 0xFFFF or len(values) != per_key * keys:
        raise TracksError("%s: %d times but %d values" % (name, keys, len(values)))
    if any(b <= a for a, b in zip(times, times[1:])):
        raise TracksError("%s: key times are not strictly increasing" % name)
    if not 1 <= len(values[0]) <= max(WIDTHS):
        raise TracksError("%s: a value of %d components; tracks hold 1 to %d" % (name, len(values[0]), max(WIDTHS)))
    if any(len(v) != len(values[0]) for v in values):
        raise TracksError("%s: inconsistent value widths" % name)
    if not all(math.isfinite(x) for x in times + [x for row in values for x in row]):
        raise TracksError("%s: a key holds a value that is not finite" % name)


def check(name, channel):
    check_curve(name, channel)
    if gltf_read.is_rotation(channel) and len(channel["values"][0]) != 4:
        raise TracksError("%s: a rotation is a quaternion" % name)


def collapse_constant(channel):
    """One key for a channel that never changes (cubic: with no slope)."""
    values = channel["values"]
    if channel["interpolation"] == "CUBICSPLINE":
        held = all(v == values[1] for v in values[1::3]) and all(
            not any(t) for i, t in enumerate(values) if i % 3 != 1)
        keep = [values[0], values[1], values[2]]
    else:
        held = all(v == values[0] for v in values)
        keep = [values[0]]
    if not held:
        return channel
    return dict(channel, times=channel["times"][:1], values=keep)


def clip_tracks(document, binary, animation):
    """Bindings and duration of a glTF clip, in each target field's units."""
    try:
        channels = gltf_read.read_animation(document, binary, animation)
        if not channels:
            raise TracksError("no channels")
        driven = {c["node"] for c in channels}
        joint_paths = None
        for skin in document.get("skins", []):
            if driven <= set(skin["joints"]):
                joint_paths = gltf_read.skin_joint_paths(document, skin)
                break
        tracks = []
        for channel in channels:
            path, component, field, value_type, camera = channel_binding(document, channel, joint_paths)
            name = binding_name(path, component, field)
            for label in (path, field):
                raw = label.encode("utf-8")
                if not raw or len(raw) > STRING_MAX or b"\0" in raw:
                    raise TracksError("channel %s: invalid name %r" % (name, label))
            check(name, channel)
            if len(channel["values"][0]) != WIDTHS[value_type]:
                raise TracksError("%s: values do not match binding type" % name)
            if component == CAMERA:
                aspect = document["cameras"][camera]["perspective"].get("aspectRatio", 1.0)
                channel = dict(channel, values=gltf_read.camera_half_fov_short_tan_curve(
                    channel["values"], channel["interpolation"], aspect))
            channel = collapse_constant(channel)
            tracks.append({"path": path, "component": component, "field": field, "type": value_type,
                           "name": name, "times": list(channel["times"]), "values": list(channel["values"]),
                           "interpolation": channel["interpolation"], "quaternion": value_type == VALUE_QUAT})
        return tracks, round(gltf_read.animation_duration(channels) * 1000), ROOT_SKELETON if joint_paths else ROOT_SCENE
    except (ValueError, OverflowError) as error:
        raise TracksError("clip %r: %s" % (animation.get("name", ""), error)) from error


def find_animation(document, name, where):
    found = [a for a in document.get("animations", []) if a.get("name") == name]
    if not found:
        raise TracksError("no animation named %r in %s" % (name, where))
    return found[0]


def encode(tracks, duration_ms, root=ROOT_SCENE):
    """TRCK bytes for bindings from clip_tracks()."""
    strings, offsets, bindings = bytearray(), {}, set()
    for track in tracks:
        binding = (track["path"], track["component"], track["field"])
        if binding in bindings:
            raise TracksError("two bindings are named %r" % track["name"])
        bindings.add(binding)
        for name in (track["path"], track["field"]):
            raw = name.encode("utf-8")
            if not raw or len(raw) > STRING_MAX or b"\0" in raw:
                raise TracksError("binding %r: name %r exceeds %d bytes or contains NUL" % (track["name"], name, STRING_MAX))
            if name not in offsets:
                offsets[name] = len(strings)
                strings += raw + b"\0"
    if len(strings) > STRINGS_MAX:
        raise TracksError("string table exceeds %d bytes" % STRINGS_MAX)
    if len(tracks) > COUNT_MAX or not 0 <= duration_ms <= DURATION_MAX:
        raise TracksError("%d bindings over %d ms do not fit the entry" % (len(tracks), duration_ms))
    if root not in (ROOT_SCENE, ROOT_SKELETON):
        raise TracksError("unknown root")
    strings_off = HEADER.size + ROW.size * len(tracks)
    padding = bytes(-(strings_off + len(strings)) % ALIGNMENT)
    offset = strings_off + len(strings) + len(padding)
    rows, data = [], bytearray()
    for track in tracks:
        check_curve(track["name"], track)
        if track["component"] not in (TRANSFORM, CAMERA) or track["type"] not in range(len(WIDTHS)):
            raise TracksError("%s: unknown component or type" % track["name"])
        if any(len(v) != WIDTHS[track["type"]] for v in track["values"]):
            raise TracksError("%s: values do not match binding type" % track["name"])
        times = struct.pack("<%df" % len(track["times"]), *track["times"])
        flat = [x for row in track["values"] for x in row]
        values = struct.pack("<%df" % len(flat), *flat)
        rows.append(ROW.pack(offsets[track["path"]], offsets[track["field"]], track["component"],
                             offset, offset + len(times), len(track["times"]), track["type"],
                             INTERPOLATIONS.index(track["interpolation"])))
        data += times + values
        offset += len(times) + len(values)
    entry = HEADER.pack(VERSION, len(tracks), duration_ms, strings_off, len(strings), root) + b"".join(rows) + strings + padding + data
    decode(entry)
    return bytes(entry)


def floats_at(entry, offset, count, table_end):
    if offset % ALIGNMENT or offset < table_end or offset + struct.calcsize("<f") * count > len(entry):
        raise TracksError("an array at %d of %d floats leaves the entry or is misaligned" % (offset, count))
    return list(struct.unpack_from("<%df" % count, entry, offset))


def decode(entry):
    """Validated bindings and duration of a TRCK entry."""
    if len(entry) < struct.calcsize("<H"):
        raise TracksError("shorter than a version")
    version = struct.unpack_from("<H", entry, AT_VERSION)[0]
    if version != VERSION:
        raise TracksError("version %d, this reads %d" % (version, VERSION))
    if len(entry) < HEADER.size:
        raise TracksError("shorter than a header")
    _, count, duration_ms, strings_off, strings_size, root = HEADER.unpack_from(entry)
    table_end = HEADER.size + ROW.size * count
    strings_end = strings_off + strings_size
    if table_end > len(entry) or strings_off < table_end or strings_off % ALIGNMENT or strings_end > len(entry):
        raise TracksError("table leaves the entry or is misaligned")
    if root not in (ROOT_SCENE, ROOT_SKELETON) or any(entry[AT_PAD:HEADER_SIZE]):
        raise TracksError("unknown root or nonzero header padding")
    def string_at(offset):
        if offset >= strings_size:
            raise TracksError("string offset outside the table")
        end = entry.find(b"\0", strings_off + offset, strings_end)
        if end < 0:
            raise TracksError("unterminated string")
        return entry[strings_off + offset:end].decode("utf-8")
    tracks = []
    for index in range(count):
        row_off = HEADER.size + ROW.size * index
        path_off, field_off, component, times_at, values_at, keys, value_type, interp = ROW.unpack_from(entry, row_off)
        path, field = string_at(path_off), string_at(field_off)
        if keys == 0 or value_type >= len(WIDTHS) or interp >= len(INTERPOLATIONS) \
                or component == 0 or any(entry[row_off + ROW_PAD:row_off + ROW_SIZE]):
            raise TracksError("binding %d: unknown field value or padding" % index)
        width = WIDTHS[value_type]
        per_key = CUBIC_RUNS if INTERPOLATIONS[interp] == "CUBICSPLINE" else 1
        times = floats_at(entry, times_at, keys, strings_end)
        values = floats_at(entry, values_at, per_key * keys * width, strings_end)
        if not all(math.isfinite(x) for x in times + values) or any(b <= a for a, b in zip(times, times[1:])):
            raise TracksError("binding %d: non-finite keys or times not increasing" % index)
        tracks.append({"path": path, "field": field, "component": component, "type": value_type, "root": root,
                       "name": binding_name(path, component, field),
                       "times": times, "values": [tuple(values[i:i + width]) for i in range(0, len(values), width)],
                       "interpolation": INTERPOLATIONS[interp], "quaternion": value_type == VALUE_QUAT})
    return tracks, duration_ms


def sample(track, seconds):
    """A decoded track at `seconds` by the Python sampler in gltf/gltf_read.py."""
    return gltf_read.sample_keys(track["times"], track["values"], seconds, track["interpolation"],
                                 track["quaternion"])


def clip_id(path):
    """The pack id of a .anim.toml: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def load_source(path):
    """(the source's path, the animation's name) a .anim.toml names."""
    path = pathlib.Path(path)
    try:
        with open(path, "rb") as source:
            values = tomllib.load(source)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise TracksError("%s: %s" % (path, error)) from error
    if set(values) != {"source", "animation"} or not all(isinstance(v, str) and v for v in values.values()):
        raise TracksError("%s: holds exactly source = \"x.glb\" (or .fbx, .keys.toml) and animation = \"<name>\"" % path)
    # Beside it, so whatever finds the .anim.toml finds its source too.
    if not SOURCE.fullmatch(values["source"]):
        raise TracksError("%s: source %r is not a .glb, .fbx or .keys.toml in the same folder" % (path, values["source"]))
    return path.parent / values["source"], values["animation"]


def bake(path):
    """The entry's bytes for a .anim.toml."""
    source, animation = load_source(path)
    try:
        document, binary = gltf_read.load_asset(source)
    except (OSError, ValueError) as error:
        raise TracksError("%s: source %s: %s" % (path, source, error)) from error
    tracks, duration_ms, root = clip_tracks(document, binary, find_animation(document, animation, source))
    try:
        return encode(tracks, duration_ms, root)
    except (ValueError, OverflowError) as error:
        raise TracksError("clip %r: %s" % (animation, error)) from error

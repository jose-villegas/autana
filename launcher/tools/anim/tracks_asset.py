"""The animation tracks pack entry (TRCK): one glTF animation, baked from the
NAME.anim.toml beside its .glb, and read back. The one writer and reader of
the entry; main/anim/anim_tracks.c is the firmware's reader.

A NAME.anim.toml names its source and the animation in it; the pack id is NAME:

    source = "NAME.glb"      # relative to the .anim.toml
    animation = "walk"       # the animation's name in the glTF

The entry, little-endian, every offset from its first byte:

    header   u16 version, u16 track_count, u32 duration_ms
    rows     per track: char name[32] (NUL padded, so at most 31 bytes),
             u32 times_off, u32 values_off, u16 count, u8 width, u8 interp,
             u8 quaternion, 3 zero bytes
    data     each track's f32 times, then its f32 values (cubic: in-tangent,
             value, out-tangent per key), 4-aligned

A track is named by its glTF binding: `node/translation`, or a
KHR_animation_pointer path with the object's index replaced by its name
(`lens/perspective/yfov`). Keys are copied as authored, except that a channel
that never changes is one key. Standard library only.
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
VERSION = 1
SUFFIX = ".anim.toml"
HEADER = struct.Struct("<HHI")
ROW = struct.Struct("<32sIIHBBB3x")
NAME_BYTES = 32
WIDTH_MAX = 4
# glTF's interpolation names, in anim_interp_t's order.
INTERPOLATIONS = ("STEP", "LINEAR", "CUBICSPLINE")
PATHS = ("translation", "rotation", "scale")
POINTER = re.compile(r"^/([A-Za-z]+)/(\d+)/(.+)$")


class TracksError(ValueError):
    """An animation that cannot be baked, or bytes that are not the entry."""


def object_name(document, collection, index):
    items = document.get(collection, [])
    if index >= len(items):
        raise TracksError("a pointer names /%s/%d, which the file does not have" % (collection, index))
    return items[index].get("name") or "%s%d" % (collection, index)


def channel_name(document, channel):
    """The name a channel has in the file: 'node/path', or its pointer with
    the object's index replaced by its name."""
    if channel["pointer"]:
        match = POINTER.match(channel["pointer"])
        if not match:
            raise TracksError("pointer %s is not /collection/index/property" % channel["pointer"])
        collection, index, rest = match.group(1), int(match.group(2)), match.group(3)
        return "%s/%s" % (object_name(document, collection, index), rest)
    if channel["path"] not in PATHS or channel["node"] is None:
        raise TracksError("a channel targets %r without a node or a KHR_animation_pointer" % channel["path"])
    return "%s/%s" % (object_name(document, "nodes", channel["node"]), channel["path"])


def check(name, channel):
    times, values = channel["times"], channel["values"]
    keys = len(times)
    per_key = 3 if channel["interpolation"] == "CUBICSPLINE" else 1
    if channel["interpolation"] not in INTERPOLATIONS:
        raise TracksError("%s: interpolation %s is not STEP, LINEAR or CUBICSPLINE" % (name, channel["interpolation"]))
    if keys < 1 or keys > 0xFFFF or len(values) != per_key * keys:
        raise TracksError("%s: %d times but %d values" % (name, keys, len(values)))
    if any(b <= a for a, b in zip(times, times[1:])):
        raise TracksError("%s: key times are not strictly increasing" % name)
    if not 1 <= len(values[0]) <= WIDTH_MAX:
        raise TracksError("%s: a value of %d components; tracks hold 1 to %d" % (name, len(values[0]), WIDTH_MAX))
    if gltf_read.is_rotation(channel) and len(values[0]) != 4:
        raise TracksError("%s: a rotation is a quaternion" % name)
    if not all(math.isfinite(x) for x in times + [x for row in values for x in row]):
        raise TracksError("%s: a key holds a value that is not finite" % name)


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
    """(tracks, duration_ms) of one animation dict, checked and with constant
    channels collapsed. Each track is a dict: `name`, `times`, `values` (one
    tuple per key, three per key when cubic), `interpolation` (glTF's name)
    and `quaternion`."""
    channels = gltf_read.read_animation(document, binary, animation)
    if not channels:
        raise TracksError("animation %r has no channels" % animation.get("name", ""))
    tracks = []
    for channel in channels:
        name = channel_name(document, channel)
        check(name, channel)
        channel = collapse_constant(channel)
        tracks.append({"name": name, "times": list(channel["times"]), "values": list(channel["values"]),
                       "interpolation": channel["interpolation"], "quaternion": gltf_read.is_rotation(channel)})
    return tracks, round(gltf_read.animation_duration(channels) * 1000)


def find_animation(document, name, where):
    found = [a for a in document.get("animations", []) if a.get("name") == name]
    if not found:
        raise TracksError("no animation named %r in %s" % (name, where))
    return found[0]


def encode(tracks, duration_ms):
    """The entry's bytes for `tracks` as clip_tracks() returns them."""
    names = set()
    for track in tracks:
        raw = track["name"].encode("utf-8")
        if not raw or len(raw) >= NAME_BYTES:
            raise TracksError("track %r: a name is 1 to %d bytes" % (track["name"], NAME_BYTES - 1))
        if track["name"] in names:
            raise TracksError("two tracks are named %r" % track["name"])
        names.add(track["name"])
    if len(tracks) > 0xFFFF or not 0 <= duration_ms <= 0xFFFFFFFF:
        raise TracksError("%d tracks over %d ms do not fit the entry" % (len(tracks), duration_ms))
    rows, data = [], bytearray()
    offset = HEADER.size + ROW.size * len(tracks)
    for track in tracks:
        times = struct.pack("<%df" % len(track["times"]), *track["times"])
        flat = [x for row in track["values"] for x in row]
        values = struct.pack("<%df" % len(flat), *flat)
        rows.append(ROW.pack(track["name"].encode("utf-8"), offset, offset + len(times), len(track["times"]),
                             len(track["values"][0]), INTERPOLATIONS.index(track["interpolation"]),
                             1 if track["quaternion"] else 0))
        data += times + values
        offset += len(times) + len(values)
    return HEADER.pack(VERSION, len(tracks), duration_ms) + b"".join(rows) + bytes(data)


def floats_at(entry, offset, count, table_end):
    if offset % 4 or offset < table_end or offset + 4 * count > len(entry):
        raise TracksError("an array at %d of %d floats leaves the entry or is misaligned" % (offset, count))
    return list(struct.unpack_from("<%df" % count, entry, offset))


def decode(entry):
    """(tracks, duration_ms) of an entry's bytes, after the checks
    anim_tracks_open() makes; the tracks as clip_tracks() returns them."""
    if len(entry) < HEADER.size:
        raise TracksError("shorter than a header")
    version, count, duration_ms = HEADER.unpack_from(entry)
    if version != VERSION:
        raise TracksError("version %d, this reads %d" % (version, VERSION))
    table_end = HEADER.size + ROW.size * count
    if table_end > len(entry):
        raise TracksError("the track table leaves the entry")
    tracks = []
    for index in range(count):
        raw, times_at, values_at, keys, width, interp, quaternion = ROW.unpack_from(entry, HEADER.size + ROW.size * index)
        pad = entry[HEADER.size + ROW.size * (index + 1) - 3:HEADER.size + ROW.size * (index + 1)]
        if b"\0" not in raw or keys == 0 or not 1 <= width <= WIDTH_MAX or interp >= len(INTERPOLATIONS) \
                or quaternion > 1 or (quaternion and width != 4) or any(pad):
            raise TracksError("track %d: a field holds a value the entry does not allow" % index)
        per_key = 3 if INTERPOLATIONS[interp] == "CUBICSPLINE" else 1
        values = floats_at(entry, values_at, per_key * keys * width, table_end)
        tracks.append({"name": raw.split(b"\0", 1)[0].decode("utf-8"),
                       "times": floats_at(entry, times_at, keys, table_end),
                       "values": [tuple(values[i:i + width]) for i in range(0, len(values), width)],
                       "interpolation": INTERPOLATIONS[interp], "quaternion": bool(quaternion)})
    return tracks, duration_ms


def sample(track, seconds):
    """A decoded track at `seconds` by the Python sampler in gltf/gltf_read.py."""
    return gltf_read.sample_keys(track["times"], track["values"], seconds, track["interpolation"],
                                 track["quaternion"])


def clip_id(path):
    """The pack id of a .anim.toml: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def load_source(path):
    """(the .glb's path, the animation's name) a .anim.toml names."""
    path = pathlib.Path(path)
    try:
        with open(path, "rb") as source:
            values = tomllib.load(source)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise TracksError("%s: %s" % (path, error)) from error
    if set(values) != {"source", "animation"} or not all(isinstance(v, str) and v for v in values.values()):
        raise TracksError("%s: holds exactly source = \"x.glb\" and animation = \"<name>\"" % path)
    return path.parent / values["source"], values["animation"]


def bake(path):
    """The entry's bytes for a .anim.toml."""
    glb, animation = load_source(path)
    try:
        document, binary = gltf_read.load_glb(glb)
    except (OSError, ValueError) as error:
        raise TracksError("%s: source %s: %s" % (path, glb, error)) from error
    tracks, duration_ms = clip_tracks(document, binary, find_animation(document, animation, glb))
    return encode(tracks, duration_ms)

"""Read a binary glTF 2.0 file: accessors, animations and their samplers.

Standard library only (an FBX source is converted by tools/fbx on demand), so
asset tests and bakers share one reader. The
sampler follows the glTF 2.0 spec (LINEAR, STEP, CUBICSPLINE; quaternions
slerp), and is the reference the C runtime in main/anim/ is held to.
"""

import json
import math
import pathlib
import struct

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

GLB_SUFFIX = ".glb"
FBX_SUFFIX = ".fbx"
# The files a tool reads as a scene: a glTF binary, or an FBX converted to one.
ASSET_SUFFIXES = (GLB_SUFFIX, FBX_SUFFIX)

COMPONENT_FORMATS = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
TYPE_WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
NORMALIZED_DIVISORS = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}


def load_glb(path):
    """Return (document, binary chunk) of a .glb file."""
    with open(path, "rb") as handle:
        data = handle.read()
    return parse_glb(data)


def load_asset(path):
    """Return (document, binary chunk) of a scene file, .glb or .fbx. An FBX is
    converted once to a .glb (tools/fbx/fbx_to_glb.py), cached by content."""
    if pathlib.Path(path).suffix.lower() == FBX_SUFFIX:
        from fbx.fbx_to_glb import cached_glb
        path = cached_glb(path)
    return load_glb(path)


def parse_glb(data):
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2 or length != len(data):
        raise ValueError("not a glTF 2.0 binary of the stated length")
    offset = 12
    document = None
    binary = b""
    while offset < length:
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8:offset + 8 + chunk_length]
        if chunk_type == CHUNK_JSON:
            document = json.loads(chunk.decode("utf-8"))
        elif chunk_type == CHUNK_BIN:
            binary = chunk
        offset += 8 + chunk_length
    if document is None:
        raise ValueError("glb has no JSON chunk")
    return document, binary


def read_accessor(document, binary, index):
    """Return an accessor's elements as a list of tuples of numbers."""
    accessor = document["accessors"][index]
    fmt, size = COMPONENT_FORMATS[accessor["componentType"]]
    width = TYPE_WIDTHS[accessor["type"]]
    view = document["bufferViews"][accessor["bufferView"]]
    base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", size * width)
    divisor = None
    if accessor.get("normalized"):
        divisor = NORMALIZED_DIVISORS[accessor["componentType"]]
    element = struct.Struct("<" + fmt * width)
    out = []
    for i in range(accessor["count"]):
        values = element.unpack_from(binary, base + i * stride)
        if divisor is not None:
            values = tuple(max(v / divisor, -1.0) for v in values)
        out.append(values)
    return out


def quat_slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = tuple(-x for x in b)
        dot = -dot
    if dot > 0.9995:
        out = tuple(x + (y - x) * t for x, y in zip(a, b))
    else:
        theta = math.acos(dot)
        sa = math.sin((1.0 - t) * theta) / math.sin(theta)
        sb = math.sin(t * theta) / math.sin(theta)
        out = tuple(sa * x + sb * y for x, y in zip(a, b))
    norm = math.sqrt(sum(x * x for x in out))
    return tuple(x / norm for x in out)


def sample_keys(times, values, time, interpolation="LINEAR", quaternion=False):
    """One glTF animation sampler evaluated at `time` seconds, clamped at
    both ends. `values` holds one tuple per key, or for CUBICSPLINE three per
    key: in-tangent, value, out-tangent."""
    cubic = interpolation == "CUBICSPLINE"
    value = (lambda k: values[3 * k + 1]) if cubic else (lambda k: values[k])
    if time <= times[0] or len(times) == 1:
        return value(0)
    if time >= times[-1]:
        return value(len(times) - 1)
    # The first key after `time`: exactly on a key, that key starts the segment.
    hi = next(i for i, t in enumerate(times) if t > time)
    lo = hi - 1
    if interpolation == "STEP":
        return value(lo)
    dt = times[hi] - times[lo]
    s = (time - times[lo]) / dt
    if cubic:
        s2, s3 = s * s, s * s * s
        out = tuple(
            (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * dt * m0
            + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * dt * m1
            for p0, m0, p1, m1 in zip(
                values[3 * lo + 1], values[3 * lo + 2],
                values[3 * hi + 1], values[3 * hi]
            )
        )
        if quaternion:
            norm = math.sqrt(sum(x * x for x in out))
            out = tuple(x / norm for x in out)
        return out
    if quaternion:
        return quat_slerp(values[lo], values[hi], s)
    return tuple(a + (b - a) * s for a, b in zip(values[lo], values[hi]))


def read_animation(document, binary, animation):
    """The channels of one animation dict: each a dict with `node` (None for a
    pointer channel), `path`, `pointer` (None unless one), `interpolation`,
    `times` and `values`."""
    channels = []
    for channel in animation["channels"]:
        sampler = animation["samplers"][channel["sampler"]]
        target = channel["target"]
        pointer = target.get("extensions", {}).get("KHR_animation_pointer", {}).get("pointer")
        channels.append({
            "node": target.get("node"),
            "path": target["path"],
            "pointer": pointer,
            "interpolation": sampler.get("interpolation", "LINEAR"),
            "times": [t[0] for t in read_accessor(document, binary, sampler["input"])],
            "values": read_accessor(document, binary, sampler["output"]),
        })
    return channels


def is_rotation(channel):
    """A channel whose value is a quaternion: a node's rotation path, or a
    KHR_animation_pointer at any object's rotation."""
    pointer = channel.get("pointer") or ""
    return channel["path"] == "rotation" or pointer.endswith("/rotation")


def animation_duration(channels):
    """Seconds an animation runs: the last key of any channel. glTF plays
    every channel of an animation on this one timeline."""
    return max(c["times"][-1] for c in channels)

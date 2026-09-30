"""The boot animation's camera and space keyframes, and the glTF animation
that holds them.

The editor works on keyframes: a time, then a position in meters, a rotation
in degrees (small3dlib's Z, X, Y order) and a scale for each of the camera
and the space, and how the segment arriving at it is eased. The firmware
plays a glTF animation through main/anim/. This module is the two-way
bridge, so the animation can be authored in either.

  keyframes_to_glb()  a glTF with two nodes, "camera" and "space". Position
                      and scale are LINEAR, or CUBICSPLINE where a segment is
                      eased: an ease is a cubic, so the tangents reproduce it
                      exactly. Rotation is a quaternion track, one key per
                      keyframe and slerped between them, so a segment
                      turning about several axes takes the shortest path
                      rather than the Euler lerp it once had.
  glb_to_keyframes()  one keyframe per key time in the file, so opening and
                      saving a file rewrites no key. A cubic shape other than
                      the three eases reads as linear.

Standard library only; the reader and sampler are tools/gltf/gltf_read.py's.
"""

import math
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from gltf import gltf_read, gltf_write  # noqa: E402

ANIMATION = "boot_motion"
NODES = ("camera", "space")

# The slope of an ease's cubic at its start and at its end, per unit of its
# own duration: linear u, ease_out 1 - (1-u)^2, ease_in u^2.
EASE_SLOPES = {"linear": (1.0, 1.0), "ease_out": (2.0, 0.0), "ease_in": (0.0, 2.0)}


def ease_value(name, u):
    if name == "ease_out":
        return 1.0 - (1.0 - u) * (1.0 - u)
    if name == "ease_in":
        return u * u
    return u


def euler_to_matrix(rx, ry, rz):
    """Degrees to the 3x3 that r3d_trs.c reads: small3dlib's rotation matrix,
    acting on column vectors. Each angle is negated before its sine, as
    small3dlib does."""
    ax, ay, az = (math.radians(-a) for a in (rx, ry, rz))
    sx, sy, sz = math.sin(ax), math.sin(ay), math.sin(az)
    cx, cy, cz = math.cos(ax), math.cos(ay), math.cos(az)
    return [
        [cy * cz + sy * sx * sz, cz * sy * sx - cy * sz, cx * sy],
        [cx * sz, cx * cz, -sx],
        [cy * sx * sz - cz * sy, cy * cz * sx + sy * sz, cy * cx],
    ]


def matrix_to_quat(m):
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        q = ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4)
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        q = (s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s)
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        q = ((m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s)
    else:
        s = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
        q = ((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s)
    length = math.sqrt(sum(c * c for c in q))
    return tuple(c / length for c in q)


def quat_to_matrix(q):
    x, y, z, w = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def euler_to_quat(rx, ry, rz):
    return matrix_to_quat(euler_to_matrix(rx, ry, rz))


def quat_to_euler(q, near=None):
    """Degrees, small3dlib's convention. A rotation has two Euler triples
    (x and z either side of a half turn); `near` picks the one, and the
    turns of 360, closest to a reference triple."""
    m = quat_to_matrix(q)
    ax = math.asin(max(-1.0, min(1.0, -m[1][2])))
    ay = math.atan2(m[0][2], m[2][2])
    az = math.atan2(m[1][0], m[1][1])
    candidates = [(ax, ay, az), (math.pi - ax, ay + math.pi, az + math.pi)]
    triples = [tuple(-math.degrees(a) for a in c) for c in candidates]
    if near is None:
        return triples[0]

    def closest(triple):
        return tuple(a + 360.0 * round((r - a) / 360.0) for a, r in zip(triple, near))

    triples = [closest(t) for t in triples]
    return min(triples, key=lambda t: sum((a - r) ** 2 for a, r in zip(t, near)))


def rotation_angle_degrees(a, b):
    dot = min(1.0, abs(sum(x * y for x, y in zip(a, b))))
    return math.degrees(2 * math.acos(dot))


def _rotation_keys(keyframes, node):
    """(seconds, quaternion) per keyframe, each in the hemisphere of the last
    so a slerp between them takes the short way."""
    keys = []
    for k in keyframes:
        q = euler_to_quat(*k[node]["rot"])
        if keys and sum(a * b for a, b in zip(keys[-1][1], q)) < 0:
            q = tuple(-c for c in q)
        keys.append((k["ms"] / 1000.0, q))
    return keys


def _vector_channel(keyframes, node, field, index):
    times = [k["ms"] / 1000.0 for k in keyframes]
    values = [tuple(float(v) for v in k[node][field]) for k in keyframes]
    eased = any(
        k["ease"] != "linear" and values[i] != values[i - 1]
        for i, k in enumerate(keyframes) if i
    )
    if not eased:
        return {"node": index, "path": "translation" if field == "pos" else "scale",
                "interpolation": "LINEAR", "times": times, "values": values}
    rows = []
    zero = (0.0, 0.0, 0.0)
    for i, value in enumerate(values):
        tangent_in = zero
        tangent_out = zero
        if i:
            dt = times[i] - times[i - 1]
            slope = EASE_SLOPES[keyframes[i]["ease"]][1]
            tangent_in = tuple((b - a) * slope / dt for a, b in zip(values[i - 1], value))
        if i + 1 < len(values):
            dt = times[i + 1] - times[i]
            slope = EASE_SLOPES[keyframes[i + 1]["ease"]][0]
            tangent_out = tuple((b - a) * slope / dt for a, b in zip(value, values[i + 1]))
        rows += [tangent_in, value, tangent_out]
    return {"node": index, "path": "translation" if field == "pos" else "scale",
            "interpolation": "CUBICSPLINE", "times": times, "values": rows}


def keyframes_to_glb(keyframes):
    """The glTF for keyframes shaped as boot_anim_timeline.json's used to be."""
    keyframes = sorted(keyframes, key=lambda k: k["ms"])
    channels = []
    for index, node in enumerate(NODES):
        channels.append(_vector_channel(keyframes, node, "pos", index))
        keys = _rotation_keys(keyframes, node)
        channels.append({"node": index, "path": "rotation", "interpolation": "LINEAR",
                         "times": [t for t, _ in keys], "values": [q for _, q in keys]})
        channels.append(_vector_channel(keyframes, node, "scale", index))
    return gltf_write.build_glb([{"name": n} for n in NODES],
                                [{"name": ANIMATION, "channels": channels}])


def _sample(channel, seconds):
    return gltf_read.sample_keys(channel["times"], channel["values"], seconds,
                                 channel["interpolation"], channel["path"] == "rotation")


def _segment_ease(channels, t0, t1):
    """Which of the three eases a cubic channel follows over [t0, t1]: read
    from the out-tangent at its start against a linear one."""
    for c in channels:
        if c["interpolation"] != "CUBICSPLINE" or c["path"] == "rotation":
            continue
        times = c["times"]
        if t0 not in times or t1 not in times:
            continue
        lo = times.index(t0)
        hi = times.index(t1)
        if hi != lo + 1:
            continue
        a, b = c["values"][3 * lo + 1], c["values"][3 * hi + 1]
        m0, m1 = c["values"][3 * lo + 2], c["values"][3 * hi]
        dt = t1 - t0
        for i in range(3):
            delta = b[i] - a[i]
            if abs(delta) < 1e-9:
                continue
            start, end = m0[i] * dt / delta, m1[i] * dt / delta
            for name, (s0, s1) in EASE_SLOPES.items():
                if abs(start - s0) < 0.05 and abs(end - s1) < 0.05:
                    return name
            return "linear"
    return "linear"


def glb_to_keyframes(data):
    """Keyframes, at every key time any channel has."""
    document, binary = gltf_read.parse_glb(data)
    animation = next(a for a in document["animations"] if a.get("name") == ANIMATION)
    by_node = {n: [] for n in NODES}
    for channel in gltf_read.read_animation(document, binary, animation):
        name = document["nodes"][channel["node"]].get("name")
        if name in by_node:
            by_node[name].append(channel)
    seconds = sorted({t for chans in by_node.values() for c in chans for t in c["times"]})
    keyframes = []
    reference = {n: None for n in NODES}
    for i, t in enumerate(seconds):
        frame = {"ms": round(t * 1000)}
        for node in NODES:
            chans = {c["path"]: c for c in by_node[node]}
            euler = quat_to_euler(_sample(chans["rotation"], t), reference[node])
            reference[node] = euler
            frame[node] = {
                "pos": [round(v, 4) for v in _sample(chans["translation"], t)],
                "rot": [round(v, 4) for v in euler],
                "scale": [round(v, 4) for v in _sample(chans["scale"], t)],
            }
        frame["ease"] = "linear"
        if i:
            frame["ease"] = _segment_ease([c for n in NODES for c in by_node[n]], seconds[i - 1], t)
        keyframes.append(frame)
    return keyframes

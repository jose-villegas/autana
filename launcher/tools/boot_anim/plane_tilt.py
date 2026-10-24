#!/usr/bin/env python3
"""Tilt the boot space toward the camera over one stretch of its motion clip
without moving the t line on screen.

    python tools/boot_anim/plane_tilt.py CLIP.glb DEGREES START_S END_S [-o OUT.glb]

A line keeps its place on screen exactly while it stays in the plane P through
the camera's eye and the line. A rotation about P's normal n = (eye - origin) x
line, through the space's origin, maps P onto itself, so the line never leaves
it. The tilt R_n(DEGREES * u), u rising linearly from START_S to END_S, is laid
over the clip's own rotation as keys every KEY_STEP_S, so interpolating between
them stays in P too.

The tilt adds to whatever the clip already holds: to go from 25 to 30 degrees,
run it again with 5. It refuses a stretch over which P moves (the camera, the
space's origin or its t line turning out of P), since no single n exists then,
and it checks the clip it wrote: the line may not leave P by more than
MAX_ERROR_DEG at any CHECK_STEP_S.
"""

import argparse
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from geometry.vectors import cross, qmul, qrot, unit  # noqa: E402
from gltf import gltf_read  # noqa: E402
from gltf.gltf_write import build_glb  # noqa: E402

CAMERA, SPACE = "camera", "space"
# The space's (re, t, im) are its (x, y, z); see boot_anim_view().
T_AXIS = (0.0, 1.0, 0.0)
KEY_STEP_S = 0.025
CHECK_STEP_S = 0.01
MAX_ERROR_DEG = 0.05
SAME = 1e-6


def fail(message):
    sys.exit("plane_tilt.py: " + message)


def read_clip(path):
    """Nodes and the first animation's channels, each with flat times and the
    values as stored (three per key for CUBICSPLINE)."""
    document, binary = gltf_read.load_glb(path)
    names = [node["name"] for node in document["nodes"]]
    animation = document["animations"][0]
    channels = []
    for channel in animation["channels"]:
        sampler = animation["samplers"][channel["sampler"]]
        channels.append({
            "node": channel["target"]["node"],
            "path": channel["target"]["path"],
            "interpolation": sampler.get("interpolation", "LINEAR"),
            "times": [t[0] if isinstance(t, tuple) else t
                      for t in gltf_read.read_accessor(document, binary, sampler["input"])],
            "values": [tuple(v) for v in gltf_read.read_accessor(document, binary, sampler["output"])],
        })
    return document, names, animation["name"], channels


def sampler(names, channels):
    def sample(node, path, t):
        channel = next(c for c in channels if names[c["node"]] == node and c["path"] == path)
        return tuple(gltf_read.sample_keys(channel["times"], channel["values"], t, channel["interpolation"],
                                           quaternion=path == "rotation"))
    return sample


def line_plane_normal(sample, t):
    eye = sample(CAMERA, "translation", t)
    origin = sample(SPACE, "translation", t)
    line = qrot(sample(SPACE, "rotation", t), T_AXIS)
    return unit(cross(tuple(e - o for e, o in zip(eye, origin)), line))


def worst_error_deg(sample, normal, start, end):
    worst = 0.0
    steps = int(round((end - start) / CHECK_STEP_S))
    for k in range(steps + 1):
        line = qrot(sample(SPACE, "rotation", start + k * (end - start) / steps), T_AXIS)
        out_of_plane = sum(a * b for a, b in zip(line, normal))
        worst = max(worst, abs(math.degrees(math.asin(max(-1.0, min(1.0, out_of_plane))))))
    return worst


def tilt(path, degrees, start, end):
    """The clip's bytes with the tilt applied, and the plane's normal."""
    document, names, name, channels = read_clip(path)
    sample = sampler(names, channels)
    normal = line_plane_normal(sample, start)
    if any(abs(a - b) > SAME for a, b in zip(normal, line_plane_normal(sample, end))) or \
            worst_error_deg(sample, normal, start, end) > MAX_ERROR_DEG:
        fail("the plane through the eye and the t line moves between %g and %g s" % (start, end))

    rotation = next(c for c in channels if names[c["node"]] == SPACE and c["path"] == "rotation")
    if rotation["interpolation"] != "LINEAR":
        fail("the space's rotation must be LINEAR, not " + rotation["interpolation"])
    count = max(1, int(round((end - start) / KEY_STEP_S)))
    times = [start + k * (end - start) / count for k in range(count + 1)]
    values = []
    for k, t in enumerate(times):
        half = math.radians(degrees) * (k / count) / 2
        turn = tuple(c * math.sin(half) for c in normal) + (math.cos(half),)
        values.append(qmul(turn, sample(SPACE, "rotation", t)))
    keep = [(t, v) for t, v in zip(rotation["times"], rotation["values"])
            if t < start - KEY_STEP_S / 2 or t > end + KEY_STEP_S / 2]
    keys = sorted(keep + list(zip(times, values)))
    rotation["times"] = [t for t, _ in keys]
    rotation["values"] = [v for _, v in keys]

    nodes = [{k: node[k] for k in ("name", "translation", "rotation", "scale") if k in node}
             for node in document["nodes"]]
    return build_glb(nodes, [{"name": name, "channels": channels}]), normal


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("clip")
    parser.add_argument("degrees", type=float)
    parser.add_argument("start_s", type=float)
    parser.add_argument("end_s", type=float)
    parser.add_argument("-o", "--out", help="write here instead of over CLIP")
    args = parser.parse_args()
    if not args.start_s < args.end_s:
        fail("START_S must be before END_S")
    data, normal = tilt(args.clip, args.degrees, args.start_s, args.end_s)
    out = args.out or args.clip
    pathlib.Path(out).write_bytes(data)

    _, names, _, channels = read_clip(out)
    worst = worst_error_deg(sampler(names, channels), normal, args.start_s, args.end_s)
    if worst > MAX_ERROR_DEG:
        fail("the written clip moves the t line %.4f degrees out of its plane" % worst)
    print("tilted %g degrees about n=(%s) from %g to %g s; the t line stays within %.4f degrees of its plane"
          % (args.degrees, ", ".join("%.4f" % c for c in normal), args.start_s, args.end_s, worst))


if __name__ == "__main__":
    main()

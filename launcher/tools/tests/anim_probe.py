#!/usr/bin/env python3
"""A test clip with every kind of track, and the pack the host C suite
suite_anim_tracks.c reads to hold the firmware's reader and sampler to the
Python ones.

    python launcher/tools/tests/anim_probe.py -o PACK

The pack holds the clip as a TRCK entry "probe" and an entry "probe_ref" of
type TREF: what the Python sampler gives for each track at a list of times,
read back from the entry by tracks_asset.decode(). Little-endian:

    u32 duration_ms, u32 row count, then per row: u16 track (its index in
    the entry), u8 exact, u8 pad, f32 seconds, f32 want[4]

`exact` marks a sample the C sampler must equal bit for bit: one that copies
a key (at or past either end, a step, a single key, or on a key time of a
track that is not a quaternion). The rest are float arithmetic against Python's double, so
they hold to a tolerance. The scene is invented here, so nothing depends on
one an app ships.
"""

import argparse
import math
import pathlib
import struct
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from anim import tracks_asset  # noqa: E402
from asset.asset_pack import build_pack  # noqa: E402
from gltf import gltf_read, gltf_write  # noqa: E402

NODES = ("lamp", "arm", "hand")
REFERENCE = b"TREF"
REFERENCE_HEADER = struct.Struct("<II")
REFERENCE_ROW = struct.Struct("<HBxf4f")
EVERY_MS = 37
UNTIL_MS = 4200


def turn(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def cubic_rows(values, slopes):
    """glTF CUBICSPLINE rows: in-tangent, value, out-tangent per key."""
    rows = []
    for value, slope in zip(values, slopes):
        rows += [slope, value, slope]
    return rows


def channel(node, path, times, values, interpolation="LINEAR", pointer=None):
    return {"node": node, "path": path, "pointer": pointer, "interpolation": interpolation,
            "times": times, "values": values}


def probe_channels(index):
    """The clip's channels, with each node at index[name] in the file."""
    lamp, arm, hand = (index[n] for n in NODES)
    return [
        channel(lamp, "translation", [0.0, 1.0, 2.5], [(0, 0, 0), (4, -2, 1), (1, 1, 1)]),
        # The second pair of keys is in the opposite hemisphere: slerp must take the short way.
        channel(lamp, "rotation", [0.0, 1.0, 2.5],
                [turn((0, 1, 0), 10), turn((0, 1, 0), 170), tuple(-x for x in turn((0, 1, 0), 200))]),
        channel(arm, "scale", [0.0, 1.0, 2.0], [(1, 1, 1), (2, 3, 4), (0.5, 0.5, 0.5)], "STEP"),
        # These start late and end last: the clip is 3 s, and these run 0.5 to 3.
        channel(arm, "translation", [0.5, 1.5, 3.0],
                cubic_rows([(0, 0, 0), (2, 1, 0), (0, 3, 3)], [(1, 0, 0), (0, 2, 0), (-1, -1, 0)]), "CUBICSPLINE"),
        channel(arm, "rotation", [0.5, 1.5, 3.0],
                cubic_rows([turn((1, 0, 0), 0), turn((1, 0, 0), 90), turn((1, 0, 0), 30)],
                           [(0.2, 0, 0, 0), (0, 0.3, 0, 0), (0, 0, 0.1, 0)]), "CUBICSPLINE"),
        channel(None, "pointer", [0.0, 0.8, 2.0], [(0.6,), (1.2,), (0.9,)],
                pointer="/cameras/0/perspective/yfov"),
        # A pointer to a rotation is a quaternion too.
        channel(None, "pointer", [0.0, 2.0], [turn((0, 0, 1), 0), turn((0, 0, 1), 200)],
                pointer="/nodes/%d/rotation" % hand),
        # A channel that never changes bakes to one key.
        channel(hand, "translation", [0.0, 3.0], [(7, 7, 7), (7, 7, 7)]),
    ]


def probe_glb(reordered=False):
    order = list(NODES)
    if reordered:
        order.reverse()
    index = {name: order.index(name) for name in NODES}
    cameras = [{"name": "lens", "type": "perspective", "perspective": {"yfov": 0.6, "znear": 0.1}}]
    return gltf_write.build_glb([{"name": n} for n in order],
                                [{"name": "clip", "channels": probe_channels(index)}], cameras=cameras)


def write_camera_clip(directory, name="fly", reach=1.0, degrees=0.0, props=()):
    """NAME.anim.toml and NAME.glb beside it: node `camera` moving from x 0 to
    `reach` over a second while turning `degrees` about +y from facing glTF's
    -Z, linearly. `props` names nodes the clip does not animate, which change
    the file and not the clip. Returns the .anim.toml's path."""
    directory = pathlib.Path(directory)
    rotation = [turn((0, 1, 0), 0)] if not degrees else [turn((0, 1, 0), 0), turn((0, 1, 0), degrees)]
    channels = [channel(0, "translation", [0.0, 1.0], [(0.0, 0.0, 0.0), (reach, 0.0, 0.0)]),
                channel(0, "rotation", [0.0, 1.0][:len(rotation)], rotation)]
    nodes = [{"name": "camera"}] + [{"name": prop} for prop in props]
    (directory / (name + ".glb")).write_bytes(gltf_write.build_glb(nodes, [{"name": name, "channels": channels}]))
    clip = directory / (name + tracks_asset.SUFFIX)
    clip.write_text('source = "%s.glb"\nanimation = "%s"\n' % (name, name))
    return clip


def probe_entry():
    """The probe clip's TRCK bytes."""
    document, binary = gltf_read.parse_glb(probe_glb())
    tracks, duration_ms = tracks_asset.clip_tracks(document, binary, document["animations"][0])
    return tracks_asset.encode(tracks, duration_ms)


def single(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def sample_times(track, duration_ms):
    """(seconds, exact) to sample a track at: its keys, either side of its
    ends, and the clip's clock every EVERY_MS ms, looping."""
    times = track["times"]
    copies = track["interpolation"] == "STEP" or len(times) == 1
    out = [(t, copies or not track["quaternion"] or t in (times[0], times[-1])) for t in times]
    out += [(times[0] - 0.25, True), (times[-1] + 0.25, True)]
    for t_ms in range(0, UNTIL_MS, EVERY_MS):
        seconds = single((t_ms % duration_ms) * 0.001)
        out.append((seconds, copies or seconds <= times[0] or seconds >= times[-1]))
    return [(single(seconds), exact) for seconds, exact in out]


def reference_entry(entry):
    tracks, duration_ms = tracks_asset.decode(entry)
    rows = []
    for index, track in enumerate(tracks):
        for seconds, exact in sample_times(track, duration_ms):
            want = list(tracks_asset.sample(track, seconds)) + [0.0] * (4 - len(track["values"][0]))
            rows.append(REFERENCE_ROW.pack(index, 1 if exact else 0, seconds, *want))
    return REFERENCE_HEADER.pack(duration_ms, len(rows)) + b"".join(rows)


def probe_pack():
    entry = probe_entry()
    return build_pack([("probe", tracks_asset.TYPE, entry), ("probe_ref", REFERENCE, reference_entry(entry))])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("-o", "--out", required=True, help="the pack to write")
    args = parser.parse_args(argv)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(probe_pack())
    return 0


if __name__ == "__main__":
    sys.exit(main())

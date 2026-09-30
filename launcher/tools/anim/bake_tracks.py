#!/usr/bin/env python3
"""Bake one glTF 2.0 animation into C tracks for main/anim/anim_track.h.

    python tools/anim/bake_tracks.py ASSET.glb --animation NAME --name PREFIX --out-dir DIR

Writes DIR/PREFIX_tracks_generated.c and .h: one `anim_track_t` per channel
of the animation, named PREFIX_<node>_<translation|rotation|scale> or, for a
KHR_animation_pointer channel, PREFIX_<the pointer path>, plus the table
PREFIX_tracks[] of every track under the name it has in the file. Keys,
tangents and interpolation are copied as authored; the runtime does the
sampling, and tools/anim/tests holds it to the Python sampler in
tools/r3d/gltf_skin.py.

The reader and sampler are gltf_skin's. Any node, and any property a
pointer names, bakes the same way; nothing here knows what a track drives.
"""

import argparse
import math
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import gltf_skin  # noqa: E402

INTERPOLATIONS = {"STEP": "ANIM_STEP", "LINEAR": "ANIM_LINEAR", "CUBICSPLINE": "ANIM_CUBIC"}
PATHS = ("translation", "rotation", "scale")


def fail(message):
    sys.exit("bake_tracks.py: " + message)


def identifier(text):
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")


def channel_name(document, channel):
    """The name a channel has in the file: 'node/path', or its pointer."""
    if channel["pointer"]:
        return channel["pointer"]
    if channel["path"] not in PATHS or channel["node"] is None:
        fail("a channel targets %r without a node or a KHR_animation_pointer" % channel["path"])
    node = document["nodes"][channel["node"]]
    return "%s/%s" % (node.get("name") or "node%d" % channel["node"], channel["path"])


def check(name, channel):
    times, values = channel["times"], channel["values"]
    keys = len(times)
    per_key = 3 if channel["interpolation"] == "CUBICSPLINE" else 1
    if channel["interpolation"] not in INTERPOLATIONS:
        fail("%s: interpolation %s is not STEP, LINEAR or CUBICSPLINE" % (name, channel["interpolation"]))
    if keys < 1 or keys > 0xFFFF or len(values) != per_key * keys:
        fail("%s: %d times but %d values" % (name, keys, len(values)))
    if any(b <= a for a, b in zip(times, times[1:])):
        fail("%s: key times are not strictly increasing" % name)
    if not 1 <= len(values[0]) <= 4:
        fail("%s: a value of %d components; tracks hold 1 to 4" % (name, len(values[0])))
    if channel["path"] == "rotation" and len(values[0]) != 4:
        fail("%s: a rotation is a quaternion" % name)


def literal(number):
    if not math.isfinite(number):
        fail("a key holds %r" % number)
    text = "%.9g" % number
    return text + ("F" if any(c in text for c in ".e") else ".0F")


def floats(numbers):
    return ", ".join(literal(n) for n in numbers)


def emit(document, binary, animation, name, command):
    banner = "/*\n * GENERATED FILE - do not edit.\n *\n *     %s\n *\n" % command
    banner += " * Animation %r of the glTF, baked as authored.\n */\n" % animation.get("name", "")
    channels = gltf_skin.read_animation(document, binary, animation)
    body, table, names = [], [], set()
    for channel in channels:
        track_name = channel_name(document, channel)
        check(track_name, channel)
        symbol = "%s_%s" % (name, identifier(track_name))
        if symbol in names:
            fail("two channels bake to %s" % symbol)
        names.add(symbol)
        flat = [x for row in channel["values"] for x in row]
        cubic = channel["interpolation"] == "CUBICSPLINE"
        body.append("static const float %s_times[] = {%s};\n" % (symbol, floats(channel["times"])))
        body.append("static const float %s_values[] = {%s};\n" % (symbol, floats(flat)))
        body.append("const anim_track_t %s = {%s_times, %s_values, %d, %d, %s, %d};\n\n" % (
            symbol, symbol, symbol, len(channel["times"]), len(channel["values"][0]),
            INTERPOLATIONS[channel["interpolation"]], 1 if channel["path"] == "rotation" else 0))
        table.append((track_name, symbol))
    entries = "".join('    {"%s", &%s},\n' % entry for entry in table)
    source = banner + '\n#include "%s_tracks_generated.h"\n\n' % name + "".join(body)
    source += "const anim_named_track_t %s_tracks[] = {\n%s};\n\n" % (name, entries)
    source += "const int %s_track_count = %d;\n" % (name, len(table))
    header = banner + '\n#pragma once\n\n#include "anim/anim_track.h"\n\n'
    header += "".join("extern const anim_track_t %s;\n" % s for _, s in table)
    header += "\nextern const anim_named_track_t %s_tracks[];\nextern const int %s_track_count;\n" % (name, name)
    return source, header


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("glb")
    parser.add_argument("--animation", required=True, help="the animation's name in the file")
    parser.add_argument("--name", required=True, help="the prefix of every symbol")
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)

    document, binary = gltf_skin.load_glb(args.glb)
    found = [a for a in document.get("animations", []) if a.get("name") == args.animation]
    if not found:
        fail("no animation named %r in %s" % (args.animation, args.glb))
    command = "python tools/anim/bake_tracks.py %s --animation %s --name %s --out-dir %s" % (
        args.glb, args.animation, args.name, args.out_dir)
    source, header = emit(document, binary, found[0], args.name, command)
    os.makedirs(args.out_dir, exist_ok=True)
    stem = os.path.join(args.out_dir, args.name + "_tracks_generated")
    for suffix, text in ((".c", source), (".h", header)):
        with open(stem + suffix, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)


if __name__ == "__main__":
    main()

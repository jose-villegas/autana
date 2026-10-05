#!/usr/bin/env python3
"""Bake one glTF 2.0 animation into C tracks for main/anim/anim_track.h.

    python tools/anim/bake_tracks.py ASSET.glb --animation NAME --name PREFIX --out-dir DIR

Writes DIR/PREFIX_tracks_generated.c and .h: one `anim_track_t` per channel
of the animation, named PREFIX_<node>_<translation|rotation|scale> or, for a
KHR_animation_pointer channel, PREFIX_<object name>_<the rest of the path>;
the clip PREFIX_clip, the duration they play over on one timeline;
PREFIX_tracks[] and PREFIX_track_count, every track; and PREFIX_track_names[],
each track's name in the file, in the same order. Objects are named by
their glTF name, so a re-export that reorders nodes binds the same symbols.
The tables are separate and nothing in the firmware refers to them, so the
linker drops them. The tracks, their checks and the collapse of a channel
that never changes are anim/tracks_asset.py's, which bakes the same
animation into a pack entry; the runtime does the sampling, and tools/tests
holds it to the Python sampler in tools/gltf.

Any node, and any property a pointer names, bakes the same way; nothing here
knows what a track drives.
"""

import argparse
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset  # noqa: E402
from gltf import gltf_read  # noqa: E402

INTERPOLATIONS = {"STEP": "ANIM_STEP", "LINEAR": "ANIM_LINEAR", "CUBICSPLINE": "ANIM_CUBIC"}


def fail(message):
    sys.exit("bake_tracks.py: " + message)


def identifier(text):
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")


def literal(number):
    text = "%.9g" % number
    return text + ("F" if any(c in text for c in ".e") else ".0F")


def floats(numbers):
    return ", ".join(literal(n) for n in numbers)


def emit(document, binary, animation, name, command):
    banner = "/*\n * GENERATED FILE - do not edit.\n *\n *     %s\n *\n" % command
    banner += " * Animation %r of the glTF, baked as authored.\n */\n" % animation.get("name", "")
    tracks, duration_ms = tracks_asset.clip_tracks(document, binary, animation)
    body, table, names = [], [], set()
    for track in tracks:
        symbol = "%s_%s" % (name, identifier(track["name"]))
        if symbol in names:
            fail("two channels bake to %s" % symbol)
        names.add(symbol)
        flat = [x for row in track["values"] for x in row]
        kind = INTERPOLATIONS[track["interpolation"]]
        body.append("static const float %s_times[] = {%s};\n" % (symbol, floats(track["times"])))
        body.append("static const float %s_values[] = {%s};\n" % (symbol, floats(flat)))
        body.append("const anim_track_t %s = {%s_times, %s_values, %d, %d, %s, %d};\n\n" % (
            symbol, symbol, symbol, len(track["times"]), len(track["values"][0]), kind,
            1 if track["quaternion"] else 0))
        table.append((track["name"], symbol))
    pointers = "".join("    &%s,\n" % symbol for _, symbol in table)
    labels = "".join('    "%s",\n' % label for label, _ in table)
    source = banner + '\n#include "%s_tracks_generated.h"\n\n' % name + "".join(body)
    source += "const anim_track_t* const %s_tracks[] = {\n%s};\n\n" % (name, pointers)
    source += "const int %s_track_count = %d;\n\n" % (name, len(table))
    source += "const anim_clip_t %s_clip = {%d};\n\n" % (name, duration_ms)
    source += "const char* const %s_track_names[] = {\n%s};\n" % (name, labels)
    header = banner + '\n#pragma once\n\n#include "anim/anim_track.h"\n\n'
    header += "".join("extern const anim_track_t %s;\n" % s for _, s in table)
    header += "\nextern const anim_track_t* const %s_tracks[];\nextern const int %s_track_count;\n" % (name, name)
    header += "extern const anim_clip_t %s_clip;\nextern const char* const %s_track_names[];\n" % (name, name)
    return source, header


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("glb")
    parser.add_argument("--animation", required=True, help="the animation's name in the file")
    parser.add_argument("--name", required=True, help="the prefix of every symbol")
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)

    command = "python tools/anim/bake_tracks.py %s --animation %s --name %s --out-dir %s" % (
        args.glb, args.animation, args.name, args.out_dir)
    try:
        document, binary = gltf_read.load_glb(args.glb)
        animation = tracks_asset.find_animation(document, args.animation, args.glb)
        source, header = emit(document, binary, animation, args.name, command)
    except tracks_asset.TracksError as error:
        fail(str(error))
    os.makedirs(args.out_dir, exist_ok=True)
    stem = os.path.join(args.out_dir, args.name + "_tracks_generated")
    for suffix, text in ((".c", source), (".h", header)):
        with open(stem + suffix, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)


if __name__ == "__main__":
    main()

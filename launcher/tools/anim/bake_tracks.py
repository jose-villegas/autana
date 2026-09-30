#!/usr/bin/env python3
"""Bake one glTF 2.0 animation into C tracks for main/anim/anim_track.h.

    python tools/anim/bake_tracks.py ASSET.glb --animation NAME --name PREFIX --out-dir DIR

Writes DIR/PREFIX_tracks_generated.c and .h: one `anim_track_t` per channel
of the animation, named PREFIX_<node>_<translation|rotation|scale> or, for a
KHR_animation_pointer channel, PREFIX_<object name>_<the rest of the path>;
the clip PREFIX_clip that plays them on one timeline; and PREFIX_track_names[],
each track's name in the file, in the clip's order. Objects are named by
their glTF name, so a re-export that reorders nodes binds the same symbols.
The names are a separate table nothing in the firmware refers to, so the
linker drops it. Keys, tangents and interpolation are copied as authored,
except that a channel that never changes is one key; the runtime does the
sampling, and tools/tests holds it to the Python sampler in tools/gltf.

Any node, and any property a pointer names, bakes the same way; nothing here
knows what a track drives.
"""

import argparse
import math
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf import gltf_read  # noqa: E402

INTERPOLATIONS = {"STEP": "ANIM_STEP", "LINEAR": "ANIM_LINEAR", "CUBICSPLINE": "ANIM_CUBIC"}
PATHS = ("translation", "rotation", "scale")
POINTER = re.compile(r"^/([A-Za-z]+)/(\d+)/(.+)$")


def fail(message):
    sys.exit("bake_tracks.py: " + message)


def identifier(text):
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")


def object_name(document, collection, index):
    items = document.get(collection, [])
    if index >= len(items):
        fail("a pointer names /%s/%d, which the file does not have" % (collection, index))
    return items[index].get("name") or "%s%d" % (collection, index)


def channel_name(document, channel):
    """The name a channel has in the file: 'node/path', or its pointer with
    the object's index replaced by its name."""
    if channel["pointer"]:
        match = POINTER.match(channel["pointer"])
        if not match:
            fail("pointer %s is not /collection/index/property" % channel["pointer"])
        collection, index, rest = match.group(1), int(match.group(2)), match.group(3)
        return "%s/%s" % (object_name(document, collection, index), rest)
    if channel["path"] not in PATHS or channel["node"] is None:
        fail("a channel targets %r without a node or a KHR_animation_pointer" % channel["path"])
    return "%s/%s" % (object_name(document, "nodes", channel["node"]), channel["path"])


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
    if gltf_read.is_rotation(channel) and len(values[0]) != 4:
        fail("%s: a rotation is a quaternion" % name)


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
    channels = gltf_read.read_animation(document, binary, animation)
    duration_ms = round(gltf_read.animation_duration(channels) * 1000)
    body, table, names = [], [], set()
    for channel in channels:
        track_name = channel_name(document, channel)
        check(track_name, channel)
        symbol = "%s_%s" % (name, identifier(track_name))
        if symbol in names:
            fail("two channels bake to %s" % symbol)
        names.add(symbol)
        channel = collapse_constant(channel)
        flat = [x for row in channel["values"] for x in row]
        kind = INTERPOLATIONS[channel["interpolation"]]
        body.append("static const float %s_times[] = {%s};\n" % (symbol, floats(channel["times"])))
        body.append("static const float %s_values[] = {%s};\n" % (symbol, floats(flat)))
        body.append("const anim_track_t %s = {%s_times, %s_values, %d, %d, %s, %d};\n\n" % (
            symbol, symbol, symbol, len(channel["times"]), len(channel["values"][0]), kind,
            1 if gltf_read.is_rotation(channel) else 0))
        table.append((track_name, symbol))
    pointers = "".join("    &%s,\n" % symbol for _, symbol in table)
    labels = "".join('    "%s",\n' % label for label, _ in table)
    source = banner + '\n#include "%s_tracks_generated.h"\n\n' % name + "".join(body)
    source += "static const anim_track_t* const %s_clip_tracks[] = {\n%s};\n\n" % (name, pointers)
    source += "const anim_clip_t %s_clip = {%s_clip_tracks, %d, %d};\n\n" % (name, name, len(table), duration_ms)
    source += "const char* const %s_track_names[] = {\n%s};\n" % (name, labels)
    header = banner + '\n#pragma once\n\n#include "anim/anim_track.h"\n\n'
    header += "".join("extern const anim_track_t %s;\n" % s for _, s in table)
    header += "\nextern const anim_clip_t %s_clip;\nextern const char* const %s_track_names[];\n" % (name, name)
    return source, header


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("glb")
    parser.add_argument("--animation", required=True, help="the animation's name in the file")
    parser.add_argument("--name", required=True, help="the prefix of every symbol")
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)

    document, binary = gltf_read.load_glb(args.glb)
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

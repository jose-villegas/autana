#!/usr/bin/env python3
"""Seeded padding sizes for the firmware's code and rodata layout.

Where hot code and its constants land modulo a cache way moves a frame's time
by a fraction of a percent. Seed 0 is the build as it is; seed N > 0 puts a
never-run pad ahead of each source's code and of its flash rodata, a whole
number of cache lines up to one way less a line, so a set of seeds samples the
layouts a change could have landed on. main/CMakeLists.txt calls this at
configure time with the cache geometry from sdkconfig:

  layout_pad.py SEED TEXT_LINE TEXT_WAY RODATA_LINE RODATA_WAY SOURCE [SOURCE ...]

and reads back "SOURCE,TEXT,RODATA;..." on one line. After a seeded link,
launcher/CMakeLists.txt checks the pads landed where they shift code:

  layout_pad.py --check MAP

fails unless the map holds pads and each one starts its object's code (or
rodata) in the flash image.
"""

import hashlib
import re
import sys
from collections import defaultdict

# The flash output sections a pad shifts, and the input sections of an object
# its pad must precede there. Literal pools are left out: the Xtensa linker
# gathers them ahead of all code. So are mergeable strings and constants
# (.strN, .cstN): the linker pools those across objects.
PADDED = {".flash.text": (".text.layout_pad", re.compile(r"\.text")),
          ".flash.rodata": (".rodata.layout_pad", re.compile(r"\.rodata(?!.*\.(str|cst)\d)"))}
OUTPUT = re.compile(r"^(\.\S+)")
INPUT = re.compile(r"^ (\.\S+)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*))?$")
PLACED = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")


def steps(seed, source, kind, count):
    digest = hashlib.sha256(f"{seed}:{source}:{kind}".encode()).digest()
    return int.from_bytes(digest[:8], "big") % count


def pad_sizes(seed, source, text=(32, 4096), rodata=(64, 8192)):
    """The (code, rodata) pad bytes for one source file under `seed`; the
    source is named by its path below the main component, forward slashes.
    Each geometry is (line bytes, way bytes): a pad is 0 to way - line bytes
    in whole lines."""
    if seed == 0:
        return 0, 0
    return tuple(steps(seed, source, kind, way // line) * line
                 for kind, (line, way) in (("text", text), ("rodata", rodata)))


def placed_sections(map_lines):
    """Yield (output section, input section, address, size, object) for every
    input section a GNU ld map lists."""
    output = None
    pending = None
    for line in map_lines:
        line = line.rstrip("\n")
        start = OUTPUT.match(line)
        if start:
            output, pending = start.group(1), None
            continue
        entry = INPUT.match(line)
        if entry:
            pending = entry.group(1)
            if entry.group(2) is None:
                continue
            address, size, obj = entry.group(2, 3, 4)
        else:
            placed = PLACED.match(line)
            if not (placed and pending):
                pending = None
                continue
            address, size, obj = placed.groups()
        yield output, pending, int(address, 16), int(size, 16), obj.strip()
        pending = None


def misplaced_pads(map_lines):
    """The pads that do not start their object's code or rodata, as
    "object: pad at A, SECTION at B" lines, and how many pads were found.

    The map names an archive member by file name only, and every object has
    one plain .text entry, so that count tells how many objects share a name.
    Where several do, the name's first section must be a pad when all of them
    are padded; when one is not, the map cannot tell which code is whose."""
    objects = defaultdict(int)
    pads, sections = defaultdict(list), defaultdict(list)
    for output, section, address, size, obj in placed_sections(map_lines):
        if section == ".text":
            objects[obj] += 1
        if output not in PADDED or not size:
            continue
        pad, kind = PADDED[output]
        if section == pad:
            pads[(output, obj)].append(address)
        elif kind.match(section):
            sections[(output, obj)].append((address, section))
    problems = []
    for (output, obj), addresses in sorted(pads.items()):
        if objects[obj] > len(addresses):
            continue
        first = min(sections[(output, obj)], default=None)
        if first is not None and first[0] < min(addresses):
            problems.append(f"{obj}: pad at {min(addresses):#x}, {first[1]} at {first[0]:#x}")
    return problems, sum(len(addresses) for addresses in pads.values())


def check(map_path):
    with open(map_path, encoding="utf-8", errors="replace") as handle:
        problems, count = misplaced_pads(handle)
    if not count:
        sys.exit(f"{map_path}: a seeded build linked no layout pads")
    if problems:
        sys.exit(f"{map_path}: {len(problems)} of {count} layout pads do not lead their object, "
                 "so the seed does not move that object's code:\n  " + "\n  ".join(problems[:20]))
    print(f"layout pads: all {count} lead their object")


def main(argv):
    if len(argv) == 2 and argv[0] == "--check":
        check(argv[1])
        return
    if len(argv) < 6 or not all(word.isdigit() for word in argv[:5]):
        sys.exit("usage: layout_pad.py SEED TEXT_LINE TEXT_WAY RODATA_LINE RODATA_WAY "
                 "SOURCE [SOURCE ...]")
    seed, text_line, text_way, rodata_line, rodata_way = (int(word) for word in argv[:5])
    rows = []
    for source in argv[5:]:
        text, data = pad_sizes(seed, source, (text_line, text_way), (rodata_line, rodata_way))
        rows.append(f"{source},{text},{data}")
    print(";".join(rows), end="")


if __name__ == "__main__":
    main(sys.argv[1:])

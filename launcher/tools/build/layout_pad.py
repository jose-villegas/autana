#!/usr/bin/env python3
"""Seeded padding sizes for the firmware's code and rodata layout.

Where hot code and its constants land modulo a cache way (4 KB of
instruction cache, 8 KB of data cache) moves a frame's time by a fraction of
a percent. Seed 0 is the build as it is; seed N > 0 puts a never-run pad
ahead of each source's code (0..4064 B in 32 B steps) and ahead of its flash
rodata (0..8128 B in 64 B steps), so a set of seeds samples the layouts a
change could have landed on. main/CMakeLists.txt calls this at configure time:

  layout_pad.py SEED SOURCE [SOURCE ...]     prints "SOURCE,TEXT,RODATA;..." on one line
"""

import hashlib
import sys

TEXT_STEP = 32
TEXT_STEPS = 128
RODATA_STEP = 64
RODATA_STEPS = 128


def steps(seed, source, kind, count):
    digest = hashlib.sha256(f"{seed}:{source}:{kind}".encode()).digest()
    return int.from_bytes(digest[:8], "big") % count


def pad_sizes(seed, source):
    """The (code, rodata) pad bytes for one source file under `seed`; the
    source is named by its path below the main component, forward slashes."""
    if seed == 0:
        return 0, 0
    return (steps(seed, source, "text", TEXT_STEPS) * TEXT_STEP,
            steps(seed, source, "rodata", RODATA_STEPS) * RODATA_STEP)


def main(argv):
    if len(argv) < 2 or not argv[0].isdigit():
        sys.exit("usage: layout_pad.py SEED SOURCE [SOURCE ...]")
    seed = int(argv[0])
    rows = []
    for source in argv[1:]:
        text, rodata = pad_sizes(seed, source)
        rows.append(f"{source},{text},{rodata}")
    print(";".join(rows), end="")


if __name__ == "__main__":
    main(sys.argv[1:])

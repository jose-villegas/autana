#!/usr/bin/env python3
"""Seeded padding sizes for the firmware's code and rodata layout.

Where hot code and its constants land modulo a cache way moves a frame's time
by a fraction of a percent. Seed 0 is the build as it is; seed N > 0 puts a
never-run pad ahead of each source's code and of its flash rodata, a whole
number of cache lines up to one way less a line, so a set of seeds samples the
layouts a change could have landed on. main/CMakeLists.txt calls this at
configure time with the cache geometry from sdkconfig:

  layout_pad.py SEED TEXT_LINE TEXT_WAY RODATA_LINE RODATA_WAY SOURCE [SOURCE ...]

and reads back "SOURCE,TEXT,RODATA;..." on one line.
"""

import hashlib
import sys


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


def main(argv):
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

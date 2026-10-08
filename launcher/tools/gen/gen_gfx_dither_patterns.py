#!/usr/bin/env python3
"""Generate the ordered threshold ranks used by gfx_dither.h.

Run from launcher/:
    python tools/gen/gen_gfx_dither_patterns.py main/gfx/draw/gfx_dither_patterns_generated.h
"""

import argparse
from pathlib import Path


def clustered(size):
    cells = [(x, y) for y in range(size) for x in range(size)]
    cells.sort(key=lambda p: ((2 * p[0] - (size - 1)) ** 2 + (2 * p[1] - (size - 1)) ** 2, p[1], p[0]))
    out = [0] * (size * size)
    for rank, (x, y) in enumerate(cells):
        out[y * size + x] = rank
    return out


def blue_noise(size):
    values = list(range(size * size))
    state = 0xD17E3
    for i in range(len(values) - 1, 0, -1):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        j = state % (i + 1)
        values[i], values[j] = values[j], values[i]
    return values


def emit_array(name, values, width):
    rows = []
    for start in range(0, len(values), width):
        rows.append("    " + ", ".join(f"{value:3}" for value in values[start:start + width]) + ",")
    return f"static const uint16_t {name}[{len(values)}] = {{\n" + "\n".join(rows) + "\n};\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    tables = [
        ("gfx_dither_cluster4", clustered(4), 4),
        ("gfx_dither_cluster8", clustered(8), 8),
        ("gfx_dither_blue32", blue_noise(32), 16),
    ]
    for _, values, _ in tables:
        if len(values) != len(set(values)):
            raise SystemExit("threshold ranks must be a permutation")
    arrays = [emit_array(name, values, width) for name, values, width in tables]

    args.output.write_text(
        "/* GENERATED FILE - do not edit.\n"
        " *\n"
        " *     python tools/gen/gen_gfx_dither_patterns.py main/gfx/draw/gfx_dither_patterns_generated.h\n"
        " */\n"
        "#pragma once\n\n"
        "#include <stdint.h>\n\n" + "\n".join(arrays),
        newline="\n",
    )


if __name__ == "__main__":
    main()

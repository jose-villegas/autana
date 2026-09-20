#!/usr/bin/env python3
"""Bake Cerro Autana's ridge line into a table of heights.

    python tools/gen_ridge_curve.py ../design/boot/ridge.png main/ui/ridge_curve_generated.h [--check]

design/boot/ridge.png is the ridge of design/boot/boot.png drawn as a soft
line on black, in the same 448x368 frame. The line is the authored source
rather than the photograph because pale cloud sits behind a pale rock face:
where the mountain ends there is a judgement, and the drawing records it.

Each column's height is the brightness-weighted centre of the line, so the
table is finer than a pixel. The drawing's own brightness and width are not
baked: how the curve glows belongs to whatever renders it.
"""

import argparse
import sys
from pathlib import Path

VIEW_W = 448
VIEW_H = 368
Q_SHIFT = 4
MAX_NEIGHBOUR_STEP_PX = 8
VALUES_PER_LINE = 12


def fail(message):
    raise ValueError(f"gen_ridge_curve.py: {message}")


def column_height(column, x):
    """The line's centre in one column; the column must cross it exactly once."""
    peak = max(column)
    lit = [(y, value) for y, value in enumerate(column) if value * 2 >= peak]
    if lit[-1][0] - lit[0][0] + 1 != len(lit):
        fail(f"column {x} crosses the line more than once; the curve must be one height per column")
    return sum(y * value for y, value in lit) / sum(value for _, value in lit)


def heights(image):
    if image.size != (VIEW_W, VIEW_H):
        fail(f"image is {image.size[0]}x{image.size[1]}, expected {VIEW_W}x{VIEW_H}")
    pixels = image.load()
    # Measured against the drawing's brightest pixel, not the column's own: a
    # faint stray mark in an empty column must not pass for the line.
    brightest = max(pixels[x, y] for x in range(VIEW_W) for y in range(VIEW_H))
    result = []
    for x in range(VIEW_W):
        column = [pixels[x, y] for y in range(VIEW_H)]
        if brightest == 0 or max(column) * 2 < brightest:
            fail(f"column {x} has no line")
        result.append(column_height(column, x))
    for x in range(1, VIEW_W):
        step = abs(result[x] - result[x - 1])
        if step > MAX_NEIGHBOUR_STEP_PX:
            fail(f"the line jumps {step:.1f} px between columns {x - 1} and {x}")
    return result


def generate(image):
    scaled = [round(height * (1 << Q_SHIFT)) for height in heights(image)]
    lines = [
        "/*",
        " * GENERATED FILE - do not edit.",
        " *",
        " *     python tools/gen_ridge_curve.py ../design/boot/ridge.png main/ui/ridge_curve_generated.h",
        " *",
        " * Cerro Autana's ridge line, one height per column of the 448x368 frame",
        " * design/boot/boot.png is drawn in, so a curve drawn from this table lies",
        " * on the photograph's own ridge. y grows downward and carries",
        " * RIDGE_CURVE_Q_SHIFT fractional bits.",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define RIDGE_CURVE_POINTS  {VIEW_W}",
        f"#define RIDGE_CURVE_VIEW_H  {VIEW_H}",
        f"#define RIDGE_CURVE_Q_SHIFT {Q_SHIFT}",
        "",
        "static const int16_t ridge_curve_y[RIDGE_CURVE_POINTS] = {",
    ]
    for start in range(0, len(scaled), VALUES_PER_LINE):
        row = ", ".join(f"{value:4d}" for value in scaled[start : start + VALUES_PER_LINE])
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true", help="fail if the output file is stale")
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        sys.exit("gen_ridge_curve.py: needs Pillow - pip install pillow")
    try:
        baked = generate(Image.open(args.source).convert("L"))
        if args.check:
            if args.output.read_text(encoding="utf-8") != baked:
                parser.exit(1, f"{args.output} is stale; regenerate it\n")
        else:
            args.output.write_text(baked, encoding="utf-8", newline="\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

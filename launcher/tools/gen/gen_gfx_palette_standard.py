#!/usr/bin/env python3
"""Generate main/gfx/gfx_palette_standard_generated.h - the two formulaic
256-entry standard palettes (arithmetic, not curated, so a table beats 512
hand-typed literals): a VGA-style default (16 EGA + a 216-colour 6x6x6
"web-safe" cube + 24 grays) and a plain 256-level grayscale ramp.

    python tools/gen_gfx_palette_standard.py > main/gfx/gfx_palette_standard_generated.h

The EGA 16, PICO-8, DawnBringer DB16/DB32 and 16-level grayscale palettes
are curated, small, and hand-typed directly in gfx_palette_standard.c
instead - a generator would not make those any more trustworthy than
citing the source next to the literals.
"""

import sys

CUBE_LEVELS = [0, 51, 102, 153, 204, 255]


def to_rgb565(rgb888):
    """GFX_RGB565(rgb) (gfx_color.h): truncating, not rounding - two 24-bit
    colours this close together can and do collapse to the same RGB565
    value, which is why every dedup below happens in THIS space, not
    24-bit RGB888."""
    r, g, b = (rgb888 >> 16) & 0xFF, (rgb888 >> 8) & 0xFF, rgb888 & 0xFF
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

EGA16 = [
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
]


def web_safe_cube():
    for r in CUBE_LEVELS:
        for g in CUBE_LEVELS:
            for b in CUBE_LEVELS:
                yield (r << 16) | (g << 8) | b


def vga256():
    # 16 EGA + a 216-colour web-safe cube + enough evenly spaced grays to
    # reach 256, skipping any colour already present - EGA's own black and
    # white already sit at two of the cube's own corners, so the traditional
    # "16 + 216 + 24" count is only exact once those two are not double-
    # counted, and this fills the resulting gap from the gray ramp instead
    # of asserting a fixed 24.
    seen565 = set()
    colours = []

    def add(rgb):
        key = to_rgb565(rgb)
        if key not in seen565:
            seen565.add(key)
            colours.append(rgb)

    for rgb in EGA16:
        add(rgb)
    for rgb in web_safe_cube():
        add(rgb)

    # Grays spaced 8 apart guarantee a new 5-bit (R/B) bucket each step -
    # the tightest channel RGB565 has - so this never wastes a candidate
    # the way a step of 1 did once collisions are judged in RGB565 space.
    step = 8
    while len(colours) < 256:
        for v in range(0, 256, step):
            if len(colours) >= 256:
                break
            add((v << 16) | (v << 8) | v)
        step = max(1, step - 1)
        assert step > 0 or len(colours) >= 256, "ran out of distinct grays to pad vga256 with"

    assert len(colours) == 256, len(colours)
    assert len({to_rgb565(c) for c in colours}) == 256, "vga256 has an RGB565-space duplicate"
    return colours


def grayscale256():
    return [(v << 16) | (v << 8) | v for v in range(256)]


def emit_array(f, name, colours):
    f.write(f"static const gfx_color_t {name}[256] = {{\n")
    for i, rgb in enumerate(colours):
        sep = "    " if i % 8 == 0 else " "
        f.write(f"{sep}GFX_RGB(0x{rgb:06X}),")
        if i % 8 == 7:
            f.write("\n")
    f.write("};\n\n")


def main():
    # Every text file in the tree is LF; plain stdout redirection on
    # Windows would otherwise write CRLF.
    sys.stdout.reconfigure(newline="\n")
    f = sys.stdout
    f.write(
        "/*=============================================================="
        "=============\n"
        " * GENERATED FILE - do not edit.\n"
        " *\n"
        " *     python tools/gen_gfx_palette_standard.py > "
        "main/gfx/gfx_palette_standard_generated.h\n"
        " *\n"
        " * Two formulaic 256-entry palettes for gfx_palette_standard.c: a\n"
        " * VGA-style default (16 EGA + a 216-colour web-safe RGB lattice +\n"
        " * enough grays to reach 256) and a plain 256-level grayscale ramp.\n"
        " *========================================================================"
        "===*/\n"
        "#pragma once\n\n"
        "#include \"gfx/gfx_color.h\"\n\n"
    )
    emit_array(f, "gfx_palette_vga256_entries", vga256())
    emit_array(f, "gfx_palette_grayscale256_entries", grayscale256())


if __name__ == "__main__":
    main()

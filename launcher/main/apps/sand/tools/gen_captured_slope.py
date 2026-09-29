#!/usr/bin/env python3
"""Sample a device screenshot into a sand-grid header.

Usage: python gen_captured_slope.py <screenshot.png> > captured_slope_data.h

The screenshot is the raw 368x448 framebuffer at NORMAL quality (4px cells,
a 92x112 grid), in the sim's own coordinate frame - no shell rotation. Each
cell's centre pixel is sampled and classified by colour, then the 92x112
result is upscaled 2x (nearest, each source cell becomes a 2x2 block) to the
184x224 grid the perf suite's own scenes use.

Colour thresholds, read off this scene's own histogram (only these three
classes appear in it):
  - empty: every channel below 20 (the background sits at (8, 12, 16))
  - water: blue-dominant (B > R) - covers every blue shade and the white
    foam flecks on a wave crest, which still carry B slightly above R
  - sand:  everything else (red-dominant, R >= B)

stdlib only - no Pillow. PNG must be 8-bit RGB (colour type 2), no
interlacing, which is what the capture tool writes.
"""
import struct
import sys
import zlib

CELL_PX = 4
DEST_SCALE = 2
MAT_SAND = 1
MAT_WATER = 2
MASS_MAX = 15


def cell_make(material, variant):
    return ((material & 0xF) << 4) | (variant & 0xF)


EMPTY = 0
SAND = cell_make(MAT_SAND, 0)
WATER = cell_make(MAT_WATER, MASS_MAX)


def read_png_rgb8(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    idat = b""
    w = h = None
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        if ctype == b"IHDR":
            w, h, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            if bit_depth != 8 or color_type != 2 or interlace != 0:
                raise ValueError("expected 8-bit RGB, non-interlaced")
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(idat)

    bpp = 3
    stride = w * bpp
    out = bytearray(w * h * bpp)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p : p + stride])
        p += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 0:
                predictor = 0
            elif ftype == 1:
                predictor = a
            elif ftype == 2:
                predictor = b
            elif ftype == 3:
                predictor = (a + b) // 2
            elif ftype == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                predictor = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else:
                raise ValueError("unsupported PNG filter type %d" % ftype)
            line[x] = (line[x] + predictor) & 0xFF
        out[y * stride : (y + 1) * stride] = line
        prev = line
    return w, h, out


def classify(r, g, b):
    if r < 20 and g < 20 and b < 20:
        return EMPTY
    if b > r:
        return WATER
    return SAND


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        sys.exit(1)

    png_path = sys.argv[1]
    w, h, px = read_png_rgb8(png_path)
    if w % CELL_PX != 0 or h % CELL_PX != 0:
        raise ValueError("panel size must be a multiple of %d" % CELL_PX)
    src_w, src_h = w // CELL_PX, h // CELL_PX

    grid = [EMPTY] * (src_w * src_h)
    for cy in range(src_h):
        for cx in range(src_w):
            sx = cx * CELL_PX + CELL_PX // 2
            sy = cy * CELL_PX + CELL_PX // 2
            i = (sy * w + sx) * 3
            grid[cy * src_w + cx] = classify(px[i], px[i + 1], px[i + 2])

    dest_w, dest_h = src_w * DEST_SCALE, src_h * DEST_SCALE
    dest = [EMPTY] * (dest_w * dest_h)
    for cy in range(src_h):
        for cx in range(src_w):
            v = grid[cy * src_w + cx]
            for oy in range(DEST_SCALE):
                for ox in range(DEST_SCALE):
                    dx = cx * DEST_SCALE + ox
                    dy = cy * DEST_SCALE + oy
                    dest[dy * dest_w + dx] = v

    out = sys.stdout
    out.write("/*" + "=" * 77 + "\n")
    out.write(" * GENERATED FILE - do not edit.\n")
    out.write(" *\n")
    out.write(" *     python tools/gen_captured_slope.py <screenshot.png> > captured_slope_data.h\n")
    out.write(" *\n")
    out.write(
        " * A device screenshot of the reported slow scenario - a sand pile\n"
        " * against a diagonal water surface - sampled at its native 4px-per-cell\n"
        " * resolution (92x112) and upscaled 2x (nearest) to the perf suite's own\n"
        " * 184x224 grid. Classified by colour: empty is near-black, water is\n"
        " * blue-dominant, sand is red-dominant - see gen_captured_slope.py for the\n"
        " * exact thresholds. Water cells are filled to MASS_MAX.\n"
    )
    out.write(" *" + "=" * 77 + "*/\n")
    out.write("#pragma once\n\n")
    out.write("#include <stdint.h>\n\n")
    out.write("#define CAPTURED_SLOPE_W %d\n" % dest_w)
    out.write("#define CAPTURED_SLOPE_H %d\n\n" % dest_h)
    out.write("static const uint8_t captured_slope_cells[CAPTURED_SLOPE_W * CAPTURED_SLOPE_H] = {\n")
    for y in range(dest_h):
        row = dest[y * dest_w : (y + 1) * dest_w]
        out.write("    " + ",".join(str(v) for v in row) + ",\n")
    out.write("};\n")


if __name__ == "__main__":
    main()

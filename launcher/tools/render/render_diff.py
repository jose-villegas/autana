"""Compare two frames of this panel pixel for pixel.

    render_diff.py <a> <b> [--quarter-a N] [--quarter-b N]
                   [--mask NAME ...] [--out diff.png]

Either side may be a host render (tools/render/render_scene.sh writes 24bpp BMPs)
or a device capture (autana screenshot writes a PNG plus a .json sidecar).

ORIENTATION IS DECLARED, NEVER GUESSED. A device capture is always the
framebuffer the way the panel holds it - 368 x 448 - whatever the shell was
rotated to at the time; the sidecar's orientation_quarter says which
rotation that was, and is reported here rather than applied. State the
quarter for anything that is a render: a quarter turn comes out 448 x 368
and is refused without one, and half a turn keeps the panel's own shape
while still being upside down, which no size can reveal. Everything is
compared in panel coordinates.

MASKS COVER WHAT THE SHELL DRAWS AND A SCENE DOES NOT - the development
build's corner mark, the swipe-home strip. They are declared in
render_masks.json, per quarter, and named on the command line; nothing is
inferred from the images themselves.

Standard library only: this runs under ESP-IDF's python, which has no
Pillow, the same constraint screenshot.py works under.
"""
import argparse
import json
import os
import struct
import sys
import zlib

PANEL_WIDTH = 368
PANEL_HEIGHT = 448

BMP_HEADER_SIZE = 54


class Image:
    """Rows of (r, g, b) bytes, top row first."""

    def __init__(self, width, height, rows):
        self.width = width
        self.height = height
        self.rows = rows

    def pixel(self, x, y):
        row = self.rows[y]
        return row[x * 3], row[x * 3 + 1], row[x * 3 + 2]


def read_bmp(data):
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    pixel_offset, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    depth, = struct.unpack_from("<H", data, 28)
    if depth != 24:
        raise ValueError("expected a 24bpp BMP, got %dbpp" % depth)
    if height <= 0:
        raise ValueError("expected a positive (bottom-up) height, got %d" % height)

    stride = ((width * 3 + 3) // 4) * 4
    rows = []
    for y in range(height):
        start = pixel_offset + (height - 1 - y) * stride
        bgr = data[start:start + width * 3]
        rgb = bytearray(len(bgr))
        rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
        rows.append(bytes(rgb))
    return Image(width, height, rows)


def _unfilter(raw, width, height, bpp):
    stride = width * bpp
    out = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        method = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = previous[i]
            upleft = previous[i - bpp] if i >= bpp else 0
            if method == 0:
                continue
            if method == 1:
                line[i] = (line[i] + left) & 0xff
            elif method == 2:
                line[i] = (line[i] + up) & 0xff
            elif method == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xff
            elif method == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                if pa <= pb and pa <= pc:
                    nearest = left
                elif pb <= pc:
                    nearest = up
                else:
                    nearest = upleft
                line[i] = (line[i] + nearest) & 0xff
            else:
                raise ValueError("unknown PNG filter %d" % method)
        out.append(bytes(line))
        previous = line
    return out


def read_png(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    header = None
    palette = b""
    idat = []
    while pos < len(data):
        length, = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break

    if header is None:
        raise ValueError("PNG with no IHDR")
    width, height, depth, colour, compression, filtering, interlace = header
    if depth != 8 or compression != 0 or filtering != 0 or interlace != 0:
        raise ValueError("only 8-bit, non-interlaced PNGs are read here")
    if colour not in (0, 2, 3, 6):
        raise ValueError("unsupported PNG colour type %d" % colour)

    samples = {0: 1, 2: 3, 3: 1, 6: 4}[colour]
    lines = _unfilter(zlib.decompress(b"".join(idat)), width, height, samples)

    rows = []
    for line in lines:
        if colour == 2:
            rows.append(line)
            continue
        rgb = bytearray(width * 3)
        for x in range(width):
            if colour == 3:
                index = line[x] * 3
                rgb[x * 3:x * 3 + 3] = palette[index:index + 3]
            elif colour == 0:
                rgb[x * 3] = rgb[x * 3 + 1] = rgb[x * 3 + 2] = line[x]
            else:
                rgb[x * 3:x * 3 + 3] = line[x * 4:x * 4 + 3]
        rows.append(bytes(rgb))
    return Image(width, height, rows)


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] == b"BM":
        return read_bmp(data)
    return read_png(data)


def sidecar_quarter(path):
    """The quarter the shell was at when a capture was taken, or None."""
    state = os.path.splitext(path)[0] + ".json"
    if not os.path.exists(state):
        return None
    with open(state) as f:
        return json.load(f).get("orientation_quarter")


def to_panel(image, quarter, label):
    """The same frame in panel coordinates, turning a read-orientation image
    back by the quarter it was rendered at.

    Half a turn keeps the panel's own shape, so an image that is already
    368 x 448 is not proof of anything: a quarter-2 render is that shape and
    still upside down. Only an undeclared quarter is taken as panel-native,
    which is what a capture always is."""
    upright = (image.width, image.height) == (PANEL_WIDTH, PANEL_HEIGHT)
    turned = (image.width, image.height) == (PANEL_HEIGHT, PANEL_WIDTH)
    if not upright and not turned:
        raise SystemExit("%s: %dx%d is neither the panel nor a quarter turn of it"
                         % (label, image.width, image.height))
    if quarter is None:
        if turned:
            raise SystemExit("%s is %dx%d, so it is in the read orientation - say "
                             "which quarter it was rendered at"
                             % (label, image.width, image.height))
        return image
    if quarter % 2 == 0 and turned:
        raise SystemExit("%s is %dx%d, which quarter %d never produces"
                         % (label, image.width, image.height, quarter))
    if quarter % 2 == 1 and upright:
        raise SystemExit("%s is %dx%d, which quarter %d never produces"
                         % (label, image.width, image.height, quarter))
    if quarter == 0:
        return image

    rows = []
    for y in range(PANEL_HEIGHT):
        row = bytearray(PANEL_WIDTH * 3)
        for x in range(PANEL_WIDTH):
            # The inverse of ui_transform_quarter_turn(): where this panel
            # pixel came from in the upright logical canvas.
            if quarter == 1:
                sx, sy = y, PANEL_WIDTH - 1 - x
            elif quarter == 2:
                sx, sy = PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y
            else:
                sx, sy = PANEL_HEIGHT - 1 - y, x
            row[x * 3:x * 3 + 3] = image.rows[sy][sx * 3:sx * 3 + 3]
        rows.append(bytes(row))
    return Image(PANEL_WIDTH, PANEL_HEIGHT, rows)


def load_masks(path, names, quarter):
    if not names:
        return []
    with open(path) as f:
        declared = json.load(f)

    rects = []
    for name in names:
        if name not in declared:
            raise SystemExit("no mask named %r in %s" % (name, path))
        quarters = declared[name]["quarters"]
        key = str(quarter if quarter is not None else 0)
        if key not in quarters:
            raise SystemExit("mask %r declares no rects for quarter %s" % (name, key))
        rects.extend(quarters[key])
    return rects


def masked(rects, x, y):
    for rx, ry, rw, rh in rects:
        if rx <= x < rx + rw and ry <= y < ry + rh:
            return True
    return False


def write_png(path, width, height, rows):
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--quarter-a", type=int)
    ap.add_argument("--quarter-b", type=int)
    ap.add_argument("--mask", action="append", default=[],
                    help="a region named in render_masks.json, repeatable")
    ap.add_argument("--masks-file",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "render_masks.json"))
    ap.add_argument("--mask-quarter", type=int,
                    help="which quarter's mask rects to use; defaults to a "
                         "capture's own sidecar, then to --quarter-a/b")
    ap.add_argument("--out", help="write a visual diff PNG here")
    args = ap.parse_args(argv)

    a = to_panel(load(args.a), args.quarter_a, args.a)
    b = to_panel(load(args.b), args.quarter_b, args.b)

    shell_quarter = sidecar_quarter(args.a)
    if shell_quarter is None:
        shell_quarter = sidecar_quarter(args.b)
    for path, declared in ((args.a, args.quarter_a), (args.b, args.quarter_b)):
        side = sidecar_quarter(path)
        if side is not None:
            print("%s: a capture, panel-native; its sidecar says the shell was at "
                  "quarter %s" % (path, side))
            if declared is not None and declared != side:
                raise SystemExit("%s was given --quarter %d but its sidecar says %s"
                                 % (path, declared, side))

    mask_quarter = args.mask_quarter
    if mask_quarter is None:
        mask_quarter = shell_quarter
    if mask_quarter is None:
        mask_quarter = args.quarter_a if args.quarter_a is not None else args.quarter_b
    rects = load_masks(args.masks_file, args.mask, mask_quarter)
    if args.mask:
        print("masked: %s at quarter %s (%d rects)"
              % (", ".join(args.mask), mask_quarter, len(rects)))

    first = None
    differing = 0
    covered = 0
    out_rows = []
    for y in range(PANEL_HEIGHT):
        row = bytearray(PANEL_WIDTH * 3)
        for x in range(PANEL_WIDTH):
            pa = a.pixel(x, y)
            pb = b.pixel(x, y)
            same = pa == pb
            if masked(rects, x, y):
                covered += 1
                row[x * 3:x * 3 + 3] = bytes((0, 0, 160))
                continue
            if same:
                grey = (pa[0] * 30 + pa[1] * 59 + pa[2] * 11) // 300
                row[x * 3:x * 3 + 3] = bytes((grey, grey, grey))
                continue
            differing += 1
            if first is None:
                first = (x, y, pa, pb)
            row[x * 3:x * 3 + 3] = bytes((255, 0, 0))
        out_rows.append(row)

    if args.out:
        write_png(args.out, PANEL_WIDTH, PANEL_HEIGHT, out_rows)
        print("wrote %s (red where they differ, blue where a mask covers)" % args.out)

    compared = PANEL_WIDTH * PANEL_HEIGHT - covered
    if first is None:
        print("identical over %d compared pixels (%d masked)" % (compared, covered))
        return 0

    x, y, pa, pb = first
    print("first difference at panel (%d, %d): %s has #%02x%02x%02x, %s has #%02x%02x%02x"
          % (x, y, args.a, pa[0], pa[1], pa[2], args.b, pb[0], pb[1], pb[2]))
    print("%d of %d compared pixels differ (%.3f%%), %d masked"
          % (differing, compared, 100.0 * differing / compared, covered))
    return 1


if __name__ == "__main__":
    sys.exit(main())

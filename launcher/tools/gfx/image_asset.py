"""The image pack entry (IMAG): a picture baked from the NAME.image.toml beside
its PNG, and read back. The one writer of the entry, and its reader on the
host; main/gfx/image/gfx_image.c is the firmware's.

A NAME.image.toml names its source, a PNG in its own folder, and how many
quarter turns clockwise take the source to the stored pixels; the pack id is
NAME. The layout is in docs/assets/README.md, "The image entry". Pixels are
stored as the panel takes them (gfx_color.h's byte-swapped RGB565), so drawing
one is a copy. An icon is the same entry with one bit a pixel
(encode_mono()), which gfx/icons_asset.py writes. Standard library only: the firmware build runs this under
ESP-IDF's python, which has no Pillow.
"""

import pathlib
import struct
import sys
import tomllib

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "device"))
sys.path.insert(0, str(TOOLS / "render"))

import gfx_color  # noqa: E402
from render_diff import read_png  # noqa: E402

TYPE = b"IMAG"
VERSION = 1
SUFFIX = ".image.toml"
# gfx_image_format_t: gfx_color_t pixels, byte-swapped RGB565; and one bit a
# pixel, the most significant leftmost, 1 ink, a row a whole number of bytes.
FORMAT_RGB565 = 1
FORMAT_MONO1 = 2
FORMATS = {FORMAT_RGB565: 16, FORMAT_MONO1: 1}  # bits per pixel
MONO1_STRIDE_STEP = 8  # a MONO1 stride is whole bytes
HEADER = struct.Struct("<HHHHII")
PIXEL_ALIGN = 4
SIDE_MAX = 0xFFFF


class ImageError(ValueError):
    """A picture that cannot be baked, or bytes that are not the entry."""


def encode(width, height, pixels):
    """The entry's bytes for `pixels`, `width` x `height` gfx_color_t values
    in rows, top row first."""
    if not (1 <= width <= SIDE_MAX and 1 <= height <= SIDE_MAX) or len(pixels) != width * height:
        raise ImageError("%d pixels are not a %d x %d image" % (len(pixels), width, height))
    if any(not 0 <= v <= 0xFFFF for v in pixels):
        raise ImageError("a pixel is not a 16-bit colour")
    header = HEADER.pack(VERSION, FORMAT_RGB565, width, height, width, HEADER.size)
    return header + struct.pack("<%dH" % len(pixels), *pixels)


def encode_mono(rows):
    """The MONO1 entry's bytes for `rows`, rows of bools (True ink), top row
    first; the stride is the width rounded up to whole bytes."""
    height, width = len(rows), len(rows[0]) if rows else 0
    if not (1 <= width <= SIDE_MAX and 1 <= height <= SIDE_MAX) or any(len(row) != width for row in rows):
        raise ImageError("rows of %d are not a %d x %d image" % (width, width, height))
    stride = -(-width // MONO1_STRIDE_STEP) * MONO1_STRIDE_STEP
    bits = bytearray(stride // 8 * height)
    for y, row in enumerate(rows):
        for x, ink in enumerate(row):
            if ink:
                bits[y * stride // 8 + x // 8] |= 0x80 >> (x % 8)
    return HEADER.pack(VERSION, FORMAT_MONO1, width, height, stride, HEADER.size) + bytes(bits)


def decode(entry):
    """(width, height, pixels) of an entry's bytes, after the checks
    gfx_image_open() makes; pixels as encode() or encode_mono() takes them."""
    if len(entry) < HEADER.size:
        raise ImageError("shorter than a header")
    version, fmt, width, height, stride, offset = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ImageError("version %d, this reads %d" % (version, VERSION))
    if fmt not in FORMATS or width == 0 or height == 0 or stride < width or offset < HEADER.size \
            or (fmt == FORMAT_MONO1 and stride % MONO1_STRIDE_STEP):
        raise ImageError("a header field holds a value the entry does not allow")
    bits = FORMATS[fmt]
    if offset % PIXEL_ALIGN or offset + (bits * (stride * (height - 1) + width) + 7) // 8 > len(entry):
        raise ImageError("the rows leave the entry or are misaligned")
    if fmt == FORMAT_MONO1:
        return width, height, [[bool(entry[offset + y * stride // 8 + x // 8] & (0x80 >> (x % 8)))
                                for x in range(width)] for y in range(height)]
    pixels = []
    for y in range(height):
        pixels += struct.unpack_from("<%dH" % width, entry, offset + 2 * stride * y)
    return width, height, pixels


def turn(width, height, pixels, quarter_turns):
    """(width, height, pixels) turned clockwise by `quarter_turns`."""
    for _ in range(quarter_turns % 4):
        pixels = [pixels[(height - 1 - x) * width + y] for y in range(width) for x in range(height)]
        width, height = height, width
    return width, height, pixels


def image_id(path):
    """The pack id of a .image.toml: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def load_source(path):
    """(the PNG's path, quarter turns) a .image.toml names."""
    path = pathlib.Path(path)
    try:
        values = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ImageError(f"{path}: {error}") from error
    source, turns = values.get("source"), values.get("quarter_turns", 0)
    if set(values) - {"source", "quarter_turns"} or not isinstance(source, str) \
            or type(turns) is not int or not 0 <= turns <= 3:
        raise ImageError("%s: holds source = \"x.png\" and optionally quarter_turns = 0 to 3" % path)
    # Beside it, so whatever finds the .image.toml finds its source too.
    if pathlib.PurePath(source).name != source or ":" in source or not source.lower().endswith(".png"):
        raise ImageError("%s: source %r is not a .png in the same folder" % (path, source))
    return path.parent / source, turns


def bake(path):
    """The entry's bytes for a .image.toml."""
    source, turns = load_source(path)
    try:
        image = read_png(source.read_bytes())
    except (OSError, ValueError) as error:
        raise ImageError("%s: source %s: %s" % (path, source, error)) from error
    if not image.opaque:
        raise ImageError("%s: source %s is not opaque: an image entry has no alpha" % (path, source))
    pixels = [gfx_color.swap(gfx_color.rgb565(*image.pixel(x, y)))
              for y in range(image.height) for x in range(image.width)]
    return encode(*turn(image.width, image.height, pixels, turns))

"""The image pack entry (IMAG): a picture baked from the NAME.image.toml beside
its PNG, and read back. The one writer of the entry, and its reader on the
host; main/gfx/image/gfx_image.c is the firmware's.

A NAME.image.toml names its source, a PNG in its own folder, and how many
quarter turns clockwise take the source to the stored pixels; the pack id is
NAME. The layout is in docs/assets/README.md, "The image entry". Pixels are
stored as the panel takes them (gfx_color.h's byte-swapped RGB565), so drawing
one is a copy. Standard library only: the firmware build runs this under
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
# gfx_image_format_t: gfx_color_t pixels, byte-swapped RGB565.
FORMAT_RGB565 = 1
FORMATS = {FORMAT_RGB565: 2}  # bytes per pixel
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


def decode(entry):
    """(width, height, pixels) of an entry's bytes, after the checks
    gfx_image_open() makes; pixels as encode() takes them."""
    if len(entry) < HEADER.size:
        raise ImageError("shorter than a header")
    version, fmt, width, height, stride, offset = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ImageError("version %d, this reads %d" % (version, VERSION))
    if fmt not in FORMATS or width == 0 or height == 0 or stride < width or offset < HEADER.size:
        raise ImageError("a header field holds a value the entry does not allow")
    size = FORMATS[fmt]
    if offset % PIXEL_ALIGN or offset + size * (stride * (height - 1) + width) > len(entry):
        raise ImageError("the rows leave the entry or are misaligned")
    pixels = []
    for y in range(height):
        pixels += struct.unpack_from("<%dH" % width, entry, offset + size * stride * y)
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
        with open(path, "rb") as source:
            values = tomllib.load(source)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ImageError("%s: %s" % (path, error)) from error
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

"""The image pack entry (IMAG): a picture baked from the NAME.image.toml beside
its PNG, and read back. The one writer of the entry, and its reader on the
host; main/draw/draw_image.c and main/draw/draw_sprite.c are the firmware's.

A NAME.image.toml names its source, a PNG in its own folder, how many quarter
turns clockwise take the source to the stored pixels and, for a sprite, its
nine-slice border; the pack id is NAME. The layout is in
docs/assets/formats/IMAG.md. Pixels are stored as the panel takes them
(image/pixel/rgb565.h's byte-swapped RGB565), so drawing one is a copy. A
source with any alpha below 255 keeps it as an A8 plane (RGB565A8); a border
makes the entry a sprite (SPRT), the border ahead of the same picture. An
icon is the same entry with one bit a pixel (encode_mono()), which
gfx/icons_asset.py writes. Standard library only: the firmware build runs
this under ESP-IDF's python, which has no Pillow.
"""

import pathlib
import struct
import sys
import tomllib

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "device"))
sys.path.insert(0, str(TOOLS / "render"))

from build.c_header import read_define  # noqa: E402
import gfx_color  # noqa: E402
from render_diff import read_png  # noqa: E402

FORMAT_OWNER = TOOLS.parent / "main/draw/draw_image_format.h"
TYPE = b"IMAG"
SPRITE_TYPE = b"SPRT"
VERSION = read_define(FORMAT_OWNER, "DRAW_IMAGE_VERSION")
SUFFIX = ".image.toml"
# IMAG format codes are owned by draw_image_format.h.
FORMAT_RGB565 = read_define(FORMAT_OWNER, "DRAW_IMAGE_FORMAT_RGB565")
FORMAT_MONO1 = read_define(FORMAT_OWNER, "DRAW_IMAGE_FORMAT_MONO1")
FORMAT_RGB565A8 = read_define(FORMAT_OWNER, "DRAW_IMAGE_FORMAT_RGB565A8")
FORMATS = {FORMAT_RGB565: 16, FORMAT_MONO1: 1, FORMAT_RGB565A8: 16}  # bits per colour pixel
MONO1_STRIDE_STEP = read_define(FORMAT_OWNER, "DRAW_IMAGE_MONO1_STRIDE_STEP")
RGB565A8_STRIDE_STEP = read_define(FORMAT_OWNER, "DRAW_IMAGE_RGB565A8_STRIDE_STEP")
STRIDE_STEPS = {FORMAT_RGB565: 1, FORMAT_MONO1: MONO1_STRIDE_STEP, FORMAT_RGB565A8: RGB565A8_STRIDE_STEP}
HEADER = struct.Struct("<HHHHII")
PIXEL_ALIGN = read_define(FORMAT_OWNER, "DRAW_IMAGE_PIXEL_ALIGN")
SIDE_MAX = 0xFFFF
OPAQUE = 0xFF
SPRITE_VERSION = read_define(FORMAT_OWNER, "DRAW_SPRITE_VERSION")
# version; border left, top, right, bottom; flat centre; centre alpha; centre colour; reserved
SPRITE_HEADER = struct.Struct("<H4BBBHH")
BORDER_MAX = 0xFF
if SPRITE_HEADER.size != read_define(FORMAT_OWNER, "DRAW_SPRITE_HEADER_SIZE"):
    raise ImportError("SPRITE_HEADER disagrees with DRAW_SPRITE_HEADER_SIZE")


class ImageError(ValueError):
    """A picture that cannot be baked, or bytes that are not the entry."""


def encode(width, height, pixels, alpha=None):
    """The entry's bytes for `pixels`, `width` x `height` gfx_color_t values
    in rows, top row first; with `alpha`, one coverage byte a pixel in the
    same order, an RGB565A8 entry whose stride is the width rounded up to
    RGB565A8_STRIDE_STEP."""
    if not (1 <= width <= SIDE_MAX and 1 <= height <= SIDE_MAX) or len(pixels) != width * height:
        raise ImageError("%d pixels are not a %d x %d image" % (len(pixels), width, height))
    if any(not 0 <= v <= 0xFFFF for v in pixels):
        raise ImageError("a pixel is not a 16-bit colour")
    if alpha is None:
        header = HEADER.pack(VERSION, FORMAT_RGB565, width, height, width, HEADER.size)
        return header + struct.pack("<%dH" % len(pixels), *pixels)
    if len(alpha) != len(pixels) or any(not 0 <= a <= OPAQUE for a in alpha):
        raise ImageError("the alpha is not one byte for each pixel")
    stride = -(-width // RGB565A8_STRIDE_STEP) * RGB565A8_STRIDE_STEP
    pad = stride - width
    color, cover = bytearray(), bytearray()
    for y in range(height):
        color += struct.pack("<%dH" % stride, *pixels[y * width:(y + 1) * width], *[0] * pad)
        cover += bytes(alpha[y * width:(y + 1) * width]) + bytes(pad)
    return HEADER.pack(VERSION, FORMAT_RGB565A8, width, height, stride, HEADER.size) + bytes(color) + bytes(cover)


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
    draw_image_open_alpha() makes; pixels as encode() or encode_mono() takes
    them. decode_alpha() also returns an RGB565A8 entry's alpha."""
    return decode_alpha(entry)[:3]


def decode_alpha(entry):
    """(width, height, pixels, alpha) of an entry's bytes; alpha as encode()
    takes it, None for an entry without one."""
    if len(entry) < HEADER.size:
        raise ImageError("shorter than a header")
    version, fmt, width, height, stride, offset = HEADER.unpack_from(entry)
    if version != VERSION:
        raise ImageError("version %d, this reads %d" % (version, VERSION))
    if fmt not in FORMATS or width == 0 or height == 0 or stride < width or offset < HEADER.size \
            or stride % STRIDE_STEPS[fmt]:
        raise ImageError("a header field holds a value the entry does not allow")
    end = offset + (FORMATS[fmt] * (stride * (height - 1) + width) + 7) // 8
    alpha_at = offset + 2 * stride * height  # the A8 plane follows every colour row whole
    if fmt == FORMAT_RGB565A8:
        end = alpha_at + stride * (height - 1) + width
    if (fmt != FORMAT_MONO1 and offset % PIXEL_ALIGN) or end > len(entry):
        raise ImageError("the rows leave the entry or are misaligned")
    if fmt == FORMAT_MONO1:
        return width, height, [[bool(entry[offset + y * stride // 8 + x // 8] & (0x80 >> (x % 8)))
                                for x in range(width)] for y in range(height)], None
    pixels = []
    for y in range(height):
        pixels += struct.unpack_from("<%dH" % width, entry, offset + 2 * stride * y)
    if fmt != FORMAT_RGB565A8:
        return width, height, pixels, None
    alpha = []
    for y in range(height):
        alpha += entry[alpha_at + stride * y:alpha_at + stride * y + width]
    return width, height, pixels, alpha


def flat_centre(width, height, pixels, alpha, border):
    """(colour, alpha) when every pixel inside `border` is one colour at one
    alpha (any colour where the alpha is 0), else None."""
    left, top, right, bottom = border
    inside = [(pixels[y * width + x], OPAQUE if alpha is None else alpha[y * width + x])
              for y in range(top, height - bottom) for x in range(left, width - right)]
    covers = {a for _, a in inside}
    colours = {c for c, a in inside if a}
    if len(covers) != 1 or len(colours) > 1:
        return None
    return (colours.pop() if colours else 0), covers.pop()


def encode_sprite(border, image):
    """The SPRT entry's bytes: `border` (left, top, right, bottom) ahead of
    `image`, an RGB565 or RGB565A8 entry encode() wrote."""
    width, height, pixels, alpha = decode_alpha(image)
    if HEADER.unpack_from(image)[1] == FORMAT_MONO1:
        raise ImageError("a sprite is not a one-bit picture")
    left, top, right, bottom = border
    if any(not 0 <= side <= BORDER_MAX for side in border) or left + right >= width or top + bottom >= height:
        raise ImageError("border %r leaves a %d x %d picture no centre" % (list(border), width, height))
    centre = flat_centre(width, height, pixels, alpha, border)
    colour, cover = centre or (0, 0)
    return SPRITE_HEADER.pack(SPRITE_VERSION, *border, centre is not None, cover, colour, 0) + image


def decode_sprite(entry):
    """(border, (colour, alpha) of a flat centre or None, (width, height,
    pixels, alpha)) of a SPRT entry, after the checks draw_sprite_open() makes."""
    if len(entry) < SPRITE_HEADER.size:
        raise ImageError("shorter than a sprite header")
    version, left, top, right, bottom, flat, cover, colour, reserved = SPRITE_HEADER.unpack_from(entry)
    if version != SPRITE_VERSION:
        raise ImageError("sprite version %d, this reads %d" % (version, SPRITE_VERSION))
    if flat > 1 or reserved:
        raise ImageError("a sprite header field holds a value the entry does not allow")
    body = entry[SPRITE_HEADER.size:]
    image = decode_alpha(body)
    if HEADER.unpack_from(body)[1] == FORMAT_MONO1:
        raise ImageError("a sprite is not a one-bit picture")
    if left + right >= image[0] or top + bottom >= image[1]:
        raise ImageError("the border leaves the picture no centre")
    return (left, top, right, bottom), ((colour, cover) if flat else None), image


def turn(width, height, pixels, quarter_turns):
    """(width, height, pixels) turned clockwise by `quarter_turns`."""
    for _ in range(quarter_turns % 4):
        pixels = [pixels[(height - 1 - x) * width + y] for y in range(width) for x in range(height)]
        width, height = height, width
    return width, height, pixels


def turn_border(border, quarter_turns):
    """(left, top, right, bottom) turned clockwise by `quarter_turns`: the
    left edge becomes the top."""
    for _ in range(quarter_turns % 4):
        left, top, right, bottom = border
        border = (bottom, left, top, right)
    return tuple(border)


def image_id(path):
    """The pack id of a .image.toml: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)


def read_toml(path):
    """The values of a root's TOML."""
    try:
        return tomllib.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ImageError(f"{path}: {error}") from error


def load_source(path, allowed_keys=("source", "quarter_turns")):
    """(the PNG's path, quarter turns) a root with `allowed_keys` names."""
    path = pathlib.Path(path)
    values = read_toml(path)
    source, turns = values.get("source"), values.get("quarter_turns", 0)
    if set(values) - set(allowed_keys) or not isinstance(source, str) \
            or type(turns) is not int or not 0 <= turns <= 3:
        expected = 'source = "x.png"'
        if "quarter_turns" in allowed_keys:
            expected += " and optionally quarter_turns = 0 to 3"
        raise ImageError(f"{path}: holds {expected}; allowed keys: {', '.join(allowed_keys)}")
    # Beside it, so whatever finds the .image.toml finds its source too.
    if pathlib.PurePath(source).name != source or ":" in source or not source.lower().endswith(".png"):
        raise ImageError("%s: source %r is not a .png in the same folder" % (path, source))
    return path.parent / source, turns


def load_border(path):
    """The (left, top, right, bottom) border a .image.toml names, or None."""
    border = read_toml(path).get("border")
    if border is None:
        return None
    if not isinstance(border, list) or len(border) != 4 \
            or any(type(side) is not int or not 0 <= side <= BORDER_MAX for side in border):
        raise ImageError(f"{path}: border is [left, top, right, bottom], each 0 to {BORDER_MAX} pixels")
    return tuple(border)


def bake(path):
    """(type, bytes) of the entry a .image.toml makes: an IMAG, or a SPRT
    when it names a border. The border is in the source's own frame and
    turns with it."""
    source, turns = load_source(path, ("source", "quarter_turns", "border"))
    border = load_border(path)
    try:
        image = read_png(source.read_bytes())
    except (OSError, ValueError) as error:
        raise ImageError("%s: source %s: %s" % (path, source, error)) from error
    order = [(x, y) for y in range(image.height) for x in range(image.width)]
    pixels = [gfx_color.swap(gfx_color.rgb565(*image.pixel(x, y))) for x, y in order]
    width, height, pixels = turn(image.width, image.height, pixels, turns)
    alpha = None
    if not image.opaque:
        alpha = turn(image.width, image.height, [image.coverage(x, y) for x, y in order], turns)[2]
    entry = encode(width, height, pixels, alpha)
    if border is None:
        return TYPE, entry
    try:
        return SPRITE_TYPE, encode_sprite(turn_border(border, turns), entry)
    except ImageError as error:
        raise ImageError(f"{path}: {error}") from error

"""LTEX v1 material textures: a source texture's albedo pattern around its mean, quantized to 16 colours.
The layout constants belong to render/texture/r3d_texture.h."""
import dataclasses
import pathlib

from build.c_header import read_define
from r3d.mesh_asset import checked_struct

OWNER = pathlib.Path(__file__).resolve().parents[3] / "main/render/texture/r3d_texture.h"
TYPE = b"LTEX"
UVS_TYPE = b"LUVS"  # a mesh's texturing, written by uv_transfer.py
VERSION = read_define(OWNER, "R3D_TEXTURE_VERSION")
INDEX4 = read_define(OWNER, "R3D_TEXTURE_INDEX4")
COLOURS = read_define(OWNER, "R3D_TEXTURE_COLOURS")
PALETTE_STRIDE = read_define(OWNER, "R3D_TEXTURE_PALETTE_STRIDE")
ONE = read_define(OWNER, "R3D_TEXTURE_ONE")
SIZE_LOG2_MIN = read_define(OWNER, "R3D_TEXTURE_SIZE_LOG2_MIN")
SIZE_LOG2_MAX = read_define(OWNER, "R3D_TEXTURE_SIZE_LOG2_MAX")
TINT_RGB = read_define(OWNER, "R3D_TEXTURE_TINT_RGB")
TINT_PADDING = read_define(OWNER, "R3D_TEXTURE_TINT_BYTES") - TINT_RGB
HEADER_FIELDS = (("VERSION", "H"), ("FORMAT", "H"), ("SIZE_LOG2", "B"), ("LEVELS", "B"),
                 ("TINT", f"{TINT_RGB}s{TINT_PADDING}x"), ("PALETTE_OFF", "I"), ("TEXELS_OFF", "I"))
HEADER = checked_struct(OWNER, "R3D_TEXTURE", HEADER_FIELDS)
BYTE_MAX = 255
NIBBLE_BITS = 4
# The source's transfer, as obj.Texture linearises it: a pure power, so a display-space product is the
# display value of the linear product and the pattern multiplies a vertex colour exactly.
GAMMA = 2.2


@dataclasses.dataclass
class Indexed:
    tint: tuple            # r, g, b the colormap lights the pattern with, 0..255 display
    palette: list          # COLOURS (r, g, b), ONE = the pattern's display mean
    levels: list           # each level's rows of palette indices, level 0 first, each half the last


def size_log2_of(size):
    log2 = size.bit_length() - 1
    if size != 1 << log2 or not SIZE_LOG2_MIN <= log2 <= SIZE_LOG2_MAX:
        raise ValueError(f"texture size {size} is not a power of two from {1 << SIZE_LOG2_MIN} to {1 << SIZE_LOG2_MAX}")
    return log2


def material_texture(linear_rgb, size, tint=None):
    """`linear_rgb` (h, w, 3) linear albedo, as obj.Texture's level 0, as an Indexed `size` texels a side: each
    level box-filtered from the source in linear light, then in display space divided by the source's mean there
    (the span multiplies display values, so the pattern averages 1.0 on screen) and quantized with Floyd-Steinberg
    to level 0's palette. `tint` (r, g, b, 0..255) is the hue the colormap lights it with, the display mean of the
    texture when None."""
    import numpy as np
    from PIL import Image

    log2 = size_log2_of(size)
    rgb = np.asarray(linear_rgb, dtype=np.float64)[..., :TINT_RGB]
    mean = np.maximum((rgb ** (1.0 / GAMMA)).reshape(-1, TINT_RGB).mean(axis=0), np.finfo(np.float64).tiny)

    def level_image(side):
        channels = [Image.fromarray(rgb[..., k].astype(np.float32), mode="F").resize((side, side), Image.BOX)
                    for k in range(TINT_RGB)]
        linear = np.stack([np.asarray(channel, dtype=np.float64) for channel in channels], axis=2)
        pattern = np.maximum(linear, 0.0) ** (1.0 / GAMMA) / mean * ONE
        return Image.fromarray(np.clip(np.rint(pattern), 0, BYTE_MAX).astype(np.uint8), mode="RGB")

    first = level_image(size).quantize(COLOURS, dither=Image.Dither.FLOYDSTEINBERG)
    palette = first.getpalette()[:COLOURS * TINT_RGB]
    palette += [0] * (COLOURS * TINT_RGB - len(palette))
    levels = [np.asarray(first)]
    for level in range(1, log2 - SIZE_LOG2_MIN + 1):
        image = level_image(size >> level).quantize(palette=first, dither=Image.Dither.FLOYDSTEINBERG)
        levels.append(np.asarray(image))
    if tint is None:
        tint = np.minimum(mean, 1.0) * BYTE_MAX
    return Indexed(tuple(int(round(float(c))) for c in tint),
                   [tuple(palette[i * TINT_RGB:(i + 1) * TINT_RGB]) for i in range(COLOURS)],
                   [level.astype(int).tolist() for level in levels])


def pack_ltex(indexed):
    """The LTEX entry: header, palette, then each level's rows, two texels a byte, the low nibble the even
    column."""
    side = len(indexed.levels[0])
    palette = b"".join(bytes(colour) + bytes(PALETTE_STRIDE - TINT_RGB) for colour in indexed.palette)
    texels = bytearray()
    for rows in indexed.levels:
        for row in rows:
            texels.extend(row[x] | (row[x + 1] << NIBBLE_BITS) for x in range(0, len(row), 2))
    header = HEADER.pack(VERSION, INDEX4, size_log2_of(side), len(indexed.levels), bytes(indexed.tint),
                         HEADER.size, HEADER.size + len(palette))
    return header + palette + bytes(texels)


def decode(entry):
    """The Indexed an LTEX entry holds."""
    version, fmt, log2, count, tint, palette_off, texels_off = HEADER.unpack_from(entry)
    if version != VERSION or fmt != INDEX4 or count < 1 or log2 - (count - 1) < SIZE_LOG2_MIN:
        raise ValueError("LTEX version, format or levels are invalid")
    palette = [tuple(entry[palette_off + i * PALETTE_STRIDE:palette_off + i * PALETTE_STRIDE + TINT_RGB])
               for i in range(COLOURS)]
    levels, at = [], texels_off
    for level in range(count):
        side = 1 << (log2 - level)
        rows = []
        for _ in range(side):
            row = []
            for byte in entry[at:at + side // 2]:
                row += [byte & ((1 << NIBBLE_BITS) - 1), byte >> NIBBLE_BITS]
            rows.append(row)
            at += side // 2
        levels.append(rows)
    if at != len(entry):
        raise ValueError("LTEX levels do not match the entry size")
    return Indexed(tuple(tint), palette, levels)


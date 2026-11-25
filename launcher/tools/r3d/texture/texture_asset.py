"""LTEX v1 material textures: a source texture's albedo pattern around its mean, quantized to 16 colours.
The layout constants belong to render/texture/r3d_texture.h."""
import dataclasses
import pathlib

from build.c_header import read_define, read_defines
from r3d.mesh_asset import checked_struct

OWNER = pathlib.Path(__file__).resolve().parents[3] / "main/render/texture/r3d_texture.h"
TYPE = b"LTEX"
UVS_TYPE = b"LUVS"  # a mesh's texturing, written by uv_transfer.py
VERSION, INDEX4, MASK1, MASK1_PER_BYTE, MASK1_SIZE_LOG2_MIN, MASK1_MIP_KEPT, MASK1_COLOURS, INDEX4_PER_BYTE, \
    COLOURS, PALETTE_STRIDE, ONE, SIZE_LOG2_MIN, SIZE_LOG2_MAX, TINT_RGB = read_defines(OWNER, "R3D_TEXTURE_",
        ("VERSION", "INDEX4", "MASK1", "MASK1_PER_BYTE", "MASK1_SIZE_LOG2_MIN", "MASK1_MIP_KEPT", "MASK1_COLOURS",
         "INDEX4_PER_BYTE", "COLOURS", "PALETTE_STRIDE", "ONE", "SIZE_LOG2_MIN", "SIZE_LOG2_MAX", "TINT_RGB"))
TINT_PADDING = read_define(OWNER, "R3D_TEXTURE_TINT_BYTES") - TINT_RGB
HEADER_FIELDS = (("VERSION", "H"), ("FORMAT", "H"), ("SIZE_LOG2", "B"), ("LEVELS", "B"),
                 ("TINT", f"{TINT_RGB}s{TINT_PADDING}x"), ("PALETTE_OFF", "I"), ("TEXELS_OFF", "I"))
HEADER = checked_struct(OWNER, "R3D_TEXTURE", HEADER_FIELDS)
BYTE_MAX = 255
INDEX4_BITS, INDEX4_MASK = read_defines(OWNER, "R3D_TEXTURE_", ("INDEX4_BITS", "INDEX4_MASK"))
# The source's transfer, as obj.Texture linearises it: a pure power, so a display-space product is the
# display value of the linear product and the pattern multiplies a vertex colour exactly.
GAMMA = 2.2


@dataclasses.dataclass
class Indexed:
    tint: tuple            # r, g, b the colormap lights the pattern with, 0..255 display
    palette: list          # COLOURS (r, g, b), ONE = the pattern's display mean
    levels: list           # each level's rows of palette indices, level 0 first, each half the last
    format: int = INDEX4


def size_log2_of(size, minimum=SIZE_LOG2_MIN):
    log2 = size.bit_length() - 1
    if size != 1 << log2 or not minimum <= log2 <= SIZE_LOG2_MAX:
        raise ValueError(f"texture size {size} is not a power of two from {1 << minimum} to {1 << SIZE_LOG2_MAX}")
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


def mask_texture(pages_bits, levels):
    """A MASK1 page and its coverage mips; palette colour 1 preserves vertex light."""
    import numpy as np

    bits = np.asarray(pages_bits, dtype=bool)
    log2 = size_log2_of(len(bits), MASK1_SIZE_LOG2_MIN)
    if bits.shape != (len(bits), len(bits)) or not 1 <= levels <= log2 - MASK1_SIZE_LOG2_MIN + 1:
        raise ValueError("MASK1 page shape or mip count is invalid")
    chain = [bits.astype(int).tolist()]
    for _ in range(1, levels):
        side = len(bits) // 2
        bits = bits.reshape(side, 2, side, 2).sum(axis=(1, 3)) >= MASK1_MIP_KEPT
        chain.append(bits.astype(int).tolist())
    palette = [(0,) * TINT_RGB, (ONE,) * TINT_RGB]
    return Indexed((BYTE_MAX,) * TINT_RGB, palette, chain, MASK1)


def pack_ltex(indexed):
    """The LTEX entry: header, palette, then levels, low bits holding the first texel."""
    import numpy as np

    if not indexed.levels:
        raise ValueError("LTEX needs at least one mip level")
    side = len(indexed.levels[0])
    masked = indexed.format == MASK1
    if indexed.format not in (INDEX4, MASK1):
        raise ValueError("LTEX format is invalid")
    minimum = MASK1_SIZE_LOG2_MIN if masked else SIZE_LOG2_MIN
    log2 = size_log2_of(side, minimum)
    count = MASK1_COLOURS if masked else COLOURS
    if len(indexed.palette) != count or not 1 <= len(indexed.levels) <= log2 - minimum + 1:
        raise ValueError("LTEX palette or mip count is invalid")
    palette = b"".join(bytes(colour) + bytes(PALETTE_STRIDE - TINT_RGB) for colour in indexed.palette)
    texels = bytearray()
    for level, rows in enumerate(indexed.levels):
        array = np.asarray(rows)
        expected = side >> level
        if array.shape != (expected, expected) or np.any((array < 0) | (array >= count)):
            raise ValueError("LTEX level shape or indices are invalid")
        if masked:
            texels.extend(np.packbits(array.astype(np.uint8).ravel(), bitorder="little").tobytes())
        else:
            for row in rows:
                texels.extend(row[x] | (row[x + 1] << INDEX4_BITS) for x in range(0, len(row), INDEX4_PER_BYTE))
    header = HEADER.pack(VERSION, indexed.format, log2, len(indexed.levels), bytes(indexed.tint),
                         HEADER.size, HEADER.size + len(palette))
    return header + palette + bytes(texels)


def decode(entry):
    """The Indexed an LTEX entry holds."""
    import numpy as np

    version, fmt, log2, count, tint, palette_off, texels_off = HEADER.unpack_from(entry)
    masked = fmt == MASK1
    minimum = MASK1_SIZE_LOG2_MIN if masked else SIZE_LOG2_MIN
    if (version != VERSION or fmt not in (INDEX4, MASK1) or count < 1 or
            not minimum <= log2 <= SIZE_LOG2_MAX or log2 - (count - 1) < minimum):
        raise ValueError("LTEX version, format or levels are invalid")
    colours = MASK1_COLOURS if masked else COLOURS
    if palette_off != HEADER.size or texels_off != palette_off + colours * PALETTE_STRIDE:
        raise ValueError("LTEX palette offsets are invalid")
    palette = [tuple(entry[palette_off + i * PALETTE_STRIDE:palette_off + i * PALETTE_STRIDE + TINT_RGB])
               for i in range(colours)]
    levels, at = [], texels_off
    for level in range(count):
        side = 1 << (log2 - level)
        per_byte = MASK1_PER_BYTE if masked else INDEX4_PER_BYTE
        size = side * side // per_byte
        packed = np.frombuffer(entry[at:at + size], dtype=np.uint8)
        if len(packed) != size:
            raise ValueError("LTEX levels do not match the entry size")
        if masked:
            indices = np.unpackbits(packed, bitorder="little")
        else:
            indices = np.column_stack([packed & INDEX4_MASK, packed >> INDEX4_BITS]).ravel()
        levels.append(indices.reshape(side, side).astype(int).tolist())
        at += size
    if at != len(entry):
        raise ValueError("LTEX levels do not match the entry size")
    return Indexed(tuple(tint), palette, levels, fmt)

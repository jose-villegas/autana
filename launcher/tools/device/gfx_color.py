"""gfx_color: the firmware's RGB565 packing (launcher/packages/image/include/image/pixel/rgb565.h) for host tools.

Channel arithmetic works on ints and NumPy integer arrays at least 16 bits
wide. Byte decoding uses only the standard library.
"""

from array import array
from functools import cache
import sys

# The 8-bit levels one step of each RGB565 channel spans: red, green, blue,
# the bits rgb565() drops from each.
STEP = (1 << 3, 1 << 2, 1 << 3)


def rgb565(r, g, b):
    """RGB565 from 8-bit channels: truncating, not rounding."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def swap(value):
    """Panel order to native RGB565 and back: the same byte swap both ways."""
    return ((value >> 8) | (value << 8)) & 0xFFFF


def expand(r5, g6, b5):
    """8-bit channels for 5/6/5-bit ones by bit replication, as
    rgb565be_to_rgb888() does, so rgb565() of the result gives them back."""
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


RGB565_GREEN_BITS = 6
RGB565_BLUE_BITS = 5
RGB565_GREEN_MASK = (1 << RGB565_GREEN_BITS) - 1
RGB565_BLUE_MASK = (1 << RGB565_BLUE_BITS) - 1
RGB565_WORDS = 1 << 16


def rgb888_of_rgb565(value):
    """RGB channels of a native RGB565 word or an array of words."""
    return expand(value >> (RGB565_GREEN_BITS + RGB565_BLUE_BITS),
                  (value >> RGB565_BLUE_BITS) & RGB565_GREEN_MASK, value & RGB565_BLUE_MASK)


@cache
def _rgb565_expansions():
    return tuple(bytes(rgb888_of_rgb565(value)) for value in range(RGB565_WORDS))


def rgb888_of_rgb565be(data: bytes) -> bytes:
    """Expand big-endian RGB565 bytes to packed RGB888 bytes."""
    codes = array("H", data)
    if sys.byteorder == "little":
        codes.byteswap()
    return b"".join(map(_rgb565_expansions().__getitem__, codes))


def rgb888_array_of_rgb565(value):
    """Expand a native RGB565 array to uint8 RGB with channels on the last axis."""
    import numpy as np

    return np.stack(rgb888_of_rgb565(value), axis=-1).astype(np.uint8)

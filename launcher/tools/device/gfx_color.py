"""gfx_color: the firmware's RGB565 packing (launcher/main/gfx/gfx_color.h) for host tools.

Every function works on plain ints and on numpy integer arrays at least 16
bits wide.
"""


def rgb565(r, g, b):
    """GFX_RGB565 from 8-bit channels: truncating, not rounding."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def swap(value):
    """Panel order to native RGB565 and back: the same byte swap both ways."""
    return ((value >> 8) | (value << 8)) & 0xFFFF


def expand(r5, g6, b5):
    """8-bit channels for 5/6/5-bit ones by bit replication, as
    gfx_color_rgb888() does, so rgb565() of the result gives them back."""
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)

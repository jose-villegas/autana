"""colour_maths: the colour conversions the render tools and the r3d fit share.

`lab` is the colour space every dE is measured in; `expand_565` is the 24-bit colour a 16-bit
framebuffer shows. Kept apart from render_compare.py so the fit and reference bakes depend on
these alone, not on that tool's CLI and sheet code.
"""
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
import gfx_color  # noqa: E402  (path must be set up first)

# The colour space every ΔE here is measured in: gamma-2.2 RGB, linear sRGB
# primaries to XYZ, and the D65 white CIELAB is relative to.
GAMMA = 2.2
SRGB_TO_XYZ = ((0.4124564, 0.3575761, 0.1804375), (0.2126729, 0.7151522, 0.0721750), (0.0193339, 0.1191920, 0.9503041))
D65_WHITE = (0.95047, 1.0, 1.08883)


def expand_565(rgb):
    """The 24-bit colour a 16-bit framebuffer shows for rgb: a tuple of three
    values, or an integer array with the channels last."""
    if isinstance(rgb, tuple):
        return tuple(int(value) for value in expand_565(np.array(rgb)))
    return np.stack(gfx_color.expand(rgb[..., 0] >> 3, rgb[..., 1] >> 2, rgb[..., 2] >> 3), axis=-1)


def lab(xp, rgb, floor=0.0):
    """CIELAB (D65) of gamma-encoded 0..1 colours, channels last, with `xp`
    NumPy or PyTorch. `floor` clamps the colours first, which keeps a
    gradient finite at black."""
    if floor:
        rgb = xp.clip(rgb, floor, 1.0)
    as_array = np.asarray if xp is np else (lambda values: xp.as_tensor(values, dtype=rgb.dtype, device=rgb.device))
    scaled = rgb**GAMMA @ as_array(SRGB_TO_XYZ).T / as_array(D65_WHITE)
    delta = 6 / 29
    cube_root = np.cbrt(scaled) if xp is np else xp.clip(scaled, delta**3, None) ** (1.0 / 3.0)
    f = xp.where(scaled > delta**3, cube_root, scaled / (3 * delta**2) + 4 / 29)
    return xp.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)

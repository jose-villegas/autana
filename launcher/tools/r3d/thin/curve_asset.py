"""LCRV radius-bearing polylines beside an object's LMSH, in its position ticks."""

import numpy as np

from asset.asset_pack import NAME_BYTES, PackError
from r3d.thin.curve_fit import Curve
from r3d.thin.curve_format import CURVE, HEADER, KNOT, NAME_SUFFIX, TYPE
U16_MAX = (1 << 16) - 1
I16_MIN, I16_MAX = -(1 << 15), (1 << 15) - 1
SRGB_MAX = (1 << 8) - 1


def name_for_mesh(name):
    result = name + NAME_SUFFIX
    if len(result.encode("ascii")) >= NAME_BYTES:
        raise PackError(f"mesh id {name!r} leaves no room for its curves")
    return result


def write_curves(curves, position_scale, tonemap_white=0.0):
    """Quantise geometry and tone-map linear knot radiance to panel RGB565."""
    from r3d.light import to_srgb8
    from r3d.lit_mesh import panel_colour

    count = sum(len(curve.knots) for curve in curves)
    if position_scale <= 0 or len(curves) > U16_MAX or count > U16_MAX:
        raise PackError("LCRV scale or count is out of range")
    records, knots, first = [], [], 0
    for curve in curves:
        if (np.shape(curve.knots) != (len(curve.knots), 3) or len(curve.knots) < 2
                or np.shape(curve.radius) != (len(curve.knots),)
                or np.shape(curve.color) != np.shape(curve.knots) or np.any(curve.color < 0)):
            raise PackError("LCRV curve arrays are invalid")
        p = np.rint(curve.knots * position_scale)
        r = np.rint(curve.radius * position_scale)
        lo = np.floor(np.min(curve.knots - curve.radius[:, None], axis=0) * position_scale)
        hi = np.ceil(np.max(curve.knots + curve.radius[:, None], axis=0) * position_scale)
        if (len(p) < 2 or not np.all(np.isfinite(np.concatenate([p.ravel(), r, curve.color.ravel()])))
                or np.any(p < I16_MIN) or np.any(p > I16_MAX) or np.any(lo < I16_MIN) or np.any(hi > I16_MAX)
                or np.any(r < 0) or np.any(r > U16_MAX)):
            raise PackError("LCRV curve is outside its encoding")
        # Quantised radii must also fit the quantised bound.
        lo = np.minimum(lo, np.min(p - r[:, None], axis=0))
        hi = np.maximum(hi, np.max(p + r[:, None], axis=0))
        if np.any(lo < I16_MIN) or np.any(hi > I16_MAX):
            raise PackError("LCRV radius exceeds its box")
        records.append(CURVE.pack(first, len(p), *lo.astype(int), *hi.astype(int)))
        colors = panel_colour(to_srgb8(curve.color, tonemap_white))
        knots.extend(KNOT.pack(*point.astype(int), int(radius), int(color)) for point, radius, color in zip(p, r, colors))
        first += len(p)
    return HEADER.pack(len(curves), count, 0, 0) + b"".join(records + knots)


def read_curves(blob, position_scale):
    """Validate an LCRV and decode its displayed sRGB colours back to linear values."""
    from r3d.lit_mesh import rgb888
    from r3d.light import LIGHT_GAMMA

    if position_scale <= 0 or len(blob) < HEADER.size:
        raise PackError("LCRV header or scale is invalid")
    count, knots, reserved0, reserved1 = HEADER.unpack_from(blob)
    if reserved0 or reserved1 or len(blob) != HEADER.size + count * CURVE.size + knots * KNOT.size:
        raise PackError("LCRV sections are invalid")
    start = HEADER.size + count * CURVE.size
    result, end = [], 0
    for index in range(count):
        first, size, *box = CURVE.unpack_from(blob, HEADER.size + index * CURVE.size)
        if first != end or size < 2 or first + size > knots:
            raise PackError("LCRV knot range is invalid")
        rows = np.asarray([KNOT.unpack_from(blob, start + k * KNOT.size) for k in range(first, first + size)])
        p, r = rows[:, :3], rows[:, 3]
        if np.any(p - r[:, None] < box[:3]) or np.any(p + r[:, None] > box[3:]):
            raise PackError("LCRV box does not contain its knots")
        srgb = rgb888(rows[:, 4]) / SRGB_MAX
        color = srgb ** LIGHT_GAMMA
        result.append(Curve(p / position_scale, r / position_scale, color))
        end = first + size
    if end != knots:
        raise PackError("LCRV has unowned knots")
    return result

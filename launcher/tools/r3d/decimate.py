"""Decimation to a triangle budget."""

import math

import fast_simplification
import numpy as np

from . import log
from .geometry import compact, snap_to_grid


def decimate(p, tris, target, keep_below=1000):
    """Quadric decimation, then, for a material made of many small
    disconnected pieces the quadric pass cannot merge, vertex clustering on
    the coarsest grid that still meets the target."""
    if len(tris) <= keep_below or target >= len(tris):
        return p, tris
    p, tris = fast_simplification.simplify(p, tris.astype(np.int32), target_count=target, preserve_border=True)
    p, tris = compact(p, np.asarray(tris, dtype=np.int64))
    if len(tris) <= target * 3:
        return p, tris
    log(f"    quadric pass stopped at {len(tris)}, clustering to {target}")
    lo, hi = 0.1, float(np.ptp(p, axis=0).max())
    best = (p, tris)
    for _ in range(24):
        cell = math.sqrt(lo * hi)
        sp, st = snap_to_grid(p, tris, cell)
        if len(st) > target:
            lo = cell
        else:
            hi = cell
            best = (sp, st)
    return best

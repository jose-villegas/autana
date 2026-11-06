"""Detect connected thin surfaces from the source's start budget, without names.

This is the sole thin-part rule. Attribute protection must use it only when
geometry.curves is off; protection and curve extraction never act together.
"""

from dataclasses import dataclass

import numpy as np

THIN_WIDTH_PER_SIDE = 0.25
THIN_LENGTH_PER_SIDE = 1.0
THIN_SLENDERNESS_MIN = 12.0
WELD_CELLS_PER_SIDE = 1000


@dataclass(frozen=True)
class ThinPart:
    triangles: np.ndarray
    centre: np.ndarray
    axes: np.ndarray
    extents: np.ndarray


def budget_side(positions, triangles, budget):
    """Area-equivalent triangle side at the source's start budget."""
    if budget <= 0:
        raise ValueError("triangle budget must be positive")
    a, b, c = np.asarray(positions)[np.asarray(triangles)].transpose(1, 0, 2)
    return float(np.sqrt(np.linalg.norm(np.cross(b - a, c - a), axis=1).sum() / (2 * budget)))


def find_thin_parts(positions, triangles, side):
    """Position-welded connected parts whose PCA shape reads as a line."""
    if not np.isfinite(side) or side <= 0:
        raise ValueError("budget side must be finite and positive")
    positions, triangles = np.asarray(positions), np.asarray(triangles)
    from r3d.simplify import object_groups

    if not len(triangles):
        return []
    grid = np.rint(positions / (side / WELD_CELLS_PER_SIDE))
    groups = object_groups(grid, triangles, np.arange(len(triangles)))
    result = []
    for group in np.unique(groups):
        rows = np.flatnonzero(groups == group)
        points = np.unique(positions[triangles[rows]].reshape(-1, 3), axis=0)
        centre = points.mean(axis=0)
        _, axes = np.linalg.eigh((points - centre).T @ (points - centre))
        axes = axes[:, ::-1]
        extents = np.ptp((points - centre) @ axes, axis=0)
        if (extents[1] <= side * THIN_WIDTH_PER_SIDE and extents[0] >= side * THIN_LENGTH_PER_SIDE
                and extents[0] >= THIN_SLENDERNESS_MIN * extents[1]):
            result.append(ThinPart(np.asarray(rows), centre, axes, extents))
    return result

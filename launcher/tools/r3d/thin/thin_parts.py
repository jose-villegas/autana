"""Detect connected thin surfaces from the source's start budget, without names.

This is the sole thin-part rule. Attribute protection must use it only when
geometry.curves is off; protection and curve extraction never act together.
"""

from dataclasses import dataclass

import numpy as np

THIN_WIDTH_PER_SIDE = 0.25
THIN_LENGTH_PER_SIDE = 0.75
THIN_SLENDERNESS_MIN = 11.0
WELD_CELLS_PER_SIDE = 1000
# Width is measured across the part's own centreline, not its straight box: a cable sagging between two walls is as
# thin as a taut one. The centreline runs through stations THIN_WIDTH_PER_SIDE long down the long axis, from
# WIDTH_SAMPLES_PER_STATION area samples each.
WIDTH_SAMPLES_PER_STATION = 32
WIDTH_SAMPLE_SEED = 0
# A part rests on a surface, and stays in the mesh, when at least RESTING_SHARE of its vertices lie within
# RESTING_REACH_PER_WIDTH of its own width from other geometry: trims and frames flush with a wall, which the
# wall's fit would cover. Poles and cables meet other geometry only at their ends. Other geometry is every
# triangle but the part's own, thin ones too: a hem along a trim rests on it though both read as thin.
RESTING_REACH_PER_WIDTH = 0.5
RESTING_SHARE = 0.5


@dataclass(frozen=True)
class ThinPart:
    triangles: np.ndarray
    centre: np.ndarray
    axes: np.ndarray
    extents: np.ndarray
    width: float


def budget_side(positions, triangles, budget):
    """Area-equivalent triangle side at the source's start budget."""
    if budget <= 0:
        raise ValueError("triangle budget must be positive")
    a, b, c = np.asarray(positions)[np.asarray(triangles)].transpose(1, 0, 2)
    return float(np.sqrt(np.linalg.norm(np.cross(b - a, c - a), axis=1).sum() / (2 * budget)))


def centreline_width(positions, triangles, rows, centre, axes, station):
    """The widest of the part's cross sections across its centreline: the line through the mean surface sample of
    each station `station` long down `axes[0]`, carried straight on past the end ones; a cross section's width
    is the spread of a station's samples off that line, across their widest direction."""
    from types import SimpleNamespace

    from r3d.surface_samples import area_samples

    length = np.ptp((positions[np.unique(triangles[rows])] - centre) @ axes[:, 0])
    stations = max(1, int(np.ceil(length / station)))
    source = SimpleNamespace(p=positions, tri_v=triangles)
    samples, _ = area_samples(source, rows, stations * WIDTH_SAMPLES_PER_STATION,
                              np.random.default_rng(WIDTH_SAMPLE_SEED))
    local = (samples - centre) @ axes
    index = np.minimum(((local[:, 0] - local[:, 0].min()) / station).astype(int), stations - 1)
    used = np.unique(index)
    mids = np.array([local[index == k].mean(axis=0) for k in used])
    along = local[:, 0]
    line = np.empty((len(local), 2))
    for k in (1, 2):
        line[:, k - 1] = np.interp(along, mids[:, 0], mids[:, k])
        for end, inner, past in ((0, 1, along < mids[0, 0]), (-1, -2, along > mids[-1, 0])):
            slope = (mids[end, k] - mids[inner, k]) / (mids[end, 0] - mids[inner, 0])
            line[past, k - 1] = mids[end, k] + slope * (along[past] - mids[end, 0])
    across = local[:, 1:] - line
    width = 0.0
    for k in used:
        spread = across[index == k] - across[index == k].mean(axis=0)
        _, directions = np.linalg.eigh(spread.T @ spread)
        width = max(width, float(np.ptp(spread @ directions[:, -1])))
    return width


def find_thin_parts(positions, triangles, side):
    """Position-welded connected parts that read as a line: long, and narrow across their own centreline."""
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
        limit = side * THIN_WIDTH_PER_SIDE
        if extents[0] < side * THIN_LENGTH_PER_SIDE:
            continue
        width = extents[1]
        if width > limit:
            corners = positions[triangles[rows]]
            area = np.linalg.norm(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1).sum()
            # A tube `limit` across has pi * limit of surface per unit of its length: with more, no centreline is
            # that thin, and the samples are not drawn.
            if area / 2 <= np.pi * limit * extents[0]:
                width = centreline_width(positions, triangles, rows, centre, axes, limit)
        if width <= limit and extents[0] >= THIN_SLENDERNESS_MIN * width:
            result.append(ThinPart(np.asarray(rows), centre, axes, extents, width))
    if not result:
        return []
    from r3d.geometry import TriangleProximity

    proximity = TriangleProximity(positions[triangles])
    return [part for part in result if not resting(part, positions, triangles, proximity)]


def resting(part, positions, triangles, proximity):
    """Whether `part` lies along the other triangles, by RESTING_SHARE and its reach; `proximity` holds them all."""
    reach = RESTING_REACH_PER_WIDTH * part.width
    own = np.zeros(len(triangles), dtype=bool)
    own[part.triangles] = True
    points = positions[np.unique(triangles[part.triangles])]
    near = [not own[proximity.within(point, reach)].all() for point in points]
    return bool(np.mean(near) >= RESTING_SHARE)

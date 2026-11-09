"""Detect connected thin surfaces from the source's start budget, without names.

This is the sole thin-part rule. Attribute protection must use it only when
geometry.curves is off; protection and curve extraction never act together.
"""

from dataclasses import dataclass, replace

import numpy as np

THIN_WIDTH_PER_SIDE = 0.25
THIN_LENGTH_PER_SIDE = 0.75
THIN_SLENDERNESS_MIN = 11.0
WELD_CELLS_PER_SIDE = 1000
# Width is measured across the part's own centreline, not its straight box: a cable sagging between two walls is as
# thin as a taut one. The centreline runs through stations one width limit long down the long axis, from
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
    from r3d.geometry import triangle_areas

    return float(np.sqrt(triangle_areas(np.asarray(positions), np.asarray(triangles)).sum() / budget))


def thin_parts_at(positions, triangles, budget, owned=None):
    """find_thin_parts at the side of the start `budget`, on the positions rounded to float32 first: the bake and the
    fit load the source at different precisions and must pick the same parts. Triangles in the `owned` mask belong
    to another step (material thinning) and are never searched; the side still counts every triangle."""
    positions = np.asarray(positions, dtype=np.float32).astype(np.float64)
    triangles = np.asarray(triangles)
    side = budget_side(positions, triangles, budget)
    if owned is None or not np.any(owned):
        return find_thin_parts(positions, triangles, side)
    rest = np.flatnonzero(~np.asarray(owned))
    return [replace(part, triangles=rest[part.triangles])
            for part in find_thin_parts(positions, triangles[rest], side)]


def thin_triangles(positions, triangles, budget, owned=None):
    """Which of `triangles` thin_parts_at() takes for curves."""
    mask = np.zeros(len(triangles), dtype=bool)
    for part in thin_parts_at(positions, triangles, budget, owned):
        mask[part.triangles] = True
    return mask


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
    from r3d.geometry import principal_frame, triangle_areas
    from r3d.simplify import object_groups

    if not len(triangles):
        return []
    grid = np.rint(positions / (side / WELD_CELLS_PER_SIDE))
    groups = object_groups(grid, triangles, np.arange(len(triangles)))
    result = []
    for group in np.unique(groups):
        rows = np.flatnonzero(groups == group)
        points = np.unique(positions[triangles[rows]].reshape(-1, 3), axis=0)
        centre, axes, local = principal_frame(points)
        axes, extents = axes[:, ::-1], np.ptp(local, axis=0)[::-1]
        limit = side * THIN_WIDTH_PER_SIDE
        if extents[0] < side * THIN_LENGTH_PER_SIDE:
            continue
        width = extents[1]
        if width > limit:
            # A tube `limit` across has pi * limit of surface per unit of its length: with more, no centreline is
            # that thin, and the samples are not drawn.
            if triangle_areas(positions, triangles[rows]).sum() <= np.pi * limit * extents[0]:
                width = centreline_width(positions, triangles, rows, centre, axes, limit)
        if width <= limit and extents[0] >= THIN_SLENDERNESS_MIN * width:
            result.append(ThinPart(np.asarray(rows), centre, axes, extents, width))
    if not result:
        return []
    from r3d.geometry import triangle_search

    search = triangle_search(positions[triangles])
    return [part for part in result if not resting(part, positions, triangles, search)]


def resting(part, positions, triangles, search):
    """Whether `part` lies along the other triangles, by RESTING_SHARE and its reach; `search` holds them all."""
    reach = RESTING_REACH_PER_WIDTH * part.width
    own = np.zeros(len(triangles), dtype=bool)
    own[part.triangles] = True
    points = positions[np.unique(triangles[part.triangles])]
    from r3d.geometry import triangles_within

    near = [not own[triangles_within(search, point, reach)].all() for point in points]
    return bool(np.mean(near) >= RESTING_SHARE)

"""Join exact shapes whose surfaces agree under a rigid placement, however each copy is triangulated.

The exact pass (instances.match_instances) needs equal vertex and triangle counts. A source reduced object by
object triangulates every copy differently, so its copies only agree as surfaces. A shape here is its surface
alone: materials are a copy's shading and stay with the copy.
"""

from dataclasses import dataclass, replace
from itertools import permutations, product

import numpy as np
from scipy.spatial import cKDTree

from r3d.geometry import closest_point_on_triangles, triangle_areas
from r3d.objects.instances import Instance, place, rigid_fit, symmetric_eigen
from r3d.surface_samples import uniform_barycentric

SURFACE_TOLERANCE = 1e-2
# An absolute floor on the gap may not loosen a small shape's limit past this share of its diagonal.
FLOOR_CAP = 0.05
SURFACE_SAMPLES = 400
# Log-space width per feature (area, three surface spreads) inside which two shapes are compared at all.
FEATURE_SPREAD = 0.10
# Signed axis permutations between the two surfaces' principal frames: 6 orders x 8 signs, mirrors included.
START_FRAMES = tuple(np.eye(3)[list(order)] * signs for order in permutations(range(3))
                     for signs in product((-1., 1.), repeat=3))
SCREEN_SAMPLES = 60
STARTS_KEPT = 3
# A start whose screening gap passes this share of the diagonal is not worth refining.
START_GAP = 0.15
ICP_SAMPLES = 150
ICP_ITERATIONS = 16
# Refinement stops once an iteration shrinks the largest sample distance by less than 1 %.
CONVERGED = 0.99
# Dense points on every triangle for the nearest-triangle lookup, at most diagonal / DENSITY from any surface point.
DENSITY = 20
# Dense points fetched per query in one vectorised call before falling back to a full ball.
BALL_POINTS = 8
SEED = 1


@dataclass(frozen=True)
class Surface:
    corners: np.ndarray
    area: float
    centre: np.ndarray
    axes: np.ndarray
    diagonal: float
    feature: np.ndarray
    samples: np.ndarray
    tree: cKDTree
    owner: np.ndarray
    spacing: float


def surface(positions, triangles):
    """Area moments, seeded area-weighted samples and a dense lookup of a shape's nonzero-area triangles, or None
    for a shape without area."""
    positions, triangles = np.asarray(positions, dtype=np.float64), np.asarray(triangles)
    areas = triangle_areas(positions, triangles)
    corners, areas = positions[triangles[areas > 0]], areas[areas > 0]
    if not len(areas):
        return None
    total = areas.sum()
    centre = (corners.mean(axis=1) * areas[:, None]).sum(axis=0) / total
    local = corners - centre
    summed = local.sum(axis=1)
    # Second moment of a triangle: area / 12 * (sum of v v^T over corners + s s^T), s the corner sum.
    moment = (np.einsum('n,nki,nkj->ij', areas, local, local) + np.einsum('n,ni,nj->ij', areas, summed, summed)) / (
        12 * total)
    # Cyclic Jacobi rather than LAPACK, so every host picks the same axis signs and the same start frames tie.
    spread, axes = symmetric_eigen(moment)
    diagonal = float(np.linalg.norm(np.ptp(corners.reshape(-1, 3), axis=0)))
    feature = np.log(np.concatenate([[total], np.sqrt(np.maximum(spread, 0)) + 1e-3 * diagonal]) + 1e-12)
    rng = np.random.default_rng(SEED)
    picked = rng.choice(len(areas), SURFACE_SAMPLES, p=areas / total)
    samples = np.einsum('nk,nki->ni', uniform_barycentric(rng, SURFACE_SAMPLES), corners[picked])
    edge = np.linalg.norm(corners - np.roll(corners, 1, axis=1), axis=2).max(axis=1)
    steps = np.maximum(np.ceil(edge * DENSITY / max(diagonal, 1e-12)), 1).astype(int)
    # Any surface point lies within its triangle's longest edge over its steps of a dense point.
    spacing = float((edge / steps).max())
    points, owner = [], []
    for m in np.unique(steps):
        chosen = np.flatnonzero(steps == m)
        for i in range(m + 1):
            for j in range(m + 1 - i):
                u, v = i / m, j / m
                points.append(corners[chosen, 0] * (1 - u - v) + corners[chosen, 1] * u + corners[chosen, 2] * v)
                owner.append(chosen)
    return Surface(corners, total, centre, axes, diagonal, feature, samples, cKDTree(np.concatenate(points)),
                   np.concatenate(owner), spacing)


def nearest(points, target):
    """Distance from each point to target's surface, and the closest surface point. A point's closest triangle
    holds a dense point within its distance to the nearest dense point plus the spacing, so the triangles owning
    the dense points in that ball are all the candidates."""
    reach, _ = target.tree.query(points)
    k = min(BALL_POINTS, target.tree.n)
    bound = reach + target.spacing
    distance, index = target.tree.query(points, k=k, distance_upper_bound=float(bound.max()))
    distance, index = distance.reshape(len(points), k), index.reshape(len(points), k)
    inside = distance <= bound[:, None]
    query, found = np.nonzero(inside)
    found = index[query, found]
    # A ball holding more than BALL_POINTS dense points is listed in full.
    full = np.flatnonzero(inside[:, -1]) if k < target.tree.n else ()
    if len(full):
        balls = target.tree.query_ball_point(points[full], bound[full])
        query = np.concatenate([query, np.repeat(full, [len(ball) for ball in balls])])
        found = np.concatenate([found, np.concatenate(balls).astype(int)])
    key = np.unique(query * len(target.corners) + target.owner[found])
    pairs = np.column_stack([key // len(target.corners), key % len(target.corners)])
    a, b, c = (target.corners[pairs[:, 1], k] for k in range(3))
    bary, distance = closest_point_on_triangles(points[pairs[:, 0]], a, b, c)
    first = np.lexsort((distance, pairs[:, 0]))
    first = first[np.r_[True, pairs[first[1:], 0] != pairs[first[:-1], 0]]]
    closest = bary[first, :1] * a[first] + bary[first, 1:2] * b[first] + bary[first, 2:] * c[first]
    return distance[first], closest


def limit(a, b, tolerance, floor):
    """The largest gap two surfaces may keep: a share of the larger diagonal, raised to an absolute floor that
    stays within FLOOR_CAP of it."""
    diagonal = max(a.diagonal, b.diagonal)
    return max(tolerance * diagonal, min(floor, FLOOR_CAP * diagonal))


def surface_fit(rep, other, tolerance, floor=0.0):
    """Rigid placement (linear, position) of other's surface onto rep's with its two-way gap, or None when no start
    frame brings both surfaces within limit() of each other. Starts from START_FRAMES between the principal frames,
    refines the best by iterative closest points (Besl and McKay 1992) with instances.rigid_fit, whose placements
    round to the float32 a copy is stored in."""
    allowed = limit(rep, other, tolerance, floor)
    screen, refine = other.samples[:SCREEN_SAMPLES], other.samples[:ICP_SAMPLES]
    starts = []
    for frame in START_FRAMES:
        linear = place(place(rep.axes, frame.T, 0.0), other.axes, 0.0)
        position = rep.centre - place(other.centre[None], linear, 0.0)[0]
        # Screened on the dense points alone: over the surface distance by at most the spacing.
        gap = rep.tree.query(place(screen, linear, position))[0].max()
        starts.append((gap, len(starts), linear, position))
    starts.sort(key=lambda start: start[:2])
    for gap, _, linear, position in starts[:STARTS_KEPT]:
        if gap > START_GAP * max(rep.diagonal, other.diagonal):
            break
        reflected = np.linalg.det(linear) < 0
        previous = np.inf
        for _ in range(ICP_ITERATIONS):
            distance, closest = nearest(place(refine, linear, position), rep)
            if distance.max() > previous * CONVERGED:
                break
            previous = distance.max()
            linear, position, _ = rigid_fit(refine, closest, reflected)
        forward = nearest(place(other.samples, linear, position), rep)[0].max()
        if forward > allowed:
            continue
        backward = nearest(place(rep.samples - position, linear.T, 0.0), other)[0].max()
        if backward <= allowed:
            return linear, position, float(max(forward, backward))
    return None


def merge(shapes, tolerance=SURFACE_TOLERANCE, floor=0.0):
    """Join shapes whose surfaces agree: the larger-area shape (earlier on a tie) keeps its geometry, and every
    copy of a joined shape is placed onto it with its gap, by_surface and no triangle order: its own triangles
    do not correspond to the shape's."""
    for value in (tolerance, floor):
        if not np.isfinite(value) or value < 0:
            raise ValueError('tolerance and floor must be finite and nonnegative')
    surfaces = [surface(shape.positions, shape.triangles) for shape in shapes]
    order = sorted((i for i, s in enumerate(surfaces) if s is not None), key=lambda i: (-surfaces[i].area, i))
    kept, features, joined = [], [], {}
    for index in order:
        here = surfaces[index]
        candidates = np.flatnonzero(np.all(np.abs(np.array(features) - here.feature) < FEATURE_SPREAD, axis=1)) \
            if features else ()
        for slot in candidates:
            target = kept[slot]
            fit = surface_fit(surfaces[target], here, tolerance, floor)
            if fit is not None:
                joined[target].append((index, fit))
                break
        else:
            kept.append(index)
            features.append(here.feature)
            joined[index] = []
    merged = []
    for index, shape in enumerate(shapes):
        if surfaces[index] is not None and index not in joined:
            continue
        instances = list(shape.instances)
        for other, (linear, position, gap) in joined.get(index, ()):
            # other's local x maps to shape's local as x linear^T + position, so shape's local y is other's
            # (y - position) linear, which each of other's copies then places in the scene.
            back = place(position[None], linear.T, 0.0)
            for copy in shapes[other].instances:
                placed = stored(place(linear, copy.linear, 0.0).T)
                offset = stored(copy.position - place(back, copy.linear, 0.0)[0])
                instances.append(Instance(copy.object_name, placed, offset, bool(np.linalg.det(placed) < 0),
                                          copy.residual, (), gap, by_surface=True))
        merged.append(replace(shape, instances=tuple(instances)))
    return merged


def stored(values):
    """Rounded to the float32 a copy's placement is stored in, as instances.rigid_fit rounds an exact match's."""
    return np.asarray(values).astype(np.float32).astype(np.float64)

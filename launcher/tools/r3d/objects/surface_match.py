"""Join exact shapes whose surfaces agree under a rigid placement, however each copy is triangulated.

The exact pass (instances.match_instances) needs equal vertex and triangle counts. A source reduced object by
object triangulates every copy differently, so its copies only agree as surfaces. A shape here is its surface
alone: materials are a copy's shading and stay with the copy.
"""

from dataclasses import dataclass, replace
from itertools import permutations, product

import numpy as np

from r3d.geometry import TriangleSearch, nearest_triangles, triangle_areas, triangle_search
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
SEED = 1


@dataclass(frozen=True)
class JoinedInstance(Instance):
    """A copy joined by its surface: its triangles do not correspond to the shape's, so it has no triangle order.
    Kept here, not on Instance, because instances.py is in every mesh bake's key."""
    # Largest distance between the copy's surface and the shape's.
    gap: float = 0.0


@dataclass(frozen=True)
class Surface:
    corners: np.ndarray
    area: float
    centre: np.ndarray
    axes: np.ndarray
    diagonal: float
    feature: np.ndarray
    samples: np.ndarray
    vertices: np.ndarray
    search: TriangleSearch


def surface(positions, triangles):
    """Area moments, seeded area-weighted samples and a nearest-triangle lookup of a shape's nonzero-area triangles,
    or None for a shape without area."""
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
    vertices = np.unique(corners.reshape(-1, 3), axis=0)
    # Bounds taken along the principal axes, so a turned copy measures the same.
    diagonal = float(np.linalg.norm(np.ptp(place(vertices - centre, axes.T, 0.0), axis=0)))
    feature = np.log(np.concatenate([[total], np.sqrt(np.maximum(spread, 0)) + 1e-3 * diagonal]) + 1e-12)
    rng = np.random.default_rng(SEED)
    picked = rng.choice(len(areas), SURFACE_SAMPLES, p=areas / total)
    samples = np.einsum('nk,nki->ni', uniform_barycentric(rng, SURFACE_SAMPLES), corners[picked])
    return Surface(corners, total, centre, axes, diagonal, feature, samples, vertices,
                   triangle_search(corners))


def nearest(points, target):
    """Distance from each point to target's surface, and the closest surface point."""
    tri, bary, distance = nearest_triangles(points, target.search)
    return distance, np.einsum('ni,nij->nj', bary, target.corners[tri])


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
        gap = nearest_triangles(place(screen, linear, position), rep.search)[2].max()
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
        # Samples cover the area; vertices add every feature too small to be sampled.
        forward = nearest(place(np.vstack([other.samples, other.vertices]), linear, position), rep)[0].max()
        if forward > allowed:
            continue
        backward = nearest(place(np.vstack([rep.samples, rep.vertices]) - position, linear.T, 0.0), other)[0].max()
        if backward <= allowed:
            return linear, position, float(max(forward, backward))
    return None


def merge(shapes, tolerance=SURFACE_TOLERANCE, floor=0.0):
    """Join shapes whose surfaces agree: the larger-area shape (earlier on a tie) keeps its geometry, and every
    copy of a joined shape is placed onto it as a JoinedInstance."""
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
                instances.append(JoinedInstance(copy.object_name, placed, offset, bool(np.linalg.det(placed) < 0),
                                                copy.residual, (), gap))
        merged.append(replace(shape, instances=tuple(instances)))
    return merged


def stored(values):
    """Rounded to the float32 a copy's placement is stored in, as instances.rigid_fit rounds an exact match's."""
    return np.asarray(values).astype(np.float32).astype(np.float64)

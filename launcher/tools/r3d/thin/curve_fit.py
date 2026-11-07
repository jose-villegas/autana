"""Fit thin source surfaces to radius-bearing polylines and bake their light."""

from dataclasses import dataclass, replace
from types import SimpleNamespace

import numpy as np

from r3d.surface_samples import area_samples

CURVE_TOLERANCE_PER_WIDTH = 0.15
CURVE_LIGHT_RING = 8
# A segment draws the mean of its two knots' colours, so each knot is lit over the stretch it draws: rings at the
# knot and these fractions of the way to each neighbour. One ring at a pole's end in a wall bracket would sit in
# the bracket and darken the whole shaft.
CURVE_LIGHT_SPAN = (0.25, 0.5)
# A ring direction that meets other geometry within CURVE_SEEN_REACH_PER_RADIUS radii faces a wall or a neighbour,
# a side no viewer stands on: the colour is the mean over the open directions, as a double-sided surface is lit on
# its open side. A ring with no open direction takes the mean over all of them.
CURVE_SEEN_REACH_PER_RADIUS = 8.0
SAMPLES_PER_WIDTH = 6
MIN_BINS = 4
SAMPLE_SEED = 0
TUBE_SIDES = 6


@dataclass(frozen=True)
class Curve:
    knots: np.ndarray
    radius: np.ndarray
    color: np.ndarray


def fit_curve(positions, triangles, part):
    """Area samples sliced along the long PCA axis, simplified in position and radius."""
    width = float(part.extents[1])
    if width <= 0:
        raise ValueError("a curve needs a nonzero surface width")
    source = SimpleNamespace(p=np.asarray(positions), tri_v=np.asarray(triangles))
    corners = source.p[source.tri_v[part.triangles]]
    area = np.linalg.norm(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1).sum() / 2
    count = max(MIN_BINS * CURVE_LIGHT_RING, int(np.ceil(area / (width / SAMPLES_PER_WIDTH) ** 2)))
    samples, _ = area_samples(source, part.triangles, count, np.random.default_rng(SAMPLE_SEED))
    local = (samples - part.centre) @ part.axes
    ends = ((corners.reshape(-1, 3) - part.centre) @ part.axes)[:, 0]
    lo, hi = ends.min(), ends.max()
    bins = max(MIN_BINS, int(np.ceil((hi - lo) / width)))
    bucket = np.minimum(((local[:, 0] - lo) * bins / (hi - lo)).astype(int), bins - 1)
    rows = []
    for index in range(bins):
        points = local[bucket == index]
        if not len(points):
            continue
        centre = (points.min(axis=0) + points.max(axis=0)) / 2
        radius = np.linalg.norm(points[:, 1:] - centre[1:], axis=1).mean()
        rows.append([*centre, radius])
    rows = np.asarray(rows)
    rows[0, 0], rows[-1, 0] = lo, hi
    rows[:, :3] = rows[:, :3] @ part.axes.T + part.centre
    keep = {0, len(rows) - 1}
    pending = [(0, len(rows) - 1)]
    tolerance = CURVE_TOLERANCE_PER_WIDTH * width
    while pending:
        first, last = pending.pop()
        if last <= first + 1:
            continue
        delta = rows[last] - rows[first]
        offset = rows[first + 1:last] - rows[first]
        denominator = delta @ delta
        t = np.clip(offset @ delta / denominator, 0, 1) if denominator else np.zeros(len(offset))
        error = np.linalg.norm(offset - t[:, None] * delta, axis=1)
        index = first + 1 + int(error.argmax())
        if error.max() > tolerance:
            keep.add(index)
            pending.extend(((first, index), (index, last)))
    rows = rows[sorted(keep)]
    return Curve(rows[:, :3], rows[:, 3], np.zeros((len(rows), 3)))


def light_stations(knots, radius):
    """Each knot's ring centres and radii over the stretch it draws: the knot, then CURVE_LIGHT_SPAN of the way to
    the knot before and the knot after; an end looks to its one neighbour on both sides."""
    index = np.arange(len(knots))
    before = np.where(index > 0, index - 1, np.minimum(index + 1, len(knots) - 1))
    after = np.where(index < len(knots) - 1, index + 1, np.maximum(index - 1, 0))
    fractions = np.array([0.0, *CURVE_LIGHT_SPAN, *CURVE_LIGHT_SPAN])
    toward = np.stack([index] + [before] * len(CURVE_LIGHT_SPAN) + [after] * len(CURVE_LIGHT_SPAN), axis=1)
    centres = knots[:, None] + fractions[None, :, None] * (knots[toward] - knots[:, None])
    radii = radius[:, None] + fractions[None] * (radius[toward] - radius[:, None])
    return centres, radii


def light_curve(curve, source, part, intersector, lights, ray_offset, bounce=None, ao=None, bounce_intensity=1.0, double_sided=False):
    """Light outward ring normals over each knot's stretch with the start bake's radiance and the part's own albedo."""
    from r3d.light import light, sample_albedo, tangent_frame

    angle = np.arange(CURVE_LIGHT_RING) * (2 * np.pi / CURVE_LIGHT_RING)
    tangent = np.gradient(curve.knots, axis=0)
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    u, v = tangent_frame(tangent)
    normals = np.cos(angle)[None, :, None] * u[:, None] + np.sin(angle)[None, :, None] * v[:, None]
    centres, radii = light_stations(curve.knots, curve.radius)
    stations = centres.shape[1]
    points = (centres[:, :, None] + radii[:, :, None, None] * normals[:, None]).reshape(-1, 3)
    normals = np.broadcast_to(normals[:, None], (len(curve.knots), stations, CURVE_LIGHT_RING, 3)).reshape(-1, 3)
    materials = source.tri_m[part.triangles]
    centres = source.p[source.tri_v[part.triangles]].mean(axis=1)
    nearest = np.linalg.norm(curve.knots[:, None] - centres, axis=2).argmin(axis=1)
    albedo = np.zeros_like(curve.knots)
    for material in np.unique(materials[nearest]):
        mask = materials[nearest] == material
        own = part.triangles[materials == material]
        kd = source.materials.get(source.names[material], {}).get("Kd", (1.0, 1.0, 1.0))
        albedo[mask] = sample_albedo(curve.knots[mask], curve.radius[mask], material, source.p, source.uv,
                                     source.tri_v[own], source.tri_t[own], source.tri_m[own], source.textures, kd,
                                     source.colors)
    radiance = light(points, normals,
                     np.repeat(np.broadcast_to(double_sided, (len(part.triangles),))[nearest], stations * CURVE_LIGHT_RING),
                     intersector, lights, ray_offset, bounce,
                     bounce_groups=np.unique(points, axis=0, return_inverse=True)[1], ao=ao,
                     bounce_intensity=bounce_intensity)
    reach = CURVE_SEEN_REACH_PER_RADIUS * np.repeat(radii.reshape(-1), CURVE_LIGHT_RING)
    seen = open_directions(points, normals, reach, intersector, ray_offset).reshape(len(curve.knots), -1)
    seen[~seen.any(axis=1)] = True
    radiance = radiance.reshape(len(curve.knots), -1, 3)
    return replace(curve, color=albedo * (radiance * seen[:, :, None]).sum(axis=1) / seen.sum(axis=1)[:, None])


def open_directions(points, normals, reach, intersector, ray_offset):
    """Whether nothing stands within `reach` of each point along its normal; with no `intersector`, nothing does."""
    if intersector is None:
        return np.ones(len(points), bool)
    hit, distance, _ = intersector.first_hits(points + normals * ray_offset, np.ascontiguousarray(normals))
    return ~hit | (distance > reach)

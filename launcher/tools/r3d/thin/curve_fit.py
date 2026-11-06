"""Fit thin source surfaces to radius-bearing polylines and bake their light."""

from dataclasses import dataclass, replace
from types import SimpleNamespace

import numpy as np

from r3d.surface_samples import area_samples

CURVE_TOLERANCE_PER_WIDTH = 0.15
CURVE_LIGHT_RING = 8
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


def light_curve(curve, source, part, intersector, lights, ray_offset, bounce=None, ao=None, bounce_intensity=1.0, double_sided=False):
    """Light outward ring normals with the start bake's radiance and the part's own albedo."""
    from r3d.light import light, sample_albedo, tangent_frame

    angle = np.arange(CURVE_LIGHT_RING) * (2 * np.pi / CURVE_LIGHT_RING)
    tangent = np.gradient(curve.knots, axis=0)
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    u, v = tangent_frame(tangent)
    normals = np.cos(angle)[None, :, None] * u[:, None] + np.sin(angle)[None, :, None] * v[:, None]
    points = (curve.knots[:, None] + curve.radius[:, None, None] * normals).reshape(-1, 3)
    normals = normals.reshape(-1, 3)
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
    radiance = light(points, normals, np.repeat(np.broadcast_to(double_sided, (len(part.triangles),))[nearest], CURVE_LIGHT_RING), intersector, lights, ray_offset, bounce,
                     bounce_groups=np.unique(points, axis=0, return_inverse=True)[1], ao=ao,
                     bounce_intensity=bounce_intensity)
    return replace(curve, color=albedo * radiance.reshape(-1, CURVE_LIGHT_RING, 3).mean(axis=1))

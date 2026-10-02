"""Baked direct light: a sun with soft shadows and sky visibility, cast against the full-detail mesh, plus albedo sampled from textures and region visibility culling."""

import math
from types import SimpleNamespace

import numpy as np
from scipy.spatial import cKDTree

from . import log
from .geometry import closest_point_on_triangles, triangle_areas
from .import_settings import LIGHT_FIELDS


def drop_masked(p, uv, tri_v, tri_t, tri_m, textures, keep_alpha=0.5):
    """Alpha-tested cards (leaves, chains) keep only triangles mostly opaque."""
    keep = np.ones(len(tri_v), dtype=bool)
    bary = np.array([[1 / 3, 1 / 3, 1 / 3], [0.6, 0.2, 0.2], [0.2, 0.6, 0.2], [0.2, 0.2, 0.6]])
    for m, tex in enumerate(textures):
        sel = np.nonzero(tri_m == m)[0]
        if tex is None or len(sel) == 0 or tex.levels[0][..., 3].min() > 0.99:
            continue
        alpha = np.zeros(len(sel))
        for b in bary:
            tuv = (uv[tri_t[sel]] * b[None, :, None]).sum(axis=1)
            alpha += tex.sample(tuv, np.full(len(sel), 2.0))[:, 3]
        keep[sel] = alpha / len(bary) >= keep_alpha
    keep &= triangle_areas(p, tri_v) > 1e-6
    log(f"masked/degenerate triangles dropped: {np.count_nonzero(~keep)}")
    return tri_v[keep], tri_t[keep], tri_m[keep]


def visible_from_region(p, tri_v, double, intersector, rounds, rng, lo, hi):
    """A triangle is kept if, in any of `rounds` tries, a random point on it
    sees a random point of the box lo..hi from its front side."""
    a, b, c = p[tri_v[:, 0]], p[tri_v[:, 1]], p[tri_v[:, 2]]
    normal = np.cross(b - a, c - a)
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
    lo, hi = np.array(lo), np.array(hi)
    seen = np.zeros(len(tri_v), dtype=bool)
    for _ in range(rounds):
        todo = np.nonzero(~seen)[0]
        if len(todo) == 0:
            break
        r1, r2 = np.sqrt(rng.random(len(todo)))[:, None], rng.random(len(todo))[:, None]
        on = (1 - r1) * a[todo] + r1 * (1 - r2) * b[todo] + r1 * r2 * c[todo]
        eye = lo + rng.random((len(todo), 3)) * (hi - lo)
        d = eye - on
        dist = np.linalg.norm(d, axis=1)
        d /= dist[:, None]
        side = (d * normal[todo]).sum(axis=1)
        facing = (side > 0.01) | double[todo]
        origin = on + normal[todo] * np.where(side >= 0, 0.5, -0.5)[:, None]
        hit = np.zeros(len(todo), dtype=bool)
        if np.any(facing):
            f = np.nonzero(facing)[0]
            locations, index_ray, _ = intersector.intersects_location(origin[f], d[f], multiple_hits=False)
            hit_dist = np.full(len(f), np.inf)
            hit_dist[index_ray] = np.linalg.norm(locations - origin[f][index_ray], axis=1)
            hit[f] = hit_dist < dist[f] - 1.0
        seen[todo[facing & ~hit]] = True
    log(f"visible from the region: {np.count_nonzero(seen)} of {len(tri_v)} triangles")
    return seen


def sun_directions(light):
    """One fixed set of directions over the sun's disc, shared by every point,
    so two points agree exactly unless something really shadows one of them."""
    sun = np.array(light["direction"], dtype=np.float64)
    sun /= np.linalg.norm(sun)
    u, v = sun_basis(sun)
    radius = math.tan(math.radians(light["disc_degrees"]))
    dirs = [sun]
    rings = max(1, light["rays"] - 1)
    for i in range(rings):
        a = 2 * math.pi * i / rings
        d = sun + u * (0.7 * radius * math.cos(a)) + v * (0.7 * radius * math.sin(a))
        dirs.append(d / np.linalg.norm(d))
    return sun, dirs


def unshadowed_count(intersector, origin, directions):
    """How many of the directions reach the sky from each origin; a direction
    is one vector for every origin or one per origin."""
    count = np.zeros(len(origin))
    for direction in directions:
        count += ~intersector.intersects_any(origin, np.ascontiguousarray(np.broadcast_to(direction, origin.shape)))
    return count


def albedo_from_uv(texture, kd, uv, lod):
    """Linear albedo at texture coordinates: the material colour alone for an
    untextured material, else the texture sampled at `lod` times the colour."""
    if texture is None:
        return np.tile(np.array(kd) ** 2.2, (len(uv), 1))
    return texture.sample(uv, lod)[:, :3] * np.array(kd)


def sample_albedo(points, spacing, m, p, uv, tri_v, tri_t, tri_m, textures, kd):
    sel = np.nonzero(tri_m == m)[0]
    tex = textures[m]
    if tex is None:
        return albedo_from_uv(None, kd, points, None)
    a, b, c = p[tri_v[sel, 0]], p[tri_v[sel, 1]], p[tri_v[sel, 2]]
    tree = cKDTree((a + b + c) / 3)
    k = min(16, len(sel))
    _, cand = tree.query(points, k=k)
    cand = cand.reshape(len(points), k)
    best_d = np.full(len(points), np.inf)
    best_bary = np.zeros((len(points), 3))
    best_tri = np.zeros(len(points), dtype=np.int64)
    for j in range(k):
        t = cand[:, j]
        bary, d = closest_point_on_triangles(points, a[t], b[t], c[t])
        better = d < best_d
        best_d[better] = d[better]
        best_bary[better] = bary[better]
        best_tri[better] = t[better]
    tris = sel[best_tri]
    tuv = (uv[tri_t[tris]] * best_bary[:, :, None]).sum(axis=1)
    world_area = triangle_areas(p, tri_v[tris])
    e1 = uv[tri_t[tris, 1]] - uv[tri_t[tris, 0]]
    e2 = uv[tri_t[tris, 2]] - uv[tri_t[tris, 0]]
    w, h = tex.size
    texel_area = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]) * w * h
    texels_per_unit = np.sqrt(texel_area / np.maximum(world_area, 1e-9))
    lod = np.log2(np.maximum(spacing * texels_per_unit, 1.0))
    return albedo_from_uv(tex, kd, tuv, lod)


def sun_basis(direction):
    helper = np.array([0.0, 0.0, 1.0]) if abs(direction[2]) < 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(direction, helper)
    u /= np.linalg.norm(u)
    return u, np.cross(direction, u)


def tangent_frame(n):
    """Two unit tangents completing each normal in n, fixed by the normal alone."""
    tu = np.where(np.abs(n[:, 2:3]) < 0.9, [[0.0, 0.0, 1.0]], [[1.0, 0.0, 0.0]])
    tu = np.cross(n, tu)
    tu /= np.linalg.norm(tu, axis=1, keepdims=True)
    return tu, np.cross(n, tu)


def sky_directions(count):
    """One fixed set of cosine-weighted hemisphere directions (z up), shared by
    every point: a golden-ratio sequence, so any prefix is evenly spread."""
    i = np.arange(count)
    r1 = (0.5 + i * 0.7548776662466927) % 1.0
    a = 2 * math.pi * ((0.5 + i * 0.5698402909980532) % 1.0)
    r = np.sqrt(r1)
    return np.stack([r * np.cos(a), r * np.sin(a), np.sqrt(1 - r1)], axis=1)


def bake_directional(light, ctx):
    sun = np.array(light["direction"], dtype=np.float64)
    sun /= np.linalg.norm(sun)
    cos_sun = np.maximum(ctx.normals @ sun, 0.0)
    if ctx.shared:
        directions = [sun] if ctx.sun_centre else sun_directions(light)[1]
    else:
        u, v = sun_basis(sun)
        radius = math.tan(math.radians(light["disc_degrees"]))
        directions = []
        for _ in range(light["rays"]):
            r, angle = math.sqrt(ctx.rng.random()) * radius, ctx.rng.random() * 2 * math.pi
            direction = sun + u * (r * math.cos(angle)) + v * (r * math.sin(angle))
            directions.append(direction / np.linalg.norm(direction))
    lit = unshadowed_count(ctx.intersector, ctx.origin, directions)
    return (cos_sun * lit / len(directions))[:, None] * np.array(light["color"]) * light["intensity"]


def bake_sky(light, ctx):
    n = ctx.normals
    tu, tv = tangent_frame(n)
    rays = ctx.shared_sky_rays if ctx.shared else light["rays"]
    if ctx.shared:
        directions = [tu * x + tv * y + n * z for x, y, z in sky_directions(rays)]
    else:
        directions = []
        for _ in range(rays):
            r1, r2 = ctx.rng.random(len(n)), ctx.rng.random(len(n))
            r, angle = np.sqrt(r1)[:, None], (2 * math.pi * r2)[:, None]
            directions.append(tu * (r * np.cos(angle)) + tv * (r * np.sin(angle)) + n * np.sqrt(1 - r1)[:, None])
    visible = unshadowed_count(ctx.intersector, ctx.origin, directions) / rays
    return visible[:, None] * np.array(light["color"]) * light["intensity"]


def bake_ambient(light, ctx):
    return np.array(light["color"]) * light["intensity"]


# The one table of what a scene light is: its fields, declared in
# import_settings.py, and what it adds to a point's radiance.
BAKERS = {"directional": bake_directional, "sky": bake_sky, "ambient": bake_ambient}
LIGHTS = {kind: (LIGHT_FIELDS[kind], bake) for kind, bake in BAKERS.items()}
assert set(LIGHTS) == set(LIGHT_FIELDS)


def face_towards_light(normals, double_sided, lights):
    """Double-sided surfaces turn to the side the directional lights, summed,
    shine on. One orientation for every light, so their order cannot matter."""
    toward = np.zeros(3)
    for light in lights:
        if light["type"] == "directional":
            direction = np.array(light["direction"], dtype=np.float64)
            toward += light["intensity"] * direction / np.linalg.norm(direction)
    flip = double_sided & (normals @ toward < 0)
    return np.where(flip[:, None], -normals, normals)


def light(points, normals, double_sided, intersector, lights, ray_offset, rng, shared_sky_rays=0, sun_centre=False,
          indirect=None, indirect_groups=None):
    """Radiance from the scene lights at each point.

    With shared_sky_rays > 0 every point uses the same directional samples and
    that many sky directions (a flat bake); otherwise rays are drawn at random
    and each sky light uses its own `rays`. `sun_centre` lights a flat bake from
    the middle of the sun's disc only, a hard shadow edge. Point and spot lights are reserved
    and not baked yet.

    The lights share one rng, so their order in the list changes which random
    rays each draws: equal on average, not byte for byte. A flat bake draws
    none and is exactly order independent.

    `indirect` is an IndirectCache whose gathered light is added to the
    direct light; `indirect_groups` is gather_indirect's `groups`.
    """
    n = face_towards_light(normals, double_sided, lights)
    ctx = SimpleNamespace(normals=n, origin=points + n * ray_offset, intersector=intersector, rng=rng,
                          shared=bool(shared_sky_rays), shared_sky_rays=shared_sky_rays,
                          sun_centre=sun_centre)
    radiance = np.zeros((len(points), 3))
    for scene_light in lights:
        radiance += LIGHTS[scene_light["type"]][1](scene_light, ctx)
    return radiance + gather_indirect(points, n, intersector, indirect, indirect_groups)


class IndirectCache:
    """Full-detail triangle radiance for a finite diffuse bounce series, with
    each triangle's normal and sidedness so a ray that reaches a one-sided
    triangle from behind finds no light."""

    def __init__(self, radiance, rays, ray_offset, normals=None, two_sided=None):
        self.radiance = radiance
        self.rays = rays
        self.ray_offset = ray_offset
        self.normals = normals
        self.two_sided = two_sided


def gather_indirect(points, normals, intersector, cache, groups=None):
    """Estimate irradiance from the cache with cosine-weighted hemisphere rays.
    Every point uses the same set of directions in its own frame, so equal
    surroundings give equal light and nothing is random. A miss adds nothing,
    because the sky light is direct.

    `groups` gives points that share one position the same result: the light
    is gathered once on their mean normal, since indirect light changes slowly
    where direct light does not."""
    if cache is None:
        return np.zeros((len(points), 3))
    if groups is not None:
        _, first = np.unique(groups, return_index=True)
        total = np.stack([np.bincount(groups, weights=normals[:, axis]) for axis in range(3)], axis=1)
        length = np.linalg.norm(total, axis=1, keepdims=True)
        mean = np.where(length > 1e-6, total / np.maximum(length, 1e-12), normals[first])
        return gather_indirect(points[first], mean, intersector, cache)[groups]
    origin = points + normals * cache.ray_offset
    out = np.zeros((len(points), 3))
    tu, tv = tangent_frame(normals)
    for x, y, z in sky_directions(cache.rays):
        direction = tu * x + tv * y + normals * z
        locations, indices, faces = intersector.intersects_location(origin, direction, multiple_hits=False)
        found = cache.radiance[:, faces].sum(axis=0)
        if cache.normals is not None:
            behind = np.einsum("ij,ij->i", direction[indices], cache.normals[faces]) > 0
            found[behind & ~cache.two_sided[faces]] = 0.0
        out[indices] += found
    return out / cache.rays


def build_indirect_cache(points, tris, tri_mat, materials, double_materials, albedo_of, intersector, lights, ray_offset,
                         indirect):
    """Bake full-detail outgoing radiance once, then gather each later bounce."""
    if indirect is None or indirect.bounces == 0:
        return None
    a, b, c = points[tris[:, 0]], points[tris[:, 1]], points[tris[:, 2]]
    normals = np.cross(b - a, c - a)
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-12)
    area = triangle_areas(points, tris)
    centres = (a + b + c) / 3
    sample_points = np.concatenate([w[0] * a + w[1] * b + w[2] * c for w in face_samples(indirect.cache_samples)])
    sample_materials = np.tile(tri_mat, indirect.cache_samples)
    albedo = np.zeros((len(sample_points), 3))
    for material in materials:
        selected = np.nonzero(sample_materials == material)[0]
        if len(selected):
            albedo[selected] = albedo_of(sample_points[selected], np.tile(np.sqrt(area), indirect.cache_samples)[selected], material)
    double = np.isin(tri_mat, list(double_materials))
    sky_rays = max([item["rays"] for item in lights if item["type"] == "sky"], default=1)
    direct = light(sample_points, np.tile(normals, (indirect.cache_samples, 1)), np.tile(double, indirect.cache_samples),
                   intersector, lights, ray_offset, None, sky_rays)
    radiance = [(albedo * direct).reshape(indirect.cache_samples, len(tris), 3).mean(axis=0)]
    albedo = albedo.reshape(indirect.cache_samples, len(tris), 3).mean(axis=0)
    lit_side = face_towards_light(normals, double, lights)
    for _ in range(indirect.bounces - 1):
        previous = IndirectCache(np.asarray(radiance[-1:]), indirect.rays, ray_offset, normals, double)
        radiance.append(albedo * gather_indirect(centres, lit_side, intersector, previous))
    return IndirectCache(np.asarray(radiance), indirect.rays, ray_offset, normals, double)


def encode_srgb8(linear):
    """Linear light to 8-bit, with a 1/2.2 gamma (not the piecewise sRGB curve)."""
    return np.clip(np.round(255.0 * np.clip(linear, 0, 1) ** (1 / 2.2)), 0, 255).astype(np.int64)


def to_srgb8(linear, tonemap_white):
    """Lit radiance to 8-bit: the tone map, then the gamma."""
    return encode_srgb8(linear / (1.0 + linear * tonemap_white))


def face_samples(count, placement="stratified"):
    """Barycentric weights of `count` fixed points over a triangle: one per
    equal-area strip, staggered along it, or with placement "centroid" all at
    the centroid."""
    i = np.arange(count)
    if placement == "centroid":
        return np.full((count, 3), 1.0 / 3.0)
    r = np.sqrt((i + 0.5) / count)
    t = (0.5 + i * 0.6180339887498949) % 1.0
    return np.stack([1 - r, r * (1 - t), r * t], axis=1)


def adaptive_sample_counts(areas, reference, cap, floor=1):
    """Samples per face: one for each `reference` of area it covers, rounded,
    at least `floor` and at most `cap`."""
    return np.clip(np.round(areas / reference), floor, cap).astype(np.int64)


def face_colours(positions, tris, tri_mat, materials, double_materials, albedo_of, intersector, lights, ray_offset,
                 tonemap_white, samples=4, sky_rays=128, max_samples=16, sample_area=None, min_samples=1,
                 placement="stratified", sun_centre=False, indirect_cache=None):
    """One sRGB colour per triangle: albedo times light averaged over fixed
    points of the triangle, lit on its face normal. `samples` is a count per
    face, or "auto" for one point per `sample_area` of face area (the mesh's
    median face by default), from `min_samples` to `max_samples`. Every face shares one set of
    sun and `sky_rays` sky directions, so equal surroundings give equal
    colours. albedo_of(points, spacing, material) gives the albedo."""
    out = np.zeros((len(tris), 3), dtype=np.int64)
    areas = triangle_areas(positions, tris)
    if samples == "auto":
        counts = adaptive_sample_counts(areas, np.median(areas) if sample_area is None else sample_area, max_samples,
                                         min_samples)
    else:
        counts = np.full(len(tris), samples, dtype=np.int64)
    for m in materials:
        for k in np.unique(counts[tri_mat == m]):
            selected = np.nonzero((tri_mat == m) & (counts == k))[0]
            faces = tris[selected]
            a, b, c = positions[faces[:, 0]], positions[faces[:, 1]], positions[faces[:, 2]]
            normals = np.cross(b - a, c - a)
            normals /= np.linalg.norm(normals, axis=1, keepdims=True)
            points = np.concatenate([w[0] * a + w[1] * b + w[2] * c for w in face_samples(k, placement)])
            spacing = np.tile(np.sqrt(areas[selected]), k)
            albedo = albedo_of(points, spacing, m)
            double = np.full(len(points), m in double_materials)
            tiled = np.tile(normals, (k, 1))
            radiance = light(points, tiled, double, intersector, lights, ray_offset, None, sky_rays, sun_centre,
                             indirect_cache)
            colour = (albedo * radiance).reshape(k, len(faces), 3).mean(axis=0)
            out[selected] = to_srgb8(colour, tonemap_white)
    return out


def merge_matching_colours(pos, rgb, tris, step=6):
    """A crease splits a vertex so each side can be lit on its own normal;
    where both sides came out the same colour, one vertex is enough."""
    key = np.concatenate([np.round(pos * 16), rgb // step], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    tris = tris[(tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])]
    return pos[first], rgb[first], tris

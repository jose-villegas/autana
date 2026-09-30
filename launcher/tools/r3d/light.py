"""Baked direct light: a sun with soft shadows and sky visibility, cast against the full-detail mesh, plus albedo sampled from textures and region visibility culling."""

import math

import numpy as np
from scipy.spatial import cKDTree

from . import log
from .geometry import closest_point_on_triangles, triangle_areas


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


def sun_directions(args):
    """One fixed set of directions over the sun's disc, shared by every point,
    so two points agree exactly unless something really shadows one of them."""
    sun = np.array(args.sun, dtype=np.float64)
    sun /= np.linalg.norm(sun)
    u, v = sun_basis(sun)
    radius = math.tan(math.radians(args.sun_disc_deg))
    dirs = [sun]
    rings = max(1, args.sun_rays - 1)
    for i in range(rings):
        a = 2 * math.pi * i / rings
        d = sun + u * (0.7 * radius * math.cos(a)) + v * (0.7 * radius * math.sin(a))
        dirs.append(d / np.linalg.norm(d))
    return sun, dirs


def sun_exposure(points, normals, intersector, args):
    """Sun visibility weighted by how squarely the surface faces the sun:
    what a shadow edge changes, and what Gouraud shading cannot carry."""
    sun, dirs = sun_directions(args)
    facing = normals @ sun
    side = np.where(facing >= 0, 1.0, -1.0)[:, None]
    origin = points + normals * side * args.ray_offset
    lit = np.zeros(len(points))
    for d in dirs:
        lit += ~intersector.intersects_any(origin, np.tile(d, (len(points), 1)))
    return np.abs(facing) * lit / len(dirs)


def sample_albedo(points, spacing, m, p, uv, tri_v, tri_t, tri_m, textures, kd):
    sel = np.nonzero(tri_m == m)[0]
    tex = textures[m]
    if tex is None:
        return np.tile(np.array(kd) ** 2.2, (len(points), 1))
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
    return tex.sample(tuv, lod)[:, :3] * np.array(kd)


def sun_basis(direction):
    helper = np.array([0.0, 0.0, 1.0]) if abs(direction[2]) < 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(direction, helper)
    u /= np.linalg.norm(u)
    return u, np.cross(direction, u)


def light(points, normals, double_sided, intersector, args, rng):
    sun = np.array(args.sun, dtype=np.float64)
    sun /= np.linalg.norm(sun)
    n = normals.copy()
    facing = n @ sun
    flip = double_sided & (facing < 0)
    n[flip] = -n[flip]
    cos_sun = np.maximum(n @ sun, 0.0)
    origin = points + n * args.ray_offset

    u, v = sun_basis(sun)
    radius = math.tan(math.radians(args.sun_disc_deg))
    lit = np.zeros(len(points))
    for _ in range(args.sun_rays):
        r, a = math.sqrt(rng.random()) * radius, rng.random() * 2 * math.pi
        d = sun + u * (r * math.cos(a)) + v * (r * math.sin(a))
        d /= np.linalg.norm(d)
        lit += ~intersector.intersects_any(origin, np.tile(d, (len(points), 1)))
    sun_visible = lit / args.sun_rays

    tu = np.where(np.abs(n[:, 2:3]) < 0.9, [[0.0, 0.0, 1.0]], [[1.0, 0.0, 0.0]])
    tu = np.cross(n, tu)
    tu /= np.linalg.norm(tu, axis=1, keepdims=True)
    tv = np.cross(n, tu)
    escaped = np.zeros(len(points))
    for _ in range(args.sky_rays):
        r1, r2 = rng.random(len(points)), rng.random(len(points))
        r, a = np.sqrt(r1)[:, None], (2 * math.pi * r2)[:, None]
        d = tu * (r * np.cos(a)) + tv * (r * np.sin(a)) + n * np.sqrt(1 - r1)[:, None]
        escaped += ~intersector.intersects_any(origin, d)
    sky_visible = escaped / args.sky_rays

    sun_color = np.array([1.0, 0.92, 0.78]) * args.sun_intensity
    sky_color = np.array([0.55, 0.68, 0.9]) * args.sky_intensity
    return (cos_sun * sun_visible)[:, None] * sun_color + sky_visible[:, None] * sky_color + args.ambient


def to_srgb8(linear, tonemap_white):
    mapped = linear / (1.0 + linear * tonemap_white)
    return np.clip(np.round(255.0 * np.clip(mapped, 0, 1) ** (1 / 2.2)), 0, 255).astype(np.int64)


def face_colours(positions, tris, tri_mat, materials, double_materials, albedo_of, intersector, args, rng):
    """One sRGB colour per triangle, lit and textured at the triangle's centre
    on its face normal. albedo_of(centres, spacing, material) gives the albedo."""
    out = np.zeros((len(tris), 3), dtype=np.int64)
    for m in materials:
        selected = np.nonzero(tri_mat == m)[0]
        if not len(selected):
            continue
        faces = tris[selected]
        a, b, c = positions[faces[:, 0]], positions[faces[:, 1]], positions[faces[:, 2]]
        normals = np.cross(b - a, c - a)
        normals /= np.linalg.norm(normals, axis=1, keepdims=True)
        centres = (a + b + c) / 3.0
        albedo = albedo_of(centres, np.sqrt(triangle_areas(positions, faces)), m)
        radiance = light(centres, normals, np.full(len(faces), m in double_materials), intersector, args, rng)
        out[selected] = to_srgb8(albedo * radiance, args.tonemap_white)
    return out


def merge_matching_colours(pos, rgb, tris, step=6):
    """A crease splits a vertex so each side can be lit on its own normal;
    where both sides came out the same colour, one vertex is enough."""
    key = np.concatenate([np.round(pos * 16), rgb // step], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    tris = tris[(tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])]
    return pos[first], rgb[first], tris

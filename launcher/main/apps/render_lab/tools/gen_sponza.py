#!/usr/bin/env python3
"""Generate main/apps/render_lab/<name>_mesh_generated.h/.c - Crytek Sponza as a
coloured, lit triangle mesh small enough for the board.

    python main/apps/render_lab/tools/gen_sponza.py <sponza-dir> --out-dir main/apps/render_lab [--name NAME]

<sponza-dir> is the unpacked sponza.zip of McGuire's Computer Graphics
Archive (https://casual-effects.com/data/): sponza.obj, sponza.mtl and
textures/. Needs numpy, scipy, pillow, trimesh, embreex and
fast_simplification (pip install them into a virtualenv).

The device does no lighting. Everything a pixel's colour depends on is baked
here into one sRGB colour per vertex:

1. Each material is decimated on its own (quadric, borders preserved, so
   material seams stay closed) to a share of its triangles, then every edge longer than --max-edge is split, so the
   mesh ends up with roughly even triangle sizes. Even sizes are what let a
   per-vertex light carry shadows at all.
2. Albedo is sampled from the diffuse texture at the closest point of the
   original mesh, at a mip level matching the vertex spacing.
3. Light is direct only: a sun (a few jittered shadow rays over its disc)
   plus sky light (cosine-weighted rays that escape), both cast against the
   full-resolution original mesh.
4. Triangles are split into an octree whose leaves are clusters, each with
   its own vertex range and bounding box, so the device culls a subtree or
   a cluster whole.

The generator validates its own output before emitting anything.
"""

import argparse
import math
import os
import sys

import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

import fast_simplification
import trimesh
from trimesh.ray.ray_pyembree import RayMeshIntersector

POSITION_SCALE = 8  # int16 ticks per model unit
INT16_MAX = 32767
MAX_VERTICES = 65535  # uint16 indices
DOUBLE_SIDED = {"fabric_a", "fabric_c", "fabric_d", "fabric_e", "fabric_f", "fabric_g", "leaf", "chain", "Material__57"}
MASK_KEEP_ALPHA = 0.5
# (share of triangles kept by decimation relative to --keep, longest edge
# relative to --max-edge): the floor carries the sharpest shadows, the roof
# is seen only edge-on from inside.
MATERIAL_TUNING = {
    "floor": (1.0, 0.6),
    "roof": (0.5, 3.0),
    "arch": (3.0, 1.0),
    "ceiling": (3.0, 1.0),
    "column_a": (3.0, 1.0),
    "column_b": (1.5, 1.0),
    "column_c": (2.0, 1.0),
}
FABRIC_TUNING = (0.5, 1.0)
# A material this coarse is flat architecture: decimating it only loses shape.
DECIMATE_ABOVE = 1000
COLOUR_MERGE_STEP = 6  # sRGB levels; below what RGB565 shows
# Where a camera may stand: the atrium and its galleries, inside the outer
# walls. A triangle no point in here can see is dropped.
INTERIOR_LO = (-1400.0, 20.0, -620.0)
INTERIOR_HI = (1270.0, 1250.0, 550.0)


def log(*args):
    print(*args, file=sys.stderr, flush=True)


def load_mtl(path):
    materials = {}
    name = None
    with open(path, encoding="latin-1") as f:
        for line in f:
            parts = line.strip().split()
            if not parts:
                continue
            if parts[0] == "newmtl":
                name = parts[1]
                materials[name] = {"Kd": (1.0, 1.0, 1.0)}
            elif name and parts[0] == "Kd":
                materials[name]["Kd"] = tuple(float(v) for v in parts[1:4])
            elif name and parts[0] in ("map_Kd", "map_d"):
                materials[name][parts[0]] = parts[1].replace("\\", "/")
    return materials


def load_obj(path):
    positions, uvs = [], []
    tri_v, tri_t, tri_m = [], [], []
    material_names = []
    material_index = {}
    current = -1
    with open(path, encoding="latin-1") as f:
        for line in f:
            if line.startswith("v "):
                positions.append([float(v) for v in line.split()[1:4]])
            elif line.startswith("vt "):
                uvs.append([float(v) for v in line.split()[1:3]])
            elif line.startswith("usemtl "):
                name = line.split()[1]
                if name not in material_index:
                    material_index[name] = len(material_names)
                    material_names.append(name)
                current = material_index[name]
            elif line.startswith("f "):
                corners = []
                for c in line.split()[1:]:
                    fields = c.split("/")
                    corners.append((int(fields[0]) - 1, int(fields[1]) - 1 if len(fields) > 1 and fields[1] else 0))
                for i in range(1, len(corners) - 1):
                    tri = (corners[0], corners[i], corners[i + 1])
                    tri_v.append([c[0] for c in tri])
                    tri_t.append([c[1] for c in tri])
                    tri_m.append(current)
    return (
        np.array(positions, dtype=np.float64),
        np.array(uvs, dtype=np.float64),
        np.array(tri_v, dtype=np.int64),
        np.array(tri_t, dtype=np.int64),
        np.array(tri_m, dtype=np.int64),
        material_names,
    )


class Texture:
    """A linear-light mip chain, sampled bilinearly with wrap-around."""

    def __init__(self, path, alpha_path=None):
        image = Image.open(path).convert("RGBA")
        rgba = np.asarray(image, dtype=np.float64) / 255.0
        rgb = rgba[..., :3] ** 2.2
        alpha = rgba[..., 3:4]
        if alpha_path is not None:
            mask = Image.open(alpha_path).convert("L").resize(image.size)
            alpha = np.asarray(mask, dtype=np.float64)[..., None] / 255.0
        level = np.concatenate([rgb, alpha], axis=2)
        self.levels = [level]
        while min(level.shape[0], level.shape[1]) > 1:
            h, w = level.shape[0] // 2 * 2, level.shape[1] // 2 * 2
            level = level[:h, :w]
            level = 0.25 * (level[0::2, 0::2] + level[1::2, 0::2] + level[0::2, 1::2] + level[1::2, 1::2])
            self.levels.append(level)

    @property
    def size(self):
        return self.levels[0].shape[1], self.levels[0].shape[0]

    def sample(self, uv, lod):
        """uv (n,2), lod (n,) in mip levels; returns (n,4) linear RGBA."""
        out = np.zeros((len(uv), 4))
        lod = np.clip(np.round(lod).astype(np.int64), 0, len(self.levels) - 1)
        for level_index in np.unique(lod):
            sel = lod == level_index
            level = self.levels[level_index]
            h, w = level.shape[:2]
            x = uv[sel, 0] * w - 0.5
            y = (1.0 - uv[sel, 1]) * h - 0.5
            x0 = np.floor(x).astype(np.int64)
            y0 = np.floor(y).astype(np.int64)
            fx = (x - x0)[:, None]
            fy = (y - y0)[:, None]
            x0m, x1m = x0 % w, (x0 + 1) % w
            y0m, y1m = y0 % h, (y0 + 1) % h
            top = level[y0m, x0m] * (1 - fx) + level[y0m, x1m] * fx
            bottom = level[y1m, x0m] * (1 - fx) + level[y1m, x1m] * fx
            out[sel] = top * (1 - fy) + bottom * fy
        return out


def load_textures(root, materials, names):
    textures = []
    for name in names:
        m = materials.get(name, {})
        if "map_Kd" in m:
            alpha = os.path.join(root, m["map_d"]) if "map_d" in m else None
            textures.append(Texture(os.path.join(root, m["map_Kd"]), alpha))
        else:
            textures.append(None)
    return textures


def triangle_areas(p, tris):
    return 0.5 * np.linalg.norm(np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]]), axis=1)


def drop_masked(p, uv, tri_v, tri_t, tri_m, textures):
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
        keep[sel] = alpha / len(bary) >= MASK_KEEP_ALPHA
    keep &= triangle_areas(p, tri_v) > 1e-6
    log(f"masked/degenerate triangles dropped: {np.count_nonzero(~keep)}")
    return tri_v[keep], tri_t[keep], tri_m[keep]


def visible_from_interior(p, tri_v, tri_m, names, intersector, rounds, rng):
    """A triangle is kept if, in any of `rounds` tries, a random point on it
    sees a random interior point from its front side."""
    double = np.array([names[m] in DOUBLE_SIDED for m in tri_m])
    a, b, c = p[tri_v[:, 0]], p[tri_v[:, 1]], p[tri_v[:, 2]]
    normal = np.cross(b - a, c - a)
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
    lo, hi = np.array(INTERIOR_LO), np.array(INTERIOR_HI)
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
    log(f"visible from the interior: {np.count_nonzero(seen)} of {len(tri_v)} triangles")
    return seen


def compact(p, tris):
    used, inverse = np.unique(tris, return_inverse=True)
    return p[used], inverse.reshape(-1, 3)


def weld(p, tris, tolerance=1e-3):
    key = np.round(p / tolerance).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    good = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
    return p[first], tris[good]


def snap_to_grid(p, tris, cell):
    key = np.floor(p / cell).astype(np.int64)
    _, inverse = np.unique(key, axis=0, return_inverse=True)
    inverse = inverse.reshape(-1)
    count = np.bincount(inverse)
    snapped = np.zeros((len(count), 3))
    np.add.at(snapped, inverse, p)
    snapped /= count[:, None]
    t = inverse[tris]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    rolled = np.where((t[:, 1:2] < t[:, 0:1]) & (t[:, 1:2] < t[:, 2:3]), np.roll(t, -1, axis=1), t)
    rolled = np.where((rolled[:, 2:3] < rolled[:, 0:1]) & (rolled[:, 2:3] < rolled[:, 1:2]), np.roll(rolled, 1, axis=1), rolled)
    t = np.unique(rolled, axis=0)
    return compact(snapped, t)


def decimate(p, tris, target):
    """Quadric decimation, then - for a material made of many small
    disconnected pieces the quadric pass cannot merge - vertex clustering on
    the coarsest grid that still meets the target."""
    if len(tris) <= DECIMATE_ABOVE or target >= len(tris):
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


def split_long_edges(p, tris, max_edge):
    """Conforming subdivision: an edge is split iff it is longer than
    max_edge, decided per edge, so neighbours always agree."""
    p = list(map(tuple, p))
    tris = [tuple(t) for t in tris]
    while True:
        midpoint = {}

        def mid(a, b):
            key = (a, b) if a < b else (b, a)
            if key not in midpoint:
                pa, pb = p[a], p[b]
                if math.dist(pa, pb) <= max_edge:
                    midpoint[key] = -1
                else:
                    midpoint[key] = len(p)
                    p.append(tuple((x + y) / 2 for x, y in zip(pa, pb)))
            return midpoint[key]

        out = []
        changed = False
        for a, b, c in tris:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            splits = (ab >= 0) + (bc >= 0) + (ca >= 0)
            if splits == 0:
                out.append((a, b, c))
                continue
            changed = True
            if splits == 3:
                out += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
                continue
            # Rotate so the first split edge starts at a.
            while ab < 0:
                a, b, c = b, c, a
                ab, bc, ca = bc, ca, ab
            if splits == 1:
                out += [(a, ab, c), (ab, b, c)]
            elif bc >= 0:
                out += [(a, ab, c), (ab, b, bc), (ab, bc, c)]
            else:
                out += [(a, ab, ca), (ab, b, c), (ab, c, ca)]
        tris = out
        if not changed:
            return np.array(p), np.array(tris, dtype=np.int64)


def tuning(name):
    if name.startswith("fabric_"):
        return FABRIC_TUNING
    return MATERIAL_TUNING.get(name, (1.0, 1.0))


def split_marked_edges(p, tris, marked):
    """Conforming split of every edge in `marked` (a set of sorted vertex
    pairs): decided per edge, so neighbours always agree. Returns the new
    positions, triangles and the midpoint index of each split edge."""
    p = list(map(tuple, p))
    midpoint = {}
    for a, b in marked:
        midpoint[(a, b)] = len(p)
        p.append(tuple((x + y) / 2 for x, y in zip(p[a], p[b])))

    def mid(a, b):
        return midpoint.get((a, b) if a < b else (b, a), -1)

    out, parent = [], []
    for index, (a, b, c) in enumerate(tris):
        ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
        splits = (ab >= 0) + (bc >= 0) + (ca >= 0)
        if splits == 0:
            pieces = [(a, b, c)]
        elif splits == 3:
            pieces = [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
        else:
            while ab < 0:
                a, b, c = b, c, a
                ab, bc, ca = bc, ca, ab
            if splits == 1:
                pieces = [(a, ab, c), (ab, b, c)]
            elif bc >= 0:
                pieces = [(a, ab, c), (ab, b, bc), (ab, bc, c)]
            else:
                pieces = [(a, ab, ca), (ab, b, c), (ab, c, ca)]
        out += pieces
        parent += [index] * len(pieces)
    return np.array(p), np.array(out, dtype=np.int64), midpoint, np.array(parent, dtype=np.int64)


def vertex_normals(p, tris):
    face = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    n = np.zeros_like(p)
    for k in range(3):
        np.add.at(n, tris[:, k], face)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)


def adaptive_split(p, tris, attrs, brightness, min_edge, tolerance, rounds=12):
    """Splits an edge where the light changes along it - where its midpoint's
    brightness is off the mean of its ends by more than `tolerance` - or where
    it is longer than the smallest limit of the triangles sharing it. Uniformly
    lit areas keep their big triangles; shadow edges get the vertices they
    need. `attrs` holds a row per triangle, its length limit first; each
    piece of a split triangle inherits its row."""
    known = {}
    for _ in range(rounds):
        all_edges = np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1)
        edges, owner = np.unique(all_edges, axis=0, return_inverse=True)
        edge_limit = np.full(len(edges), np.inf)
        np.minimum.at(edge_limit, owner.reshape(-1), np.tile(attrs[:, 0], 3))
        length = np.linalg.norm(p[edges[:, 0]] - p[edges[:, 1]], axis=1)
        normals = vertex_normals(p, tris)
        need = [v for v in np.unique(edges) if v not in known]
        if need:
            values = brightness(p[need], normals[need])
            known.update(zip(need, values))
        long_enough = length > min_edge
        cand = edges[long_enough]
        if len(cand) == 0:
            break
        mid_n = normals[cand[:, 0]] + normals[cand[:, 1]]
        mid_n /= np.maximum(np.linalg.norm(mid_n, axis=1, keepdims=True), 1e-12)
        mid_b = brightness((p[cand[:, 0]] + p[cand[:, 1]]) / 2, mid_n)
        ends = np.array([(known[a] + known[b]) / 2 for a, b in cand])
        marked = (np.abs(mid_b - ends) > tolerance) | (length[long_enough] > edge_limit[long_enough])
        if not np.any(marked):
            break
        chosen = cand[marked]
        p, tris, midpoint, parent = split_marked_edges(p, tris, set(map(tuple, chosen)))
        attrs = attrs[parent]
        for (a, b), value in zip(map(tuple, cand), mid_b):
            if (a, b) in midpoint:
                known[midpoint[(a, b)]] = value
    return p, tris, attrs


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


def build_display_mesh(p, tri_v, tri_m, names, keep, max_edge, brightness, args):
    """Decimates each material on its own, then splits the light across the
    whole welded mesh at once: an edge two materials share is split for both
    or for neither, so no T-junction opens a crack between them."""
    positions, tris, labels, limits = [], [], [], []
    base = 0
    for m, name in enumerate(names):
        sel = tri_m == m
        if not np.any(sel):
            continue
        mp, mt = compact(p, tri_v[sel])
        mp, mt = weld(mp, mt)
        target = max(8, int(len(mt) * keep * tuning(name)[0]))
        mp, mt = decimate(mp, mt, target)
        log(f"  {name:14s} {np.count_nonzero(sel):6d} -> {len(mt):5d} decimated")
        positions.append(mp)
        tris.append(mt + base)
        labels.append(np.full(len(mt), m))
        limits.append(np.full(len(mt), max_edge * tuning(name)[1]))
        base += len(mp)

    whole_p = np.concatenate(positions)
    whole_t = np.concatenate(tris)
    attrs = np.column_stack([np.concatenate(limits), np.concatenate(labels)])
    whole_p, whole_t = weld_keeping(whole_p, whole_t)
    before = len(whole_t)
    whole_p, whole_t, attrs = adaptive_split(whole_p, whole_t, attrs, brightness, args.min_edge, args.light_tolerance)
    log(f"  light split: {before} -> {len(whole_t)} triangles")
    parts = []
    for m, name in enumerate(names):
        sel = attrs[:, 1] == m
        if np.any(sel):
            mp, mt = compact(whole_p, whole_t[sel])
            parts.append((m, mp, mt))
    return parts


def weld_keeping(p, tris, tolerance=1e-3):
    """Merges coincident vertices across materials; every triangle stays."""
    key = np.round(p / tolerance).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    return p[first], inverse.reshape(-1)[tris]


def corner_normals(p, tris, crease_deg=40.0):
    """One normal per triangle corner, smoothed only across neighbours within
    the crease angle, so a column's edge stays sharp."""
    face_n = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    face_area = np.linalg.norm(face_n, axis=1, keepdims=True)
    face_unit = face_n / np.maximum(face_area, 1e-12)
    cos_crease = math.cos(math.radians(crease_deg))
    corner_face = np.repeat(np.arange(len(tris)), 3)
    corner_vertex = tris.reshape(-1)
    order = np.argsort(corner_vertex, kind="stable")
    bounds = np.searchsorted(corner_vertex[order], np.arange(len(p) + 1))
    normals = np.zeros((len(corner_vertex), 3))
    for v in range(len(p)):
        corners = order[bounds[v] : bounds[v + 1]]
        faces = corner_face[corners]
        units = face_unit[faces]
        weighted = face_n[faces]
        similar = units @ units.T >= cos_crease
        n = similar.astype(np.float64) @ weighted
        normals[corners] = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    return normals.reshape(-1, 3, 3)


def closest_point_on_triangles(q, a, b, c):
    """Ericson's closest-point test, vectorised; returns barycentrics."""
    ab, ac, ap = b - a, c - a, q - a
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = q - b
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = q - c
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    denom = np.where(np.abs(va + vb + vc) < 1e-30, 1e-30, va + vb + vc)
    v = vb / denom
    w = vc / denom
    bary = np.stack([1 - v - w, v, w], axis=1)

    def edge(t, i, j):
        t = np.clip(t, 0, 1)
        r = np.zeros((len(t), 3))
        r[:, i] = 1 - t
        r[:, j] = t
        return r

    with np.errstate(divide="ignore", invalid="ignore"):
        cases = [
            ((d1 <= 0) & (d2 <= 0), edge(np.zeros_like(d1), 0, 1)),
            ((d3 >= 0) & (d4 <= d3), edge(np.ones_like(d1), 0, 1)),
            ((d6 >= 0) & (d5 <= d6), edge(np.ones_like(d1), 0, 2)),
            ((vc <= 0) & (d1 >= 0) & (d3 <= 0), edge(d1 / (d1 - d3), 0, 1)),
            ((vb <= 0) & (d2 >= 0) & (d6 <= 0), edge(d2 / (d2 - d6), 0, 2)),
            ((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), edge((d4 - d3) / ((d4 - d3) + (d5 - d6)), 1, 2)),
        ]
    done = np.zeros(len(q), dtype=bool)
    for cond, value in cases:
        cond = cond & ~done
        bary[cond] = value[cond]
        done |= cond
    point = bary[:, 0:1] * a + bary[:, 1:2] * b + bary[:, 2:3] * c
    return bary, np.linalg.norm(point - q, axis=1)


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


def merge_matching_colours(pos, rgb, tris):
    """A crease splits a vertex so each side can be lit on its own normal;
    where both sides came out the same colour, one vertex is enough."""
    key = np.concatenate([np.round(pos * 16), rgb // COLOUR_MERGE_STEP], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    tris = tris[(tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])]
    return pos[first], rgb[first], tris


def build_octree(positions, tris, leaf_triangles, max_depth):
    """Splits the triangles by centroid until a node holds leaf_triangles or
    fewer. Only axes at least half as long as the node's longest are split,
    so a long thin node halves along its length first."""
    centroid = positions[tris].mean(axis=1)

    def build(members, lo, hi, depth):
        if len(members) <= leaf_triangles or depth >= max_depth:
            return {"leaf": members}
        extent = hi - lo
        axes = [a for a in range(3) if extent[a] >= 0.5 * extent.max()]
        mid = (lo + hi) / 2
        code = np.zeros(len(members), dtype=np.int64)
        for bit, a in enumerate(axes):
            code |= (centroid[members, a] >= mid[a]).astype(np.int64) << bit
        children = []
        for c in np.unique(code):
            clo, chi = lo.copy(), hi.copy()
            for bit, a in enumerate(axes):
                if (c >> bit) & 1:
                    clo[a] = mid[a]
                else:
                    chi[a] = mid[a]
            children.append(build(members[code == c], clo, chi, depth + 1))
        if len(children) == 1:
            return children[0]
        return {"children": children}

    return build(np.arange(len(tris)), positions.min(axis=0), positions.max(axis=0), 0)


def flatten_octree(root, tri_double):
    """Nodes breadth first, so each node's children sit together; leaves'
    clusters depth first, so a subtree's clusters sit together too. A leaf
    holding both single- and double-sided triangles owns two clusters."""
    clusters = []

    def collect(node):
        if "leaf" in node:
            node["first_cluster"] = len(clusters)
            for double in (False, True):
                members = node["leaf"][tri_double[node["leaf"]] == int(double)]
                if len(members):
                    clusters.append((double, members))
            node["cluster_count"] = len(clusters) - node["first_cluster"]
        else:
            for child in node["children"]:
                collect(child)

    collect(root)
    queue = [root]
    nodes = []
    i = 0
    while i < len(queue):
        node = queue[i]
        if "leaf" in node:
            nodes.append({"leaf": True, "first": node["first_cluster"], "count": node["cluster_count"]})
        else:
            nodes.append({"leaf": False, "first": len(queue), "count": len(node["children"])})
            queue.extend(node["children"])
        i += 1
    return clusters, nodes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sponza_dir")
    parser.add_argument("--keep", type=float, default=0.04, help="share of each material's triangles decimation keeps")
    parser.add_argument("--max-edge", type=float, default=900.0, help="longest edge kept, model units")
    parser.add_argument("--min-edge", type=float, default=60.0, help="shortest edge the light may split")
    parser.add_argument("--light-tolerance", type=float, default=0.3,
                        help="sun exposure (0..1) an edge midpoint may miss by before splitting")
    parser.add_argument("--sun", type=float, nargs=3, default=[-0.25, 1.0, 0.22], help="direction towards the sun")
    parser.add_argument("--sun-disc-deg", type=float, default=1.2)
    parser.add_argument("--sun-rays", type=int, default=8)
    parser.add_argument("--sky-rays", type=int, default=48)
    parser.add_argument("--sun-intensity", type=float, default=3.0)
    parser.add_argument("--sky-intensity", type=float, default=0.9)
    parser.add_argument("--ambient", type=float, default=0.06)
    parser.add_argument("--tonemap-white", type=float, default=0.35)
    parser.add_argument("--ray-offset", type=float, default=0.5)
    parser.add_argument("--leaf-triangles", type=int, default=160, help="most triangles an octree leaf holds")
    parser.add_argument("--max-depth", type=int, default=10)
    parser.add_argument("--out-dir", required=True, help="where <name>_mesh_generated.h and .c are written")
    parser.add_argument("--name", default="sponza", help="prefix of the files and of every symbol they define")
    parser.add_argument("--visibility-rounds", type=int, default=160)
    parser.add_argument("--leaf-keep", type=float, default=0.35, help="fraction of leaf triangles kept")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    rng = np.random.default_rng(args.seed)

    root = args.sponza_dir
    materials = load_mtl(os.path.join(root, "sponza.mtl"))
    p, uv, tri_v, tri_t, tri_m, names = load_obj(os.path.join(root, "sponza.obj"))
    log(f"loaded {len(p)} vertices, {len(tri_v)} triangles, {len(names)} materials")
    textures = load_textures(root, materials, names)
    tri_v, tri_t, tri_m = drop_masked(p, uv, tri_v, tri_t, tri_m, textures)
    intersector = RayMeshIntersector(trimesh.Trimesh(p, tri_v, process=False))
    seen = visible_from_interior(p, tri_v, tri_m, names, intersector, args.visibility_rounds, rng)
    leaf = np.isin(tri_m, [i for i, n in enumerate(names) if n == "leaf"])
    seen &= ~leaf | (rng.random(len(tri_v)) < args.leaf_keep)
    shown_v, shown_m = tri_v[seen], tri_m[seen]

    log("decimating")
    def brightness(points, normals):
        return sun_exposure(points, normals, intersector, args)

    parts = build_display_mesh(p, shown_v, shown_m, names, args.keep, args.max_edge, brightness, args)

    all_pos, all_rgb, all_tris, all_double = [], [], [], []
    base = 0
    for m, mp, mt in parts:
        double = names[m] in DOUBLE_SIDED
        normals = corner_normals(mp, mt)
        corner_pos = mp[mt].reshape(-1, 3)
        corner_n = normals.reshape(-1, 3)
        # Weld corners that share a position and a normal: one colour each.
        key = np.concatenate([np.round(corner_pos * 16), np.round(corner_n * 64)], axis=1).astype(np.int64)
        _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
        vpos, vn = corner_pos[first], corner_n[first]
        vtris = inverse.reshape(-1, 3)
        edge_len = np.linalg.norm(vpos[vtris] - vpos[np.roll(vtris, 1, axis=1)], axis=2)
        spacing = np.zeros(len(vpos))
        count = np.zeros(len(vpos))
        np.add.at(spacing, vtris.reshape(-1), edge_len.reshape(-1))
        np.add.at(count, vtris.reshape(-1), 1)
        spacing /= np.maximum(count, 1)
        kd = materials.get(names[m], {}).get("Kd", (1.0, 1.0, 1.0))
        albedo = sample_albedo(vpos, spacing, m, p, uv, tri_v, tri_t, tri_m, textures, kd)
        radiance = light(vpos, vn, np.full(len(vpos), double), intersector, args, rng)
        vrgb = to_srgb8(albedo * radiance, args.tonemap_white)
        vpos, vrgb, vtris = merge_matching_colours(vpos, vrgb, vtris)
        all_pos.append(vpos)
        all_rgb.append(vrgb)
        all_tris.append(vtris + base)
        all_double.append(np.full(len(vtris), int(double)))
        base += len(vpos)
        log(f"  lit {names[m]}: {len(vpos)} vertices")

    positions = np.concatenate(all_pos)
    rgb = np.concatenate(all_rgb)
    tris = np.concatenate(all_tris)
    tri_double = np.concatenate(all_double)

    root = build_octree(positions, tris, args.leaf_triangles, args.max_depth)
    clusters, nodes = flatten_octree(root, tri_double)
    out_pos, out_rgb, out_tris, out_clusters = [], [], [], []
    vbase = 0
    for double, members in clusters:
        ct = tris[members]
        used, local = np.unique(ct, return_inverse=True)
        out_pos.append(positions[used])
        out_rgb.append(rgb[used])
        out_tris.append(local.reshape(-1, 3) + vbase)
        q = np.round(positions[used] * POSITION_SCALE).astype(np.int64)
        tbase = sum(len(t) for t in out_tris[:-1])
        out_clusters.append((vbase, len(used), tbase, len(ct), q.min(axis=0), q.max(axis=0), double))
        vbase += len(used)

    q_pos = np.round(np.concatenate(out_pos) * POSITION_SCALE).astype(np.int64)
    q_rgb = np.concatenate(out_rgb)
    q_tris = np.concatenate(out_tris)
    node_bounds(nodes, out_clusters)
    validate(q_pos, q_rgb, q_tris, out_clusters, nodes)
    emit(args, q_pos, q_rgb, q_tris, out_clusters, nodes)
    log(f"emitted {len(q_pos)} vertices, {len(q_tris)} triangles, {len(out_clusters)} clusters, {len(nodes)} nodes")


def node_bounds(nodes, clusters):
    """Tight bounds of what a node holds; children always follow their parent."""
    for node in reversed(nodes):
        if node["leaf"]:
            parts = clusters[node["first"] : node["first"] + node["count"]]
            node["lo"] = np.min([c[4] for c in parts], axis=0)
            node["hi"] = np.max([c[5] for c in parts], axis=0)
        else:
            parts = nodes[node["first"] : node["first"] + node["count"]]
            node["lo"] = np.min([n["lo"] for n in parts], axis=0)
            node["hi"] = np.max([n["hi"] for n in parts], axis=0)


def validate(pos, rgb, tris, clusters, nodes):
    assert len(pos) <= MAX_VERTICES, f"{len(pos)} vertices exceed uint16 indices"
    assert np.abs(pos).max() <= INT16_MAX, "a position does not fit int16"
    assert rgb.min() >= 0 and rgb.max() <= 255
    assert tris.min() >= 0 and tris.max() < len(pos)
    assert np.all((tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2]))
    next_v = next_t = 0
    for vbase, vcount, tbase, tcount, lo, hi, _ in clusters:
        assert vbase == next_v and tbase == next_t, "clusters must tile both arrays in order"
        ct = tris[tbase : tbase + tcount]
        assert ct.min() >= vbase and ct.max() < vbase + vcount, "a triangle reaches outside its cluster"
        cp = pos[vbase : vbase + vcount]
        assert np.all(cp >= lo) and np.all(cp <= hi)
        next_v, next_t = vbase + vcount, tbase + tcount
    assert next_v == len(pos) and next_t == len(tris)
    reached = np.zeros(len(clusters), dtype=np.int64)
    stack = [0]
    while stack:
        node = nodes[stack.pop()]
        assert node["count"] <= 255
        if node["leaf"]:
            reached[node["first"] : node["first"] + node["count"]] += 1
        else:
            stack.extend(range(node["first"], node["first"] + node["count"]))
    assert np.all(reached == 1), "every cluster must hang off exactly one leaf"
    assert len(nodes) <= 65535


def emit_rows(out, name, ctype, rows, per_line):
    print(f"static const {ctype} {name}[][{len(rows[0])}] = {{", file=out)
    for i in range(0, len(rows), per_line):
        chunk = rows[i : i + per_line]
        print("    " + " ".join("{" + ",".join(str(int(v)) for v in r) + "}," for r in chunk), file=out)
    print("};", file=out)


def banner(args, out):
    lines = [
        "GENERATED FILE - do not edit.",
        "",
        "    python main/apps/render_lab/tools/gen_sponza.py <sponza-dir> --out-dir main/apps/render_lab \\",
        f"        --name {args.name} --keep {args.keep:g} --light-tolerance {args.light_tolerance:g} --min-edge {args.min_edge:g}",
        "",
        "Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in",
        "McGuire's Computer Graphics Archive, casual-effects.com/data.",
        "Decimated, lit by a sun and sky with baked shadows, one sRGB colour",
        "per vertex, clusters as the leaves of an octree. Other settings:",
        f"  --max-edge {args.max_edge:g} --sun {args.sun[0]:g} {args.sun[1]:g} {args.sun[2]:g}",
        f"  --sun-rays {args.sun_rays} --sky-rays {args.sky_rays} --leaf-triangles {args.leaf_triangles}",
    ]
    print("/*", file=out)
    for line in lines:
        print((" * " + line).rstrip(), file=out)
    print(" */", file=out)


def triple(v):
    return "{" + ", ".join(str(int(x)) for x in v) + "}"


def c_bool(v):
    return "true" if v else "false"


def emit(args, pos, rgb, tris, clusters, nodes):
    low, up = args.name, args.name.upper()
    header = f"{low}_mesh_generated.h"
    with open(os.path.join(args.out_dir, header), "w", newline="\n") as out:
        banner(args, out)
        print("#pragma once", file=out)
        print(file=out)
        print('#include "lit_mesh.h"', file=out)
        print(file=out)
        print(f"#define {up}_VERTEX_COUNT {len(pos)}", file=out)
        print(f"#define {up}_TRIANGLE_COUNT {len(tris)}", file=out)
        print(f"#define {up}_CLUSTER_COUNT {len(clusters)}", file=out)
        print(f"#define {up}_NODE_COUNT {len(nodes)}", file=out)
        print(f"#define {up}_POSITION_SCALE {POSITION_SCALE}", file=out)
        print(file=out)
        print(f"extern const lit_mesh_t {low}_mesh;", file=out)

    with open(os.path.join(args.out_dir, f"{low}_mesh_generated.c"), "w", newline="\n") as out:
        banner(args, out)
        print(f'#include "{header}"', file=out)
        print(file=out)
        emit_rows(out, f"{low}_positions", "int16_t", pos.tolist(), 8)
        print(file=out)
        emit_rows(out, f"{low}_colors", "uint8_t", rgb.tolist(), 10)
        print(file=out)
        emit_rows(out, f"{low}_triangles", "uint16_t", tris.tolist(), 8)
        print(file=out)
        print(f"static const lit_cluster_t {low}_clusters[] = {{", file=out)
        for vbase, vcount, tbase, tcount, lo, hi, double in clusters:
            print(f"    {{{vbase}, {vcount}, {tbase}, {tcount}, {triple(lo)}, {triple(hi)}, {c_bool(double)}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"static const lit_node_t {low}_nodes[] = {{", file=out)
        for n in nodes:
            print(f"    {{{triple(n['lo'])}, {triple(n['hi'])}, {n['first']}, {n['count']}, {c_bool(n['leaf'])}}},", file=out)
        print("};", file=out)
        print(file=out)
        print(f"const lit_mesh_t {low}_mesh = {{", file=out)
        print(f"    {low}_positions, {low}_colors, {low}_triangles, {low}_clusters, {low}_nodes,", file=out)
        print(f"    {up}_VERTEX_COUNT, {up}_TRIANGLE_COUNT, {up}_CLUSTER_COUNT, {up}_NODE_COUNT,", file=out)
        print(f"    {up}_POSITION_SCALE,", file=out)
        print("};", file=out)


if __name__ == "__main__":
    main()

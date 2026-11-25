"""Baked light: the sun and sky cast against the full-detail mesh by shadow rays, the light that bounces (PathLight), the scene ambient, and albedo sampled from textures and region visibility culling."""

import math
from types import SimpleNamespace
from pathlib import Path
from build.c_header import read_define

LIGHT_OWNER = Path(__file__).resolve().parents[2] / "main/render/skin/r3d_skin_light.h"
LIGHT_GAMMA = read_define(LIGHT_OWNER,
                          "R3D_SKIN_LIGHT_GAMMA")

import numpy as np

from . import log
from .geometry import distinct_corners, hit_barycentrics, nearest_triangles, triangle_areas, triangle_search
from .import_settings import LIGHT_FIELDS, LIGHT_MODE_BAKED
from .obj import alpha_tested
from .ray_query import VARIANT, is_jit, rows, vectors


# Where a triangle's alpha is sampled: its centre and three points towards its corners, as barycentric weights.
ALPHA_POINTS = np.array([[1 / 3, 1 / 3, 1 / 3], [0.6, 0.2, 0.2], [0.2, 0.6, 0.2], [0.2, 0.2, 0.6]])
# Mip level a triangle's alpha is sampled at, so one texel's hole does not decide a whole triangle.
TRIANGLE_ALPHA_LOD = 2.0


def triangle_alpha(uv, tri_t, texture):
    """Each triangle's mean texture alpha over ALPHA_POINTS."""
    alpha = np.zeros(len(tri_t))
    for b in ALPHA_POINTS:
        tuv = (uv[tri_t] * b[None, :, None]).sum(axis=1)
        alpha += texture.sample(tuv, np.full(len(tri_t), TRIANGLE_ALPHA_LOD))[:, 3]
    return alpha / len(ALPHA_POINTS)


def drop_masked(p, uv, tri_v, tri_t, tri_m, textures, keep_alpha=0.5):
    """Alpha-tested cards (leaves, chains) keep only triangles mostly opaque."""
    keep = np.ones(len(tri_v), dtype=bool)
    tested = alpha_tested(textures)
    for m, tex in enumerate(textures):
        sel = np.nonzero(tri_m == m)[0]
        if not tested[m] or len(sel) == 0:
            continue
        keep[sel] = triangle_alpha(uv, tri_t[sel], tex) >= keep_alpha
    keep &= triangle_areas(p, tri_v) > 1e-6
    log(f"masked/degenerate triangles dropped: {np.count_nonzero(~keep)}")
    result = (tri_v[keep], tri_t[keep], tri_m[keep])
    return (*result, keep)


class SurfaceTable:
    """A source's surfaces on the device, for hits a RayQuery over its triangles finds: each triangle's material, UV
    corners, corner normals (`source.corner_normals`) and twin (`source.twin`) where the source has them, and each
    textured material's level-zero linear RGBA as a Dr.Jit texture, sampled bilinearly with wrap-around like
    obj.Texture.sample at level 0 (in fp32, not the hardware's fixed-point filter). Built once per source and
    variant (`surface_table`); a texture uploads on first use."""

    def __init__(self, source, mi):
        import drjit as dr

        self.source, self.mi, self.dr, self.variant = source, mi, dr, mi.variant()
        self.inputs = table_inputs(source)
        tri_t = np.asarray(source.tri_t)
        uv = np.asarray(getattr(source, "uv", ()), np.float32).reshape(-1, 2)
        self.uv = None if not len(uv) else [mi.Point2f(mi.Float(uv[tri_t[:, corner], 0]),
                                                       mi.Float(uv[tri_t[:, corner], 1])) for corner in range(3)]
        self.tri_m = mi.UInt32(np.asarray(source.tri_m, np.uint32))
        self.textures, self.uploads = {}, {}
        normals = getattr(source, "corner_normals", None)
        self.normals = None if normals is None else [vectors(mi.Vector3f, normals[:, corner]) for corner in range(3)]
        twin = getattr(source, "twin", None)
        self.twin = None if twin is None else mi.Int32(np.asarray(twin, np.int32))
        materials = getattr(source, "materials", {})
        # Materials are numbered by texture; a candidate's card pages are textures with no material name.
        names = list(getattr(source, "names", []))[:len(source.textures)]
        names += [None] * (len(source.textures) - len(names))
        kd = np.array([materials.get(name, {}).get("Kd", (1.0, 1.0, 1.0)) for name in names] or [(1.0, 1.0, 1.0)],
                      dtype=np.float64)
        flat = np.array([texture is None for texture in source.textures] or [True])
        # albedo_from_uv: the texture times Kd, or Kd decoded by LIGHT_GAMMA where untextured.
        kd = np.where(flat[:, None], kd ** LIGHT_GAMMA, kd).astype(np.float32)
        self.kd = vectors(mi.Color3f, kd)
        colors = getattr(source, "colors", None)
        self.colors = None if colors is None else vectors(mi.Color3f, colors)
        self.tri_v = None if colors is None else mi.UInt32(np.asarray(source.tri_v, np.uint32).ravel())

    def texture(self, material):
        if material not in self.textures:
            level = np.asarray(self.source.textures[material].levels[0], np.float32)
            self.textures[material] = self.mi.Texture2f(self.mi.TensorXf(level), filter_mode=self.dr.FilterMode.Linear,
                                                        wrap_mode=self.dr.WrapMode.Repeat, use_accel=False)
        return self.textures[material]

    def upload(self, name, values, kind=None):
        """A host array on the device (a Bool mask unless `kind` names another type), kept under `name`."""
        if name not in self.uploads:
            self.uploads[name] = (kind or self.mi.Bool)(np.asarray(values, np.uint32 if kind else bool))
        return self.uploads[name]

    def interpolate(self, corners, prim, bary, active):
        dr = self.dr
        return sum((dr.gather(type(corner), corner, prim, active) * weight for corner, weight in zip(corners, bary)),
                   type(corners[0])(0))

    def rgba(self, prim, bary, active, materials=None):
        """Level-zero linear RGBA at hits on triangles `prim` with barycentric weights `bary`, for the textured
        materials `materials` selects (every one by default); white elsewhere. One dispatch over the materials."""
        dr, mi = self.dr, self.mi
        chosen = [index for index, texture in enumerate(self.source.textures)
                  if texture is not None and (materials is None or materials[index])]
        if not chosen or self.uv is None:
            return mi.Vector4f(1.0)
        slot = np.zeros(len(self.source.textures), np.uint32)
        slot[chosen] = np.arange(1, len(chosen) + 1)
        slots = self.upload(("slots", tuple(chosen)), slot, mi.UInt32)
        uv = self.interpolate(self.uv, prim, bary, active)
        where = mi.Point2f(uv.x, 1.0 - uv.y)

        def sampler(texture):
            return lambda position, active: mi.Vector4f(texture.eval(position, active))

        callables = [lambda position, active: mi.Vector4f(1.0)] + [sampler(self.texture(index)) for index in chosen]
        slot = dr.gather(mi.UInt32, slots, dr.gather(mi.UInt32, self.tri_m, prim, active), active)
        return dr.switch(slot, callables, where, active)

    def albedo(self, prim, bary, active):
        """Linear albedo at hits: albedo_from_uv at level 0, times the source's interpolated vertex colours."""
        dr, mi = self.dr, self.mi
        rgba = self.rgba(prim, bary, active)
        material = dr.gather(mi.UInt32, self.tri_m, prim, active)
        albedo = mi.Color3f(rgba.x, rgba.y, rgba.z) * dr.gather(type(self.kd), self.kd, material, active)
        if self.colors is not None:
            albedo = albedo * sum((dr.gather(type(self.colors), self.colors, dr.gather(mi.UInt32, self.tri_v, prim * 3 + corner, active),
                                     active) * weight for corner, weight in enumerate(bary)), mi.Color3f(0))
        return albedo

    def normal(self, prim, bary, active):
        """The crease-limited corner normals interpolated at hits, normalised."""
        normal = self.interpolate(self.normals, prim, bary, active)
        return normal / self.dr.maximum(self.dr.norm(normal), 1e-12)


TABLE_INPUTS = ("tri_v", "tri_t", "tri_m", "uv", "textures", "names", "materials", "corner_normals", "twin", "colors")


def table_inputs(source):
    """The source arrays a SurfaceTable reads (TABLE_INPUTS), as the objects themselves."""
    return tuple(getattr(source, name, None) for name in TABLE_INPUTS)


def surface_table(source, mi):
    """The source's SurfaceTable on Mitsuba's current variant, built once and built again when the variant changes
    or any array it reads is replaced. A copy of the source (with_triangles) inherits the attribute but builds its
    own table, which replaces it on the copy alone."""
    table = vars(source).get("_surface_table")
    if (table is None or table.source is not source or table.variant != mi.variant()
            or any(old is not new for old, new in zip(table.inputs, table_inputs(source)))):
        table = vars(source)["_surface_table"] = SurfaceTable(source, mi)
    return table


class AlphaTest:
    """A `RayQuery` hit filter: a hit is solid where the source's level-zero texture alpha at its interpolated UV is
    at least `keep_alpha` (not tested when None) and its triangle is not in the per-triangle `hidden` mask. The test
    runs on the device (`device`); `__call__` and `rgba` answer NumPy hits through the same SurfaceTable."""

    def __init__(self, source, keep_alpha, hidden=None):
        self.source, self.keep_alpha = source, keep_alpha
        self.tested = (alpha_tested(source.textures) if keep_alpha is not None
                       else np.zeros(len(source.textures), bool))
        self.hidden = None if hidden is None or not np.any(hidden) else np.asarray(hidden, bool)
        self._masks = {}

    def device(self, query):
        """solid(prim, where, active) for hits of `query`, a RayQuery over this source's triangles; None when every
        hit is solid."""
        if not self.tested.any() and self.hidden is None:
            return None
        dr, mi = query.dr, query.mi
        table = surface_table(self.source, mi) if self.tested.any() else None
        masks = self._masks
        if mi.variant() not in masks:
            masks[mi.variant()] = (None if self.hidden is None else mi.Bool(self.hidden), mi.Bool(self.tested))
        hidden, tested = masks[mi.variant()]

        def solid(prim, where, active):
            keep = mi.Bool(active)
            if self.tested.any():
                cut = active & dr.gather(mi.Bool, tested, dr.gather(mi.UInt32, table.tri_m, prim, active), active)
                alpha = table.rgba(prim, query.barycentrics(prim, where, cut), cut, self.tested).w
                keep = keep & (~cut | (alpha >= self.keep_alpha))
            if hidden is not None:
                keep = keep & ~dr.gather(mi.Bool, hidden, prim, active)
            return keep

        return solid

    def rgba(self, hits, locations):
        """Level-zero linear RGBA at NumPy source hits; untextured materials return white."""
        from r3d.mitsuba_reference import import_mitsuba

        if not len(hits):
            return np.ones((0, 4))
        mi = import_mitsuba()
        if not is_jit(mi.variant()):
            mi.set_variant(VARIANT)
        bary = hit_barycentrics(self.source.p, self.source.tri_v, hits, locations).astype(np.float32)
        rgba = surface_table(self.source, mi).rgba(mi.UInt32(np.asarray(hits, np.uint32)),
                                                   [mi.Float(bary[:, corner]) for corner in range(3)], mi.Bool(True))
        return rows(rgba, len(hits), 4)

    def __call__(self, hits, locations):
        hits = np.asarray(hits)
        solid = np.ones(len(hits), dtype=bool)
        tested = self.tested[self.source.tri_m[hits]]
        if tested.any():
            solid[tested] = self.rgba(hits[tested], np.asarray(locations)[tested])[:, 3] >= self.keep_alpha
        if self.hidden is not None:
            solid &= ~self.hidden[hits]
        return solid


def visible_from_region(p, tri_v, double, intersector, rounds, rng, lo, hi):
    """A triangle is kept if, in any of `rounds` tries, a random point on it
    sees a random point of the box lo..hi from its front side. A try tests
    one point plus one for each whole multiple of the mean triangle area, so a
    large face is kept by how much of it shows, not by where a single point
    lands; with the mean as the unit a round never casts more than twice as
    many rays as there are triangles."""
    a, b, c = p[tri_v[:, 0]], p[tri_v[:, 1]], p[tri_v[:, 2]]
    normal = np.cross(b - a, c - a)
    area = 0.5 * np.linalg.norm(normal, axis=1)
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
    points = 1 + (area / max(area.mean(), 1e-12)).astype(np.int64)
    lo, hi = np.array(lo), np.array(hi)
    packed = np.stack([a, b, c, normal], axis=1)
    seen = np.zeros(len(tri_v), dtype=bool)
    todo = np.repeat(np.arange(len(tri_v)), points)
    gathered = packed[todo].transpose(1, 0, 2).copy()
    gathered_double = double[todo]
    for _ in range(rounds):
        if len(todo) == 0:
            break
        gathered_a, gathered_b, gathered_c, gathered_normal = gathered
        r1, r2 = np.sqrt(rng.random(len(todo)))[:, None], rng.random(len(todo))[:, None]
        on = (1 - r1) * gathered_a
        on += r1 * (1 - r2) * gathered_b
        on += r1 * r2 * gathered_c
        eye = lo + rng.random((len(todo), 3)) * (hi - lo)
        d = eye - on
        dist = np.linalg.norm(d, axis=1)
        d /= dist[:, None]
        side = (d * gathered_normal).sum(axis=1)
        facing = (side > 0.01) | gathered_double
        origin = on + gathered_normal * np.where(side >= 0, 0.5, -0.5)[:, None]
        hit = np.zeros(len(todo), dtype=bool)
        if np.any(facing):
            f = np.nonzero(facing)[0]
            locations, index_ray, _ = intersector.first_hit(origin[f], d[f])
            hit_dist = np.full(len(f), np.inf)
            hit_dist[index_ray] = np.linalg.norm(locations - origin[f][index_ray], axis=1)
            hit[f] = hit_dist < dist[f] - 1.0
        visible = facing & ~hit
        seen[todo[visible]] = True
        if np.any(visible):
            keep = ~seen[todo]
            todo = todo[keep]
            gathered = gathered[:, keep].copy()
            gathered_double = gathered_double[keep]
    log(f"visible from the region: {np.count_nonzero(seen)} of {len(tri_v)} triangles")
    return seen


def coincident_faces(p, tri_v):
    """(twin, group) per triangle: twin is a face over the same three
    positions wound the other way, or -1, the two sides of a thin wall that a
    ray tracer meets as one; faces over the same positions wound the same way
    share a group, and one being seen means all are, since only the depth
    test picks between them."""
    corners = np.round(p[tri_v] * 1024).astype(np.int64)
    twin = np.full(len(tri_v), -1)
    group = np.arange(len(tri_v))
    first = {}
    for index, corner in enumerate(corners):
        rows = [tuple(row) for row in corner]
        order = sorted(range(3), key=lambda k: rows[k])
        winding = (order[0], order[1], order[2]) in ((0, 1, 2), (1, 2, 0), (2, 0, 1))
        key = tuple(sorted(rows))
        same = first.setdefault((key, winding), index)
        group[index] = same
        other = first.get((key, not winding), -1)
        if other >= 0:
            twin[index] = other
            if twin[other] < 0:
                twin[other] = index
    return twin, group


def visible_from_path(p, tri_v, double, intersector, poses, width, height, lens, near, samples, margin, tie=1e-2):
    """A triangle is kept if it is the first face, or within `tie` of it,
    that some ray of a pose's view would draw: front-facing or double-sided.
    Rays start at the `near` plane and pass a single-sided face seen from
    behind, as the rasterizer clips and culls; such a face's twin wound the
    other way is what the ray sees. `poses` are (eye, forward) rows; see
    camera_rays for the rest. Each ray stops after a hit beyond its first
    drawn distance plus `tie`; rays with no drawn hit continue to the query limit."""
    from r3d.poses import camera_rays

    a, b, c = p[tri_v[:, 0]], p[tri_v[:, 1]], p[tri_v[:, 2]]
    normal = np.cross(b - a, c - a)
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
    double = np.asarray(double, dtype=bool)
    twin, group = coincident_faces(p, tri_v)
    seen = np.zeros(len(tri_v), dtype=bool)
    for pose in poses:
        origin, direction = camera_rays(width, height, lens, pose[:3], pose[3:], samples, margin)
        ahead = pose[3:] / np.linalg.norm(pose[3:])
        origin = origin + direction * (near / (direction @ ahead))[:, None]
        nearest = np.full(len(origin), np.inf)
        active = np.ones(len(origin), dtype=bool)
        for hit, ray, where in intersector.hit_rounds(origin, direction, active):
            distance = ((where - origin[ray]) * direction[ray]).sum(axis=1)
            drawn = double[hit] | ((normal[hit] * direction[ray]).sum(axis=1) < 0)
            shown = np.where(drawn, hit, twin[hit])
            valid = shown >= 0
            nearest[ray[valid]] = np.minimum(nearest[ray[valid]], distance[valid])
            within = distance <= nearest[ray] + tie
            seen[shown[valid & within]] = True
            active[ray[~within]] = False
    reached = np.zeros(len(tri_v), dtype=bool)
    reached[group[seen]] = True
    seen = reached[group]
    log(f"visible from the camera path: {np.count_nonzero(seen)} of {len(tri_v)} triangles")
    return seen


def albedo_from_uv(texture, kd, uv, lod):
    """Linear albedo at texture coordinates: the material colour alone for an
    untextured material, else the texture sampled at `lod` times the colour."""
    if texture is None:
        return np.tile(np.array(kd) ** LIGHT_GAMMA, (len(uv), 1))
    return texture.sample(uv, lod)[:, :3] * np.array(kd)


def sample_albedo(points, spacing, m, p, uv, tri_v, tri_t, tri_m, textures, kd, colors=None):
    """Linear albedo of material m at points on its surface: its texture or colour, times the source's linear vertex
    colours (indexed like p) where it has them."""
    sel = np.nonzero(tri_m == m)[0]
    tex = textures[m]
    if tex is None and colors is None:
        return albedo_from_uv(None, kd, points, None)
    corners = p[tri_v[sel]]
    tris, bary, _ = nearest_triangles(points, triangle_search(corners))
    tris = sel[tris]
    if tex is None:
        albedo = albedo_from_uv(None, kd, points, None)
    else:
        tuv = (uv[tri_t[tris]] * bary[:, :, None]).sum(axis=1)
        world_area = triangle_areas(p, tri_v[tris])
        e1 = uv[tri_t[tris, 1]] - uv[tri_t[tris, 0]]
        e2 = uv[tri_t[tris, 2]] - uv[tri_t[tris, 0]]
        w, h = tex.size
        texel_area = 0.5 * np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]) * w * h
        texels_per_unit = np.sqrt(texel_area / np.maximum(world_area, 1e-9))
        lod = np.log2(np.maximum(spacing * texels_per_unit, 1.0))
        albedo = albedo_from_uv(tex, kd, tuv, lod)
    if colors is not None:
        albedo = albedo * (colors[tri_v[tris]] * bary[:, :, None]).sum(axis=1)
    return albedo


def summed_rays(mi, kind, count, rays, value):
    """For `count` points, the sum over each point's `rays` rays of value(point, ray, chunk), a `kind` (mi.Float or
    mi.Color3f) per ray. Points go through in chunks of at most ray_query.LAUNCH rays, a constant, numbered from 0;
    each chunk's sums are Dr.Jit block sums, whose order is fixed."""
    import drjit as dr

    from r3d.ray_query import LAUNCH, chunks

    if rays > LAUNCH:
        raise ValueError(f"{rays} rays per point exceed one launch of {LAUNCH}")
    out = dr.zeros(kind, count)
    for chunk, (start, size) in enumerate(chunks(count, LAUNCH // rays)):
        lane = dr.arange(mi.UInt32, size * rays)
        # Opaque, so every full chunk reuses one compiled kernel instead of baking its start in.
        got = kind(value(dr.opaque(mi.UInt32, start) + lane // rays, lane % rays, chunk))
        # Evaluated once here: each channel's block sum would otherwise re-run the whole ray kernel.
        dr.eval(got)
        total = (dr.block_sum(got, rays, mode="evaluated") if kind is mi.Float
                 else kind(*(dr.block_sum(got[channel], rays, mode="evaluated") for channel in range(3))))
        dr.scatter(out, total, dr.arange(mi.UInt32, start, start + size))
        dr.eval(out)
    return out


def tangent_frame(n):
    """Two unit tangents completing each unit normal, fixed by the normal alone: Dr.Jit Vector3f in and out, or
    NumPy (n, 3) rows on Mitsuba's current variant."""
    import drjit as dr

    if isinstance(n, np.ndarray):
        from r3d.mitsuba_reference import import_mitsuba

        mi = import_mitsuba()
        tu, tv = tangent_frame(vectors(mi.Vector3f, n))
        return rows(tu, len(n)), rows(tv, len(n))
    kind = type(n)
    tu = dr.normalize(dr.cross(n, dr.select(dr.abs(n.z) < 0.9, kind(0.0, 0.0, 1.0), kind(1.0, 0.0, 0.0))))
    return tu, dr.cross(n, tu)


def sky_directions(count):
    """One fixed set of cosine-weighted hemisphere directions (z up), shared by
    every point: a golden-ratio sequence, so any prefix is evenly spread."""
    i = np.arange(count)
    r1 = (0.5 + i * 0.7548776662466927) % 1.0
    a = 2 * math.pi * ((0.5 + i * 0.5698402909980532) % 1.0)
    r = np.sqrt(r1)
    return np.stack([r * np.cos(a), r * np.sin(a), np.sqrt(1 - r1)], axis=1)


def hemisphere_ray(mi, origins, normals, local):
    """ray(point, k): from origins[point], along the k-th of the `local` (z-up) directions turned into the frame of
    normals[point]."""
    import drjit as dr

    table = vectors(mi.Vector3f, local)

    def ray(point, k):
        n = dr.gather(type(normals), normals, point)
        tu, tv = tangent_frame(n)
        d = dr.gather(type(table), table, k)
        return mi.Ray3f(dr.gather(type(origins), origins, point), tu * d.x + tv * d.y + n * d.z)

    return ray


def bake_directional(light, ctx):
    """The sun is a point source: one shadow ray, and the cosine of the angle to the normal."""
    import drjit as dr

    mi, sun = ctx.mi, np.array(light["direction"], dtype=np.float64)
    sun = mi.Vector3f(*(sun / np.linalg.norm(sun)).tolist())
    lit = summed_rays(mi, mi.Float, ctx.count, 1, lambda point, _k, _chunk: dr.select(
        ctx.query.device_blocked(mi.Ray3f(dr.gather(type(ctx.origin), ctx.origin, point), sun)), 0.0, 1.0))
    return dr.maximum(dr.dot(ctx.normals, sun), 0.0) * lit * colour(mi, light)


def bake_sky(light, ctx):
    """The share of the light's `rays` fixed cosine-weighted directions that reach the sky."""
    import drjit as dr

    mi = ctx.mi
    ray = hemisphere_ray(mi, ctx.origin, ctx.normals, sky_directions(light["rays"]))
    visible = summed_rays(mi, mi.Float, ctx.count, light["rays"], lambda point, k, _chunk: dr.select(
        ctx.query.device_blocked(ray(point, k)), 0.0, 1.0))
    return visible / light["rays"] * colour(mi, light)


def bake_ambient(light, ctx):
    return colour(ctx.mi, light) * (1.0 if ctx.occlusion is None else ctx.occlusion)


def colour(mi, light):
    return mi.Color3f(*(np.array(light["color"], dtype=np.float64) * light["intensity"]).tolist())


# The one table of what a scene light is: its fields, declared in
# import_settings.py, and what it adds to a point's radiance.
BAKERS = {"directional": bake_directional, "sky": bake_sky, "ambient": bake_ambient}
LIGHTS = {kind: (LIGHT_FIELDS[kind], bake) for kind, bake in BAKERS.items()}
assert set(LIGHTS) == set(LIGHT_FIELDS)


def device_local_occlusion(query, points, normals, ao, ray_offset):
    """Distance-limited ambient occlusion per point (Dr.Jit arrays): 1 where nothing stands within `ao.distance`
    of the point's hemisphere, down to 1 - `ao.strength` where it is walled in. Every point uses the same
    cosine-weighted directions in its own frame, and a hit counts for less the farther it is, linearly to nothing at
    the distance."""
    import drjit as dr

    mi = query.mi
    origin = points + normals * ray_offset
    ray = hemisphere_ray(mi, origin, normals, sky_directions(ao.rays))

    def covered(point, k, _chunk):
        traced = ray(point, k)
        hit = query.device_hits(traced)
        return dr.select(hit.valid, dr.clip(1.0 - dr.norm(hit.where - traced.o) / ao.distance, 0.0, 1.0), 0.0)

    return 1.0 - ao.strength * summed_rays(mi, mi.Float, dr.width(points), ao.rays, covered) / ao.rays


def device_occlusion(query, points, normals, double_sided, ao, ray_offset):
    """device_local_occlusion on each point's own side. A double-sided surface has no side it is meant to be seen
    from, so it takes the less occluded of its two: a curtain hanging against a wall is lit by the open side it is
    seen from, not darkened by the wall behind it."""
    import drjit as dr

    mi = query.mi
    factor = device_local_occlusion(query, points, normals, ao, ray_offset)
    both = dr.compress(double_sided)
    if dr.width(both):
        other = device_local_occlusion(query, dr.gather(type(points), points, both), -dr.gather(type(normals), normals, both), ao, ray_offset)
        best = dr.maximum(dr.gather(mi.Float, factor, both), other)
        dr.eval(best)
        dr.scatter(factor, best, both)
        dr.eval(factor)
    return factor


def local_occlusion(points, normals, intersector, ao, ray_offset):
    """device_local_occlusion for NumPy points and unit normals."""
    mi = intersector.mi
    factor = device_local_occlusion(intersector, vectors(mi.Point3f, points), vectors(mi.Vector3f, normals), ao,
                                    ray_offset)
    return np.array(factor, np.float64).reshape(-1)


def open_side_occlusion(points, normals, double_sided, intersector, ao, ray_offset):
    """device_occlusion for NumPy points, unit normals and double-sided flags."""
    mi = intersector.mi
    factor = device_occlusion(intersector, vectors(mi.Point3f, points), vectors(mi.Vector3f, normals),
                              mi.Bool(np.asarray(double_sided, bool)), ao, ray_offset)
    return np.array(factor, np.float64).reshape(-1)


def baked_lights(lights):
    """The surface and probe bakes share the baked rig; sky and ambient are bake settings."""
    return [light for light in lights if light["mode"] == LIGHT_MODE_BAKED]


def face_towards_light(normals, double_sided, lights):
    """The orientation a double-sided surface is first lit on: the side the directional lights, summed, shine on. One
    orientation for every light, so their order cannot matter. It also keeps the copies of a vertex at one position
    facing alike when their bounce is gathered together; `light` then tries the other side too."""
    import drjit as dr

    toward = np.zeros(3)
    for light in lights:
        if light["type"] == "directional":
            direction = np.array(light["direction"], dtype=np.float64)
            toward += light["intensity"] * direction / np.linalg.norm(direction)
    flip = double_sided & (dr.dot(normals, type(normals)(*toward.tolist())) < 0)
    return dr.select(flip, -normals, normals)


def device_light(points, normals, double_sided, query, lights, ray_offset, bounce=None, bounce_groups=None, ao=None,
                 bounce_intensity=1.0):
    """Radiance from the scene lights at each point (Dr.Jit points, unit normals and double-sided flags), in the
    unit of irradiance over pi.

    The sun and sky come from shadow rays against `query`, the same fixed directions at every point so equal
    surroundings give equal light. `bounce` is a PathLight whose bounced light is added; `bounce_groups` (NumPy) gives
    points that share one position the same bounce, gathered once on their mean normal, since bounced light changes
    slowly where direct light does not. `ao` is the scene's local occlusion setting: it scales the ambient light,
    and the bounced light when `ao.indirect`. A double-sided point has no side it is meant to be seen from: it is lit
    on both sides and keeps the brighter, so a card in the sun takes its sunlit side and a curtain in shade takes
    its open side, not the back that faces the wall. Point and spot lights are reserved and not baked yet.
    """
    import drjit as dr

    mi = query.mi
    lights = baked_lights(lights)
    n = face_towards_light(normals, double_sided, lights)
    occlusion = None if ao is None else device_occlusion(query, points, normals, double_sided, ao, ray_offset)
    radiance = lit_side(query, points, n, lights, ray_offset, occlusion, bounce, bounce_groups, ao, bounce_intensity,
                        0)
    both = dr.compress(double_sided)
    if dr.width(both):
        groups = None if bounce_groups is None else np.asarray(bounce_groups)[np.array(both, np.int64)]
        other = lit_side(query, dr.gather(type(points), points, both), -dr.gather(type(n), n, both), lights,
                         ray_offset, None if occlusion is None else dr.gather(mi.Float, occlusion, both), bounce,
                         groups, ao, bounce_intensity, 1)
        mine = dr.gather(type(radiance), radiance, both)
        best = dr.select(dr.sum(other) > dr.sum(mine), other, mine)
        dr.eval(best)
        dr.scatter(radiance, best, both)
        dr.eval(radiance)
    return radiance


def light(points, normals, double_sided, intersector, lights, ray_offset, bounce=None, bounce_groups=None, ao=None,
          bounce_intensity=1.0):
    """device_light for NumPy points, unit normals and double-sided flags, on the intersector's variant."""
    mi = intersector.mi
    if not len(points):
        return np.zeros((0, 3))
    radiance = device_light(vectors(mi.Point3f, points), vectors(mi.Vector3f, normals),
                            mi.Bool(np.asarray(double_sided, bool)), intersector, lights, ray_offset, bounce,
                            bounce_groups, ao, bounce_intensity)
    return rows(radiance, len(points))


def lit_side(query, points, n, lights, ray_offset, occlusion, bounce, bounce_groups, ao, bounce_intensity, side):
    """The light at each point on the side its unit normal `n` faces; `side` (0, 1) keeps the two sides' bounced
    light on separate sample streams."""
    import drjit as dr

    mi = query.mi
    ctx = SimpleNamespace(mi=mi, query=query, normals=n, origin=points + n * ray_offset, occlusion=occlusion,
                          count=dr.width(points))
    radiance = dr.zeros(mi.Color3f, ctx.count)
    for scene_light in lights:
        radiance = radiance + LIGHTS[scene_light["type"]][1](scene_light, ctx)
    if bounce is None:
        return radiance
    bounced = bounced_light(bounce, points, n, ray_offset, bounce_groups, side) * bounce_intensity
    if ao is not None and ao.indirect:
        bounced = bounced * occlusion
    return radiance + bounced


def bounced_light(bounce, points, normals, ray_offset, groups=None, side=0):
    """`bounce.device_bounce` for every point; with NumPy `groups`, once per group on the group's mean normal."""
    import drjit as dr

    if groups is None:
        return bounce.device_bounce(points, normals, ray_offset, side)
    mi = bounce.mi
    _, first, inverse = np.unique(groups, return_index=True, return_inverse=True)
    inverse = inverse.reshape(-1)
    host = rows(normals, len(inverse))
    total = np.stack([np.bincount(inverse, weights=host[:, axis]) for axis in range(3)], axis=1)
    length = np.linalg.norm(total, axis=1, keepdims=True)
    mean = np.where(length > 1e-6, total / np.maximum(length, 1e-12), host[first])
    gathered = bounce.device_bounce(dr.gather(type(points), points, mi.UInt32(first.astype(np.uint32))),
                                    vectors(mi.Vector3f, mean), ray_offset, side)
    return dr.gather(type(gathered), gathered, mi.UInt32(inverse.astype(np.uint32)))


def encode_srgb8(linear):
    """Linear light to 8-bit, with a 1/2.2 gamma (not the piecewise sRGB curve)."""
    return np.clip(np.round(255.0 * np.clip(linear, 0, 1) ** (1 / LIGHT_GAMMA)), 0, 255).astype(np.int64)


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
                 tonemap_white, samples=4, max_samples=16, sample_area=None, min_samples=1,
                 placement="stratified", bounce=None, ao=None, bounce_intensity=1.0):
    """One sRGB colour per triangle: albedo times light averaged over fixed
    points of the triangle, lit on its face normal. `samples` is a count per
    face, or "auto" for one point per `sample_area` of face area (the mesh's
    median face by default), from `min_samples` to `max_samples`. Every face
    shares one set of sky directions, so equal surroundings give equal
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
            radiance = light(points, tiled, double, intersector, lights, ray_offset, bounce, ao=ao,
                             bounce_intensity=bounce_intensity)
            colour = (albedo * radiance).reshape(k, len(faces), 3).mean(axis=0)
            out[selected] = to_srgb8(colour, tonemap_white)
    return out


def merge_matching_colours(pos, rgb, tris, weld_grid, step=6, *, labels):
    """A crease splits a vertex so each side can be lit on its own normal;
    where both sides came out the same colour, one vertex is enough. Positions
    are one where they round alike on a grid of `weld_grid` per unit."""
    key = np.concatenate([np.round(pos * weld_grid), rgb // step], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    keep = distinct_corners(tris)
    result = (pos[first], rgb[first], tris[keep])
    return (*result, np.asarray(labels)[keep])

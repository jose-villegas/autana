"""Bounce light for the bake, traced by Mitsuba's path integrator.

`PathLight` exports the full-detail source and the sun and sky once. For a point and a normal it sends the fixed
cosine-weighted rays of `light.sky_directions` into the scene and averages what Mitsuba's path integrator gathers
along them: light that has bounced at least once, with next-event estimation at every hit, so a hit's own albedo,
shadow and further bounces are all in it. Light that reaches the point without a bounce is not here; `light.light`
adds it from shadow rays. The mean of the cosine-weighted radiance is the irradiance over pi, the unit `light.light`
returns, so the albedo of the surface being baked multiplies it once.

Mitsuba's variant is process-wide and the ray queries pin it, so this runs on `ray_query.VARIANT` too.
"""
import numpy as np

from r3d import ray_query
from r3d.light import sky_directions, tangent_frame
from r3d.mitsuba_reference import integrator, prepare

SEED = 0
# Rays per Mitsuba call: bounds the arrays a large bake allocates.
BATCH = 1 << 18


class PathLight:
    def __init__(self, source, lights, double_sided, indirect, look):
        """`indirect` is the scene's `[bake].indirect` (bounces, rays) and `look` its `[indirect]` (intensity,
        albedo_boost). Ambient has no transport meaning and stays with `light.light`."""
        emitters = [item for item in lights if item["type"] != "ambient"]
        self.scene = prepare(source, emitters, double_sided, variant=ray_query.VARIANT, keep_textures=True,
                             albedo_boost=look.albedo_boost)
        self.mi = self.scene.mi
        # Mitsuba counts the ray itself as depth 1, so each bounce adds one.
        self.integrator = self.mi.load_dict(integrator(indirect.bounces + 1))
        self.rays, self.intensity = indirect.rays, look.intensity
        self.scalar = ray_query.VARIANT.startswith("scalar")

    def bounce(self, points, normals, ray_offset):
        """The bounced light at each point, on its `normals` side, scaled by the scene's indirect intensity. Points
        go through in chunks so the rays in flight stay within BATCH however many points there are."""
        local = sky_directions(self.rays)
        out = np.zeros((len(points), 3))
        step = max(1, BATCH // self.rays)
        for index, start in enumerate(range(0, len(points), step)):
            chunk = slice(start, start + step)
            n = normals[chunk]
            tu, tv = tangent_frame(n)
            origin = np.repeat(points[chunk] + n * ray_offset, self.rays, axis=0)
            direction = (tu[:, None] * local[None, :, 0:1] + tv[:, None] * local[None, :, 1:2]
                         + n[:, None] * local[None, :, 2:3]).reshape(-1, 3)
            found = self._radiance(origin, direction, SEED + index)
            out[chunk] = found.reshape(len(n), self.rays, 3).mean(axis=1)
        return out * self.intensity

    def _radiance(self, origin, direction, seed):
        mi = self.mi
        sampler = mi.load_dict({"type": "independent"})
        if self.scalar:
            sampler.seed(seed)
            out = np.zeros((len(origin), 3))
            for index in range(len(origin)):
                ray = mi.Ray3f(mi.Point3f(*origin[index].astype(np.float32)), mi.Vector3f(*direction[index].astype(np.float32)))
                out[index] = np.array(self.integrator.sample(self.scene.scene, sampler, ray)[0])
            return out
        import drjit as dr

        sampler.seed(seed, len(origin))
        o, d = origin.astype(np.float32), direction.astype(np.float32)
        ray = mi.Ray3f(mi.Point3f(mi.Float(o[:, 0]), mi.Float(o[:, 1]), mi.Float(o[:, 2])),
                       mi.Vector3f(mi.Float(d[:, 0]), mi.Float(d[:, 1]), mi.Float(d[:, 2])))
        radiance = self.integrator.sample(self.scene.scene, sampler, ray)[0]
        dr.eval(radiance)
        return np.array(radiance).T

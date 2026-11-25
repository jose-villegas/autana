"""Bounce light for the bake, traced by Mitsuba's path integrator.

`PathLight` exports the full-detail source and the sun and sky once. For a point and a normal it sends the fixed
cosine-weighted rays of `light.sky_directions` into the scene and averages what Mitsuba's path integrator gathers
along them: light that has bounced at least once, with next-event estimation at every hit, so a hit's own albedo,
shadow and further bounces are all in it. Light that reaches the point without a bounce is not here; `light.light`
adds it from shadow rays. The mean of the cosine-weighted radiance is the irradiance over pi, the unit `light.light`
returns, so the albedo of the surface being baked multiplies it once.

Mitsuba's variant is process-wide, so this runs on the variant of the ray queries beside it, `ray_query.VARIANT`
unless the caller names one. Rays launch in chunks of at most `ray_query.LAUNCH`, each seeded by its number and the
side being lit, so the light at a point depends on the points before it and never on the device.
"""

from r3d import ray_query
from r3d.geometry import corner_normals
from r3d.light import baked_lights, hemisphere_ray, rows, sky_directions, summed_rays, vectors
from r3d.mitsuba_reference import integrator, prepare

SEED = 0
# Seeds apart per side of a double-sided point (light.lit_side), so the two sides draw separate samples.
SIDE_SEEDS = 1 << 16


class PathLight:
    def __init__(self, source, lights, double_sided, indirect, albedo_boost, variant=None):
        """`indirect` is the scene's `[bake].indirect` (bounces, rays) and `albedo_boost` its `[indirect]` boost, which
        belongs to the exported materials. Pure transport: the intensity is applied where the light is summed.
        Ambient has no transport meaning and stays with `light.light`."""
        emitters = [item for item in baked_lights(lights) if item["type"] != "ambient"]
        source.corner_normals = corner_normals(source.p, source.tri_v)
        variant = variant or ray_query.VARIANT
        self.scene = prepare(source, emitters, double_sided, variant=variant, keep_textures=True,
                             albedo_boost=albedo_boost)
        self.mi = self.scene.mi
        # Mitsuba counts the ray itself as depth 1, so each bounce adds one.
        self.integrator = self.mi.load_dict(integrator(indirect.bounces + 1, self.scene.masked))
        self.rays = indirect.rays

    def bounce(self, points, normals, ray_offset):
        """device_bounce for NumPy points and unit normals."""
        mi = self.mi
        return rows(self.device_bounce(vectors(mi.Point3f, points), vectors(mi.Vector3f, normals), ray_offset),
                    len(points))

    def device_bounce(self, points, normals, ray_offset, side=0):
        """The bounced light at each point (Dr.Jit arrays), on its `normals` side: the mean over the point's rays."""
        import drjit as dr

        mi = self.mi
        ray = hemisphere_ray(mi, points + normals * ray_offset, normals, sky_directions(self.rays))

        def radiance(point, k, chunk):
            sampler = mi.load_dict({"type": "independent"})
            sampler.seed(SEED + side * SIDE_SEEDS + chunk, dr.width(point))
            return self.integrator.sample(self.scene.scene, sampler, ray(point, k))[0]

        return summed_rays(mi, mi.Color3f, dr.width(points), self.rays, radiance) / self.rays

    def _radiance(self, origin, direction, seed):
        """What the path integrator gathers along NumPy rays, one sample stream seeded by `seed`."""
        import drjit as dr

        mi = self.mi
        sampler = mi.load_dict({"type": "independent"})
        sampler.seed(seed, len(origin))
        ray = mi.Ray3f(vectors(mi.Point3f, origin), vectors(mi.Vector3f, direction))
        radiance = self.integrator.sample(self.scene.scene, sampler, ray)[0]
        dr.eval(radiance)
        return rows(radiance, len(origin))

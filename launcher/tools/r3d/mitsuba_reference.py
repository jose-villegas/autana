"""Path-traced backend for reference_render.py: the same source mesh, albedo, camera and lights, rendered by Mitsuba 3.

The adapter only translates what the bake reference already interprets (geometry after alpha rejection, the
albedo decode, the pinhole camera, the scene's lights) into a Mitsuba scene, so the two backends differ in
transport and nothing else. It returns linear radiance and per-pixel coverage; exposure, tone map and RGB565
conversion stay with reference_render.device_picture. Mitsuba and Dr.Jit are pinned in requirements.txt.
"""

import math

import numpy as np

from .poses import camera_basis

DEFAULT_SPP = 256
DEFAULT_DEPTH = 12


def import_mitsuba():
    """The mitsuba module, or None when it is not installed."""
    try:
        import mitsuba
    except ImportError:
        return None
    return mitsuba


def default_variant(mi):
    """The fastest variant this machine can run: CUDA, then LLVM, then the scalar CPU one. A CUDA device without
    OptiX, as under WSL, loads no scene, so each candidate is proven with one."""
    import drjit as dr

    for backend, variant in ((dr.JitBackend.CUDA, "cuda_ad_rgb"), (dr.JitBackend.LLVM, "llvm_ad_rgb")):
        if variant in mi.variants() and dr.has_backend(backend) and traces_rays(mi, variant):
            return variant
    return "scalar_rgb"


def traces_rays(mi, variant):
    mi.set_variant(variant)
    try:
        mi.load_dict({"type": "scene", "shape": {"type": "rectangle"}})
    except RuntimeError:
        return False
    return True


ALBEDO_CEILING = 0.99


def boosted_albedo(albedo, boost):
    """The reflectance bounces use: albedo times boost, held below ALBEDO_CEILING but never lowered below the
    albedo itself, so a boost of 1 changes nothing."""
    return np.minimum(albedo * boost, np.maximum(albedo, ALBEDO_CEILING))


def material_bsdf(mi, kd, texture, two_sided, boost=1.0):
    """The diffuse BSDF of one material: texture (linear, level 0) times Kd as `albedo_from_uv` samples it, or Kd
    decoded with the same power 2.2 when untextured, both through `boosted_albedo`. `raw` keeps Mitsuba from
    decoding the bitmap a second time."""
    kd = np.array(kd, dtype=np.float64)
    if texture is None:
        reflectance = {"type": "rgb", "value": boosted_albedo(kd**2.2, boost).tolist()}
    else:
        pixels = boosted_albedo(texture.levels[0][..., :3] * kd, boost).astype(np.float32)
        reflectance = {"type": "bitmap", "bitmap": mi.Bitmap(pixels), "raw": True, "filter_type": "bilinear",
                       "wrap_mode": "repeat"}
    bsdf = {"type": "diffuse", "reflectance": reflectance}
    return {"type": "twosided", "bsdf": bsdf} if two_sided else bsdf


def source_meshes(mi, source, double_sided, keep_textures=False, boost=1.0):
    """One Mitsuba mesh per source material. Corners are not shared, since the source indexes positions and UVs
    separately; the shading normals are the import's crease-limited corner normals. The texture V axis is flipped
    to the orientation `obj.Texture.sample` reads. Each texture is dropped from the source once exported, which a
    large scene needs to keep its host memory flat, so a source is exported once; `keep_textures` leaves them for
    a caller that still samples them."""
    meshes = {}
    for index, name in enumerate(source.names):
        chosen = source.tri_m == index
        count = int(chosen.sum())
        if not count:
            continue
        properties = mi.Properties()
        properties["bsdf"] = mi.load_dict(material_bsdf(mi, source.materials.get(name, {}).get("Kd", (1.0, 1.0, 1.0)),
                                                         source.textures[index], name in double_sided, boost))
        mesh = mi.Mesh(name, count * 3, count, properties, has_vertex_normals=True, has_vertex_texcoords=True)
        params = mi.traverse(mesh)
        params["vertex_positions"] = source.p[source.tri_v[chosen]].astype(np.float32).ravel()
        params["faces"] = np.arange(count * 3, dtype=np.uint32)
        params["vertex_normals"] = source.corner_normals[chosen].astype(np.float32).ravel()
        uv = source.uv[source.tri_t[chosen]].astype(np.float32).reshape(-1, 2)
        uv[:, 1] = 1.0 - uv[:, 1]
        params["vertex_texcoords"] = uv.ravel()
        params.update()
        meshes[f"mesh_{index}"] = mesh
        if not keep_textures:
            source.textures[index] = None
    return meshes


def emitters(mi, lights, sky=None):
    """The scene lights as Mitsuba emitters, in the units `light()` returns: its value is E/pi times albedo, so a
    directional light's irradiance is pi * colour * intensity and a flat sky is a constant radiance of colour *
    intensity. A directional light is a point source here, so soft sun discs are not reproduced. Ambient is a
    non-physical constant with no transport meaning and is rejected unless it is zero.

    `sky` (turbidity, albedo) replaces the lights by the Hosek-Wilkie sun and sky, the sun taking its direction from
    the first directional light."""
    if sky is not None:
        sun = next((light for light in lights if light["type"] == "directional"), None)
        if sun is None:
            raise ValueError("a physical sky needs the scene's directional light for the sun direction")
        direction = np.array(sun["direction"], dtype=np.float64)
        return {"sunsky": {"type": "sunsky", "sun_direction": (direction / np.linalg.norm(direction)).tolist(),
                           "to_world": y_up_to_z_up(mi), "turbidity": float(sky["turbidity"]), "albedo": float(sky["albedo"])}}
    found = {}
    for index, light in enumerate(lights):
        colour = (np.array(light["color"], dtype=np.float64) * light["intensity"]).tolist()
        if light["type"] == "directional":
            direction = np.array(light["direction"], dtype=np.float64)
            found[f"light_{index}"] = {"type": "directional", "direction": (-direction / np.linalg.norm(direction)).tolist(),
                                       "irradiance": {"type": "rgb", "value": [math.pi * c for c in colour]}}
        elif light["type"] == "sky":
            found[f"light_{index}"] = {"type": "constant", "radiance": {"type": "rgb", "value": colour}}
        elif light["type"] == "ambient" and any(colour):
            raise ValueError("the path-traced backend has no ambient term: use a sky light or the physical sky")
    return found


def y_up_to_z_up(mi):
    """Mitsuba's sunsky is Z-up; the engine's world is Y-up."""
    return mi.ScalarTransform4f.rotate([1, 0, 0], -90)


def sensor(mi, pose, width, height, lens, near):
    """The engine's pinhole camera: `lens` is the tangent of the half field of view along the shorter side, y up."""
    _right, _up, forward = camera_basis(pose[3:])
    eye = np.array(pose[:3], dtype=np.float64)
    return mi.load_dict({"type": "perspective", "fov": math.degrees(2.0 * math.atan(lens)), "fov_axis": "smaller",
                         "near_clip": float(near), "far_clip": 1e7,
                         "to_world": mi.ScalarTransform4f.look_at(origin=eye.tolist(), target=(eye + forward).tolist(), up=[0, 1, 0]),
                         "sampler": {"type": "independent"},
                         "film": {"type": "hdrfilm", "width": width, "height": height, "rfilter": {"type": "box"},
                                  "pixel_format": "rgba", "component_format": "float32"}})


def integrator(max_depth):
    """A path tracer with next-event estimation, MIS and Russian roulette from depth 5, emitters hidden so the film's
    alpha is the coverage of the mesh."""
    return {"type": "path", "max_depth": int(max_depth), "rr_depth": 5, "hide_emitters": True}


class PathScene:
    """The exported source and lights, traced from any pose at any spp and depth."""

    def __init__(self, mi, scene, pixels_per_pass):
        self.mi, self.scene, self.pixels_per_pass, self.integrators = mi, scene, pixels_per_pass, {}

    def trace(self, pose, width, height, lens, near, spp=DEFAULT_SPP, seed=0, max_depth=DEFAULT_DEPTH):
        """(linear RGB, coverage) of a pose: `spp` paths per pixel averaged over passes small enough to bound
        memory. Coverage is the share of each pixel's paths that hit the mesh."""
        if max_depth not in self.integrators:
            self.integrators[max_depth] = self.mi.load_dict(integrator(max_depth))
        camera = sensor(self.mi, pose, width, height, lens, near)
        per_pass = min(spp, max(1, self.pixels_per_pass // (width * height)))
        total, done, index = np.zeros((height, width, 4)), 0, 0
        while done < spp:
            take = min(per_pass, spp - done)
            total += np.array(self.mi.render(self.scene, sensor=camera, spp=take, seed=seed * 100003 + index,
                                             integrator=self.integrators[max_depth])) * take
            done, index = done + take, index + 1
        image = total / spp
        return image[..., :3], np.clip(image[..., 3], 0.0, 1.0)


def prepare(source, lights, double_sided, sky=None, variant=None, pixels_per_pass=1 << 24, keep_textures=False,
            albedo_boost=1.0):
    """Export the source and the lights once; the source's textures are consumed unless `keep_textures`.
    `pixels_per_pass` bounds the paths in flight at once."""
    mi = import_mitsuba()
    if mi is None:
        raise RuntimeError("the path-traced backend needs Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
    mi.set_variant(variant or default_variant(mi))
    scene = {"type": "scene", "integrator": integrator(DEFAULT_DEPTH)}
    scene.update(source_meshes(mi, source, double_sided, keep_textures, albedo_boost))
    scene.update(emitters(mi, lights, sky))
    return PathScene(mi, mi.load_dict(scene), pixels_per_pass)


def add_options(parser):
    """The options naming a path-traced run's sky and variant, shared by the reference and the sweep."""
    parser.add_argument("--variant", help="mitsuba variant (default: cuda_ad_rgb, llvm_ad_rgb, else scalar_rgb)")
    parser.add_argument("--sky", choices=("hosek-wilkie",), help="replace the scene lights by this sun and sky")
    parser.add_argument("--turbidity", type=float, default=3.0, help="--sky turbidity")
    parser.add_argument("--ground-albedo", type=float, default=0.3, help="--sky ground albedo")


def sky_from(args):
    """The `sky` of prepare() the options of add_options name, or None."""
    return {"turbidity": args.turbidity, "albedo": args.ground_albedo} if args.sky else None

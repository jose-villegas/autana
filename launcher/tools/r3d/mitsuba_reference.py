"""Path-traced backend for reference_render.py: the same source mesh, albedo, camera and lights, rendered by Mitsuba 3.

The adapter only translates what the Embree reference already interprets (geometry after alpha rejection, the
albedo decode, the pinhole camera, the scene's lights) into a Mitsuba scene, so the two backends differ in
transport and nothing else. It returns linear radiance and per-pixel coverage; exposure, tone map and RGB565
conversion stay with reference_render.device_picture. Mitsuba and Dr.Jit are optional, pinned in
requirements-gpu.txt.
"""

import contextlib
import math

import numpy as np

from .obj import Texture
from .poses import camera_basis


def import_mitsuba():
    """The mitsuba module, or None when it is not installed."""
    try:
        import mitsuba
    except ImportError:
        return None
    return mitsuba


@contextlib.contextmanager
def lean_textures():
    """Load textures as float32 inside the block: the export reads level 0 only, and a large scene's float64 mip chains
    are several times its source size."""
    saved, Texture.dtype = Texture.dtype, np.float32
    try:
        yield
    finally:
        Texture.dtype = saved


def default_variant(mi):
    """The fastest variant this machine can run: CUDA, then LLVM, then the scalar CPU one."""
    import drjit as dr

    for backend, variant in ((dr.JitBackend.CUDA, "cuda_ad_rgb"), (dr.JitBackend.LLVM, "llvm_ad_rgb")):
        if variant in mi.variants() and dr.has_backend(backend):
            return variant
    return "scalar_rgb"


def material_bsdf(mi, kd, texture, two_sided):
    """The diffuse BSDF of one material: texture (linear, level 0) times Kd as `albedo_from_uv` samples it, or Kd
    decoded with the same power 2.2 when untextured. `raw` keeps Mitsuba from decoding the bitmap a second time."""
    kd = np.array(kd, dtype=np.float64)
    if texture is None:
        reflectance = {"type": "rgb", "value": (kd**2.2).tolist()}
    else:
        pixels = (texture.levels[0][..., :3] * kd).astype(np.float32)
        reflectance = {"type": "bitmap", "bitmap": mi.Bitmap(pixels), "raw": True, "filter_type": "bilinear",
                       "wrap_mode": "repeat"}
    bsdf = {"type": "diffuse", "reflectance": reflectance}
    return {"type": "twosided", "bsdf": bsdf} if two_sided else bsdf


def source_meshes(mi, source, double_sided):
    """One Mitsuba mesh per source material. Corners are not shared, since the source indexes positions and UVs
    separately; the shading normals are the import's crease-limited corner normals. The texture V axis is flipped
    to the orientation `obj.Texture.sample` reads."""
    meshes = {}
    for index, name in enumerate(source.names):
        chosen = source.tri_m == index
        count = int(chosen.sum())
        if not count:
            continue
        properties = mi.Properties()
        properties["bsdf"] = mi.load_dict(material_bsdf(mi, source.materials.get(name, {}).get("Kd", (1.0, 1.0, 1.0)),
                                                         source.textures[index], name in double_sided))
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
    return meshes


def emitters(mi, lights, sky=None):
    """The scene lights as Mitsuba emitters, in the units `light()` returns: its value is E/pi times albedo, so a
    directional light's irradiance is pi * colour * intensity and a flat sky is a constant radiance of colour *
    intensity. A directional light is a point source here, so soft sun discs are not reproduced. Ambient is a
    non-physical constant with no transport meaning and is rejected unless it is zero.

    `sky` (turbidity, albedo, sun_aperture_degrees) replaces the lights by the Hosek-Wilkie sun and sky, the sun
    taking its direction from the first directional light."""
    if sky is not None:
        sun = next((light for light in lights if light["type"] == "directional"), None)
        if sun is None:
            raise ValueError("a physical sky needs the scene's directional light for the sun direction")
        direction = np.array(sun["direction"], dtype=np.float64)
        return {"sunsky": {"type": "sunsky", "sun_direction": (direction / np.linalg.norm(direction)).tolist(),
                           "to_world": y_up_to_z_up(mi), "turbidity": float(sky["turbidity"]),
                           "albedo": float(sky["albedo"]), "sun_aperture": float(sky.get("sun_aperture_degrees", 0.5338))}}
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


def sensor(mi, pose, width, height, lens, near, spp):
    """The engine's pinhole camera: `lens` is the tangent of the half field of view along the shorter side, y up."""
    right, up, forward = camera_basis(pose[3:])
    eye = np.array(pose[:3], dtype=np.float64)
    return {"type": "perspective", "fov": math.degrees(2.0 * math.atan(lens)), "fov_axis": "x" if width <= height else "y",
            "near_clip": float(near), "far_clip": 1e7,
            "to_world": mi.ScalarTransform4f.look_at(origin=eye.tolist(), target=(eye + forward).tolist(), up=[0, 1, 0]),
            "sampler": {"type": "independent", "sample_count": spp},
            "film": {"type": "hdrfilm", "width": width, "height": height, "rfilter": {"type": "box"},
                     "pixel_format": "rgba", "component_format": "float32"}}


def build_scene(mi, source, lights, double_sided, pose, width, height, lens, near, spp, max_depth, sky=None):
    """The Mitsuba scene of one pose: the source meshes, the lights, a path integrator, the camera."""
    scene = {"type": "scene", "integrator": {"type": "path", "max_depth": int(max_depth), "rr_depth": 5,
                                             "hide_emitters": True},
             "sensor": sensor(mi, pose, width, height, lens, near, spp)}
    scene.update(source_meshes(mi, source, double_sided))
    scene.update(emitters(mi, lights, sky))
    return mi.load_dict(scene)


class PathScene:
    """One pose's Mitsuba scene, traced any number of times at any spp."""

    def __init__(self, mi, scene, width, height, per_pass):
        self.mi, self.scene, self.width, self.height, self.per_pass = mi, scene, width, height, per_pass

    def trace(self, spp, seed=0):
        """(linear RGB, coverage): `spp` paths per pixel averaged over passes small enough to bound memory.
        Coverage is the share of each pixel's paths that hit the mesh."""
        per_pass = min(spp, self.per_pass)
        total, done, index = np.zeros((self.height, self.width, 4)), 0, 0
        while done < spp:
            take = min(per_pass, spp - done)
            total += np.array(self.mi.render(self.scene, spp=take, seed=seed * 100003 + index)) * take
            done, index = done + take, index + 1
        image = total / spp
        return image[..., :3], np.clip(image[..., 3], 0.0, 1.0)


def prepare(source, lights, double_sided, pose, width, height, lens, near, max_depth=12, sky=None, variant=None,
            pixels_per_pass=1 << 24):
    """Export the source and the lights for one pose; `pixels_per_pass` bounds the paths in flight at once."""
    mi = import_mitsuba()
    if mi is None:
        raise RuntimeError("the path-traced backend needs Mitsuba: pip install -r launcher/tools/r3d/requirements-gpu.txt")
    mi.set_variant(variant or default_variant(mi))
    per_pass = max(1, pixels_per_pass // (width * height))
    scene = build_scene(mi, source, lights, double_sided, pose, width, height, lens, near, per_pass, max_depth, sky)
    return PathScene(mi, scene, width, height, per_pass)


def render(source, lights, double_sided, pose, width, height, lens, near, spp=256, max_depth=12, seed=0, sky=None,
           variant=None, pixels_per_pass=1 << 24):
    """(linear RGB, coverage) of a pose at `spp` paths per pixel."""
    return prepare(source, lights, double_sided, pose, width, height, lens, near, max_depth, sky, variant,
                   pixels_per_pass).trace(spp, seed)

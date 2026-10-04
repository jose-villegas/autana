#!/usr/bin/env python3
"""Render an undecimated import source with the scene's bake lighting.

    python launcher/tools/r3d/reference_render.py SCENE.scene.toml [--object NAME] --poses poses.txt --out DIR
        [--backend mitsuba --spp N --max-depth N [--sky hosek-wilkie]]

The poses file is the ``size``, ``lens`` and ``pose`` format emitted by
tools/anim/sample_tracks.sh.  Each pose writes a floating-point .npy image and
an RGB565-expanded PNG.  The renderer deliberately has no scene knowledge:
the scene supplies the object's source, lights, camera lens and pose path.

The default backend is the Embree reference with the scene's bake lighting. ``--backend mitsuba`` traces the same
source, albedo, camera and lights as a path-traced reference (r3d.mitsuba_reference, needs requirements-gpu.txt)
and shares the exposure, tone map and RGB565 conversion below.
"""

import argparse
import pathlib
import sys

import numpy as np
import trimesh
from PIL import Image
from trimesh.ray.ray_pyembree import RayMeshIntersector

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import mitsuba_reference
from r3d.geometry import corner_normals
from r3d.import_settings import load_scene
from r3d.light import albedo_from_uv, drop_masked, light, to_srgb8
from r3d.mesh_import import indirect_cache_for, load_source
from r3d.poses import camera_rays, read_poses

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from render_compare import expand_565  # noqa: E402


def hit_barycentrics(source, hits, locations):
    """Barycentric weights of ray hits on their source triangles."""
    faces = source.tri_v[hits]
    a, b, c = source.p[faces[:, 0]], source.p[faces[:, 1]], source.p[faces[:, 2]]
    total = np.abs(np.cross(b - a, c - a)).sum(axis=1)
    wa = np.abs(np.cross(b - locations, c - locations)).sum(axis=1) / total
    wb = np.abs(np.cross(c - locations, a - locations)).sum(axis=1) / total
    return np.stack([wa, wb, 1.0 - wa - wb], axis=1)


def hit_normals(source, hits, bary):
    """Smooth the import's crease-limited source normals at ray hits."""
    corners = source.corner_normals[hits]
    normal = (corners * bary[:, :, None]).sum(axis=1)
    return normal / np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)


def hit_albedo(source, hits, bary):
    """Sample each hit's source texture at the source triangle's UV point."""
    material = source.tri_m[hits]
    out = np.zeros((len(hits), 3), dtype=float)
    for index in np.unique(material):
        chosen = material == index
        kd = source.materials.get(source.names[index], {}).get("Kd", (1.0, 1.0, 1.0))
        uv = (source.uv[source.tri_t[hits[chosen]]] * bary[chosen, :, None]).sum(axis=1)
        out[chosen] = albedo_from_uv(source.textures[index], kd, uv, np.zeros(len(uv)))
    return out


def render_linear(source, job, scene, pose, width, height, lens, samples=4):
    """Return a linear RGB source render, supersampled then box filtered."""
    return trace(source, job, scene, pose, width, height, lens, samples)[0]


def trace(source, job, scene, pose, width, height, lens, samples=4):
    """(linear RGB, share of each pixel's subpixels that hit the mesh, the
    world-space shading normal per pixel turned toward the eye, averaged over
    the pixel's rays and zero where they all miss)."""
    settings = job.settings
    eye, forward = pose[:3], pose[3:]
    origin, direction = camera_rays(width, height, lens, eye, forward, samples)
    locations, rays, faces = source.intersector.intersects_location(origin, direction, multiple_hits=False)
    linear = np.zeros((len(origin), 3), dtype=float)
    covered = np.zeros(len(origin))
    covered[rays] = 1.0
    shading = np.zeros((len(origin), 3), dtype=float)
    if len(rays):
        bary = hit_barycentrics(source, faces, locations)
        normal = hit_normals(source, faces, bary)
        material = source.tri_m[faces]
        albedo = hit_albedo(source, faces, bary)
        double = np.isin(material, [source.names.index(name) for name in settings.double_sided])
        radiance = light(locations, normal, double, source.intersector, scene.lights, job.bake.ray_offset,
                         np.random.default_rng(settings.seed), shared_sky_rays=0, indirect=source.indirect_cache)
        linear[rays] = albedo * radiance
        shading[rays] = normal * np.where((normal * direction[rays]).sum(axis=1) > 0, -1.0, 1.0)[:, None]
    shading = shading.reshape(height, width, samples * samples, 3).sum(axis=2)
    return (linear.reshape(height, width, samples * samples, 3).mean(axis=2),
            covered.reshape(height, width, samples * samples).mean(axis=2),
            shading / np.maximum(np.linalg.norm(shading, axis=2, keepdims=True), 1e-12))


def device_picture(linear, covered, tonemap_white, background):
    """The 8-bit picture the device shows: the tone-mapped mesh over the scene's
    background colour in proportion to what each pixel leaves uncovered, then
    RGB565-quantised."""
    lit = to_srgb8(linear, tonemap_white).astype(float)
    colour = np.array([background >> 16, (background >> 8) & 255, background & 255], dtype=float)
    lit = lit * covered[..., None] + colour * (1.0 - covered[..., None])
    return expand_565(np.round(lit).astype(np.uint8))


def source_for(scene, name=None, lit=True):
    """The full-detail source of the scene object `name` (the first mesh renderer
    when None), alpha-masked and lit as that renderer is baked; returns it with
    the object's job. `lit=False` skips the Embree intersector and the indirect
    cache, which only the Embree backend uses."""
    named = [item for item in scene.renderers if name in (None, item.object.name)]
    if not named:
        raise ValueError(f"the scene places no mesh renderer named {name!r}")
    job = named[0]
    settings = job.settings
    if lit:
        source = load_source(settings)
    else:
        with mitsuba_reference.lean_textures():
            source = load_source(settings)
    if settings.alpha_keep is not None:
        source.tri_v, source.tri_t, source.tri_m = drop_masked(source.p, source.uv, source.tri_v, source.tri_t,
                                                                 source.tri_m, source.textures, settings.alpha_keep)
    source.corner_normals = corner_normals(source.p, source.tri_v)
    if not lit:
        return source, job
    source.intersector = RayMeshIntersector(trimesh.Trimesh(source.p, source.tri_v, process=False))
    source.indirect_cache = indirect_cache_for(source, job, scene, source.intersector)
    return source, job


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--object", help="the scene's mesh renderer to render, lit as it is baked (default: the first)")
    parser.add_argument("--poses", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--samples", type=int, default=4)
    parser.add_argument("--skip", type=int, default=0, help="ignore this many leading poses")
    parser.add_argument("--normals", action="store_true", help="also write each pose's shading normals as NNNN.normal.npy")
    parser.add_argument("--backend", choices=("embree", "mitsuba"), default="embree",
                        help="embree: the scene's bake lighting (default); mitsuba: a path-traced reference")
    parser.add_argument("--spp", type=int, default=256, help="mitsuba: paths per pixel")
    parser.add_argument("--max-depth", type=int, default=12, help="mitsuba: path depth, 2 is direct light only")
    parser.add_argument("--seed", type=int, default=0, help="mitsuba: sampler seed")
    parser.add_argument("--variant", help="mitsuba: variant (default: cuda_ad_rgb, llvm_ad_rgb, else scalar_rgb)")
    parser.add_argument("--sky", choices=("hosek-wilkie",), help="mitsuba: replace the scene lights by this sun and sky")
    parser.add_argument("--turbidity", type=float, default=3.0, help="mitsuba: --sky turbidity")
    parser.add_argument("--ground-albedo", type=float, default=0.3, help="mitsuba: --sky ground albedo")
    args = parser.parse_args(argv)
    if args.samples < 1 or args.skip < 0:
        parser.error("--samples must be positive and --skip cannot be negative")
    if args.backend == "mitsuba" and (args.spp < 1 or args.max_depth < 1):
        parser.error("--spp and --max-depth must be positive")
    if args.backend == "mitsuba" and args.normals:
        parser.error("--normals is an embree backend output")
    if args.backend == "embree" and args.sky:
        parser.error("--sky needs --backend mitsuba")
    scene = load_scene(args.scene)
    width, height, lens, near, poses = read_poses(args.poses)
    poses = poses[args.skip :]
    if not poses:
        parser.error("--skip removes every pose")
    if scene.camera is None or scene.tonemap_white is None:
        parser.error("scene needs a camera and tone map")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        source, job = source_for(scene, args.object, lit=args.backend == "embree")
    except ValueError as error:
        parser.error(str(error))
    sky = {"turbidity": args.turbidity, "albedo": args.ground_albedo} if args.sky else None
    for index, pose in enumerate(poses):
        if args.backend == "mitsuba":
            linear, covered = mitsuba_reference.render(source, scene.lights, job.settings.double_sided, pose, width, height,
                                                       lens, near, args.spp, args.max_depth, args.seed, sky, args.variant)
            normal = None
        else:
            linear, covered, normal = trace(source, job, scene, pose, width, height, lens, args.samples)
        np.save(out / ("%04d.linear.npy" % index), linear)
        if args.normals:
            np.save(out / ("%04d.normal.npy" % index), normal.astype(np.float32))
        picture = device_picture(linear, covered, scene.tonemap_white, scene.camera.component.background)
        Image.fromarray(picture).save(out / ("%04d.png" % index))
    return 0


if __name__ == "__main__":
    sys.exit(main())

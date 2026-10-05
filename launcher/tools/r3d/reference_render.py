#!/usr/bin/env python3
"""Render an undecimated import source with the scene's bake lighting.

    python launcher/tools/r3d/reference_render.py SCENE.scene.toml [--object NAME] --poses poses.txt --out DIR
        [--backend mitsuba --spp N --max-depth N [--sky hosek-wilkie]]

The poses file is the ``size``, ``lens`` and ``pose`` format emitted by
tools/anim/sample_tracks.sh.  Each pose writes a floating-point .npy image and
an RGB565-expanded PNG.  The renderer deliberately has no scene knowledge:
the scene supplies the object's source, lights, camera lens and pose path.

The default backend is the bake reference: the scene's bake lighting. ``--backend mitsuba`` traces the same
source, albedo, camera and lights as a path-traced reference (r3d.mitsuba_reference)
and shares the exposure, tone map and RGB565 conversion below. Only the bake backend reads `[bake].ao`.
"""

import argparse
import os
import multiprocessing
import concurrent.futures
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import mitsuba_reference
from r3d.geometry import corner_normals
from r3d.import_settings import load_scene
from r3d.light import albedo_from_uv, drop_masked, light, open_side_occlusion, to_srgb8
from r3d.mesh_import import load_source, path_light_for
from r3d.poses import camera_rays, read_poses
from r3d.ray_query import RayQuery

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


def primary_hits(source, job, pose, width, height, lens, samples):
    """The camera rays of a pose and what they hit: (ray origins, directions, hit locations, the ray each hit belongs to,
    the source triangle hit, its bary-interpolated normal, its barycentrics, whether its material is double-sided)."""
    origin, direction = camera_rays(width, height, lens, pose[:3], pose[3:], samples)
    locations, rays, faces = source.intersector.first_hit(origin, direction)
    bary = hit_barycentrics(source, faces, locations) if len(rays) else np.zeros((0, 3))
    normal = hit_normals(source, faces, bary) if len(rays) else np.zeros((0, 3))
    double = np.isin(source.tri_m[faces], [source.names.index(name) for name in job.settings.double_sided])
    return origin, direction, locations, rays, faces, normal, bary, double


def trace(source, job, scene, pose, width, height, lens, samples=4):
    """(linear RGB, share of each pixel's subpixels that hit the mesh, the
    world-space shading normal per pixel turned toward the eye, averaged over
    the pixel's rays and zero where they all miss)."""
    settings = job.settings
    origin, direction, locations, rays, faces, normal, bary, double = primary_hits(source, job, pose, width, height, lens,
                                                                                    samples)
    linear = np.zeros((len(origin), 3), dtype=float)
    covered = np.zeros(len(origin))
    covered[rays] = 1.0
    shading = np.zeros((len(origin), 3), dtype=float)
    if len(rays):
        albedo = hit_albedo(source, faces, bary)
        radiance = light(locations, normal, double, source.intersector, scene.lights, job.bake.ray_offset,
                         source.bounce, ao=job.bake.ao)
        linear[rays] = albedo * radiance
        shading[rays] = normal * np.where((normal * direction[rays]).sum(axis=1) > 0, -1.0, 1.0)[:, None]
    shading = shading.reshape(height, width, samples * samples, 3).sum(axis=2)
    return (linear.reshape(height, width, samples * samples, 3).mean(axis=2),
            covered.reshape(height, width, samples * samples).mean(axis=2),
            shading / np.maximum(np.linalg.norm(shading, axis=2, keepdims=True), 1e-12))


def occlusion_map(source, job, pose, width, height, lens, samples=4):
    """(the scene's local occlusion factor per pixel, 1 where open and where nothing is hit; the share of each pixel's
    subpixels that hit the mesh), box filtered like trace(). It is the factor `[bake].ao` scales the ambient and bounce
    light by, so the scene must set it."""
    if job.bake.ao is None:
        raise ValueError("the scene sets no [bake].ao")
    origin, _direction, locations, rays, _faces, normal, _bary, double = primary_hits(source, job, pose, width, height, lens,
                                                                                       samples)
    factor = np.ones(len(origin))
    covered = np.zeros(len(origin))
    covered[rays] = 1.0
    if len(rays):
        factor[rays] = open_side_occlusion(locations, normal, double, source.intersector, job.bake.ao, job.bake.ray_offset)
    shape = (height, width, samples * samples)
    return factor.reshape(shape).mean(axis=2), covered.reshape(shape).mean(axis=2)


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
    the object's job. `lit=False` skips the ray queries and the indirect
    cache, which only the bake backend uses, and loads float32 textures."""
    named = [item for item in scene.renderers if name in (None, item.object.name)]
    if not named:
        raise ValueError(f"the scene places no mesh renderer named {name!r}")
    job = named[0]
    settings = job.settings
    source = load_source(settings, np.float64 if lit else np.float32)
    if settings.alpha_keep is not None:
        source.tri_v, source.tri_t, source.tri_m = drop_masked(source.p, source.uv, source.tri_v, source.tri_t,
                                                                 source.tri_m, source.textures, settings.alpha_keep)
    source.corner_normals = corner_normals(source.p, source.tri_v)
    if not lit:
        return source, job
    source.intersector = RayQuery(source.p, source.tri_v)
    source.bounce = path_light_for(source, job, scene)
    return source, job


# Estimated bytes per ray: trace/hit buffers 256, ray-query/lighting scratch 256,
# sky tangents, samples, directions and temporaries 256.
POSE_BASE_BYTES_PER_RAY = 768
POSE_STATE = None


def _write_pose(item):
    source, job, scene, width, height, lens, samples, out, normals, occlusion = POSE_STATE
    index, pose = item
    linear, covered, normal = trace(source, job, scene, pose, width, height, lens, samples)
    np.save(out / ("%04d.linear.npy" % index), linear)
    if normals:
        np.save(out / ("%04d.normal.npy" % index), normal.astype(np.float32))
    if occlusion:
        factor, share = occlusion_map(source, job, pose, width, height, lens, samples)
        np.save(out / ("%04d.occlusion.npy" % index), factor.astype(np.float32))
        background = scene.camera.component.background
        shade = np.round(255 * factor)[..., None] * np.ones(3)
        colour = np.array([background >> 16, (background >> 8) & 255, background & 255], dtype=float)
        shown = shade * share[..., None] + colour * (1.0 - share[..., None])
        Image.fromarray(np.round(shown).astype(np.uint8)).save(out / ("%04d.occlusion.png" % index))
    picture = device_picture(linear, covered, scene.tonemap_white, scene.camera.component.background)
    Image.fromarray(picture).save(out / ("%04d.png" % index))
    from r3d.process_budget import pss_bytes
    return os.getpid(), pss_bytes(os.getpid())


def reservation_pose_capacity(reservation, rss, estimate, cores):
    return max(1, min(cores, (reservation - rss) // estimate))


def render_poses(source, job, scene, poses, width, height, lens, samples, out, normals=False, workers=None, occlusion=False):
    """Return summed per-worker maxima of end-of-pose PSS samples and worker count.
    Caller must not have initialised CUDA. Each pose seeds its own RNG, matching serial output.
    """
    from r3d import process_budget
    from r3d.process_budget import available_bytes, worker_capacity, cores_available, FLOORS, parent_death_signal
    import os
    global POSE_STATE
    if workers is None:
        estimate = max(64 * 1024 ** 2, width * height * samples * samples * POSE_BASE_BYTES_PER_RAY)
        reservation = process_budget.task_reservation()
        if reservation is not None:
            rss = process_budget.resident_bytes(os.getpid(), {})[0]
            budget = process_budget.POSE_POOL_BYTES
            if reservation == process_budget.SMOKE_PREPARE_BYTES:
                budget = min(budget, reservation[0])
            workers = reservation_pose_capacity(budget, rss, estimate, cores_available())
        else:
            workers = worker_capacity(available_bytes(), (estimate, 0, estimate), FLOORS, cores_available())
            if not workers:
                raise RuntimeError("not enough available memory for a reference pose worker")
    workers = min(workers, len(poses))
    if workers < 1:
        raise ValueError("workers must be positive")
    if workers > 1 and "fork" not in multiprocessing.get_all_start_methods():
        workers = 1
    POSE_STATE = (source, job, scene, width, height, lens, samples, pathlib.Path(out), normals, occlusion)
    pose_peaks = {}
    try:
        if workers == 1:
            for item in enumerate(poses):
                pid, peak = _write_pose(item)
                pose_peaks[pid] = max(pose_peaks.get(pid, 0), peak)
        else:
            with concurrent.futures.ProcessPoolExecutor(max_workers=workers,
                    mp_context=multiprocessing.get_context("fork"),
                    initializer=parent_death_signal, initargs=(os.getpid(),)) as pool:
                for pid, peak in pool.map(_write_pose, enumerate(poses)):
                    pose_peaks[pid] = max(pose_peaks.get(pid, 0), peak)
    finally:
        POSE_STATE = None
    return sum(pose_peaks.values()), workers


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--object", help="the scene's mesh renderer to render, lit as it is baked (default: the first)")
    parser.add_argument("--poses", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--workers", type=int, help="pose workers; default: available CPU and memory budget")
    parser.add_argument("--samples", type=int, default=4)
    parser.add_argument("--skip", type=int, default=0, help="ignore this many leading poses")
    parser.add_argument("--normals", action="store_true", help="also write each pose's shading normals as NNNN.normal.npy")
    parser.add_argument("--occlusion", action="store_true",
                        help="also write each pose's local occlusion factor as NNNN.occlusion.png (white is open) and "
                             ".npy; the scene must set [bake].ao")
    parser.add_argument("--backend", choices=("bake", "mitsuba"), default="bake",
                        help="bake: the scene's bake lighting (default); mitsuba: a path-traced reference")
    parser.add_argument("--spp", type=int, default=mitsuba_reference.DEFAULT_SPP, help="mitsuba: paths per pixel")
    parser.add_argument("--max-depth", type=int, default=mitsuba_reference.DEFAULT_DEPTH,
                        help="mitsuba: path depth, 2 is direct light only")
    parser.add_argument("--seed", type=int, default=0, help="mitsuba: sampler seed")
    mitsuba_reference.add_options(parser)
    args = parser.parse_args(argv)
    if args.samples < 1 or args.skip < 0:
        parser.error("--samples must be positive and --skip cannot be negative")
    if args.backend == "mitsuba" and (args.spp < 1 or args.max_depth < 1):
        parser.error("--spp and --max-depth must be positive")
    if args.backend == "mitsuba" and args.normals:
        parser.error("--normals is a bake backend output")
    if args.occlusion and args.backend != "bake":
        parser.error("--occlusion is a bake backend output")
    if args.backend == "bake" and args.sky:
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
        source, job = source_for(scene, args.object, lit=args.backend == "bake")
    except ValueError as error:
        parser.error(str(error))
    path = None
    if args.backend == "mitsuba":
        path = mitsuba_reference.prepare(source, scene.lights, job.settings.double_sided, mitsuba_reference.sky_from(args),
                                         args.variant)
    if args.occlusion and job.bake.ao is None:
        parser.error("the scene sets no [bake].ao")
    if path is None:
        try:
            peak, workers = render_poses(source, job, scene, poses, width, height, lens, args.samples, out, args.normals, args.workers, args.occlusion)
        except ValueError as error:
            parser.error(str(error))
        print(f"worker reference_poses pid={os.getpid()} out={out} pose_peak_pss_bytes={peak} pose_workers={workers}", flush=True)
        return 0
    for index, pose in enumerate(poses):
        linear, covered = path.trace(pose, width, height, lens, near, args.spp, args.seed, args.max_depth)
        np.save(out / ("%04d.linear.npy" % index), linear)
        picture = device_picture(linear, covered, scene.tonemap_white, scene.camera.component.background)
        Image.fromarray(picture).save(out / ("%04d.png" % index))
    return 0


if __name__ == "__main__":
    sys.exit(main())

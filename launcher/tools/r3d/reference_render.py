#!/usr/bin/env python3
"""Render an undecimated import source with the scene's bake lighting.

    python launcher/tools/r3d/reference_render.py SCENE.scene.toml [--object NAME] --poses poses.txt --out DIR
        [--backend mitsuba --spp N --max-depth N [--sky hosek-wilkie]]

The poses file is the ``size``, ``lens`` and ``pose`` format emitted by
tools/anim/track_host.py.  Each pose writes a floating-point .npy image and
an RGB565-expanded PNG.  The renderer deliberately has no scene knowledge:
the scene supplies the object's source, lights, camera lens and pose path.

The default backend is the bake reference: the scene's bake lighting. ``--backend mitsuba`` traces the same
source, albedo, camera and lights as a path-traced reference (r3d.mitsuba_reference)
and shares the exposure, tone map and RGB565 conversion below. Only the bake backend reads `[bake].ao`.
"""

import argparse
import functools
import os
import multiprocessing
import concurrent.futures
import pathlib
import shutil
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import mesh_source, mitsuba_reference, ray_query
from r3d.cards import emit as card_emit
from r3d.geometry import corner_normals, hit_barycentrics
from r3d.import_settings import load_scene
from r3d.light import AlphaTest, albedo_from_uv, coincident_faces, light, open_side_occlusion, to_srgb8
from r3d.mesh_source import load_source, path_light_for, camera_filter
from r3d.poses import camera_rays, read_poses
from r3d.ray_query import RayQuery

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from colour_maths import expand_565  # noqa: E402


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
    if source.colors is not None:
        out *= (source.colors[source.tri_v[hits]] * bary[:, :, None]).sum(axis=1)
    return out


def render_linear(source, job, scene, pose, width, height, lens, samples=4):
    """Return a linear RGB source render, supersampled then box filtered."""
    return trace(source, job, scene, pose, width, height, lens, samples)[0]


def drawn_hits(source, double_materials, origin, direction):
    """(locations, ray index, triangle) of each ray's first hit the device draws, as visible_from_path finds it: a
    single-sided triangle seen from behind is culled (facing_away in r3d_triangle_tests.h) and the ray passes it,
    unless its twin wound the other way lies on it. Where a reduced source's two shells cross, the inner shell's back
    would otherwise show through the outer one."""
    active = np.ones(len(origin), bool)
    keep = camera_filter(source, alpha=False)
    # The first round yields even when every ray misses, so each list holds at least one array.
    found_at, found_ray, found_tri = [], [], []
    for tri, alive, where in source.intersector.hit_rounds(origin, direction, active):
        a, b, c = (source.p[source.tri_v[tri, corner]] for corner in range(3))
        front = (np.cross(b - a, c - a) * direction[alive]).sum(axis=1) < 0
        shown = np.where(front | np.isin(source.tri_m[tri], double_materials), tri, source.twin[tri])
        drawn = shown >= 0
        if keep is not None:
            drawn &= keep(tri, where)
        found_at.append(where[drawn])
        found_ray.append(alive[drawn])
        found_tri.append(shown[drawn])
        active[alive[drawn]] = False
    order = np.argsort(np.concatenate(found_ray))
    return tuple(np.concatenate(found)[order] for found in (found_at, found_ray, found_tri))


def primary_hits(source, job, pose, width, height, lens, samples):
    """The camera rays of a pose and what they hit, as drawn_hits finds it: (ray origins, directions, hit locations,
    the ray each hit belongs to, the source triangle hit, its bary-interpolated normal, its barycentrics, whether its
    material is double-sided)."""
    origin, direction = camera_rays(width, height, lens, pose[:3], pose[3:], samples)
    double_materials = [source.names.index(name) for name in job.settings.double_sided]
    locations, rays, faces = drawn_hits(source, double_materials, origin, direction)
    bary = hit_barycentrics(source.p, source.tri_v, faces, locations) if len(rays) else np.zeros((0, 3))
    normal = hit_normals(source, faces, bary) if len(rays) else np.zeros((0, 3))
    double = np.isin(source.tri_m[faces], double_materials)
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
                         source.bounce, ao=job.bake.ao,
                         bounce_intensity=scene.indirect.intensity if source.bounce is not None else 1.0)
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


def source_for(scene, name=None, lit=True, hide_cards=False, hidden_materials=None):
    """The full-detail source of the scene object `name` (the first mesh renderer
    when None), alpha-tested per hit with that renderer's bake lighting; returns it with
    the object's job. `lit=False` skips lighting queries and the bounced-light tracer,
    which only the bake backend uses. Textures are float32 unless selecting cards, whose masks still need rays.
    `hide_cards` hides the materials accepted by `geometry.cards` from camera rays only (see lit_source)."""
    named = [item for item in scene.renderers if name in (None, item.object.name)]
    if not named:
        raise ValueError(f"the scene places no mesh renderer named {name!r}")
    return lit_source(named[0], scene, lit, hide_cards=hide_cards, hidden_materials=hidden_materials), named[0]


def lit_source(job, scene, lit=True, hide_cards=False, hidden_materials=None, variant=None):
    """Full-detail alpha-tested source with bake lighting, its rays and bounced light traced on `variant`
    (`ray_query.VARIANT` when None). `hide_cards` passes accepted materials on camera rays; they still shadow and
    bounce light onto the surfaces behind them."""
    settings = job.settings
    selecting_cards = hide_cards and hidden_materials is None and settings.cards is not None
    source = load_source(settings, np.float64 if lit or selecting_cards else np.float32)
    source.camera_hidden = (camera_hidden(source, settings, scene, hidden_materials)
                            if hide_cards else np.zeros(len(source.tri_v), bool))
    source.alpha_test = AlphaTest(source, settings.alpha_keep) if settings.alpha_keep is not None else None
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.twin = coincident_faces(source.p, source.tri_v)[0]
    if not lit:
        return source
    source.intersector = RayQuery(source.p, source.tri_v, variant, hit_filter=source.alpha_test)
    source.bounce = path_light_for(source, job, scene, variant)
    return source


def camera_hidden(source, settings, scene=None, hidden_materials=None):
    """Per source triangle, whether an accepted card material hides it on camera rays."""
    if hidden_materials is None:
        accepted, _ = card_emit.carded(source, settings, scene)
        hidden_materials = [source.names[material] for material, _, _ in accepted]
    return np.isin(source.tri_m, [source.names.index(name) for name in hidden_materials])


@functools.cache
def reference_variant():
    """The variant a reference set traces on: the fastest this machine runs (mitsuba_reference.default_variant),
    `cuda_ad_rgb` where Mitsuba loads a scene on the GPU, else `llvm_ad_rgb`; `ray_query.VARIANT` without Mitsuba,
    where the ray query names what to install. Mitsuba is left on the variant it was on."""
    mi = mitsuba_reference.import_mitsuba()
    if mi is None:
        return ray_query.VARIANT
    current = mi.variant()
    try:
        return mitsuba_reference.default_variant(mi)
    finally:
        if current is not None:
            mi.set_variant(current)


# A pose's files, as a set names them after the pose's index and a pose store after POSE_NAME.
POSE_SUFFIXES = (".png", ".linear.npy", ".normal.npy")
POSE_NAME = "pose"


def link_or_copy(source, target):
    target.unlink(missing_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def render_sets(inputs, sets, samples=4, store=None):
    """Renders each (poses file, folder) of `sets` with normals from `inputs` alone (a
    reference_set.ReferenceInputs) on `reference_variant()`. Given a pose `store` (bake/produce.PoseStore), each pose
    is traced once per store folder: a pose the store holds is taken from it, the others are traced and put in it as
    each finishes, so a run cut short keeps the poses it traced, and each set folder is assembled from the store.
    The poses to trace go to `gpu_workers` processes on CUDA, else to this process (render_poses); each pose is
    traced alone either way, so its files do not depend on which process traced it."""
    if store is not None:
        from bake.produce import DONE, read_references
    plans = []
    for poses_path, out in sets:
        out = pathlib.Path(out)
        width, height, lens, near, poses = read_poses(poses_path)
        out.mkdir(parents=True, exist_ok=True)
        folders = [None] * len(poses) if store is None else [store.folder(width, height, lens, near, pose)
                                                             for pose in poses]
        missing = [index for index, folder in enumerate(folders) if folder is None or not (folder / DONE).is_file()]
        target = out if store is None else out / ".tracing"
        if missing:
            target.mkdir(exist_ok=True)
        plans.append((out, target, folders, missing, [poses[index] for index in missing], (width, height, lens)))

    def traced(plan, index):
        """Puts the `index`th traced pose of `plan` in the store."""
        _out, target, folders, missing, _poses, _size = plan
        if store is not None:
            store.keep(folders[missing[index]], lambda folder: [
                os.replace(target / f"{index:04d}{suffix}", folder / f"{POSE_NAME}{suffix}") for suffix in POSE_SUFFIXES])

    workers = gpu_workers(sum(len(plan[3]) for plan in plans))
    (trace_in_workers if workers > 1 else trace_here)(inputs, plans, samples, traced, workers)
    if store is None:
        return
    for out, target, folders, _missing, _poses, _size in plans:
        shutil.rmtree(target, ignore_errors=True)
        for index, folder in enumerate(folders):
            read_references(folder)
            for suffix in POSE_SUFFIXES:
                link_or_copy(folder / f"{POSE_NAME}{suffix}", out / f"{index:04d}{suffix}")


def gpu_workers(poses):
    """How many CUDA processes trace `poses` poses side by side: the trace is NumPy-bound in each process, so one
    leaves the GPU mostly idle (README, "Reference on the GPU"). As many as the free host and GPU memory hold at
    process_budget.REFERENCE_GPU_BYTES each; 1 off CUDA, where render_poses chooses its own pool, and off Linux, whose
    /proc the free memory is read from."""
    if poses < 2 or sys.platform != "linux" or not reference_variant().startswith("cuda"):
        return 1
    from r3d.process_budget import FLOORS, REFERENCE_GPU_BYTES, available_bytes, cores_available, worker_capacity
    return max(1, min(poses, worker_capacity(available_bytes(gpu=True), REFERENCE_GPU_BYTES, FLOORS, cores_available())))


def trace_here(inputs, plans, samples, traced, workers=1):
    """The poses of `plans` traced in this process, the source loaded and its bounced light built once for all of
    them only when one is missing; traced(plan, index) as each pose's files are written. The bounced-light tracer,
    which holds the source's textures a second time, is dropped before it returns, for the fit that follows in the
    process."""
    job, scene = inputs.job(), inputs.scene()
    source = None
    try:
        for plan in plans:
            out, target, folders, _missing, poses, (width, height, lens) = plan
            if not poses:
                continue
            if source is None:
                source = lit_source(job, scene, hide_cards=True, variant=reference_variant())
            peak, used = render_poses(source, job, scene, poses, width, height, lens, samples, target, normals=True,
                                      done=lambda index, plan=plan: traced(plan, index))
            print(f"worker reference_poses pid={os.getpid()} out={out} traced={len(poses)} of {len(folders)} "
                  f"pose_peak_pss_bytes={peak} pose_workers={used}", flush=True)
    finally:
        del source
        mesh_source.PATH_LIGHTS.clear()


def trace_in_workers(inputs, plans, samples, traced, workers):
    """The poses of `plans` traced by `workers` spawned processes (CUDA does not survive a fork), each loading the
    source once and taking the next pose as it finishes one; traced(plan, index) as each pose's result returns, in
    the order they finish."""
    tasks = [(plan, index) for plan in plans for index in range(len(plan[4]))]
    peaks = {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers, mp_context=multiprocessing.get_context("spawn"),
                                                initializer=_load_worker, initargs=(inputs, samples, os.getpid())) as pool:
        futures = {pool.submit(_trace_task, (plan[1], index, plan[4][index], plan[5])): (plan, index)
                   for plan, index in tasks}
        for future in concurrent.futures.as_completed(futures):
            pid, peak = future.result()
            traced(*futures[future])
            peaks[pid] = max(peaks.get(pid, 0), peak)
    print(f"worker reference_poses pid={os.getpid()} traced={len(tasks)} gpu_workers={workers} "
          f"pose_peak_pss_bytes={sum(peaks.values())}", flush=True)


WORKER_STATE = None


def _load_worker(inputs, samples, parent_pid):
    global WORKER_STATE
    from r3d.process_budget import parent_death_signal
    parent_death_signal(parent_pid)
    job, scene = inputs.job(), inputs.scene()
    WORKER_STATE = (lit_source(job, scene, hide_cards=True, variant=reference_variant()), job, scene, samples)


def _trace_task(task):
    global POSE_STATE
    target, index, pose, (width, height, lens) = task
    source, job, scene, samples = WORKER_STATE
    POSE_STATE = pose_state(source, job, scene, width, height, lens, samples, target)
    return _write_pose((index, pose))


# Estimated bytes per ray: trace/hit buffers 256, ray-query/lighting scratch 256,
# sky tangents, samples, directions and temporaries 256.
POSE_BASE_BYTES_PER_RAY = 768
POSE_STATE = None


def pose_state(source, job, scene, width, height, lens, samples, out, normals=True, occlusion=False):
    """What _write_pose reads, set once per set of poses in the process that writes them."""
    return source, job, scene, width, height, lens, samples, pathlib.Path(out), normals, occlusion


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


def budget_pose_capacity(budget, rss, estimate, cores):
    return max(1, min(cores, (budget - rss) // estimate))


def render_poses(source, job, scene, poses, width, height, lens, samples, out, normals=False, workers=None, occlusion=False,
                 done=None):
    """Return summed per-worker maxima of end-of-pose PSS samples and worker count.
    Every pose's rays are fixed, so the output matches a serial run. done(index) is called as each pose's files
    are written, in order.
    """
    from r3d import process_budget
    from r3d.process_budget import available_bytes, worker_capacity, cores_available, FLOORS, parent_death_signal
    import os
    global POSE_STATE
    on_gpu = getattr(getattr(source, "intersector", None), "variant", "").startswith("cuda")
    if on_gpu or getattr(source, "bounce", None) is not None:
        # One process traces every pose on the GPU, where forked workers would share it and only add memory. On the
        # CPU, Mitsuba's LLVM threads trace one pose on every core, which a forked worker cannot (it traces on one
        # thread and holds its own copy of the scene, which ran a 15 GB machine out of memory), so a pose with
        # bounced light renders in this process too.
        if workers not in (None, 1):
            raise ValueError("poses on the GPU or with bounced light render in the main process; leave --workers unset")
        workers = 1
    budget_capacity = None
    if workers is None:
        estimate = max(64 * 1024 ** 2, width * height * samples * samples * POSE_BASE_BYTES_PER_RAY)
        reservation = process_budget.task_reservation()
        if reservation is not None:
            rss = process_budget.resident_bytes(os.getpid(), {})[0]
            cores = cores_available()
            budget_capacity = budget_pose_capacity(reservation[0], rss, estimate, cores)
            memory_capacity = worker_capacity(available_bytes(), (estimate, 0, estimate), FLOORS, cores)
            workers = max(1, min(budget_capacity, memory_capacity))
        else:
            workers = worker_capacity(available_bytes(), (estimate, 0, estimate), FLOORS, cores_available())
            if not workers:
                raise RuntimeError("not enough available memory for a reference pose worker")
    workers = min(workers, len(poses))
    if workers < 1:
        raise ValueError("workers must be positive")
    if workers > 1 and "fork" not in multiprocessing.get_all_start_methods():
        workers = 1
    if budget_capacity is not None:
        print(f"pose pool workers={workers} budget_capacity={budget_capacity} memory_capacity={memory_capacity} rss={rss}", flush=True)
    POSE_STATE = pose_state(source, job, scene, width, height, lens, samples, out, normals, occlusion)
    pose_peaks = {}
    try:
        if workers == 1:
            for index, pose in enumerate(poses):
                pid, peak = _write_pose((index, pose))
                pose_peaks[pid] = max(pose_peaks.get(pid, 0), peak)
                if done is not None:
                    done(index)
        else:
            with concurrent.futures.ProcessPoolExecutor(max_workers=workers,
                    mp_context=multiprocessing.get_context("fork"),
                    initializer=parent_death_signal, initargs=(os.getpid(),)) as pool:
                for index, (pid, peak) in enumerate(pool.map(_write_pose, enumerate(poses))):
                    pose_peaks[pid] = max(pose_peaks.get(pid, 0), peak)
                    if done is not None:
                        done(index)
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
    parser.add_argument("--hide-cards", action="store_true",
                        help="camera rays pass materials accepted by geometry.cards, which still shadow and bounce light, "
                             "as in the fit's references")
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
    if args.hide_cards and args.backend != "bake":
        parser.error("--hide-cards is a bake backend option")
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
        source, job = source_for(scene, args.object, lit=args.backend == "bake", hide_cards=args.hide_cards)
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

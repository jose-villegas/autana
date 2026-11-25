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
import pathlib
import shutil
import sys
from typing import NamedTuple

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import mesh_source, mitsuba_reference, ray_query
from r3d.cards import emit as card_emit
from r3d.geometry import corner_normals
from r3d.import_settings import load_scene
from r3d.light import (AlphaTest, coincident_faces, device_light, device_occlusion, surface_table, to_srgb8,
                       turned_frame)
from r3d.mesh_source import load_source, path_light_for, camera_filter
from r3d.poses import camera_rays, read_poses
from r3d.ray_query import RayQuery, chunks, joined, rows, vectors

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from colour_maths import expand_565  # noqa: E402


def hit_albedo(source, hits, bary):
    """Each NumPy hit's source albedo at its barycentric weights (SurfaceTable.albedo), on the source's ray query's
    variant, in launches of at most LAUNCH hits."""
    mi = source.intersector.mi
    table = surface_table(source, mi)
    hits, bary = np.asarray(hits, np.uint32), np.asarray(bary, np.float32)
    albedo = np.zeros((len(hits), 3))
    for start, size in chunks(len(hits), ray_query.LAUNCH):
        part = slice(start, start + size)
        albedo[part] = rows(table.albedo(mi.UInt32(hits[part]), [mi.Float(bary[part, corner]) for corner in range(3)],
                                         mi.Bool(True)), size)
    return albedo


def render_linear(source, job, scene, pose, width, height, lens, samples):
    """Return a linear RGB source render, supersampled then box filtered."""
    return trace(source, job, scene, pose, width, height, lens, samples)[0]


class DrawnHits(NamedTuple):
    """The camera rays' drawn hits: the index of each ray that has one (ascending), its location, the source triangle
    drawn there, its barycentric weights, its interpolated shading normal (None without corner normals) and whether
    its material is double-sided. Dr.Jit arrays from device_drawn_hits, NumPy rows from drawn_hit_rows."""
    rays: object
    where: object
    faces: object
    bary: object
    normal: object
    double: object


def device_drawn_hits(source, double_materials, origin, direction):
    """Each camera ray's first hit the device draws, as visible_from_path finds it: a single-sided triangle seen from
    behind is culled (facing_away in r3d_triangle_tests.h) and the ray passes it, unless its twin wound the other way
    lies on it. Where a reduced source's two shells cross, the inner shell's back would otherwise show through the
    outer one. Hidden card materials (camera_filter) pass too."""
    import drjit as dr

    query = source.intersector
    mi = query.mi
    table = surface_table(source, mi)
    double = table.upload(("double", tuple(double_materials)),
                          np.isin(np.arange(len(source.textures)), double_materials))
    keep = camera_filter(source, alpha=False)
    keep = None if keep is None else keep.device(query)

    def shown(prim, d, active):
        a, b, c = query.corners(prim, active)
        front = dr.dot(dr.cross(b - a, c - a), d) < 0
        drawn = front | dr.gather(mi.Bool, double, dr.gather(mi.UInt32, table.tri_m, prim, active), active)
        twin = mi.Int32(-1) if table.twin is None else dr.gather(mi.Int32, table.twin, prim, active)
        return dr.select(drawn, mi.Int32(prim), twin)

    def passes(prim, where, d, active):
        through = shown(prim, d, active) < 0
        return through if keep is None else through | ~keep(prim, where, active)

    ray = query.rays(origin, direction)
    hit = query.device_hits(ray, passes=passes)
    rays = dr.compress(hit.valid)
    if not dr.width(rays):
        empty = mi.Float()
        return DrawnHits(rays, mi.Point3f(), mi.UInt32(), (empty, empty, empty),
                         None if table.normals is None else mi.Vector3f(), mi.Bool())
    where = dr.gather(type(hit.where), hit.where, rays)
    faces = mi.UInt32(shown(dr.gather(mi.UInt32, hit.prim, rays), dr.gather(type(ray.d), ray.d, rays), True))
    bary = query.barycentrics(faces, where)
    active = dr.full(mi.Bool, True, dr.width(rays))
    normal = table.normal(faces, bary, active) if table.normals is not None else None
    hits_double = dr.gather(mi.Bool, double, dr.gather(mi.UInt32, table.tri_m, faces))
    return DrawnHits(rays, where, faces, bary, normal, hits_double)


def drawn_hit_rows(source, double_materials, origin, direction):
    """device_drawn_hits for NumPy rays, in launches of at most LAUNCH rays, as NumPy DrawnHits."""
    import drjit as dr

    parts = []
    for start, size in chunks(len(origin), ray_query.LAUNCH):
        found = device_drawn_hits(source, double_materials, origin[start:start + size], direction[start:start + size])
        count = dr.width(found.rays)
        parts.append(DrawnHits(np.array(found.rays, np.int64).reshape(-1) + start, rows(found.where, count),
                               np.array(found.faces, np.int64).reshape(-1), rows(found.bary, count),
                               None if found.normal is None else rows(found.normal, count),
                               np.array(found.double, bool).reshape(-1)))
    if not parts:
        return DrawnHits(np.zeros(0, np.int64), np.zeros((0, 3)), np.zeros(0, np.int64), np.zeros((0, 3)),
                         None, np.zeros(0, bool))
    return DrawnHits(*(None if field[0] is None else np.concatenate(field) for field in zip(*parts)))


def drawn_hits(source, double_materials, origin, direction):
    """drawn_hit_rows as (locations, ray index, triangle)."""
    found = drawn_hit_rows(source, double_materials, origin, direction)
    return found.where, found.rays, found.faces


def primary_hit_rows(source, job, pose, width, height, lens, samples):
    """The camera rays of a pose and their NumPy DrawnHits."""
    origin, direction = camera_rays(width, height, lens, pose[:3], pose[3:], samples)
    double_materials = [source.names.index(name) for name in job.settings.double_sided]
    return origin, direction, drawn_hit_rows(source, double_materials, origin, direction)


def device_primary_hits(source, job, pose, width, height, lens, samples):
    """The camera rays of a pose (NumPy), their drawn hits and each hit's albedo, on the device: found and shaded in
    launches of at most LAUNCH rays and joined, each hit's ray counted over the whole pose. None and None where no
    ray hits."""
    import drjit as dr

    origin, direction = camera_rays(width, height, lens, pose[:3], pose[3:], samples)
    double_materials = [source.names.index(name) for name in job.settings.double_sided]
    mi = source.intersector.mi
    table = surface_table(source, mi)
    hits, albedo = [], []
    for start, size in chunks(len(origin), ray_query.LAUNCH):
        found = device_drawn_hits(source, double_materials, origin[start:start + size], direction[start:start + size])
        if dr.width(found.rays):
            hits.append(found._replace(rays=found.rays + start))
            albedo.append(table.albedo(found.faces, found.bary, mi.Bool(True)))
    if not hits:
        return origin, direction, None, None
    found = DrawnHits(*(None if field[0] is None else tuple(map(joined, zip(*field))) if isinstance(field[0], tuple)
                        else joined(field) for field in zip(*hits)))
    return origin, direction, found, joined(albedo)


def primary_hits(source, job, pose, width, height, lens, samples):
    """The camera rays of a pose and what they hit, as drawn_hits finds it: (ray origins, directions, hit locations,
    the ray each hit belongs to, the source triangle hit, its bary-interpolated normal, its barycentrics, whether its
    material is double-sided)."""
    origin, direction, found = primary_hit_rows(source, job, pose, width, height, lens, samples)
    return origin, direction, found.where, found.rays, found.faces, found.normal, found.bary, found.double


def trace(source, job, scene, pose, width, height, lens, samples, denoise=False):
    """(linear RGB, share of each pixel's subpixels that hit the mesh, the
    world-space shading normal per pixel turned toward the eye, averaged over
    the pixel's rays and zero where they all miss). The pose stays on the device from its camera rays to each hit's
    colour (device_primary_hits, device_light); the light is gathered over every hit of the pose at once, so a hit's
    bounce samples depend on its place among them and not on how the camera rays were split. Every hemisphere term
    turns its directions per point (light.turned_frame); with `denoise` the colour passes through `denoised`."""
    origin, direction, found, albedo = device_primary_hits(source, job, pose, width, height, lens, samples)
    linear = np.zeros((len(origin), 3), dtype=float)
    covered = np.zeros(len(origin))
    shading = np.zeros((len(origin), 3), dtype=float)
    surface = np.zeros((len(origin), 3), dtype=float)
    if found is not None:
        radiance = device_light(found.where, found.normal, found.double, source.intersector, scene.lights,
                                job.bake.ray_offset, source.bounce, ao=job.bake.ao,
                                bounce_intensity=scene.indirect.intensity if source.bounce is not None else 1.0,
                                frame=turned_frame)
        rays = np.array(found.rays, np.int64).reshape(-1)
        normal = rows(found.normal, len(rays))
        covered[rays] = 1.0
        linear[rays] = rows(albedo * radiance, len(rays))
        surface[rays] = rows(albedo, len(rays))
        shading[rays] = normal * np.where((normal * direction[rays]).sum(axis=1) > 0, -1.0, 1.0)[:, None]
    shape = (height, width, samples * samples, 3)
    shading = shading.reshape(shape).sum(axis=2)
    shading = shading / np.maximum(np.linalg.norm(shading, axis=2, keepdims=True), 1e-12)
    linear = linear.reshape(shape).mean(axis=2)
    if denoise:
        linear = denoised(linear, surface.reshape(shape).mean(axis=2), shading)
    return linear, covered.reshape(height, width, samples * samples).mean(axis=2), shading


def denoised(linear, albedo, normal):
    """`linear` through Open Image Denoise's ray-tracing filter (OIDN, pinned in requirements.txt) on the CPU,
    guided by the pixel's albedo and shading normal. The guides come from the primary rays alone, which carry no
    sampling noise, so OIDN takes them as they are (clean_aux) and only the colour is filtered."""
    import pyoidn

    def image(values):
        return np.ascontiguousarray(values, dtype=np.float32)

    out = pyoidn.denoise(image(linear), image(np.clip(albedo, 0.0, 1.0)), image(normal), hdr=True, clean_aux=True,
                         quality="high")
    return np.maximum(out.astype(np.float64), 0.0)


def occlusion_map(source, job, pose, width, height, lens, samples):
    """(the scene's local occlusion factor per pixel, 1 where open and where nothing is hit; the share of each pixel's
    subpixels that hit the mesh), box filtered like trace(). It is the factor `[bake].ao` scales the ambient and bounce
    light by, so the scene must set it."""
    if job.bake.ao is None:
        raise ValueError("the scene sets no [bake].ao")
    origin, _direction, found = primary_hit_rows(source, job, pose, width, height, lens, samples)
    factor = np.ones(len(origin))
    covered = np.zeros(len(origin))
    if len(found.rays):
        mi = source.intersector.mi
        covered[found.rays] = 1.0
        factor[found.rays] = np.array(device_occlusion(source.intersector, vectors(mi.Point3f, found.where),
                                                       vectors(mi.Vector3f, found.normal), mi.Bool(found.double),
                                                       job.bake.ao, job.bake.ray_offset, turned_frame),
                                      np.float64).reshape(-1)
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
    `cuda_ad_rgb` where Mitsuba loads a scene on the GPU, else `llvm_ad_rgb`; `ray_query.VARIANT` without Mitsuba or
    without a JIT variant, where the ray query names what to install. Mitsuba is left on the variant it was on."""
    mi = mitsuba_reference.import_mitsuba()
    if mi is None:
        return ray_query.VARIANT
    current = mi.variant()
    try:
        found = mitsuba_reference.default_variant(mi)
        return found if ray_query.is_jit(found) else ray_query.VARIANT
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


def render_sets(inputs, sets, store=None, progress=None):
    """Renders each (poses file, folder) of `sets` with normals from `inputs` alone (a
    reference_set.ReferenceInputs: its `samples` rays per pixel side, denoised when it says so) from
    `reference_source`. Given a pose `store` (bake/produce.PoseStore), each pose
    is traced once per store folder: a pose the store holds is taken from it, the others are traced and put in it as
    each finishes, so a run cut short keeps the poses it traced, and each set folder is assembled from the store.
    The poses are traced in this process (trace_here). progress() runs once per traced pose, store or not: a caller
    under a stage guard passes its progress call."""
    if store is not None:
        from bake.produce import DONE, keyed_folder, read_references
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
        """Puts the `index`th traced pose of `plan` in the store, then tells progress."""
        _out, target, folders, missing, _poses, _size = plan
        if store is not None:
            keyed_folder(folders[missing[index]], lambda folder: [
                os.replace(target / f"{index:04d}{suffix}", folder / f"{POSE_NAME}{suffix}") for suffix in POSE_SUFFIXES])
        if progress is not None:
            progress()

    trace_here(inputs, plans, traced)
    if store is None:
        return
    for out, target, folders, _missing, _poses, _size in plans:
        shutil.rmtree(target, ignore_errors=True)
        for index, folder in enumerate(folders):
            read_references(folder)
            for suffix in POSE_SUFFIXES:
                link_or_copy(folder / f"{POSE_NAME}{suffix}", out / f"{index:04d}{suffix}")


def reference_source(job, scene):
    """The source every pose of a reference set is traced from: lit, its accepted card materials hidden from camera
    rays (the fit makes them), on `reference_variant()`."""
    return lit_source(job, scene, hide_cards=True, variant=reference_variant())


def trace_here(inputs, plans, traced):
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
                source = reference_source(job, scene)
            peak = render_poses(source, job, scene, poses, width, height, lens, inputs.samples, target,
                                normals=REFERENCE_NORMALS, done=lambda index, plan=plan: traced(plan, index),
                                denoise=inputs.denoise)
            print(f"worker reference_poses pid={os.getpid()} out={out} traced={len(poses)} of {len(folders)} "
                  f"pose_peak_pss_bytes={peak}", flush=True)
    finally:
        del source
        mesh_source.PATH_LIGHTS.clear()


# A reference pose writes its normals beside its picture: the fit reads both.
REFERENCE_NORMALS = True


def write_pose(source, job, scene, width, height, lens, samples, out, normals, occlusion, denoise, index, pose):
    """Trace one pose and write its files under `out`."""
    linear, covered, normal = trace(source, job, scene, pose, width, height, lens, samples, denoise)
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


def render_poses(source, job, scene, poses, width, height, lens, samples, out, normals=False, occlusion=False,
                 done=None, denoise=False):
    """Trace the poses one after another in this process, which keeps the device busy alone: on the GPU a launch
    fills it, on the CPU Mitsuba's LLVM threads trace one pose on every core. done(index) is called as each pose's
    files are written. Returns the process's peak PSS after a pose."""
    from r3d.process_budget import pss_bytes

    out = pathlib.Path(out)
    peak = 0
    for index, pose in enumerate(poses):
        write_pose(source, job, scene, width, height, lens, samples, out, normals, occlusion, denoise, index, pose)
        peak = max(peak, pss_bytes(os.getpid()))
        if done is not None:
            done(index)
    return peak


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--object", help="the scene's mesh renderer to render, lit as it is baked (default: the first)")
    parser.add_argument("--poses", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--samples", type=int, default=4, help="rays per pixel side (default 4)")
    parser.add_argument("--denoise", action="store_true",
                        help="pass each pose's colour through Open Image Denoise, as a fit recipe with denoise does")
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
            peak = render_poses(source, job, scene, poses, width, height, lens, args.samples, out, args.normals,
                                args.occlusion, denoise=args.denoise)
        except ValueError as error:
            parser.error(str(error))
        print(f"worker reference_poses pid={os.getpid()} out={out} pose_peak_pss_bytes={peak}", flush=True)
        return 0
    for index, pose in enumerate(poses):
        linear, covered = path.trace(pose, width, height, lens, near, args.spp, args.seed, args.max_depth)
        np.save(out / ("%04d.linear.npy" % index), linear)
        picture = device_picture(linear, covered, scene.tonemap_white, scene.camera.component.background)
        Image.fromarray(picture).save(out / ("%04d.png" % index))
    return 0


if __name__ == "__main__":
    sys.exit(main())

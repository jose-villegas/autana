#!/usr/bin/env python3
"""Fit a renderer against a fitting-friendly copy of its training references, which hold no texture detail
smaller than the triangle its budget affords (fit/footprint.py).

    friendly_reference.py SCENE.scene.toml --object NAME --mode MODE --out DIR [--cache DIR] [--no-fit]

The reference modes make the training references (train and landscape poses) of the locked reference set anew:

- atrous filters the traced pictures, no re-trace: edge-avoiding a-trous wavelet passes, stopped at source part
  borders, normal creases and off-plane taps, each pixel taking as many passes as the affordable triangle is
  wide there (Dammertz et al. 2010, with a part stop and a plane distance in place of their colour stop);
- budget re-traces them with each textured material read at the mip level of the affordable triangle;
- average re-traces them with each texture flattened to its mean;
- locked fits the locked set unchanged: the control, whose spread from the bake is the fit's own noise.

The start modes fit the locked set from a start made anew, whose simplifier sees each vertex's albedo as the
budget mode reads it (start-budget) or as its material's mean (start-average); light is as the bake's:

- start-budget reads each texture at the affordable triangle's edge where the vertex spacing is finer;
- start-average reads it at its coarsest level.

DIR/start/ holds that start; with --no-fit it is the only output.

The shader modes change only the albedo the camera rays see: the bounced light is built before, from the full
textures, as the locked set's is. The held-out references are the locked set's, unchanged, so

    objects_report.py SCENE --object NAME --out DIR2 --held-out --fitted DIR/FITTED

scores the fit against the unfiltered trace. DIR gets inputs/ (the friendly reference set) and the fitted mesh
the bake would write; an inputs/ already there is reused, so --no-fit and a later fit split the work. The
renderer's start and reference set come from launcher/bakes.lock via the bake cache.
"""

import argparse
import pathlib
import shutil
import sys
from typing import NamedTuple

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from r3d import log  # noqa: E402
from r3d.fit.footprint import affordable_area, mip_levels  # noqa: E402
from r3d.fit.stage import TRAINING  # noqa: E402

MODES = ("atrous", "budget", "average", "locked", "start-budget", "start-average")
# A-trous: the B3-spline taps of one pass, and how many passes at most (taps up to 2^(LEVELS-1) pixels apart).
B3 = np.array([1.0, 4.0, 6.0, 4.0, 1.0]) / 16.0
ATROUS_LEVELS = 5
# A tap's weight is its normal's cosine to the pixel's to this power (Dammertz's normal stop, as a power)...
NORMAL_POWER = 64
# ...and falls to 1/e when its point stands this many affordable triangle edges off the pixel's tangent plane.
PLANE_SIGMA = 0.25
# The pictures are filtered in this power of their 8-bit values, which reference_render's tone map raised.
PICTURE_GAMMA = 2.2


class Guide(NamedTuple):
    """Per pixel of a reference, what its centre ray hits on the source."""
    part: np.ndarray       # [h, w] part label (objects/object_error.part_labels), -1 where nothing is hit
    point: np.ndarray      # [h, w, 3] world hit point
    normal: np.ndarray     # [h, w, 3] shading normal
    passes: np.ndarray     # [h, w] a-trous passes whose support reaches half the affordable triangle's width


def passes_for(footprint):
    """A-trous passes whose combined radius 2 (2^n - 1) reaches half of `footprint` pixels."""
    return np.clip(np.ceil(np.log2(np.maximum(footprint, 0.0) / 4.0 + 1.0)), 0, ATROUS_LEVELS).astype(np.int64)


def guide(source, job, labels, edge, pose, width, height, lens):
    """The Guide of one pose: its rays as reference_render's primary hits, one per pixel."""
    from r3d.poses import camera_basis, focal_px
    from r3d.reference_render import primary_hits

    _, _, locations, rays, faces, normal, _, _ = primary_hits(source, job, pose, width, height, lens, 1)
    part = np.full(width * height, -1, dtype=np.int64)
    point = np.zeros((width * height, 3))
    normals = np.zeros((width * height, 3))
    footprint = np.zeros(width * height)
    part[rays], point[rays], normals[rays] = labels[faces], locations, normal
    depth = (locations - np.asarray(pose[:3])) @ camera_basis(pose[3:])[2]
    footprint[rays] = edge * focal_px(width, height, lens) / np.maximum(depth, 1e-9)
    shape = (height, width)
    return Guide(part.reshape(shape), point.reshape(shape + (3,)), normals.reshape(shape + (3,)),
                 passes_for(footprint).reshape(shape))


def shifted(values, dy, dx, fill):
    """`values` moved by (dy, dx) pixels: out[y, x] = values[y + dy, x + dx], `fill` outside."""
    out = np.full_like(values, fill)
    h, w = values.shape[:2]
    ys, yd = slice(max(dy, 0), h + min(dy, 0)), slice(max(-dy, 0), h + min(-dy, 0))
    xs, xd = slice(max(dx, 0), w + min(dx, 0)), slice(max(-dx, 0), w + min(-dx, 0))
    out[yd, xd] = values[ys, xs]
    return out


def atrous(colour, guide, edge):
    """`colour` [h, w, 3] filtered within each source part: each pixel keeps the result of its own pass count.
    Pixels that hit nothing stay as they are and feed nothing."""
    hit = guide.part >= 0
    current, result = colour.copy(), colour.copy()
    for level in range(ATROUS_LEVELS):
        step = 2 ** level
        total, weights = np.zeros_like(colour), np.zeros(colour.shape[:2])
        for ky, wy in enumerate(B3):
            for kx, wx in enumerate(B3):
                dy, dx = (ky - 2) * step, (kx - 2) * step
                part = shifted(guide.part, dy, dx, -1)
                cosine = (shifted(guide.normal, dy, dx, 0.0) * guide.normal).sum(-1)
                off_plane = ((shifted(guide.point, dy, dx, 0.0) - guide.point) * guide.normal).sum(-1)
                weight = wy * wx * (part == guide.part) * hit * np.maximum(cosine, 0.0) ** NORMAL_POWER \
                    * np.exp(-(off_plane / (PLANE_SIGMA * edge)) ** 2)
                total += weight[..., None] * shifted(current, dy, dx, 0.0)
                weights += weight
        current = np.where((weights > 0)[..., None], total / np.maximum(weights, 1e-12)[..., None], current)
        result[guide.passes == level + 1] = current[guide.passes == level + 1]
    return result


def filter_references(source, job, labels, edge, poses_path, reference, out):
    """The atrous mode's pictures of one training folder; the normals are the reference's own."""
    from r3d.poses import read_poses
    from r3d.reference_render import expand_565

    width, height, lens, _near, poses = read_poses(poses_path)
    out.mkdir(parents=True)
    for index, pose in enumerate(poses):
        name = "%04d" % index
        picture = np.asarray(Image.open(reference / f"{name}.png").convert("RGB"), dtype=np.float64) / 255.0
        filtered = atrous(picture ** PICTURE_GAMMA, guide(source, job, labels, edge, pose, width, height, lens), edge)
        srgb = np.round(255.0 * np.clip(filtered, 0.0, 1.0) ** (1.0 / PICTURE_GAMMA)).astype(np.uint8)
        Image.fromarray(expand_565(srgb)).save(out / f"{name}.png")
        shutil.copyfile(reference / f"{name}.normal.npy", out / f"{name}.normal.npy")


def shade(source, mode, area):
    """Rewrites the source's textures as `mode` reads them: from the affordable triangle's mip level up, or one
    texel of their mean."""
    levels = mip_levels(source, area) if mode == "budget" else None
    for material, texture in enumerate(source.textures):
        if texture is None:
            continue
        if mode == "average":
            texture.levels = [texture.levels[0].mean(axis=(0, 1), keepdims=True)]
        elif levels.get(material):
            texture.levels = texture.levels[levels[material]:]
    if levels is not None:
        log("mip levels: " + ", ".join(f"{source.names[m]} {level}" for m, level in sorted(levels.items()) if level))


def trace_references(inputs, mode, area, sets):
    """The shader modes' pictures: reference_set.prepare_references' trace of `sets` with the textures shaded
    after the bounced light is built."""
    from r3d import mesh_source, ray_query
    from r3d.poses import read_poses
    from r3d.reference_render import lit_source, render_poses

    job, scene = inputs.job(), inputs.scene()
    variant = ray_query.trace_variant()
    source = lit_source(job, scene, variant=variant)
    shade(source, mode, area)
    try:
        for poses_path, out in sets:
            width, height, lens, _near, poses = read_poses(poses_path)
            out.mkdir(parents=True)
            render_poses(source, job, scene, poses, width, height, lens, 4, out, normals=True, workers=1)
    finally:
        if variant == ray_query.GPU_VARIANT:
            del source
            mesh_source.PATH_LIGHTS.clear()
            ray_query.release_gpu()


def friendly_set(scene, job, mode, locked, out):
    """`out`: the locked reference set with its training references made by `mode`."""
    from r3d.objects.object_error import part_labels
    from r3d.reference_render import lit_source
    from r3d.reference_set import reference_inputs

    # The training folders are made anew; the rest is linked from the locked set.
    out.mkdir(parents=True)
    made = {folder for _poses, folder in TRAINING}
    for item in locked.iterdir():
        if item.name not in made:
            (out / item.name).symlink_to(item.resolve(), target_is_directory=item.is_dir())
    sets = [(locked / poses, out / folder) for poses, folder in TRAINING]
    if mode == "atrous":
        source = lit_source(job, scene, lit=False)
        from r3d.ray_query import RayQuery

        source.intersector = RayQuery(np.asarray(source.p, dtype=np.float64), source.tri_v)
        area = affordable_area(source.p, source.tri_v, job.renderer.fit.budget)
        labels, _shared = part_labels(np.asarray(source.p, dtype=np.float64), source.tri_v, source.tri_o)
        log(f"affordable triangle edge {np.sqrt(area):.4g}")
        for (poses, reference), (_, folder) in zip(sets, TRAINING):
            filter_references(source, job, labels, np.sqrt(area), poses, locked / folder, reference)
    else:
        source = lit_source(job, scene, lit=False)
        area = affordable_area(source.p, source.tri_v, job.renderer.fit.budget)
        del source
        trace_references(reference_inputs(job, scene), mode, area, sets)


def start_spacing(mode, area):
    """The finest vertex spacing a start mode lets the texture be read at: past the last mip for start-average."""
    return np.inf if mode == "start-average" else float(np.sqrt(area))


def make_start(start, mode, area, out):
    """`start` (a fit's start bake) written in `out` by mesh_import, each vertex's albedo read no finer than
    start_spacing; returns its path."""
    from r3d import mesh_import

    read, floor = mesh_import.albedo_at, start_spacing(mode, area)
    mesh_import.albedo_at = lambda src, points, spacing, material: read(src, points, np.maximum(spacing, floor),
                                                                         material)
    try:
        out.mkdir(parents=True)
        name = start.job.asset_name if start.job.settings.meshes else start.output.removesuffix(start.suffix)
        mesh_import.write_baked(start.job, start.scene, out, name)
    finally:
        mesh_import.albedo_at = read
    return out / (name + start.suffix)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", type=pathlib.Path)
    parser.add_argument("--object", required=True, help="a fitted mesh renderer of the scene")
    parser.add_argument("--mode", required=True, choices=MODES)
    parser.add_argument("--out", type=pathlib.Path, required=True, help="a new folder")
    parser.add_argument("--cache", type=pathlib.Path, help="the bake cache (bake.py's default when unset)")
    parser.add_argument("--no-fit", action="store_true", help="make the friendly reference set only")
    args = parser.parse_args(argv)
    from bake import bake, produce
    from r3d.fit.stage import fit
    from r3d.import_settings import load_scene
    from r3d.reference_render import lit_source

    scene = load_scene(args.scene)
    job = next((j for j in scene.renderers if j.object.name == args.object), None)
    if job is None or job.renderer.fit is None:
        parser.error("--object must name a fitted mesh renderer")
    cache = args.cache or bake.default_cache()
    tables = bake.read_lock_tables()
    found = bake.bakes([args.scene], bake.locked_starts(tables["bake"]), bake.Poses(tables["poses"]))
    item = next(entry for entry in found if entry.kind == "fit" and entry.job.object.name == args.object)
    locked = produce.reference_set(item.stages["reference"], job, scene, cache)
    if args.mode.startswith("start-"):
        inputs, made = locked, args.out / "start"
        start = next(made.glob("*" + item.start.suffix), None) if made.exists() else None
        if start is None:
            source = lit_source(job, scene, lit=False)
            start = make_start(item.start, args.mode, affordable_area(source.p, source.tri_v, job.renderer.fit.budget),
                               made)
        print(f"start: {start}")
    else:
        inputs = locked if args.mode == "locked" else args.out / "inputs"
        if not inputs.exists():
            friendly_set(scene, job, args.mode, locked, inputs)
        print(f"friendly references: {inputs}")
        start = produce.start_file(item, cache, tables["bake"])
    if args.no_fit:
        return 0
    work = args.out / "work"
    work.mkdir(exist_ok=True)
    target = args.out / item.output
    fit(args.scene.resolve(), scene, job, work, target=target, inputs=inputs, start=start)
    print(f"fitted: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Report a scene renderer's source objects, rigid instances and estimated path LOD.

    objects_report.py SCENE.scene.toml --object NAME --out DIR
        [--every-ms N] [--size W H] [--tolerance X]
        [--held-out [--fitted BUNDLE] [--cache DIR] [--docs]]

Reads source geometry after alpha masking. Writes only to --out, which must
be outside the repository. The selected variant's budget is shared in
proportion to source triangles; culling uses world bounds, not occlusion.

--held-out, for a fitted renderer, also scores its locked fit (or --fitted)
against its held-out references, made into the bake cache when missing, and
splits the error by source object part (objects/object_error.py): error.csv,
error.md and error.png. --docs keeps error.csv and error.json as
docs/render/data/SCENE-object-error.*, which render_doc_images.sh makes the
page's figure and generated block from.
"""

import argparse
import csv
import json
import pathlib
import shutil
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from docs import plot_style  # noqa: E402
from r3d.appearance_simplify import projection  # noqa: E402
from r3d.cost_model import clusters_in_view  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.light import drop_masked  # noqa: E402
from r3d.objects.instances import MATCH_TOLERANCE, bounds_diagonal, match_instances, placed_corners, source_objects  # noqa: E402
from r3d.mesh_import import load_source  # noqa: E402
from r3d.poses import camera_basis, focal_px, sample_camera_path  # noqa: E402

LOD_LEVELS = 4
LOD_TRIANGLE_RATIO = 4
DEFAULT_EVERY_MS = 1000
TOP_SHAPES = 25
RESIDUAL_BINS = 32
# Exact matches need a positive display floor on the logarithmic axis.
RESIDUAL_DISPLAY_FLOOR = 1e-16
REPO = pathlib.Path(__file__).resolve().parents[4]


def masked_objects(src, settings):
    """Return source objects and merged source-index triangles after alpha masking."""
    if settings.alpha_keep is None:
        return source_objects(src.p, src.tri_v, src.tri_m, src.tri_o, src.object_names), src.tri_v
    objects, triangles = [], []
    for index, name in enumerate(src.object_names):
        selected = src.tri_o == index
        tv, _, tm, _ = drop_masked(src.p, src.uv, src.tri_v[selected], src.tri_t[selected], src.tri_m[selected],
                                src.textures, settings.alpha_keep)
        triangles.append(tv)
        objects.extend(source_objects(src.p, tv, tm, np.zeros(len(tv), dtype=int), [name]))
    return objects, np.concatenate(triangles) if triangles else np.empty((0, 3), dtype=int)


def pose_rows(shapes, scene, job, every_ms, size):
    """Selected variant budget, shared per source triangle, after bounds culling and estimated LOD."""
    camera = scene.camera.component
    width, height, lens, near, poses = sample_camera_path(
        camera.path.animation, camera.path.node, every_ms, *size, camera.half_fov_short_tan, camera.near_z)
    total = sum(len(shape.triangles) * len(shape.instances) for shape in shapes)
    budget = job.renderer.variant.triangles
    budget = total if budget is None else budget
    placements, boxes = [], []
    renderer_matrix = np.asarray(job.object.matrix)
    renderer_position = np.asarray(job.object.position)
    for shape in shapes:
        radius = np.linalg.norm(shape.positions, axis=1).max()
        share = budget * len(shape.triangles) / total if total else 0
        for instance in shape.instances:
            centre = renderer_matrix @ instance.position + renderer_position
            # A nonuniform renderer scale needs an enclosing sphere.
            placed_radius = radius * np.linalg.norm(renderer_matrix, ord=2)
            placements.append((centre, placed_radius, share, len(shape.instances) > 1))
            placed = placed_corners(shape, instance, renderer_matrix, renderer_position)
            boxes.append([placed.min(axis=0), placed.max(axis=0)])
    picture_area = width * height
    views = []
    for pose in poses:
        view = []
        clip = projection(pose[:3], pose[3:], width, height, lens, near)
        visible = clusters_in_view(np.asarray(boxes).reshape(-1, 2, 3), clip)
        forward = camera_basis(pose[3:])[2]
        for shown, (centre, radius, share, repeated) in zip(visible, placements):
            if shown:
                z = float((centre - pose[:3]) @ forward)
                pixels = radius * focal_px(width, height, lens) / max(z, near)
                view.append((share, min(picture_area, np.pi * pixels ** 2), repeated))
        views.append(view)
    mean_in_view = np.mean([sum(share for share, _, _ in view) for view in views])
    pixels_per_triangle = picture_area / mean_in_view if mean_in_view else float('inf')
    rows = []
    for index, view in enumerate(views):
        in_view, drawn = 0.0, 0.0
        for share, area, repeated in view:
            level = 0
            if repeated:
                while level < LOD_LEVELS - 1 and share / LOD_TRIANGLE_RATIO ** level > area / pixels_per_triangle:
                    level += 1
            in_view += share
            drawn += share / LOD_TRIANGLE_RATIO ** level
        rows.append(dict(time_ms=index * every_ms, instances_in_view=len(view), budget_triangles=budget,
                         budget_triangles_in_view=in_view, triangles_with_lod=drawn, saved=in_view - drawn))
    return budget, rows, pixels_per_triangle


def write_report(out, objects, shapes, budget, rows, pixels_per_triangle, tolerance):
    """Write machine-readable counts, the human summary and the two diagnostic figures."""
    totals = dict(objects=len(objects), shapes=len(shapes), repeated_shapes=sum(len(s.instances) > 1 for s in shapes),
                  source_triangles=sum(len(o.triangles) for o in objects),
                  unique_triangles=sum(len(s.triangles) for s in shapes))
    entries = [dict(triangles=len(s.triangles), instance_count=len(s.instances),
                    mirrored_count=sum(i.mirrored for i in s.instances),
                    max_residual=max(i.residual for i in s.instances),
                    object_names=[i.object_name for i in s.instances]) for s in shapes]
    (out / 'objects.json').write_text(json.dumps(dict(totals=totals, shapes=entries), indent=2) + '\n')
    with (out / 'poses.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    lines = ['# Objects and instances', '', '| Total | Count |', '|---|---:|']
    lines += [f'| {name.replace("_", " ")} | {value} |' for name, value in totals.items()]
    lines += ['', f'Largest triangle savings (up to {TOP_SHAPES} shapes):', '',
              '| First object | Triangles | Instances | Saved |', '|---|---:|---:|---:|']
    ranked = sorted(entries, key=lambda e: e['triangles'] * (e['instance_count'] - 1), reverse=True)
    for entry in ranked[:TOP_SHAPES]:
        name = entry['object_names'][0].replace('|', '\\|').replace('\n', ' ')
        lines.append(f'| {name} | {entry["triangles"]} | {entry["instance_count"]} | '
                     f'{entry["triangles"] * (entry["instance_count"] - 1)} |')
    lines += ['', '## Path estimate', '',
              f'Calibrated pixels per triangle: {pixels_per_triangle:.6g}.', '',
              'Pixels per triangle is relative by construction: calibration divides picture area',
              'by the path’s own mean in-view budget.',
              'Only repeated shapes use LOD; single-instance shapes retain their full share.', '',
              f'Choose the smallest level k with share / {LOD_TRIANGLE_RATIO}^k <= projected area / pixels per',
              f'triangle, capped at LOD_LEVELS - 1 (LOD_LEVELS = {LOD_LEVELS}). Projected area',
              'is pi times the projected sphere radius squared, capped at picture area.', '',
              'Shares use the selected variant budget, proportional to source triangles.',
              'World bounds include renderer placement; visibility excludes occlusion.', '',
              'A sphere’s disk overstates tall thin props’ footprint, so their saving is probably understated.', '',
              '| Draw rule | Mean triangles | Maximum triangles |', '|---|---:|---:|']
    draw_rules = [('Whole budget', 'budget_triangles'), ('In view', 'budget_triangles_in_view'),
                  ('In view with LOD on repeated shapes', 'triangles_with_lod'), ('Triangles LOD frees', 'saved')]
    for label, key in draw_rules:
        values = [row[key] for row in rows]
        lines.append(f'| {label} | {np.mean(values):.2f} | {max(values):.2f} |')
    lines += ['', '![Accepted relative residuals](residuals.png)', '', '![Path triangle estimate](lod.png)', '']
    (out / 'objects.md').write_text('\n'.join(lines))
    diagonals = {obj.name: bounds_diagonal(obj.positions) for obj in objects}
    residuals = [i.residual / diagonals[i.object_name] if diagonals[i.object_name] else 0
                 for s in shapes for i in s.instances[1:]]
    fig, ax = plot_style.subplots(1, 1, plot_style.FIGURE_SIZE)
    upper = max(tolerance, RESIDUAL_DISPLAY_FLOOR * 2)
    ax.hist(np.maximum(residuals, RESIDUAL_DISPLAY_FLOOR),
            bins=np.geomspace(RESIDUAL_DISPLAY_FLOOR, upper, RESIDUAL_BINS), color=plot_style.SLOTS[0])
    ax.axvline(max(tolerance, RESIDUAL_DISPLAY_FLOOR), color=plot_style.SLOTS[1], label=f'Tolerance {tolerance:g}')
    ax.set(xscale='log', xlabel='Residual / bounds diagonal (zero at display floor)', ylabel='Accepted copies')
    ax.legend()
    plot_style.style_axes(ax, 'Rigid matching residuals')
    plot_style.save(fig, out / 'residuals.png')
    fig, ax = plot_style.subplots(1, 1, plot_style.FIGURE_SIZE)
    for label, key in draw_rules:
        ax.plot([r['time_ms'] for r in rows], [r[key] for r in rows], marker='o', label=label)
    ax.set(xlabel='Path time (ms)', ylabel='Budget triangles', ylim=(0, None))
    ax.legend()
    plot_style.style_axes(ax, 'Per-instance culling and estimated LOD')
    plot_style.save(fig, out / 'lod.png')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('scene', type=pathlib.Path)
    parser.add_argument('--object', required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--every-ms', type=int)
    parser.add_argument('--size', type=int, nargs=2)
    parser.add_argument('--tolerance', type=float, default=MATCH_TOLERANCE)
    parser.add_argument('--held-out', action='store_true')
    parser.add_argument('--fitted', type=pathlib.Path)
    parser.add_argument('--cache', type=pathlib.Path)
    parser.add_argument('--docs', action='store_true')
    args = parser.parse_args(argv)
    if (args.fitted or args.cache or args.docs) and not args.held_out:
        parser.error('--fitted, --cache and --docs need --held-out')
    out = args.out.resolve()
    if out.is_relative_to(REPO):
        parser.error('--out must be outside the repository')
    if args.every_ms is not None and args.every_ms <= 0 or args.size and min(args.size) <= 0:
        parser.error('step and image dimensions must be positive')
    scene = load_scene(args.scene)
    job = next((j for j in scene.renderers if j.object.name == args.object), None)
    if job is None or job.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    if args.held_out and job.renderer.fit is None:
        parser.error('--held-out needs a fitted renderer')
    if scene.camera is None or scene.camera.component.path is None:
        parser.error('scene needs a camera path')
    visibility = job.renderer.visibility
    size = args.size or (job.renderer.fit.size if job.renderer.fit else None) or getattr(visibility, 'size', None)
    if size is None:
        parser.error('--size is required when the selected renderer has no fit or visibility size')
    every_ms = args.every_ms or (visibility.every_ms if visibility and visibility.source == 'camera_path' else DEFAULT_EVERY_MS)
    src = load_source(job.settings)
    objects, _ = masked_objects(src, job.settings)
    shapes = match_instances(objects, args.tolerance)
    budget, rows, pixels_per_triangle = pose_rows(shapes, scene, job, every_ms, size)
    out.mkdir(parents=True, exist_ok=True)
    write_report(out, objects, shapes, budget, rows, pixels_per_triangle, args.tolerance)
    if args.held_out:
        write_error_report(args, scene, job, out)
    return 0


def write_error_report(args, scene, job, out):
    """The --held-out report; see the module docstring."""
    from bake import bake, produce
    from r3d.objects import object_error
    from r3d.scene_name import scene_id

    cache = args.cache or bake.default_cache()
    tables = bake.read_lock_tables()
    found = bake.bakes([args.scene], bake.locked_starts(tables['bake']), bake.Poses(tables['poses']))
    fit = next(item for item in found if item.kind == 'fit' and item.job.object.name == job.object.name)
    bundle = args.fitted or bake.fetch_all([fit], tables['bake'], cache)[fit]
    inputs = produce.reference_set(fit.stages['reference'], job, scene, cache)
    work = out / 'held_out'
    work.mkdir(exist_ok=True)
    rows, score, unseen, held_out_mean = object_error.held_out_error(args.scene.resolve(), scene, job,
                                                                     pathlib.Path(bundle), inputs, work)
    name = scene_id(args.scene)
    object_error.write(out, rows, score, unseen, held_out_mean, f'{name} {job.object.name}: held-out error by part')
    print(f'mean dE76 {score:.3f} by parts, {held_out_mean:.3f} by the held-out score')
    if args.docs:
        for suffix in ('.csv', '.json'):
            shutil.copyfile(out / f'error{suffix}', REPO / 'docs/render/data' / f'{name}-object-error{suffix}')


if __name__ == '__main__':
    raise SystemExit(main())

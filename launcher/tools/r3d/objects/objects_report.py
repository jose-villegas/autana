#!/usr/bin/env python3
"""Report a scene renderer's source objects, rigid instances and estimated path LOD.

    objects_report.py SCENE.scene.toml --object NAME --out DIR
        [--every-ms N] [--size W H] [--tolerance X]

Reads source geometry after alpha masking. Writes only to --out, which must
be outside the repository. The selected variant's budget is shared in
proportion to source triangles; culling uses bounding spheres, not occlusion.
"""

import argparse
import csv
import json
import pathlib
import sys
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from docs import plot_style  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.mesh_import import camera_path_poses  # noqa: E402
from r3d.light import drop_masked  # noqa: E402
from r3d.objects.instances import MATCH_TOLERANCE, match_instances, source_objects  # noqa: E402
from r3d.objects.source import load_object_source  # noqa: E402
from r3d.poses import camera_basis  # noqa: E402

LOD_FULL_RADIUS_PX = 64.0
LOD_LEVELS = 4
DEFAULT_EVERY_MS = 1000
DEFAULT_SIZE = (184, 224)
TOP_SHAPES = 25
RESIDUAL_BINS = 32
# Exact matches need a positive display floor on the logarithmic axis.
RESIDUAL_DISPLAY_FLOOR = 1e-16
REPO = pathlib.Path(__file__).resolve().parents[4]


def masked_objects(src, settings):
    """Apply the import's alpha predicate separately to each object's triangles."""
    if settings.alpha_keep is None:
        return source_objects(src.p, src.tri_v, src.tri_m, src.tri_o, src.object_names)
    objects = []
    for index, name in enumerate(src.object_names):
        selected = src.tri_o == index
        tv, _, tm = drop_masked(src.p, src.uv, src.tri_v[selected], src.tri_t[selected], src.tri_m[selected],
                                src.textures, settings.alpha_keep)
        objects.extend(source_objects(src.p, tv, tm, np.zeros(len(tv), dtype=int), [name]))
    return objects


def sphere_view(centre, radius, pose, width, height, lens, near):
    """Conservative sphere/frustum test and pinhole radius estimate in camera_basis conventions."""
    right, up, forward = camera_basis(pose[3:])
    offset = centre - pose[:3]
    x, y, z = (float(offset @ axis) for axis in (right, up, forward))
    horizontal, vertical = lens * width / min(width, height), lens * height / min(width, height)
    visible = (z + radius >= near and
               abs(x) <= z * horizontal + radius * np.hypot(1, horizontal) and
               abs(y) <= z * vertical + radius * np.hypot(1, vertical))
    projected = radius * min(width, height) / (2 * lens * max(z, near))
    return visible, projected


def pose_rows(shapes, scene, job, every_ms, size):
    """Selected variant budget, shared per source triangle, after sphere culling and estimated LOD."""
    visibility = SimpleNamespace(size=size, every_ms=every_ms)
    width, height, lens, near, poses = camera_path_poses(scene, visibility, either_way_up=False)
    total = sum(len(shape.triangles) * len(shape.instances) for shape in shapes)
    budget = job.renderer.variant.triangles
    budget = total if budget is None else budget
    placements = []
    renderer_matrix = np.asarray(job.object.matrix)
    renderer_position = np.asarray(job.object.position)
    for shape in shapes:
        radius = np.linalg.norm(shape.positions, axis=1).max()
        share = budget * len(shape.triangles) / total if total else 0
        for instance in shape.instances:
            centre = renderer_matrix @ instance.position + renderer_position
            # A nonuniform renderer scale needs an enclosing sphere.
            placed_radius = radius * np.linalg.norm(renderer_matrix, ord=2)
            placements.append((centre, placed_radius, share))
    rows = []
    for index, pose in enumerate(poses):
        count, in_view, drawn = 0, 0.0, 0.0
        for centre, radius, share in placements:
            visible, pixels = sphere_view(centre, radius, pose, width, height, lens, near)
            if not visible:
                continue
            level = 0
            while level < LOD_LEVELS - 1 and pixels < LOD_FULL_RADIUS_PX / 2 ** (level + 1):
                level += 1
            count += 1
            in_view += share
            drawn += share / 4 ** level
        rows.append(dict(time_ms=index * every_ms, instances_in_view=count,
                         budget_triangles_in_view=in_view, triangles_with_lod=drawn, saved=in_view - drawn))
    return budget, rows


def write_report(out, objects, shapes, budget, rows, tolerance):
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
              f'LOD_FULL_RADIUS_PX = {LOD_FULL_RADIUS_PX:g}; LOD_LEVELS = {LOD_LEVELS}.', '',
              'Projected-radius halving is an estimate, not a design. Choose the largest',
              'level k whose sphere radius is below LOD_FULL_RADIUS_PX / 2^k, or 0',
              'for larger spheres. Cap k at LOD_LEVELS - 1; retain 1 / 4^k of its share.', '',
              'Shares use the selected variant budget, proportional to source triangles.',
              'Bounding spheres include renderer placement; visibility excludes occlusion.', '',
              '| Draw rule | Mean triangles | Worst-pose triangles |', '|---|---:|---:|',
              f'| Whole budget | {budget:.2f} | {budget:.2f} |']
    for label, key in [('In view', 'budget_triangles_in_view'), ('In view with LOD', 'triangles_with_lod')]:
        values = [row[key] for row in rows]
        lines.append(f'| {label} | {np.mean(values):.2f} | {max(values):.2f} |')
    lines += ['', '![Accepted relative residuals](residuals.png)', '', '![Path triangle estimate](lod.png)', '']
    (out / 'objects.md').write_text('\n'.join(lines))
    diagonals = {obj.name: np.linalg.norm(np.ptp(obj.positions, axis=0)) for obj in objects}
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
    for key, label in [('budget_triangles_in_view', 'In view'), ('triangles_with_lod', 'With estimated LOD')]:
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
    args = parser.parse_args(argv)
    out = args.out.resolve()
    if out.is_relative_to(REPO):
        parser.error('--out must be outside the repository')
    if args.every_ms is not None and args.every_ms <= 0 or args.size and min(args.size) <= 0:
        parser.error('step and image dimensions must be positive')
    scene = load_scene(args.scene)
    job = next((j for j in scene.renderers if j.object.name == args.object), None)
    if job is None or job.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    if scene.camera is None or scene.camera.component.path is None:
        parser.error('scene needs a camera path')
    fitted = next((j.renderer.fit for j in scene.renderers if j.renderer.fit), None)
    size = args.size or (fitted.size if fitted else DEFAULT_SIZE)
    visibility = job.renderer.visibility
    every_ms = args.every_ms or (visibility.every_ms if visibility and visibility.source == 'camera_path' else DEFAULT_EVERY_MS)
    src = load_object_source(job.settings)
    objects = masked_objects(src, job.settings)
    shapes = match_instances(objects, args.tolerance)
    budget, rows = pose_rows(shapes, scene, job, every_ms, size)
    out.mkdir(parents=True, exist_ok=True)
    write_report(out, objects, shapes, budget, rows, args.tolerance)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

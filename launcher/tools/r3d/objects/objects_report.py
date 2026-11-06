#!/usr/bin/env python3
"""Report a scene renderer's source objects and rigid instances.

    objects_report.py SCENE.scene.toml --object NAME --out DIR
        [--tolerance X] [--parts] [--surface-tolerance [X] [--surface-floor D]]
        [--held-out [--fitted BUNDLE] [--cache DIR] [--docs]]

Reads source geometry after alpha masking. Writes only to --out, which must
be outside the repository.

--parts matches every object's connected parts instead of whole objects.
--surface-tolerance also joins shapes whose surfaces agree within X of their
bounds diagonal, however each copy is triangulated and whatever its
materials (objects/surface_match.py); --surface-floor raises that limit to D
source units, for a source reduced object by object with up to D of error.
shapes.png then draws the repeated shapes saving the most triangles, every
copy in its shape's frame. What levels of detail save along the camera path
is measured on the fitted mesh by lod/lod_replay.py.

--held-out, for a fitted renderer, also scores its locked fit (or --fitted)
against its held-out references, made into the bake cache when missing, and
splits the error by source object part (objects/object_error.py): error.csv,
error.md and error.png. --docs keeps error.csv and error.json as
docs/render/data/SCENE-OBJECT-object-error.*, which render_doc_images.sh makes the
page's figure and generated block from.
"""

import argparse
import json
import pathlib
import shutil
import sys

import matplotlib.colors
import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from docs import plot_style  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.light import drop_masked  # noqa: E402
from r3d.objects.instances import MATCH_TOLERANCE, bounds_diagonal, match_instances, place, source_objects  # noqa: E402
from r3d.objects.object_error import part_labels, part_names  # noqa: E402
from r3d.objects.surface_match import SURFACE_TOLERANCE  # noqa: E402
from r3d.mesh_import import load_source  # noqa: E402

TOP_SHAPES = 25
SHEET_SHAPES = 6
SHEET_COPIES = 6
# Shapes this small (a plain box is 12) say little on the sheet; the table still lists them.
SHEET_MIN_TRIANGLES = 20
# Degrees: azimuth and elevation of the sheet's eye in each shape's principal frame, so depth shows.
SHEET_VIEW = (-35.0, 30.0)
SHEET_AMBIENT = 0.35
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


def split_parts(objects):
    """Every object's connected parts as objects of their own, named as object_error.part_names names them."""
    parts = []
    for obj in objects:
        owner = np.zeros(len(obj.triangles), dtype=int)
        labels, _ = part_labels(obj.positions, obj.triangles, owner)
        names, _, _ = part_names(labels, owner, [obj.name])
        parts.extend(source_objects(obj.positions, obj.triangles, obj.materials, labels, names))
    return parts


def write_report(out, objects, shapes, tolerance):
    """Write machine-readable counts, the human summary and the residual figure."""
    counts = {obj.name: len(obj.triangles) for obj in objects}
    repeated = [s for s in shapes if len(s.instances) > 1]
    totals = dict(objects=len(objects), shapes=len(shapes), repeated_shapes=len(repeated),
                  source_triangles=sum(counts.values()),
                  unique_triangles=sum(len(s.triangles) for s in shapes),
                  repeated_triangles=sum(counts[i.object_name] for s in repeated for i in s.instances),
                  surface_copies=sum(i.triangle_order == () for s in shapes for i in s.instances[1:]))
    entries = [dict(triangles=len(s.triangles), instance_count=len(s.instances),
                    mirrored_count=sum(i.mirrored for i in s.instances),
                    surface_count=sum(i.triangle_order == () for i in s.instances[1:]),
                    max_residual=max(i.residual for i in s.instances),
                    max_gap=max(i.gap for i in s.instances),
                    copy_triangles=[counts[i.object_name] for i in s.instances],
                    object_names=[i.object_name for i in s.instances]) for s in shapes]
    (out / 'objects.json').write_text(json.dumps(dict(totals=totals, shapes=entries), indent=2) + '\n')
    lines = ['# Objects and instances', '', '| Total | Count |', '|---|---:|']
    lines += [f'| {name.replace("_", " ")} | {value} |' for name, value in totals.items()]
    lines += ['', f'Largest triangle savings (up to {TOP_SHAPES} shapes):', '',
              '| First object | Triangles | Instances | Mirrored | By surface | Largest gap | Saved |',
              '|---|---:|---:|---:|---:|---:|---:|']
    for entry in sorted(entries, key=saved, reverse=True)[:TOP_SHAPES]:
        name = entry['object_names'][0].replace('|', '\\|').replace('\n', ' ')
        lines.append(f'| {name} | {entry["triangles"]} | {entry["instance_count"]} | {entry["mirrored_count"]} | '
                     f'{entry["surface_count"]} | {entry["max_gap"]:.3g} | {saved(entry)} |')
    lines += ['', '![Accepted relative residuals](residuals.png)', '']
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
    plot_style.save_plot(fig, ax, out / 'residuals.png', 'Rigid matching residuals')


def saved(entry):
    """Source triangles that every copy but the shape's own holds."""
    return sum(entry['copy_triangles']) - entry['triangles']


def write_sheet(path, objects, shapes):
    """The SHEET_SHAPES repeated shapes of at least SHEET_MIN_TRIANGLES saving the most triangles, up to SHEET_COPIES
    copies each: every copy taken back into its shape's frame, seen from SHEET_VIEW off its principal axes, shaded
    by facing, nearest triangles last."""
    from matplotlib.collections import PolyCollection

    by_name = {obj.name: obj for obj in objects}

    def copies_saved(shape):
        return sum(len(by_name[i.object_name].triangles) for i in shape.instances) - len(shape.triangles)

    repeated = sorted((s for s in shapes if len(s.instances) > 1 and len(s.triangles) >= SHEET_MIN_TRIANGLES),
                      key=copies_saved, reverse=True)[:SHEET_SHAPES]
    rows = max(len(repeated), 1)
    fig, axes = plot_style.subplots(rows, SHEET_COPIES, (1.6 * SHEET_COPIES, 1.7 * rows), squeeze=False)
    for axis in axes.flat:
        axis.set_axis_off()
    turn = sheet_turn(*SHEET_VIEW)
    colour = np.array(matplotlib.colors.to_rgb(plot_style.SLOTS[0]))
    for row, shape in zip(axes, repeated):
        centred = shape.positions - shape.positions.mean(axis=0)
        view = np.linalg.eigh(centred.T @ centred)[1][:, ::-1] @ turn
        for axis, instance in zip(row, shape.instances):
            obj = by_name[instance.object_name]
            corners = (place(obj.positions - instance.position, instance.linear.T, 0.0) @ view)[obj.triangles]
            normal = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
            facing = np.abs(normal[:, 2]) / np.maximum(np.linalg.norm(normal, axis=1), 1e-12)
            order = np.argsort(corners[:, :, 2].mean(axis=1))
            shade = SHEET_AMBIENT + (1 - SHEET_AMBIENT) * facing[order, None]
            axis.add_collection(PolyCollection(corners[order, :, :2], facecolor=colour * shade, edgecolor='white',
                                               linewidth=0.2))
            axis.autoscale_view()
            axis.set_aspect('equal')
            axis.set_title(f'{len(obj.triangles)} tris' + (' mirrored' if instance.mirrored else ''),
                           fontsize=plot_style.LABEL_SIZE)
    plot_style.save(fig, path)


def sheet_turn(azimuth, elevation):
    """Columns: the screen's right, up and towards-the-eye axes, for an eye at azimuth around the third principal
    axis and elevation above the first two, in degrees."""
    a, e = np.radians(azimuth), np.radians(elevation)
    eye = np.array([np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)])
    right = np.array([-np.sin(a), np.cos(a), 0.0])
    return np.column_stack([right, np.cross(eye, right), eye])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('scene', type=pathlib.Path)
    parser.add_argument('--object', required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--tolerance', type=float, default=MATCH_TOLERANCE)
    parser.add_argument('--parts', action='store_true')
    parser.add_argument('--surface-tolerance', type=float, nargs='?', const=SURFACE_TOLERANCE)
    parser.add_argument('--surface-floor', type=float, default=0.0)
    parser.add_argument('--held-out', action='store_true')
    parser.add_argument('--fitted', type=pathlib.Path)
    parser.add_argument('--cache', type=pathlib.Path)
    parser.add_argument('--docs', action='store_true')
    args = parser.parse_args(argv)
    if args.surface_floor and args.surface_tolerance is None:
        parser.error('--surface-floor needs --surface-tolerance')
    if (args.fitted or args.cache or args.docs) and not args.held_out:
        parser.error('--fitted, --cache and --docs need --held-out')
    if args.fitted and args.docs:
        parser.error('--docs keeps the locked fit\'s rows and keys, so it does not take --fitted')
    out = args.out.resolve()
    if out.is_relative_to(REPO):
        parser.error('--out must be outside the repository')
    scene = load_scene(args.scene)
    job = next((j for j in scene.renderers if j.object.name == args.object), None)
    if job is None or job.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    if args.held_out and job.renderer.fit is None:
        parser.error('--held-out needs a fitted renderer')
    src = load_source(job.settings)
    objects, _ = masked_objects(src, job.settings)
    if args.parts:
        objects = split_parts(objects)
    shapes = match_instances(objects, args.tolerance, surface_tolerance=args.surface_tolerance,
                             surface_floor=args.surface_floor)
    out.mkdir(parents=True, exist_ok=True)
    write_report(out, objects, shapes, args.tolerance)
    if args.surface_tolerance is not None:
        write_sheet(out / 'shapes.png', objects, shapes)
    if args.held_out:
        write_error_report(args, scene, job, out)
    return 0


def write_error_report(args, scene, job, out):
    """The --held-out report; see the module docstring."""
    from bake import bake, produce
    from r3d.objects import object_error
    from r3d.scene_name import scene_id

    cache = args.cache or bake.default_cache()
    fit = bake.locked_fit(args.scene, job.object.name)
    bundle = args.fitted or bake.fetch(fit, cache)
    inputs = produce.reference_set(fit.stages['reference'], job, scene, cache)
    work = out / 'held_out'
    work.mkdir(exist_ok=True)
    rows, score, unseen = object_error.held_out_error(args.scene.resolve(), scene, job,
                                                                     pathlib.Path(bundle), inputs, work)
    name = f'{scene_id(args.scene)}-{job.object.name}'.replace('_', '-')
    object_error.write(out, rows, score, unseen, f'{scene_id(args.scene)} {job.object.name}: held-out error by part',
                       dict(fit=fit.key, reference=fit.stages['reference']))
    print(f'mean dE76 {score:.3f} over the parts, as the held-out score')
    if args.docs:
        for suffix in ('.csv', '.json'):
            shutil.copyfile(out / f'error{suffix}', REPO / 'docs/render/data' / f'{name}-object-error{suffix}')


if __name__ == '__main__':
    raise SystemExit(main())

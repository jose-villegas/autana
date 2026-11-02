#!/usr/bin/env python3
"""Report a scene renderer's source objects and rigid instances.

    objects_report.py SCENE.scene.toml --object NAME --out DIR
        [--tolerance X] [--held-out [--fitted BUNDLE] [--cache DIR] [--docs]]

Reads source geometry after alpha masking. Writes only to --out, which must
be outside the repository. What levels of detail save along the camera path
is lod/lod_replay.py's, measured on the fitted mesh.

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

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from docs import plot_style  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.light import drop_masked  # noqa: E402
from r3d.objects.instances import MATCH_TOLERANCE, bounds_diagonal, match_instances, source_objects  # noqa: E402
from r3d.mesh_import import load_source  # noqa: E402

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


def write_report(out, objects, shapes, tolerance):
    """Write machine-readable counts, the human summary and the residual figure."""
    totals = dict(objects=len(objects), shapes=len(shapes), repeated_shapes=sum(len(s.instances) > 1 for s in shapes),
                  source_triangles=sum(len(o.triangles) for o in objects),
                  unique_triangles=sum(len(s.triangles) for s in shapes))
    entries = [dict(triangles=len(s.triangles), instance_count=len(s.instances),
                    mirrored_count=sum(i.mirrored for i in s.instances),
                    max_residual=max(i.residual for i in s.instances),
                    object_names=[i.object_name for i in s.instances]) for s in shapes]
    (out / 'objects.json').write_text(json.dumps(dict(totals=totals, shapes=entries), indent=2) + '\n')
    lines = ['# Objects and instances', '', '| Total | Count |', '|---|---:|']
    lines += [f'| {name.replace("_", " ")} | {value} |' for name, value in totals.items()]
    lines += ['', f'Largest triangle savings (up to {TOP_SHAPES} shapes):', '',
              '| First object | Triangles | Instances | Saved |', '|---|---:|---:|---:|']
    ranked = sorted(entries, key=lambda e: e['triangles'] * (e['instance_count'] - 1), reverse=True)
    for entry in ranked[:TOP_SHAPES]:
        name = entry['object_names'][0].replace('|', '\\|').replace('\n', ' ')
        lines.append(f'| {name} | {entry["triangles"]} | {entry["instance_count"]} | '
                     f'{entry["triangles"] * (entry["instance_count"] - 1)} |')
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
    ax.legend()
    plot_style.style_axes(ax, 'Rigid matching residuals')
    plot_style.save(fig, out / 'residuals.png')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('scene', type=pathlib.Path)
    parser.add_argument('--object', required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--tolerance', type=float, default=MATCH_TOLERANCE)
    parser.add_argument('--held-out', action='store_true')
    parser.add_argument('--fitted', type=pathlib.Path)
    parser.add_argument('--cache', type=pathlib.Path)
    parser.add_argument('--docs', action='store_true')
    args = parser.parse_args(argv)
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
    shapes = match_instances(objects, args.tolerance)
    out.mkdir(parents=True, exist_ok=True)
    write_report(out, objects, shapes, args.tolerance)
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

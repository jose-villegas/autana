#!/usr/bin/env python3
"""Report candidate cards and their share of a scene renderer's triangle ceiling."""

import argparse
from copy import copy
import csv
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from r3d.import_settings import load_scene  # noqa: E402
from r3d.light import drop_masked  # noqa: E402
from r3d.mesh_import import bake_geometry, load_source  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402

REPO = Path(__file__).resolve().parents[4]
FIELDS = ('material', 'see_through', 'covered', 'accepted', 'replaced', 'cards',
          'triangles', 'ceiling', 'budget', 'scene_total')


def rows_for(job, scene):
    job = copy(job)
    job.bake = None
    src = load_source(job.settings)
    materials = src.tri_m
    if job.settings.alpha_keep is not None:
        _, _, materials, _ = drop_masked(src.p, src.uv, src.tri_v, src.tri_t, src.tri_m,
                                          src.textures, job.settings.alpha_keep)
    geometry = bake_geometry(job, scene)
    rows = []
    for name, report in geometry.mesh_report.get('cards', {}).items():
        rows.append(dict(material=name, see_through='always' if report['forced'] else report['see_through'],
                         covered=report['covered'], accepted=report['accepted'],
                         replaced=int(np.count_nonzero(materials == src.names.index(name))),
                         cards=report['triangles'] if report['accepted'] else 0))
    requested = job.renderer.variant.triangles
    ceiling = getattr(job.renderer.variant, 'ceiling', None)
    rows.append(dict(triangles=requested, ceiling='' if ceiling is None else ceiling,
                     budget=geometry.mesh_report.get('rest_budget', requested),
                     scene_total=geometry.mesh_report.get('scene_total', len(geometry.tris))))
    return rows


def write(path, rows):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def table(data):
    with data.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != FIELDS:
            raise ValueError(f'{data}: expected fields {FIELDS}')
        rows = list(reader)
    lines = ['| Material | See-through | Covered | Carded | Triangles replaced | Card triangles |',
             '|---|---:|---:|---|---:|---:|']
    for row in rows[:-1]:
        name = row['material'].replace('|', '\\|').replace('\n', ' ')
        share = row['see_through']
        see_through = share if share == 'always' else f'{float(share):.1%}'
        lines.append(f"| {name} | {see_through} | {float(row['covered']):.1%} | "
                     f"{'yes' if row['accepted'] == str(True) else 'no'} | {row['replaced']} | {row['cards']} |")
    scene = rows[-1]
    lines += ['', f"requested {scene['triangles']}, ceiling {scene['ceiling'] or 'none'}, "
                  f"rest budget {scene['budget']}, emitted {scene['scene_total']}", '']
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scene', type=Path)
    parser.add_argument('--object')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--docs', action='store_true')
    mode.add_argument('--table', type=Path)
    args = parser.parse_args(argv)
    if args.table:
        args.table.write_text(table(args.scene), encoding='utf-8')
        return 0
    if not args.object:
        parser.error('--docs requires --object')
    scene = load_scene(args.scene)
    job = next((job for job in scene.renderers if job.object.name == args.object), None)
    if job is None or job.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    name = f'{scene_id(args.scene)}-{job.object.name}'.replace('_', '-')
    path = REPO / 'docs/render/data' / f'{name}-cards.csv'
    write(path, rows_for(job, scene))
    print(path)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

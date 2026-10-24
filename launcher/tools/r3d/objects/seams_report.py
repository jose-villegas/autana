#!/usr/bin/env python3
"""Report mesh policies and compare free/locked contacts through the scene host.

    seams_report.py SCENE --object NAME --settings SCRATCH.import.toml --host HOST --out DIR

The scratch import uses the selected renderer's variant name. Output stays
outside the checkout. Each affected contact gets a comparison and crop sheet.
"""

import argparse
from copy import copy, deepcopy
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from asset.asset_pack import build_pack  # noqa: E402
from asset.engine_frame import to_engine  # noqa: E402
from r3d import scene_asset  # noqa: E402
from r3d.import_settings import load_import_settings, load_scene, match_meshes  # noqa: E402
from r3d.lit_mesh import POSITION_SCALE  # noqa: E402
from r3d.mesh_asset import TYPE as MESH_TYPE  # noqa: E402
from r3d.mesh_import import bake_geometry, load_source, write_baked  # noqa: E402
from r3d.objects.contact import contact_graph
from r3d.objects.instances_figure import project_points  # noqa: E402
from r3d.objects.objects_report import REPO, masked_objects  # noqa: E402
from r3d.poses import camera_basis  # noqa: E402

DEFAULT_DIRECTION = (0.0, 0.0, -1.0)
DEFAULT_LENS = 0.6
VIEW_MARGIN = 1.4
MIN_VIEW_TICKS = 8
NEAR_RADIUS_RATIO = 0.125
RENDER_FRAMES = 1
CROP_COUNT = 1
CONTACT_MARGIN = 8
BACKGROUND = 0x202020
PACK_NAME = 'seams'
MESH_NAME = 'mesh'


def contact_cameras(src, edges, direction, position_scale):
    cameras = []
    for index, edge in enumerate(edges):
        a = src.p[np.unique(src.tri_v[src.tri_o == edge.a])]
        b = src.p[np.unique(src.tri_v[src.tri_o == edge.b])]
        low = np.maximum(a.min(axis=0), b.min(axis=0))
        high = np.maximum(low, np.minimum(a.max(axis=0), b.max(axis=0)))
        centre = (low + high) / 2
        radius = max(np.linalg.norm(high - low) / 2, MIN_VIEW_TICKS / position_scale)
        right, up, forward = camera_basis(direction)
        eye = centre - forward * radius * VIEW_MARGIN / DEFAULT_LENS
        cameras.append(SimpleNamespace(name=f'contact{index}', kind='camera', position=eye,
                        matrix=np.column_stack([right, up, -forward]),
                        component=SimpleNamespace(half_fov_short_tan=DEFAULT_LENS, near_z=radius * NEAR_RADIUS_RATIO,
                                                  background=BACKGROUND, path=None),
                        border=src.p[np.unique(np.concatenate([edge.vertices_a, edge.vertices_b]))]))
    return cameras


def render_contacts(out, host, job, scene, cameras, geometries):
    render_job = copy(job)
    render_job.object = SimpleNamespace(name=MESH_NAME, kind='mesh_renderer', position=np.zeros(3), matrix=np.eye(3))
    render_job.asset_name = MESH_NAME
    view_scene = SimpleNamespace(objects=[render_job.object, *cameras], renderers=[render_job],
                                 lights=scene.lights, tonemap_white=scene.tonemap_white)
    scene_bytes = to_engine(scene_asset.TYPE, scene_asset.encode(view_scene))
    for policy, geometry in geometries.items():
        folder = out / policy
        folder.mkdir(exist_ok=True)
        write_baked(job, scene, folder, MESH_NAME, geometry)
        mesh_bytes = to_engine(MESH_TYPE, (folder / f'{MESH_NAME}.mesh').read_bytes())
        (folder / f'{PACK_NAME}.apak').write_bytes(build_pack([(PACK_NAME, scene_asset.TYPE, scene_bytes),
                                                             (MESH_NAME, MESH_TYPE, mesh_bytes)]))
        for camera in cameras:
            subprocess.run([str(host), '--scene', PACK_NAME, '--camera', camera.name, '--frames', str(RENDER_FRAMES),
                            '-o', str(folder / f'{camera.name}.bmp')], check=True,
                           env={**os.environ, 'AUTANA_ASSET_DIR': str(folder)})
            image_path = folder / f'{camera.name}.bmp'
            with Image.open(image_path) as image:
                pose = np.concatenate([camera.position, -camera.matrix[:, 2]])
                pixels, _ = project_points(camera.border, pose, image.size, DEFAULT_LENS, camera.component.near_z)
                low = np.maximum(0, np.floor(pixels.min(axis=0)).astype(int) - CONTACT_MARGIN)
                high = np.minimum(image.size, np.ceil(pixels.max(axis=0)).astype(int) + CONTACT_MARGIN)
                if np.any(high <= low):
                    raise ValueError(f"{camera.name}: contact is outside the view; choose --direction")
                image.crop((*low, *high)).save(folder / f'{camera.name}.contact.bmp')
    compare = REPO / 'launcher/tools/render/render_compare.py'
    for camera in cameras:
        subprocess.run([sys.executable, str(compare), '--out', str(out / f'{camera.name}.png'),
                        '--label-a', 'free', '--label-b', 'lock', '--clear', f'{BACKGROUND:06x}',
                        '--crops', str(CROP_COUNT), '--row', camera.name,
                        str(out / 'free' / f'{camera.name}.contact.bmp'), str(out / 'lock' / f'{camera.name}.contact.bmp')], check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scene', type=Path)
    parser.add_argument('--object', required=True)
    parser.add_argument('--settings', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--direction', nargs=3, type=float, default=DEFAULT_DIRECTION)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    if out.is_relative_to(REPO):
        parser.error('--out must be outside the checkout')
    direction = np.asarray(args.direction)
    if not np.all(np.isfinite(direction)) or np.linalg.norm(np.cross(direction, [0, 1, 0])) == 0:
        parser.error('--direction must be finite and not parallel to world up')
    scene = load_scene(args.scene)
    original = next((j for j in scene.renderers if j.object.name == args.object), None)
    if original is None or original.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    job = copy(original)
    job.settings = load_import_settings(args.settings)
    if not job.settings.meshes:
        parser.error('--settings must contain [[objects.meshes]]')
    job.renderer = copy(original.renderer)
    job.renderer.variant = next((v for v in job.settings.variants if v.name == original.renderer.variant.name), None)
    if job.renderer.variant is None:
        parser.error('--settings must include the selected renderer variant')
    src = load_source(job.settings)
    objects, triangles = masked_objects(src, job.settings)
    indices = {name: i for i, name in enumerate(src.object_names)}
    if job.settings.alpha_keep is None:
        owners, materials = src.tri_o, src.tri_m
    else:
        owners = np.concatenate([np.full(len(obj.triangles), indices[obj.name]) for obj in objects])
        materials = np.concatenate([obj.materials for obj in objects])
    src = copy(src)
    src.tri_v, src.tri_o, src.tri_m = triangles, np.asarray(owners), np.asarray(materials)
    selected = match_meshes(src.tri_o, src.tri_m, src.object_names, src.names, job.settings.meshes)
    scale = job.settings.position_scale or POSITION_SCALE
    edges = contact_graph(src.p, triangles, src.tri_o, scale)
    affected = [edge for edge in edges if edge.a in selected or edge.b in selected]
    out.mkdir(parents=True, exist_ok=True)
    geometry = bake_geometry(job, scene)
    report = geometry.mesh_report
    report['lock_cost_measure'] = 'emitted triangles above the pass target; shared cost is per copy'
    report['source_contacts'] = [dict(a=src.object_names[e.a], b=src.object_names[e.b], unshared=e.unshared,
                                     vertices_a=e.vertices_a.tolist(), vertices_b=e.vertices_b.tolist()) for e in edges]
    (out / 'seams.json').write_text(json.dumps(report, indent=2) + '\n')
    geometries = {}
    for policy in ('free', 'lock'):
        policy_job = copy(job)
        policy_job.settings = copy(job.settings)
        policy_job.settings.meshes = deepcopy(job.settings.meshes)
        for rule in policy_job.settings.meshes:
            rule['seam'] = policy
        geometries[policy] = bake_geometry(policy_job, scene)
    render_contacts(out, args.host.resolve(), job, scene, contact_cameras(src, affected, direction, scale), geometries)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

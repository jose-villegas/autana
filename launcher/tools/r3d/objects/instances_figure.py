#!/usr/bin/env python3
"""Render detected repeated shapes over the camera path's richest view.

    instances_figure.py SCENE.scene.toml --object NAME --host HOST --out PNG
        [--camera NAME] [--every-ms N]
"""

import argparse
from collections import Counter
from itertools import product
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device"))

from docs.plot_style import INK, MUTED, SLOTS  # noqa: E402
from panel_size import PANEL_HEIGHT, PANEL_WIDTH  # noqa: E402
from r3d.appearance_simplify import projection  # noqa: E402
from r3d.cost_model import clusters_in_view  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.mesh_import import camera_path_poses  # noqa: E402
from r3d.objects.instances import bounds_diagonal, match_instances  # noqa: E402
from r3d.objects.objects_report import DEFAULT_EVERY_MS, masked_objects  # noqa: E402
from r3d.objects.source import load_object_source  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402

DOC_SIZE = (PANEL_HEIGHT, PANEL_WIDTH)
EDGE_SAMPLES = 128
OCCLUSION_TOLERANCE = 1e-4
ROW_HEIGHT = 24
PADDING = 12
FONT_SIZE = 14
LINE_WIDTH = 2
OTHER_LABEL = 'other repeated shapes'
CORNER_BITS = np.array(list(product((0, 1), repeat=3)))
BOX_EDGES = tuple((a, b) for a in range(len(CORNER_BITS)) for b in range(a + 1, len(CORNER_BITS))
                  if np.count_nonzero(CORNER_BITS[a] != CORNER_BITS[b]) == 1)


def placed_boxes(shapes, job):
    """Local bounds under rigid instance and renderer placements, with their shape indices."""
    boxes, owners = [], []
    matrix, position = np.asarray(job.object.matrix), np.asarray(job.object.position)
    for index, shape in enumerate(shapes):
        if len(shape.instances) < 2:
            continue
        corners = np.where(CORNER_BITS, shape.positions.max(axis=0), shape.positions.min(axis=0))
        for instance in shape.instances:
            boxes.append((corners @ instance.linear.T + instance.position) @ matrix.T + position)
            owners.append(index)
    return np.asarray(boxes).reshape(-1, len(CORNER_BITS), 3), np.asarray(owners, dtype=int)


def choose_view(boxes, owners, poses, size, lens, near):
    """Prefer distinct repeated shapes, then visible copies, then the earliest sample."""
    bounds = np.stack((boxes.min(axis=1), boxes.max(axis=1)), axis=1)
    best, chosen = (-1, -1), None
    for index, pose in enumerate(poses):
        shown = clusters_in_view(bounds, projection(pose[:3], pose[3:], *size, lens, near))
        score = (len(set(owners[shown])), int(shown.sum()))
        if score > best:
            best, chosen = score, (index, shown)
    if chosen is None:
        raise ValueError('camera path has no poses')
    return chosen


def project_points(points, pose, size, lens, near):
    """Pixel coordinates and forward depth using the shared pinhole projection."""
    clip = np.column_stack((points, np.ones(len(points)))) @ projection(pose[:3], pose[3:], *size, lens, near).T
    depth = clip[:, 3]
    safe = np.where(depth > 0, depth, np.finfo(float).eps)
    pixels = (clip[:, :2] / safe[:, None] * [1, -1] + 1) * np.asarray(size) / 2
    return pixels, depth


def edge_visibility(query, eye, start, end, tolerance):
    """Sample edge midpoints; a nearer source surface hides the corresponding segment."""
    points = np.linspace(start, end, EDGE_SAMPLES + 1)
    midpoints = (points[:-1] + points[1:]) / 2
    directions = midpoints - eye
    distances = np.linalg.norm(directions, axis=1)
    directions /= np.maximum(distances[:, None], np.finfo(float).eps)
    origins = np.broadcast_to(eye, directions.shape)
    hit, first, _ = query.first_hits(origins, directions)
    return points, ~(hit & (first < distances - tolerance))


def legend_rows(shapes, owners, material_names, palette=SLOTS):
    """Dominant source materials label ranked shapes; overflow shares one neutral row."""
    ranked = sorted(set(owners), key=lambda i: (-len(shapes[i].triangles) * (len(shapes[i].instances) - 1), i))
    names = [material_names[int(np.bincount(shapes[i].materials).argmax())] for i in ranked]
    duplicates, used = Counter(names), Counter()
    colours, rows = {}, []
    for slot, (index, name) in enumerate(zip(ranked, names)):
        used[name] += 1
        label = f'{name} ({used[name]})' if duplicates[name] > 1 else name
        colours[index] = palette[slot] if slot < len(palette) else MUTED
        if slot < len(palette):
            rows.append((colours[index], label, len(shapes[index].instances)))
    if len(ranked) > len(palette):
        rows.append((MUTED, OTHER_LABEL, sum(len(shapes[i].instances) for i in ranked[len(palette):])))
    return colours, rows


def render_host(host, scene, object_name, camera_name, time_ms):
    """The scene advances before drawing: one frame with dt=time reaches the sampled pose."""
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / 'frame.bmp'
        subprocess.run([str(host), '--scene', scene, '--object', object_name, '--camera', camera_name,
                        '--quarter', '1', '--frames', '1', '--dt', str(time_ms), '-o', str(output)], check=True)
        with Image.open(output) as image:
            return image.convert('RGB')


def draw_figure(image, shapes, boxes, owners, shown, pose, lens, near, query, material_names):
    """Skip source-occluded segments and append material swatches below the host image."""
    image = image.copy()
    colours, rows = legend_rows(shapes, owners[shown], material_names)
    draw = ImageDraw.Draw(image)
    tolerance = max((bounds_diagonal(box) for box in boxes), default=0) * OCCLUSION_TOLERANCE
    for box, owner in zip(boxes[shown], owners[shown]):
        for a, b in BOX_EDGES:
            points, visible = edge_visibility(query, pose[:3], box[a], box[b], tolerance)
            pixels, depth = project_points(points, pose, image.size, lens, near)
            for index in np.flatnonzero(visible & (depth[:-1] >= near) & (depth[1:] >= near)):
                draw.line([tuple(pixels[index]), tuple(pixels[index + 1])], fill=colours[owner], width=LINE_WIDTH)
    font = ImageFont.load_default(size=FONT_SIZE)
    legend_width = max((draw.textbbox((0, 0), f'{label}  {count} copies', font=font)[2]
                        for _, label, count in rows), default=0) + ROW_HEIGHT + PADDING * 2
    canvas = Image.new('RGB', (max(image.width, legend_width), image.height + len(rows) * ROW_HEIGHT + PADDING * 2), 'white')
    canvas.paste(image, ((canvas.width - image.width) // 2, 0))
    draw = ImageDraw.Draw(canvas)
    for index, (colour, label, count) in enumerate(rows):
        y = image.height + PADDING + index * ROW_HEIGHT
        draw.rectangle((PADDING, y, PADDING + FONT_SIZE, y + FONT_SIZE), fill=colour)
        draw.text((PADDING + ROW_HEIGHT, y), f'{label}  {count} copies', fill=INK, font=font)
    return canvas


def generate(scene, job, camera, host, out, every_ms, host_render=render_host):
    src = load_object_source(job.settings)
    objects = masked_objects(src, job.settings)
    shapes = match_instances(objects)
    boxes, owners = placed_boxes(shapes, job)
    visibility = SimpleNamespace(size=DOC_SIZE, every_ms=every_ms)
    _, _, lens, near, poses = camera_path_poses(SimpleNamespace(camera=camera), visibility, either_way_up=False)
    index, shown = choose_view(boxes, owners, poses, DOC_SIZE, lens, near)
    time_ms = index * every_ms
    image = host_render(host, scene_id(scene.path), job.object.name, camera.name, time_ms)
    if image.size != DOC_SIZE:
        raise ValueError(f'host image must be {DOC_SIZE}, got {image.size}')
    positions = np.concatenate([obj.positions for obj in objects])
    offsets = np.cumsum([0] + [len(obj.positions) for obj in objects[:-1]])
    triangles = np.concatenate([obj.triangles + offset for obj, offset in zip(objects, offsets)])
    positions = positions @ np.asarray(job.object.matrix).T + job.object.position
    query = RayQuery(positions, triangles)
    picture = draw_figure(image, shapes, boxes, owners, shown, poses[index], lens, near, query, src.names)
    out.parent.mkdir(parents=True, exist_ok=True)
    picture.save(out)
    print(f'chosen time: {time_ms} ms; repeated shapes in view: {len(set(owners[shown]))}')
    return time_ms


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('scene', type=Path)
    parser.add_argument('--object', required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--camera')
    parser.add_argument('--every-ms', type=int, default=DEFAULT_EVERY_MS)
    args = parser.parse_args(argv)
    scene = load_scene(args.scene)
    job = next((job for job in scene.renderers if job.object.name == args.object), None)
    camera = next((obj for obj in scene.objects if obj.kind == 'camera' and
                   (obj.name == args.camera if args.camera else obj.component.path is not None)), None)
    if job is None or job.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    if camera is None or camera.component.path is None:
        parser.error('camera must have a path')
    if args.every_ms <= 0:
        parser.error('--every-ms must be positive')
    generate(scene, job, camera, args.host, args.out, args.every_ms)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

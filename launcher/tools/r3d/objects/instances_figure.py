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

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "render"))

from docs.plot_style import INK, SLOTS  # noqa: E402
from panel_size import PANEL_HEIGHT, PANEL_WIDTH  # noqa: E402
from r3d.appearance_simplify import projection  # noqa: E402
from r3d.cost_model import clusters_in_view  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.objects.instances import bounds_diagonal, match_instances  # noqa: E402
from r3d.objects.object_boxes import placed_boxes  # noqa: E402
from r3d.objects.objects_report import DEFAULT_EVERY_MS, masked_objects  # noqa: E402
from r3d.mesh_import import load_source  # noqa: E402
from r3d.poses import sample_camera_path  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.scene_name import scene_id  # noqa: E402
from render.render_compare import _font  # noqa: E402

DOC_SIZE = (PANEL_HEIGHT, PANEL_WIDTH)
EDGE_SAMPLES = 128
OCCLUSION_TOLERANCE = 1e-4
ROW_HEIGHT = 24
PADDING = 12
FONT_SIZE = 14
LINE_WIDTH = 2
CORNER_BITS = np.array(list(product((0, 1), repeat=3)))
BOX_EDGES = tuple((a, b) for a in range(len(CORNER_BITS)) for b in range(a + 1, len(CORNER_BITS))
                  if np.count_nonzero(CORNER_BITS[a] != CORNER_BITS[b]) == 1)


def choose_view(boxes, owners, poses, size, lens, near, query):
    """Prefer coloured visible length, then distinct shapes, copies, and earliest sample."""
    best, chosen = (-1, -1, -1), None
    bounds = np.stack((boxes.min(axis=1), boxes.max(axis=1)), axis=1)
    for index, pose in enumerate(poses):
        shown = clusters_in_view(bounds, projection(pose[:3], pose[3:], *size, lens, near))
        segments, segment_owners = visible_segments(boxes, owners, pose, size, lens, near, query, shown=shown)
        lengths = Counter()
        for owner, length in zip(segment_owners, np.linalg.norm(segments[:, 1] - segments[:, 0], axis=1)):
            lengths[int(owner)] += float(length)
        coloured_length = sum(sorted(lengths.values(), reverse=True)[:len(SLOTS)])
        score = (coloured_length, len(set(owners[shown])), int(shown.sum()))
        if score > best:
            best, chosen = score, (index, segments, segment_owners, lengths, score)
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
    points = np.linspace(start, end, EDGE_SAMPLES + 1, axis=-2)
    midpoints = (points[..., :-1, :] + points[..., 1:, :]) / 2
    directions = (midpoints - eye).reshape(-1, 3)
    distances = np.linalg.norm(directions, axis=1)
    directions /= np.maximum(distances[:, None], np.finfo(float).eps)
    origins = np.broadcast_to(eye, directions.shape)
    hit, first, _ = query.first_hits(origins, directions)
    return points, (~(hit & (first < distances - tolerance))).reshape(midpoints.shape[:-1])


def visible_segments(boxes, owners, pose, size, lens, near, query, shown=None):
    """Source-visible box segments clipped to the picture, shared by scoring and drawing."""
    if shown is None:
        bounds = np.stack((boxes.min(axis=1), boxes.max(axis=1)), axis=1)
        shown = clusters_in_view(bounds, projection(pose[:3], pose[3:], *size, lens, near))
    edges = boxes[shown][:, BOX_EDGES].reshape(-1, 2, 3)
    edge_owners = np.repeat(owners[shown], len(BOX_EDGES))
    _, depth = project_points(edges.reshape(-1, 3), pose, size, lens, near)
    depth = depth.reshape(-1, 2)
    keep = depth.max(axis=1) >= near
    edges, depth, edge_owners = edges[keep], depth[keep], edge_owners[keep]
    for endpoint in (0, 1):
        clipped = depth[:, endpoint] < near
        fraction = (near - depth[clipped, endpoint]) / (depth[clipped, 1 - endpoint] - depth[clipped, endpoint])
        edges[clipped, endpoint] += fraction[:, None] * (edges[clipped, 1 - endpoint] - edges[clipped, endpoint])
    if not len(edges):
        return np.empty((0, 2, 2)), np.empty(0, dtype=int)
    tolerance = max((bounds_diagonal(box) for box in boxes), default=0) * OCCLUSION_TOLERANCE
    points, visible = edge_visibility(query, pose[:3], edges[:, 0], edges[:, 1], tolerance)
    pixels, _ = project_points(points.reshape(-1, 3), pose, size, lens, near)
    pixels = pixels.reshape(len(edges), EDGE_SAMPLES + 1, 2)
    segments = np.stack((pixels[:, :-1], pixels[:, 1:]), axis=2)[visible]
    segment_owners = np.broadcast_to(edge_owners[:, None], visible.shape)[visible]
    start, delta = segments[:, 0], segments[:, 1] - segments[:, 0]
    lower, upper = np.zeros(len(start)), np.ones(len(start))
    for axis, limit in enumerate(size):
        moving = delta[:, axis] != 0
        first = np.full(len(start), -np.inf)
        last = np.full(len(start), np.inf)
        first[moving] = -start[moving, axis] / delta[moving, axis]
        last[moving] = (limit - start[moving, axis]) / delta[moving, axis]
        lower = np.maximum(lower, np.minimum(first, last))
        upper = np.minimum(upper, np.maximum(first, last))
        upper[~moving & ((start[:, axis] < 0) | (start[:, axis] > limit))] = -1
    keep = upper > lower
    clipped = start[keep, None] + np.stack((lower[keep], upper[keep]), axis=1)[..., None] * delta[keep, None]
    return clipped, segment_owners[keep]


def legend_rows(shapes, lengths, material_names, palette=SLOTS):
    """Rank by visible length; material names and triangle counts distinguish shapes."""
    ranked = sorted((i for i in lengths if lengths[i] > 0), key=lambda i: (-lengths[i], i))[:len(palette)]
    repeated = [i for i, shape in enumerate(shapes) if len(shape.instances) > 1]
    names = {i: material_names[int(np.bincount(shapes[i].materials).argmax())] for i in repeated}
    colours, rows = {}, []
    for slot, index in enumerate(ranked):
        name = names[index]
        label = f'{name}, {len(shapes[index].triangles)} tris'
        colours[index] = palette[slot]
        rows.append((colours[index], label, len(shapes[index].instances)))
    overflow = len(repeated) - len(ranked)
    if overflow:
        rows.append((None, f'{overflow} more repeated shapes not drawn', None))
    return colours, rows


def render_host(host, scene, object_name, camera_name, time_ms):
    """The scene advances before drawing: one frame with dt=time reaches the sampled pose."""
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / 'frame.bmp'
        subprocess.run([str(host), '--scene', scene, '--object', object_name, '--camera', camera_name,
                        '--quarter', '1', '--frames', '1', '--dt', str(time_ms), '-o', str(output)], check=True)
        with Image.open(output) as image:
            return image.convert('RGB')


def draw_figure(image, segments, owners, colours, rows):
    """Draw only selected shapes and append their material legend."""
    image = image.copy()
    draw = ImageDraw.Draw(image)
    for segment, owner in zip(segments, owners):
        if owner in colours:
            draw.line([tuple(point) for point in segment], fill=colours[owner], width=LINE_WIDTH)
    font = _font()
    labels = [f'{label}, {count} copies' if count is not None else label for _, label, count in rows]
    legend_width = max((draw.textbbox((0, 0), label, font=font)[2] + (ROW_HEIGHT if colour else 0)
                        for (colour, _, _), label in zip(rows, labels)), default=0) + PADDING * 2
    canvas = Image.new('RGB', (image.width + legend_width,
                               max(image.height, len(rows) * ROW_HEIGHT + PADDING * 2)), 'white')
    canvas.paste(image, (0, 0))
    draw = ImageDraw.Draw(canvas)
    for index, ((colour, _, _), label) in enumerate(zip(rows, labels)):
        x = image.width + PADDING
        y = PADDING + index * ROW_HEIGHT
        if colour is not None:
            draw.rectangle((x, y, x + FONT_SIZE, y + FONT_SIZE), fill=colour)
        draw.text((x + ROW_HEIGHT if colour else x, y), label, fill=INK, font=font)
    return canvas


def generate(scene, job, cameras, host, out, every_ms, host_render=render_host):
    src = load_source(job.settings)
    objects, triangles = masked_objects(src, job.settings)
    shapes = match_instances(objects)
    boxes, owners = placed_boxes(shapes, job, min_copies=2)
    positions = src.p @ np.asarray(job.object.matrix).T + job.object.position
    query = RayQuery(positions, triangles)
    best, chosen = (-1, -1, -1, -1), None
    for camera in cameras:
        component = camera.component
        _, _, lens, near, poses = sample_camera_path(
            component.path.animation, component.path.node, every_ms, *DOC_SIZE,
            component.half_fov_short_tan, component.near_z)
        index, segments, segment_owners, lengths, score = choose_view(boxes, owners, poses, DOC_SIZE, lens, near, query)
        colours, rows = legend_rows(shapes, lengths, src.names)
        score = (*score, -index)
        if score > best:
            best = score
            chosen = camera, index * every_ms, segments, segment_owners, colours, rows
    if chosen is None:
        raise ValueError('no camera paths')
    camera, time_ms, segments, segment_owners, colours, rows = chosen
    image = host_render(host, scene_id(scene.path), job.object.name, camera.name, time_ms)
    if image.size != DOC_SIZE:
        raise ValueError(f'host image must be {DOC_SIZE}, got {image.size}')
    picture = draw_figure(image, segments, segment_owners, colours, rows)
    out.parent.mkdir(parents=True, exist_ok=True)
    picture.save(out)
    print(f'chosen camera: {camera.name}; time: {time_ms} ms; render: {DOC_SIZE[0]}x{DOC_SIZE[1]}; canvas: {picture.width}x{picture.height}')
    for _, label, count in rows:
        print(f'{label}' + (f', {count} copies' if count is not None else ''))
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
    cameras = [obj for obj in scene.objects if obj.kind == 'camera' and obj.component.path is not None
               and (args.camera is None or obj.name == args.camera)]
    if job is None or job.object.kind != 'mesh_renderer':
        parser.error('--object must name a mesh renderer')
    if not cameras:
        parser.error('camera must have a path')
    if args.every_ms <= 0:
        parser.error('--every-ms must be positive')
    generate(scene, job, cameras, args.host, args.out, args.every_ms)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

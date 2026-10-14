#!/usr/bin/env python3
"""Diagnose enclosed sky and estimate fitted mesh coverage with Mitsuba.

rays
    rays SCENE.scene.toml --mesh DRAWN.mesh --video FRAMES.avi --dt MS
        [--frames 0,1,2] [--against OTHER.mesh ...]
    Use a --quarter 1 host video without its HUD. Frame i samples (i+1)*dt;
    its render size is half the video size. Scalar CPU rays distinguish
    raster misses from geometry holes. Projection agreement should approach
    one; a low value means the pose, size or mesh does not match the video.
    Against meshes are front within 15% of neighbouring drawn depth, behind,
    or miss. A missing neighbour depth is reported separately as unknown.

shown
    shown SCENE.scene.toml --object OBJECT START.mesh [START.mesh ...]
    LLVM CPU first-hit rays count triangles visible in the fit's square
    coverage view, sampled at coverage_every_ms with fit.poses.size. This is
    an estimate of pruning coverage, not a GPU raster coverage measurement.
    Requires Mitsuba and libLLVM; rays requires no LLVM runtime.
"""
import argparse
import pathlib
import sys
import tempfile

import numpy as np
from scipy import ndimage

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from anim import track_host
from r3d.import_settings import load_scene
from r3d.lit_mesh import finest_triangles, read_lit_mesh
from r3d.poses import camera_basis, camera_rays, either_way, read_poses, sample_camera_rows
from r3d.ray_query import MAX_HITS, STEP, STEP_RELATIVE, RayQuery
from render.render_compare import expand_565, read_video

SCALAR_VARIANT = 'scalar_rgb'
LLVM_VARIANT = 'llvm_ad_rgb'
VIDEO_SCALE = 2
PIXEL_SAMPLES = 1
PROJECTION_SAMPLES = 400
NEIGHBOUR_RADIUS_PX = 2
FRONT_DEPTH_TOLERANCE = 0.15
# appearance_simplify.py's --scale default, which fitted_variant.fit keeps: the fit draws its landscape training
# size times this, and counts coverage on that grid.
FIT_RENDER_SCALE = 2


def mesh_query(path, variant):
    mesh = read_lit_mesh(path)
    positions, _, triangles, double, _ = finest_triangles(mesh)
    positions = positions / float(mesh.position_scale)
    a, b, c = (positions[triangles[:, corner]] for corner in range(3))
    return RayQuery(positions, triangles, variant=variant), np.cross(b - a, c - a), double.astype(bool)


def pose_rays(width, height, lens, near, pose):
    origins, directions = camera_rays(width, height, lens, pose[:3], pose[3:], PIXEL_SAMPLES)
    forward = camera_basis(pose[3:])[2]
    origins += directions * (near / (directions @ forward))[:, None]
    return origins, directions


def front_hits(query, normals, double, origins, directions):
    """First drawable triangle and near-plane distance; back faces are transparent."""
    triangles = np.full(len(origins), -1, dtype=np.int64)
    depths = np.full(len(origins), np.inf)
    start = origins.copy()
    alive = np.arange(len(origins))
    for _ in range(MAX_HITS):
        if not len(alive):
            break
        hit, distance, tri = query.first_hits(start[alive], directions[alive])
        alive, distance, tri = alive[hit], distance[hit], tri[hit]
        points = start[alive] + directions[alive] * distance[:, None]
        front = double[tri] | ((normals[tri] * directions[alive]).sum(axis=1) < 0)
        accepted = alive[front]
        triangles[accepted] = tri[front]
        depths[accepted] = ((points[front] - origins[accepted]) * directions[accepted]).sum(axis=1)
        alive, points = alive[~front], points[~front]
        step = np.maximum(STEP, STEP_RELATIVE * np.abs(points).max(axis=1, initial=0.0))
        start[alive] = points + directions[alive] * step[:, None]
    else:
        if len(alive):
            raise RuntimeError(f'{len(alive)} rays exceed the {MAX_HITS}-intersection limit')
    return triangles, depths


def enclosed_sky(frame, sky):
    mask = (frame == np.asarray(sky)).all(axis=2)
    seed = np.zeros_like(mask)
    seed[0] = mask[0]
    return mask, mask & ~ndimage.binary_propagation(seed, mask=mask)


def classify_against(depths, neighbours):
    result = {'front': 0, 'behind': 0, 'miss': 0, 'unknown': 0}
    for depth, nearby in zip(depths, neighbours):
        if not np.isfinite(depth):
            category = 'miss'
        elif not np.isfinite(nearby):
            category = 'unknown'
        elif depth <= nearby * (1 + FRONT_DEPTH_TOLERANCE):
            category = 'front'
        else:
            category = 'behind'
        result[category] += 1
    return result


def diagnose_frame(frame, sky, pose, lens, near, drawn, against, frame_index):
    height, width = frame.shape[:2]
    mask, enclosed = enclosed_sky(frame, sky)
    holes = np.flatnonzero(enclosed)
    origins, directions = pose_rays(width, height, lens, near, pose)
    sample = np.random.default_rng(frame_index).integers(width * height, size=PROJECTION_SAMPLES)
    neighbourhoods = []
    for y, x in zip(*np.nonzero(enclosed)):
        pixels = [yy * width + xx
                  for yy in range(max(0, y - NEIGHBOUR_RADIUS_PX), min(height, y + NEIGHBOUR_RADIUS_PX + 1))
                  for xx in range(max(0, x - NEIGHBOUR_RADIUS_PX), min(width, x + NEIGHBOUR_RADIUS_PX + 1))
                  if not mask[yy, xx]]
        neighbourhoods.append(pixels)
    wanted = np.unique(np.concatenate([holes, sample, *[np.asarray(n, dtype=int) for n in neighbourhoods]]))
    triangles, distances = front_hits(*drawn, origins[wanted], directions[wanted])
    ids = np.full(width * height, -1, dtype=int)
    depths = np.full(width * height, np.inf)
    ids[wanted], depths[wanted] = triangles, distances
    agreement = np.mean((ids[sample] < 0) == mask.ravel()[sample])
    hits = int((ids[holes] >= 0).sum())
    print(f'frame {frame_index}: hole pixels {len(holes)}; hits {hits}; pass through {len(holes) - hits}; '
          f'projection agreement {agreement:.4f}')
    neighbours = []
    for pixels in neighbourhoods:
        values = depths[pixels]
        values = values[np.isfinite(values)]
        neighbours.append(float(np.median(values)) if len(values) else np.inf)
    for name, query in against:
        _, other_depths = front_hits(*query, origins[holes], directions[holes])
        counts = classify_against(other_depths, neighbours)
        print(f'  {name}: ' + '; '.join(f'{key} {value}' for key, value in counts.items()))


def rays(args, scene):
    camera = scene.camera.component
    rgb = camera.background
    sky = expand_565((rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff))
    drawn = mesh_query(args.mesh, SCALAR_VARIANT)
    against = [(str(path), mesh_query(path, SCALAR_VARIANT)) for path in args.against]
    wanted = None if args.frames is None else set(args.frames)
    found = set()
    for index, video_frame in enumerate(read_video(args.video)[1]):
        if wanted is not None and index not in wanted:
            continue
        if any(size % VIDEO_SCALE for size in video_frame.shape[:2]):
            raise ValueError('video dimensions must be divisible by the quarter-render scale')
        frame = video_frame[::VIDEO_SCALE, ::VIDEO_SCALE]
        if wanted is None and not enclosed_sky(frame, sky)[1].any():
            continue
        height, width = frame.shape[:2]
        text = track_host.poses(camera.path.animation, camera.path.node, args.dt, width, height,
                                camera.half_fov_short_tan, camera.near_z,
                                until_ms=(index + 2) * args.dt, number_format='.9g')
        with tempfile.TemporaryDirectory(prefix='pinhole_poses_') as work:
            path = pathlib.Path(work) / 'poses.txt'
            path.write_text(text)
            _, _, lens, near, poses = read_poses(path)
        if len(poses) <= index + 1:
            raise ValueError(f'camera path has no pose for frame {index}')
        diagnose_frame(frame, sky, poses[index + 1], lens, near, drawn, against, index)
        found.add(index)
    if wanted is not None and wanted - found:
        raise ValueError(f'frames absent from video: {sorted(wanted - found)}')


def grid_rays(width, height, lens, near, pose, grid):
    """One ray per pixel centre of a `grid` (width, height) stretched over the pose's width x height view, as
    appearance_simplify.coverage draws a pose at the fit's render size."""
    right, up, forward = camera_basis(pose[3:])
    columns, rows = grid
    x, y = np.meshgrid((np.arange(columns) + 0.5) / columns, (np.arange(rows) + 0.5) / rows)
    short = min(width, height)
    directions = (forward + right * ((2 * x.ravel() - 1) * lens * width / short)[:, None]
                  + up * ((1 - 2 * y.ravel()) * lens * height / short)[:, None])
    directions /= np.linalg.norm(directions, axis=1, keepdims=True)
    origins = np.asarray(pose[:3], dtype=float) + directions * (near / (directions @ forward))[:, None]
    return origins, directions


def count_shown(path, coverage, grid, variant=LLVM_VARIANT):
    width, height, lens, near, poses = coverage
    query, normals, double = mesh_query(path, variant)
    seen = np.zeros(len(normals), bool)
    for pose in poses:
        origins, directions = grid_rays(width, height, lens, near, pose, grid)
        triangles, _ = front_hits(query, normals, double, origins, directions)
        seen[triangles[triangles >= 0]] = True
    return int(seen.sum()), len(seen)


def shown(args, scene):
    jobs = [job for job in scene.renderers if job.object.name == args.object and job.renderer.fit is not None]
    if len(jobs) != 1:
        raise ValueError('--object must select exactly one fitted mesh renderer')
    fit = jobs[0].renderer.fit
    camera = scene.camera.component
    coverage = either_way(*sample_camera_rows(camera.path.animation, camera.path.node, fit.coverage_every_ms,
                                              *fit.size, camera.half_fov_short_tan, camera.near_z))
    grid = (fit.size[1] * FIT_RENDER_SCALE, fit.size[0] * FIT_RENDER_SCALE)
    for path in args.meshes:
        visible, total = count_shown(path, coverage, grid)
        print(f'{path}: triangles {total}; shown {visible}', flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    ray_parser = commands.add_parser('rays')
    ray_parser.add_argument('scene', type=pathlib.Path)
    ray_parser.add_argument('--mesh', required=True, type=pathlib.Path)
    ray_parser.add_argument('--video', required=True, type=pathlib.Path)
    ray_parser.add_argument('--dt', required=True, type=int)
    ray_parser.add_argument('--frames', type=lambda text: [int(item) for item in text.split(',')])
    ray_parser.add_argument('--against', nargs='+', action='extend', default=[], type=pathlib.Path)
    shown_parser = commands.add_parser('shown')
    shown_parser.add_argument('scene', type=pathlib.Path)
    shown_parser.add_argument('--object', required=True)
    shown_parser.add_argument('meshes', nargs='+', type=pathlib.Path)
    args = parser.parse_args(argv)
    if args.command == 'rays' and (args.dt <= 0 or args.frames is not None and any(i < 0 for i in args.frames)):
        parser.error('dt must be positive and frame indices nonnegative')
    try:
        scene = load_scene(args.scene)
        if args.command == 'rays':
            rays(args, scene)
        else:
            shown(args, scene)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'pinhole_rays: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

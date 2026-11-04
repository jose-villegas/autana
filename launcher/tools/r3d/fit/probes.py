"""Fit views from probes spread through a region, beside the camera path's (experiment).

    python -m r3d.fit.probes select SCENE.scene.toml --object NAME --work DIR --spacing S [--face PX] [--seed N]
    python -m r3d.fit.probes render SCENE.scene.toml --object NAME --work DIR

Run from launcher/tools. DIR is a `fitted_variant.py prepare` folder: the start mesh and the path's coverage poses.
`select` puts one jittered probe in every `spacing`-sized cell of the scene camera's region (the source's bounds
without one), moves probes out of geometry and drops those still inside, gives each probe six 90 degree cube
faces, and keeps faces greedily by the start triangles they show that neither the path nor a kept face shows yet.
It writes `probes.txt` (kept faces), `probes_held_out.txt` (faces of a second seed, scored only) and
`probes.json` (every probe, where it moved and why it was dropped). `render` traces their references.

Inside geometry is the DDGI probe classification: a probe whose rays hit back faces more than BACKFACE of the time
is inside, and one too near a front face would clip its views. Either moves at most RELOCATE of a cell first.
"""

import argparse
import heapq
import json
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from r3d import log  # noqa: E402

RAYS = 32
BACKFACE = 0.25
RELOCATE = 0.45
JITTER = 0.4
# Nearest front face a probe may have, in near planes: nearer, its faces clip the surface into holes.
MIN_FRONT_NEARS = 2.0
# Cube face field: tan of 45 degrees.
FACE_LENS = 1.0
# Off vertical, so camera_basis (world +y up) has a right vector for the up and down faces.
VERTICAL_NUDGE = 1e-4
FACES = ((1, 0, 0), (-1, 0, 0), (0, 1, VERTICAL_NUDGE), (0, -1, VERTICAL_NUDGE), (0, 0, 1), (0, 0, -1))
FACE_NAMES = ("+x", "-x", "+y", "-y", "+z", "-z")
# Pixels a triangle needs in a face before the face counts it as shown.
MIN_PIXELS = 4
# Greedy selection stops when a face adds less than this share of the first face's gain.
PLATEAU = 0.01
SELECT_SIZE = 96
HELD_OUT_FACES = 48


def probe_points(lo, hi, spacing, seed):
    """One point per `spacing` cell of the box lo..hi, jittered inside its cell."""
    lo, hi = np.asarray(lo, dtype=float), np.asarray(hi, dtype=float)
    cells = np.maximum(np.ceil((hi - lo) / spacing).astype(int), 1)
    step = (hi - lo) / cells
    grid = np.stack(np.meshgrid(*[np.arange(n) for n in cells], indexing="ij"), axis=-1).reshape(-1, 3)
    jitter = np.random.default_rng(seed).uniform(-JITTER, JITTER, grid.shape)
    return lo + (grid + 0.5 + jitter) * step, float(step.min())


def sphere_directions(count):
    """`count` near-even unit directions (Fibonacci sphere)."""
    i = np.arange(count) + 0.5
    polar = np.arccos(1 - 2 * i / count)
    azimuth = np.pi * (1 + 5 ** 0.5) * i
    return np.stack([np.cos(azimuth) * np.sin(polar), np.cos(polar), np.sin(azimuth) * np.sin(polar)], axis=1)


def probe_hits(query, normals, points):
    """Per probe and ray: hit, distance, and whether the hit face's back is toward the probe."""
    directions = sphere_directions(RAYS)
    origins = np.repeat(points, RAYS, axis=0)
    rays = np.tile(directions, (len(points), 1))
    hit, distance, tri = query.first_hits(origins, rays)
    back = hit & (np.einsum("ij,ij->i", normals[np.where(hit, tri, 0)], rays) > 0)
    shape = (len(points), RAYS)
    return hit.reshape(shape), np.where(hit, distance, np.inf).reshape(shape), back.reshape(shape), directions


def classify(positions, tris, points, cell, near):
    """(moved points, state per probe: "kept", "inside" or "near"), moving each probe at most RELOCATE * cell
    through its nearest back face, or away from a front face nearer than MIN_FRONT_NEARS near planes, then testing
    again."""
    from r3d.ray_query import RayQuery

    a, b, c = (positions[tris[:, k]] for k in range(3))
    normals = np.cross(b - a, c - a)
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-30)
    query = RayQuery(positions, tris)
    min_front = MIN_FRONT_NEARS * near
    moved = points.copy()
    hit, distance, back, directions = probe_hits(query, normals, moved)
    for i in range(len(points)):
        if back[i].mean() > BACKFACE:
            ray = np.argmin(np.where(back[i], distance[i], np.inf))
            moved[i] += directions[ray] * min(distance[i, ray] + min_front, RELOCATE * cell)
            continue
        front = np.where(hit[i] & ~back[i], distance[i], np.inf)
        ray = np.argmin(front)
        if front[ray] < min_front:
            moved[i] -= directions[ray] * min(min_front - front[ray], RELOCATE * cell)
    hit, distance, back, _ = probe_hits(query, normals, moved)
    front = np.where(hit & ~back, distance, np.inf).min(axis=1)
    state = np.where(back.mean(axis=1) > BACKFACE, "inside", np.where(front < min_front, "near", "kept"))
    return moved, state


def cube_poses(points):
    """Six (eye, forward) poses per point, in FACES order."""
    return [np.concatenate([point, np.asarray(face, dtype=float)]) for point in points for face in FACES]


def shown_sets(mesh, poses, size, near):
    """Per pose, the start triangles it shows at MIN_PIXELS or more, at a square `size` with the cube lens."""
    from r3d.appearance_simplify import projection, visible
    import torch

    views = [(projection(pose[:3], pose[3:], size, size, FACE_LENS, near),) for pose in poses]
    sets = []
    for ids in visible(mesh, views, (size, size)):
        counts = torch.bincount(ids[ids >= 0], minlength=len(mesh.tris))
        sets.append(torch.nonzero(counts >= MIN_PIXELS).flatten().cpu().numpy())
    return sets


def path_shown(mesh, poses_path):
    """The start triangles the path's coverage poses show at MIN_PIXELS or more."""
    from r3d.appearance_simplify import pose_views, visible
    import torch

    views, size = pose_views(poses_path, 1)
    seen = torch.zeros(len(mesh.tris), dtype=torch.int64, device="cuda")
    for ids in visible(mesh, views, size):
        seen += torch.bincount(ids[ids >= 0], minlength=len(mesh.tris)) >= MIN_PIXELS
    return (seen > 0).cpu().numpy()


def greedy_faces(sets, seen, plateau=PLATEAU, limit=None):
    """Lazy greedy maximum coverage: (picked face indices, each pick's gain), picking the face that adds the most
    triangles not in `seen`, until a pick adds less than `plateau` of the first pick's gain."""
    seen = seen.copy()
    heap = [(-int(np.count_nonzero(~seen[s])), i) for i, s in enumerate(sets)]
    heapq.heapify(heap)
    picked, gains = [], []
    while heap and (limit is None or len(picked) < limit):
        stale, i = heapq.heappop(heap)
        gain = int(np.count_nonzero(~seen[sets[i]]))
        if gain != -stale:
            heapq.heappush(heap, (-gain, i))
            continue
        if gain == 0 or (gains and gain < plateau * gains[0]):
            break
        picked.append(i)
        gains.append(gain)
        seen[sets[i]] = True
    return picked, gains


def select(scene_path, object_name, work, spacing, face, seed):
    from r3d.appearance_simplify import start_mesh
    from r3d.fitted_variant import placed_variant
    from r3d.import_settings import load_scene
    from r3d.reference_render import source_for
    from r3d.reference_set import poses_text

    work = pathlib.Path(work)
    scene = load_scene(scene_path)
    job = placed_variant(scene, object_name)
    near = scene.camera.component.near_z
    source, _ = source_for(scene, job.object.name, lit=False)
    positions, tris = source.p, source.tri_v
    lo, hi = scene.region if scene.region is not None else (positions.min(axis=0), positions.max(axis=0))
    mesh = start_mesh(work / f"{job.renderer.variant.name}.mesh")
    seen = path_shown(mesh, work / "coverage.txt")
    log(f"path shows {np.count_nonzero(seen)} of {len(mesh.tris)} start triangles")
    record = {"spacing": spacing, "face": face, "seed": seed, "region": [list(map(float, lo)), list(map(float, hi))],
              "start_triangles": len(mesh.tris), "path_shown": int(np.count_nonzero(seen))}
    held_out = []
    for role, role_seed in (("train", seed), ("held_out", seed + 1)):
        points, cell = probe_points(lo, hi, spacing, role_seed)
        moved, state = classify(positions, tris, points, cell, near)
        live = np.flatnonzero(state == "kept")
        poses = cube_poses(moved[live])
        sets = shown_sets(mesh, poses, SELECT_SIZE, near)
        start = seen if role == "train" else np.zeros_like(seen)
        picked, gains = greedy_faces(sets, start, limit=None if role == "train" else HELD_OUT_FACES)
        log(f"{role}: {len(points)} probes, {np.count_nonzero(state == 'inside')} inside, "
            f"{np.count_nonzero(state == 'near')} near, {len(live)} live; {len(picked)} of {len(poses)} faces kept")
        faces = {(int(live[i // 6]), i % 6): gain for i, gain in zip(picked, gains)}
        record[role] = {
            "probes": [{"grid": list(map(float, points[p])), "at": list(map(float, moved[p])), "state": str(state[p]),
                        "faces": [FACE_NAMES[k] for k in range(6) if (p, k) in faces],
                        "gains": [faces[p, k] for k in range(6) if (p, k) in faces]} for p in range(len(points))],
            "picked": [[int(live[i // 6]), FACE_NAMES[i % 6], g] for i, g in zip(picked, gains)],
            "shown_after": int(np.count_nonzero(start)) + int(sum(gains))}
        kept_poses = [poses[i] for i in picked]
        name = "probes.txt" if role == "train" else "probes_held_out.txt"
        (work / name).write_text(poses_text(face, face, FACE_LENS, near, kept_poses))
        if role == "held_out":
            held_out = kept_poses
    (work / "probes.json").write_text(json.dumps(record) + "\n")
    return record


def render(scene_path, object_name, work):
    from r3d.fitted_variant import placed_variant
    from r3d.import_settings import load_scene
    from r3d.reference_render import render_sets
    from r3d.reference_set import reference_inputs

    work = pathlib.Path(work)
    scene = load_scene(scene_path)
    job = placed_variant(scene, object_name)
    render_sets(reference_inputs(job, scene), [(work / "probes.txt", work / "reference_probes"),
                                               (work / "probes_held_out.txt", work / "reference_probes_held_out")])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("select", "render"))
    parser.add_argument("scene")
    parser.add_argument("--object", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--spacing", type=float)
    parser.add_argument("--face", type=int, default=192)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args(argv)
    if args.step == "select":
        if not args.spacing or args.spacing <= 0:
            parser.error("select needs a positive --spacing")
        select(pathlib.Path(args.scene).resolve(), args.object, args.work, args.spacing, args.face, args.seed)
    else:
        render(pathlib.Path(args.scene).resolve(), args.object, args.work)
    return 0


if __name__ == "__main__":
    sys.exit(main())

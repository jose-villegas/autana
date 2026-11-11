"""Fit views from probes spread through a region, beside the camera path's (geometry.probes in an import).

`select` puts one jittered probe in every `spacing`-sized cell of the scene camera's region (the source's bounds
without one), moves probes out of geometry and drops those still inside, gives each probe six 90 degree cube
faces, and keeps faces greedily by the source triangles they show that neither the path nor a kept face shows
yet. Shown sets are traced on the source alone, so the probes, like the rest of a reference set, never depend on
the start. Probes reach surface seen from somewhere off the path, never surface seen from nowhere.

Inside geometry is DDGI's probe classification and relocation (Majercik et al. 2021): a probe whose rays hit back
faces more than BACKFACE of the time is inside, and one too near a front face would clip its views. Either moves at
most RELOCATE of a cell first. The pick is lazy greedy maximum coverage (Minoux 1978).
"""

import heapq

import numpy as np

from r3d.geometry import face_normals
from r3d.poses import camera_rays

# Rays each probe casts to classify itself.
RAYS = 32
# Share of those rays hitting back faces past which the probe is inside geometry.
BACKFACE = 0.25
# Farthest a probe moves off a wall or out of geometry, as a share of its cell.
RELOCATE = 0.45
# Largest offset of a probe from its cell centre, as a share of the cell.
JITTER = 0.4
# Nearest front face a probe may have, in near planes: nearer, its faces clip the surface into holes.
MIN_FRONT_NEARS = 2.0
# Probe spacing without geometry.probes.spacing, in near planes: while imports keep their source's units, the
# camera's near plane is the one length every scene states; a world-unit constant once imports share one unit.
PROBE_SPACING_NEAR_PLANES = 32.0
# Most probes a region gets; a finer spacing widens to stay under it.
PROBE_COUNT_MAX = 1024
# Cube face field: tan of 45 degrees.
FACE_LENS = 1.0
# Off vertical, so camera_basis (world +y up) has a right vector for the up and down faces.
VERTICAL_NUDGE = 1e-4
FACES = ((1, 0, 0), (-1, 0, 0), (0, 1, VERTICAL_NUDGE), (0, -1, VERTICAL_NUDGE), (0, 0, 1), (0, 0, -1))
# Pixels a triangle needs in a view before the view counts it as shown.
MIN_PIXELS = 4
# Greedy selection stops when a face adds less than this share of the first face's gain.
PLATEAU = 0.01
# Square pixels a face is traced at to choose it; the kept faces render at geometry.probes.face.
SELECT_SIZE = 96
# Faces the held-out set keeps, to count holes over without fitting on them.
HELD_OUT_FACES = 48
# A kept face's square size in pixels without geometry.probes.face.
PROBE_FACE = 192


def spacing_for(lo, hi, spacing, near):
    """The probe spacing: `spacing`, or PROBE_SPACING_NEAR_PLANES near planes without one, widened until the box
    lo..hi holds at most PROBE_COUNT_MAX cells."""
    spacing = PROBE_SPACING_NEAR_PLANES * near if spacing is None else spacing
    extent = np.maximum(np.asarray(hi, dtype=float) - np.asarray(lo, dtype=float), 0.0)
    while np.prod(np.maximum(np.ceil(extent / spacing), 1)) > PROBE_COUNT_MAX:
        spacing *= 2 ** (1 / 3)
    return spacing


def probe_points(lo, hi, spacing, seed):
    """(one point per `spacing` cell of the box lo..hi, jittered inside its cell; the cell's shortest side)."""
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
    rays = np.tile(directions, (len(points), 1))
    hit, distance, tri = query.first_hits(np.repeat(points, RAYS, axis=0), rays)
    back = hit & (np.einsum("ij,ij->i", normals[np.where(hit, tri, 0)], rays) > 0)
    shape = (len(points), RAYS)
    return hit.reshape(shape), np.where(hit, distance, np.inf).reshape(shape), back.reshape(shape), directions


def classify(query, normals, points, cell, near):
    """(moved points, state per probe: "kept", "inside" or "near"), moving each probe at most RELOCATE * cell
    through its nearest back face, or away from a front face nearer than MIN_FRONT_NEARS near planes, then testing
    again."""
    min_front = MIN_FRONT_NEARS * near
    moved = np.array(points, dtype=float)
    hit, distance, back, directions = probe_hits(query, normals, moved)
    for i in range(len(moved)):
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


def shown(query, triangles, poses, width, height, lens):
    """Per pose, the triangles it shows at MIN_PIXELS or more, one ray per pixel."""
    sets = []
    for pose in poses:
        origins, directions = camera_rays(width, height, lens, pose[:3], pose[3:6], 1)
        hit, _, tri = query.first_hits(origins, directions)
        counts = np.bincount(tri[hit], minlength=triangles)
        sets.append(np.flatnonzero(counts >= MIN_PIXELS))
    return sets


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


def select(positions, tris, region, near, spacing, seed, path=None):
    """(training poses, held-out poses, record): the kept cube faces of probes in `region` (lo, hi), or the
    source's bounds when None, beside what the `path` views ((width, height, lens, poses), or None) show. The
    held-out faces come from a second seed and start from nothing shown."""
    from r3d.ray_query import RayQuery

    positions, tris = np.asarray(positions, dtype=float), np.asarray(tris)
    lo, hi = region if region is not None else (positions.min(axis=0), positions.max(axis=0))
    spacing = spacing_for(lo, hi, spacing, near)
    query, normals = RayQuery(positions, tris), face_normals(positions, tris)[0]
    seen = np.zeros(len(tris), bool)
    if path is not None:
        for triangles in shown(query, len(tris), path[3], *path[:3]):
            seen[triangles] = True
    record = {"spacing": spacing, "region": [list(map(float, lo)), list(map(float, hi))],
              "triangles": len(tris), "path_shown": int(np.count_nonzero(seen))}
    kept = {}
    for role, role_seed, start, limit in (("train", seed, seen, None),
                                          ("held_out", seed + 1, np.zeros_like(seen), HELD_OUT_FACES)):
        points, cell = probe_points(lo, hi, spacing, role_seed)
        moved, state = classify(query, normals, points, cell, near)
        poses = cube_poses(moved[state == "kept"])
        picked, gains = greedy_faces(shown(query, len(tris), poses, SELECT_SIZE, SELECT_SIZE, FACE_LENS), start,
                                     limit=limit)
        kept[role] = [poses[i] for i in picked]
        record[role] = {"probes": len(points), "inside": int(np.count_nonzero(state == "inside")),
                        "near": int(np.count_nonzero(state == "near")), "faces": len(poses), "kept": len(picked),
                        "shown_after": int(np.count_nonzero(start)) + int(sum(gains))}
    return kept["train"], kept["held_out"], record

"""Views from probes spread through a region, beside the camera path's. The fit uses them (`geometry.probes` in an
import).

`select` puts one jittered probe in every `spacing`-sized cell of the scene camera's region (the source's bounds
without one), moves probes out of geometry and drops those still inside, gives each probe six 90 degree cube
faces, and keeps faces greedily by the target triangles (every triangle without `targets`) they show that neither
the path nor a kept face shows yet. Every triangle occludes; a `query` with a hit filter lets rays through cut-out
texels. Shown sets are traced on the source alone, so the probes, like the rest of a reference set, never depend on
the start. Probes reach surface seen from somewhere off the path, never surface seen from nowhere.

Inside geometry is DDGI's probe classification and relocation (Majercik et al. 2021): a probe whose rays hit back
faces more than BACKFACE of the time is inside, and one too near a front face would clip its views. Either moves at
most RELOCATE of a cell first. The pick is lazy greedy maximum coverage (Minoux 1978).
"""

import heapq
import math
from typing import NamedTuple

import numpy as np

from r3d.geometry import face_normals
from r3d.poses import camera_rays
from r3d.ray_query import RayQuery

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
# Near plane for a source without a scene camera, as a share of its bounds diagonal.
PROBE_NEAR_SHARE = 1e-3
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
    """`count` near-even unit directions (Fibonacci sphere). The trigonometry is the C library's, one value at a
    time: NumPy's float64 sin and cos take AVX-512 kernels where the host has them, an ulp off the AVX2 ones, and
    an ulp in a direction moves a probe across a threshold, so two runners would plan different faces. The C
    library's are the same on every x86-64-v3 Linux host, the scope r3d/isa.py pins."""
    rows = []
    for index in range(count):
        height = 1 - 2 * (index + 0.5) / count
        ring, azimuth = math.sqrt(1 - height * height), math.pi * (1 + math.sqrt(5)) * (index + 0.5)
        rows.append((math.cos(azimuth) * ring, height, math.sin(azimuth) * ring))
    return np.array(rows)


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


class ProbeViews(NamedTuple):
    training: list  # (eye, forward) per kept cube face
    held_out: list
    record: dict
    face_lens: float = FACE_LENS


def select(positions, tris, region, near, spacing, seed, path=None, *, query=None, targets=None,
           select_size=SELECT_SIZE, face_lens=FACE_LENS, held_out_faces=HELD_OUT_FACES):
    """ProbeViews: the kept cube faces of probes in `region` (lo, hi), or the source's bounds when None, beside what
    the `path` views ((width, height, lens, poses), or None) show. `query` is a RayQuery over positions and tris
    (built plain without one); `targets`, a bool per triangle, limits what a face gains to those triangles. Faces
    are traced at `select_size` pixels and `face_lens`. The held-out faces, at most `held_out_faces`, come from a
    second seed and start from nothing shown."""
    positions, tris = np.asarray(positions, dtype=float), np.asarray(tris)
    lo, hi = region if region is not None else (positions.min(axis=0), positions.max(axis=0))
    near = float(np.linalg.norm(np.ptp(positions, axis=0))) * PROBE_NEAR_SHARE if near is None else near
    if near <= 0:
        raise ValueError("probe source bounds have no extent")
    spacing = spacing_for(lo, hi, spacing, near)
    query = RayQuery(positions, tris) if query is None else query
    normals = face_normals(positions, tris)[0]
    unseen = np.zeros(len(tris), bool) if targets is None else ~np.asarray(targets, dtype=bool)
    seen = unseen.copy()
    if path is not None:
        for triangles in shown(query, len(tris), path[3], *path[:3]):
            seen[triangles] = True
    record = {"spacing": spacing, "region": [list(map(float, lo)), list(map(float, hi))],
              "triangles": len(tris), "path_shown": int(np.count_nonzero(seen & ~unseen))}
    if targets is not None:
        record["targets"] = int(np.count_nonzero(~unseen))
    kept = {}
    for role, role_seed, start, limit in (("train", seed, seen, None),
                                          ("held_out", seed + 1, unseen, held_out_faces)):
        points, cell = probe_points(lo, hi, spacing, role_seed)
        moved, state = classify(query, normals, points, cell, near)
        poses = cube_poses(moved[state == "kept"])
        picked, gains = greedy_faces(shown(query, len(tris), poses, select_size, select_size, face_lens), start,
                                     limit=limit)
        kept[role] = [poses[i] for i in picked]
        record[role] = {"probes": len(points), "inside": int(np.count_nonzero(state == "inside")),
                        "near": int(np.count_nonzero(state == "near")), "faces": len(poses), "kept": len(picked),
                        "shown_after": int(np.count_nonzero(start & ~unseen)) + int(sum(gains))}
    return ProbeViews(kept["train"], kept["held_out"], record, face_lens)

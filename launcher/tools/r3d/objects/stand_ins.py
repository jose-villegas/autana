"""Choose a carved shape's budgeted geometry by its unlit appearance from every side."""

from dataclasses import dataclass
from itertools import product

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

from render.colour_maths import lab
from r3d import log
from r3d.geometry import bounds_diagonal, closest_point_on_triangles
from r3d.meshopt import simplify_with_update
from r3d.objects.contact import _surface_border
from r3d.ray_query import RayQuery
from r3d.simplify import _label_after, object_groups

# Every side a copy can be seen from, without a scene camera.
VIEW_DIRECTIONS = np.array([v for v in product((-1., 0., 1.), repeat=3) if any(v)])
VIEW_DIRECTIONS /= np.linalg.norm(VIEW_DIRECTIONS, axis=1, keepdims=True)
# About the on-screen diameter of a copy at its p95.
VIEW_PIXELS = 96
# A plate's thickness must be small relative to both in-plane dimensions.
PLATE_RATIO = 0.1
# Slightly misaligned plates may still describe one surface.
PARALLEL_COS = 0.97
# Plane displacement is relative to the whole shape, independent of source units.
PLANE_SHARE = 0.02
# Small gaps between separate plates may be filled by one surface.
TOUCH_SHARE = 0.03
# Numerically equal scores keep the earlier candidate.
ERROR_TIE = 1e-9
# Vertex colours and the two backgrounds use the source's byte channel scale.
RGB_MAX = 255.
# Both windings let a rectangle be seen from either side.
RECTANGLE_TRIS = np.array([[0, 1, 2], [0, 2, 3], [2, 1, 0], [3, 2, 0]])


@dataclass(frozen=True)
class StandIn:
    """One way to bake a carved shape at its budget, in the shape's local frame."""
    kind: str
    positions: np.ndarray
    rgb: np.ndarray
    tris: np.ndarray
    materials: np.ndarray
    kept: np.ndarray | None


def regular(positions, rgb, tris, materials, budget, weight, options, lock):
    p, c, t, kept = simplify_with_update(positions, rgb, tris, budget, weight, options, vertex_lock=lock)
    return StandIn('regular', p, c, t, _label_after(tris, materials, kept, t), kept)


def _frame(points):
    centre = points.mean(axis=0)
    _, axes = np.linalg.eigh((points - centre).T @ (points - centre))
    local = (points - centre) @ axes
    order = np.argsort(np.ptp(local, axis=0))
    axes, local = axes[:, order], local[:, order]
    return centre, axes, local.min(axis=0), local.max(axis=0)


def plane(positions, rgb, tris, materials, budget, weight, options, lock):
    if lock is not None and np.any(lock):
        return None
    parts = object_groups(positions, tris, np.arange(len(tris)))
    plates = []
    for part in np.unique(parts):
        faces = np.flatnonzero(parts == part)
        vertices = np.unique(tris[faces])
        centre, axes, low, high = _frame(positions[vertices])
        extents = high - low
        if extents[1] > 0 and extents[0] <= PLATE_RATIO * extents[1]:
            plates.append((faces, vertices, centre, axes[:, 0]))
    links = []
    diagonal = bounds_diagonal(positions)
    for a, (fa, va, ca, na) in enumerate(plates):
        for b in range(a):
            fb, vb, cb, nb = plates[b]
            if abs(na @ nb) < PARALLEL_COS:
                continue
            if max(abs((ca - cb) @ na), abs((ca - cb) @ nb)) > PLANE_SHARE * diagonal:
                continue
            tolerance = TOUCH_SHARE * diagonal
            if (_surface_border(positions, va, tris[fb], tolerance)[0] or
                    _surface_border(positions, vb, tris[fa], tolerance)[0]):
                links.append((a, b))
    a, b = np.asarray(links, dtype=int).reshape(-1, 2).T
    graph = coo_matrix((np.ones(len(a)), (a, b)), shape=(len(plates), len(plates)))
    count, labels = connected_components(graph, directed=False)
    groups = [np.flatnonzero(labels == group) for group in range(count)]
    groups = [group for group in groups if len(group) >= 2]
    if not groups or len(groups) * len(RECTANGLE_TRIS) > budget:
        return None
    out_p, out_c, out_t, out_m = [], [], [], []
    remaining = np.ones(len(tris), dtype=bool)
    for group in groups:
        faces = np.concatenate([plates[i][0] for i in group])
        vertices = np.unique(tris[faces])
        remaining[faces] = False
        centre, axes, low, high = _frame(positions[vertices])
        corners = np.array([[0., low[1], low[2]], [0., high[1], low[2]],
                            [0., high[1], high[2]], [0., low[1], high[2]]])
        out_p.append(corners @ axes.T + centre)
        out_c.append(np.tile(rgb[vertices].mean(axis=0), (len(corners), 1)))
        out_t.append(RECTANGLE_TRIS + sum(len(p) for p in out_p[:-1]))
        values, counts = np.unique(materials[faces], return_counts=True)
        out_m.append(np.full(len(RECTANGLE_TRIS), values[np.argmax(counts)]))
    if np.any(remaining):
        used, local = np.unique(tris[remaining], return_inverse=True)
        rest = regular(positions[used], rgb[used], local.reshape(-1, 3), materials[remaining],
                       max(1, budget - len(groups) * len(RECTANGLE_TRIS)), weight, options, None)
        out_t.append(rest.tris + sum(len(p) for p in out_p))
        out_p.append(rest.positions)
        out_c.append(rest.rgb)
        out_m.append(rest.materials)
    if sum(len(t) for t in out_t) > budget:
        return None
    return StandIn('plane', np.concatenate(out_p), np.concatenate(out_c), np.concatenate(out_t),
                   np.concatenate(out_m), None)


def _draw(mesh, query, origins, directions):
    colours = np.zeros((len(origins), 3))
    locations, rays, hits = query.first_hit(origins, directions)
    faces = mesh.positions[mesh.tris[hits]]
    bary, _ = closest_point_on_triangles(locations, faces[:, 0], faces[:, 1], faces[:, 2])
    colours[rays] = (mesh.rgb[mesh.tris[hits]] * bary[:, :, None]).sum(axis=1) / RGB_MAX
    covered = np.zeros(len(origins), dtype=bool)
    covered[rays] = True
    return colours, covered


def view_error(source, candidate):
    centre = (source.positions.min(axis=0) + source.positions.max(axis=0)) / 2
    radius = np.linalg.norm(source.positions - centre, axis=1).max()
    if radius == 0:
        return 0.
    coordinates = ((np.arange(VIEW_PIXELS) + 0.5) / VIEW_PIXELS * 2 - 1) * radius
    x, y = np.meshgrid(coordinates, coordinates)
    queries = [RayQuery(mesh.positions, mesh.tris) for mesh in (source, candidate)]
    errors = []
    for direction in VIEW_DIRECTIONS:
        right = np.cross(direction, np.eye(3)[np.argmin(abs(direction))])
        right /= np.linalg.norm(right)
        up = np.cross(right, direction)
        origins = centre + direction * radius * 2 + x.ravel()[:, None] * right + y.ravel()[:, None] * up
        directions = np.broadcast_to(-direction, origins.shape)
        images = [_draw(mesh, query, origins, directions) for mesh, query in zip((source, candidate), queries)]
        for background in (0., 1.):
            a, b = [lab(np, np.where(mask[:, None], colours, background)) for colours, mask in images]
            errors.append(np.linalg.norm(a - b, axis=1).mean())
    return float(np.mean(errors))


CANDIDATES = (regular, plane)


def best_stand_in(positions, rgb, tris, materials, budget, weight, options, lock):
    source = StandIn('source', positions, rgb, tris, materials, np.arange(len(positions)))
    candidates = [candidate for bake in CANDIDATES
                  if (candidate := bake(positions, rgb, tris, materials, budget, weight, options, lock)) is not None]
    errors = [view_error(source, candidate) for candidate in candidates] if len(candidates) > 1 else [None]
    chosen = 0
    for i in range(1, len(candidates)):
        if errors[i] < errors[chosen] - ERROR_TIE:
            chosen = i
    rows = [dict(kind=candidate.kind, triangles=len(candidate.tris), error=error, chosen=i == chosen)
            for i, (candidate, error) in enumerate(zip(candidates, errors))]
    result = candidates[chosen]
    error_text = 'unscored' if errors[chosen] is None else f'error {errors[chosen]:.6g}'
    log(f'  simplified {len(tris)} -> {len(result.tris)} triangles '
        f'(budget {budget}, colour weight {weight:.3g}, {result.kind}, {error_text})')
    return result, rows

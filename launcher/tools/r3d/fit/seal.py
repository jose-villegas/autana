"""The two steps after a fit (appearance_simplify.py), in order: snap_back closes sky the fit shows and its start
did not, then split_t_junctions splits each edge a fitted point lies on.

The fit holds a seam's border points to the other part's triangles, but an edge between two held points stays
straight while the surface under it bends, so sky can open along the edge. snap_back looks from the start's leak
probes (leaks.LeakProbes, the views simplify seals with) and moves the points around each new hole back to their
start positions, a ring wider each round, until no view shows one. A hold also puts a point on another triangle's
edge without sharing it, a T-junction the rasterizer can leave a pixel open along; split_t_junctions splits that
edge through the point, a standard repair that costs a triangle per split.
"""

import numpy as np
from scipy.spatial import cKDTree

from r3d import log
from r3d.leaks import LeakProbes

# A fitted point within this distance (model units) of the inside of an edge it does not end splits that edge.
# A held seam point lands on its target's edge: one fit had 363 points this close where its start had 9.
T_JUNCTION_DISTANCE = 0.05
# Most rounds of moving points back to the start, each a pixel wider round the holes, before the step fails.
SNAP_BACK_ROUNDS = 8


def geometry(mesh, copies):
    """(positions, triangles) of a FitMesh and the copies drawn with it, the mesh's triangles first."""
    positions, tris = mesh.points, mesh.vertex_point[mesh.tris]
    if copies is None:
        return positions, tris
    return (np.concatenate([positions, copies.points]),
            np.concatenate([tris, copies.vertex_point[copies.tris] + len(positions)]))


def start_probes(start, copies, camera):
    """Leak probes over the start and its copies from `camera`, (region, near[, path]) as
    leaks.seal_camera gives it: sky they show counts as the start's own."""
    return LeakProbes(*geometry(start, copies), *camera)


def snap_back(start, fitted, copies, probes):
    """`fitted` with the points round each hole `probes` find in it moved back to `start`, which shares its
    points: (the mesh, how many points moved). Raises ValueError when holes are left after SNAP_BACK_ROUNDS."""
    points = fitted.points.copy()
    moved = np.zeros(len(points), dtype=bool)
    count = len(fitted.tris)
    for width in range(1, SNAP_BACK_ROUNDS + 1):
        rims = probes.rims(*geometry(fitted._replace(points=points), copies), width)
        if not rims:
            return fitted._replace(points=points), int(np.count_nonzero(moved))
        rows = np.unique(np.concatenate(rims))
        ring = np.unique(fitted.vertex_point[fitted.tris[rows[rows < count]]])
        moved[ring] |= np.any(points[ring] != start.points[ring], axis=1)
        points[ring] = start.points[ring]
    left = len(probes.holes(*geometry(fitted._replace(points=points), copies)))
    if left:
        raise ValueError(f"the fit still shows {left} holes its start does not after {SNAP_BACK_ROUNDS} rounds "
                         "of moving points back")
    return fitted._replace(points=points), int(np.count_nonzero(moved))


def split_t_junctions(mesh, distance=T_JUNCTION_DISTANCE):
    """`mesh` with each edge a point lies within `distance` of split through that point, both triangles on a
    shared edge alike: (the mesh, the number of triangles split, each adding one). A smooth mesh's split vertex
    takes the colour along the edge; each half of a flat mesh's triangle keeps the triangle's colour. A pass
    splits a triangle once; the next looks only at the halves it made. Raises ValueError past one split per
    triangle the mesh started with: splits that keep making room for more are undoing each other."""
    splits, most = 0, len(mesh.tris)
    tree = cKDTree(mesh.points)
    rows = range(len(mesh.tris))
    while True:
        found = _edge_points(mesh, tree, distance, rows)
        if not found:
            return mesh, splits
        mesh = _split(mesh, found)
        splits += len(found)
        if splits > most:
            raise ValueError(f"T-junction splits passed {most}, one per starting triangle: they undo each other")
        rows = range(len(mesh.tris) - 2 * len(found), len(mesh.tris))


def _edge_points(mesh, tree, distance, rows):
    """Per triangle of `rows`, the first (side, point, parameter) of a point on the inside of one of its edges."""
    corners = mesh.vertex_point[mesh.tris]
    p = mesh.points
    found = {}
    for row in rows:
        triangle = corners[row]
        for side in range(3):
            a, b = int(triangle[side]), int(triangle[(side + 1) % 3])
            edge = p[b] - p[a]
            length = float(np.linalg.norm(edge))
            if length <= 2 * distance:
                continue
            near = [int(q) for q in tree.query_ball_point((p[a] + p[b]) / 2, length / 2 + distance)
                    if q not in triangle]
            if not near:
                continue
            s = (p[near] - p[a]) @ edge / (length * length)
            off = np.linalg.norm(p[a] + s[:, None] * edge - p[near], axis=1)
            # Neither half may come out flat: a point on the line to the third corner too would make a sliver
            # whose own edge holds the next point, and the two splits would undo each other.
            c = p[int(triangle[(side + 2) % 3])]
            inside = ((off < distance) & (s * length > distance) & ((1 - s) * length > distance)
                      & (_line_distance(p[near], p[a], c) > distance) & (_line_distance(p[near], p[b], c) > distance))
            if np.any(inside):
                first = np.flatnonzero(inside)[np.argmin(s[inside])]
                found[row] = (side, near[first], float(s[first]))
                break
    return found


def _line_distance(points, a, b):
    """Each point's distance from the line through `a` and `b`, or from `a` when the two coincide."""
    offset = points - a
    length = np.linalg.norm(b - a)
    if length == 0:
        return np.linalg.norm(offset, axis=1)
    direction = (b - a) / length
    return np.linalg.norm(offset - (offset @ direction)[:, None] * direction, axis=1)


def _split(mesh, found):
    """`mesh` with each triangle row of `found` split at its (side, point, parameter) into two."""
    tris, rgb, vertex_point = [mesh.tris], [mesh.rgb], [mesh.vertex_point]
    keep = np.ones(len(mesh.tris), dtype=bool)
    halves, half_rgb, half_double = [], [], []
    added = len(mesh.vertex_point)
    for row, (side, point, s) in found.items():
        a, b, c = (mesh.tris[row][(side + k) % 3] for k in range(3))
        if mesh.flat:
            vertex = _point_vertex(mesh, point)
        else:
            vertex = added
            added += 1
            vertex_point.append(np.array([point]))
            rgb.append(((1 - s) * mesh.rgb[a] + s * mesh.rgb[b])[None])
        keep[row] = False
        halves += [(a, vertex, c), (vertex, b, c)]
        if mesh.flat:
            half_rgb += [mesh.rgb[row], mesh.rgb[row]]
        half_double += [mesh.double[row]] * 2
    tris = np.concatenate([mesh.tris[keep], np.array(halves, dtype=mesh.tris.dtype)])
    double = np.concatenate([np.asarray(mesh.double)[keep], np.array(half_double, dtype=np.asarray(mesh.double).dtype)])
    if mesh.flat:
        return mesh._replace(tris=tris, double=double, rgb=np.concatenate([mesh.rgb[keep], np.array(half_rgb)]))
    return mesh._replace(tris=tris, double=double, rgb=np.concatenate(rgb), vertex_point=np.concatenate(vertex_point))


def _point_vertex(mesh, point):
    """A vertex of a flat mesh at `point`: flat vertices are one per position."""
    return int(np.flatnonzero(mesh.vertex_point == point)[0])


def log_lines(moved, splits, before):
    """The fit log's account of the two steps."""
    log(f"snap back: {moved} points moved back to their start")
    log(f"T-junctions: {splits} edges split, {before} -> {before + splits} triangles")

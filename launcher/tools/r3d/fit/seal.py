"""Seal a pruned fit by splitting T-junctions, snapping back points around new sky, then splitting again."""

import numpy as np
from scipy.spatial import cKDTree

from r3d import log
from r3d.leaks import LeakProbes

# A fitted point within this distance (model units) of the inside of an edge it does not end splits that edge.
# A held seam point lands on its target's edge: one fit had 363 points this close where its start had 9.
T_JUNCTION_DISTANCE = 0.05
# Maximum restore rounds, each a pixel wider around the holes.
SNAP_BACK_ROUNDS = 8
# Sky connected to the start's sky can move with the outline.
SNAP_BACK_REACH = None


def geometry(mesh, copies):
    """(positions, triangles) of a FitMesh and the copies drawn with it, the mesh's triangles first."""
    positions, tris = mesh.points, mesh.vertex_point[mesh.tris]
    if copies is None:
        return positions, tris
    return (np.concatenate([positions, copies.points]),
            np.concatenate([tris, copies.vertex_point[copies.tris] + len(positions)]))


def start_probes(start, copies, camera):
    """Leak probes over the start and its copies from `camera`, (region, near) as
    returned by `import_settings.seal_camera`. Sky they show counts as the start's own."""
    return LeakProbes(*geometry(start, copies), *camera)


def snap_back(start, fitted, copies, probes):
    """Restore points on fitted rims and start triangles seen through unshared sky regions.
    Return (mesh, points moved, holes left, restore rounds run)."""
    points = fitted.points.copy()
    fitted_count, start_count = len(fitted.tris), len(start.tris)
    rounds = left = 0
    for width in range(1, SNAP_BACK_ROUNDS + 1):
        positions, tris = geometry(fitted._replace(points=points), copies)
        rims = probes.rims(positions, tris, width, reach=SNAP_BACK_REACH)
        if not rims:
            break
        holes = probes.holes(positions, tris, reach=SNAP_BACK_REACH)
        rim_rows = np.unique(np.concatenate(rims))
        seen_rows = np.unique(np.concatenate(holes))
        rim_points = tris[rim_rows[rim_rows < fitted_count]]
        seen_points = start.vertex_point[start.tris[seen_rows[seen_rows < start_count]]]
        restore = np.union1d(rim_points, seen_points)
        points[restore] = start.points[restore]
        rounds = width
    else:
        left = len(probes.holes(*geometry(fitted._replace(points=points), copies), reach=SNAP_BACK_REACH))
    moved = int(np.count_nonzero(np.any(points != fitted.points, axis=1)))
    return fitted._replace(points=points), moved, left, rounds


def split_t_junctions(mesh, distance=T_JUNCTION_DISTANCE):
    """`mesh` with each edge a point lies within `distance` of split through that point, both triangles on a
    shared edge alike: (the mesh, the number of triangles split, each adding one). The point array keeps
    its indices and positions. A smooth mesh's split vertex takes the edge colour. Each half of a flat
    mesh's triangle keeps the triangle's colour. A pass splits a triangle once, and the next looks only
    at the halves it made. Raises ValueError if the running split count exceeds the input triangle count."""
    splits, most = 0, len(mesh.tris)
    referenced = np.unique(mesh.vertex_point[mesh.tris])
    tree = cKDTree(mesh.points[referenced])
    rows = range(len(mesh.tris))
    while True:
        found = _edge_points(mesh, tree, referenced, distance, rows)
        if not found:
            return mesh, splits
        splits += len(found)
        if splits > most:
            raise ValueError(f"T-junction splits reached {splits} for {most} starting triangles, "
                             "exceeding one per triangle")
        mesh = _split(mesh, found)
        rows = range(len(mesh.tris) - 2 * len(found), len(mesh.tris))


def _edge_points(mesh, tree, referenced, distance, rows):
    """Schedule one split per face, with every incident face of an edge split through the same point."""
    corners = mesh.vertex_point[mesh.tris]
    p = mesh.points
    edges = {}
    candidates = {}
    for row, triangle in enumerate(corners):
        for side, a in enumerate(triangle):
            b = triangle[(side + 1) % 3]
            key = (min(a, b), max(a, b))
            edges.setdefault(key, []).append((row, side, a > b))
            if row in rows:
                candidates[key] = None
    found = {}
    for a, b in candidates:
        faces = edges[a, b]
        if any(row in found for row, _, _ in faces):
            continue
        edge = p[b] - p[a]
        length = float(np.linalg.norm(edge))
        if length <= 2 * distance:
            continue
        near = referenced[tree.query_ball_point((p[a] + p[b]) / 2, length / 2 + distance, return_sorted=True)]
        near = near[(near != a) & (near != b)]
        if not len(near):
            continue
        s = (p[near] - p[a]) @ edge / (length * length)
        off = np.linalg.norm(p[a] + s[:, None] * edge - p[near], axis=1)
        inside = (off < distance) & (s * length > distance) & ((1 - s) * length > distance)
        if not np.any(inside):
            continue
        # A sliver can leave an edge that invites a split undoing this one.
        for row, side, _ in faces:
            third = corners[row, (side + 2) % 3]
            inside &= ((near != third) & (_line_distance(p[near], p[a], p[third]) > distance)
                       & (_line_distance(p[near], p[b], p[third]) > distance))
        if np.any(inside):
            first = np.flatnonzero(inside)[np.argmin(s[inside])]
            for row, side, reverse in faces:
                found[row] = (side, int(near[first]), float(1 - s[first] if reverse else s[first]))
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
    rgb, vertex_point = [mesh.rgb], [mesh.vertex_point]
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


def log_lines(moved, left, rounds, splits, before):
    """Log snap-back results and the combined triangle increase from both splits."""
    log(f"snap back: {moved} points moved back to their start")
    log(f"snap back: {left} holes left after {rounds} rounds")
    log(f"T-junctions: {splits} edges split, {before} -> {before + splits} triangles")

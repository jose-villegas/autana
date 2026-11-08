"""A lit mesh's levels of detail as lit_mesh.py bakes them: every level's
clusters with their colours, and the group records the device's pick reads
(render/r3d_lit_mesh.h, docs/plans/Cluster-LOD.md).

Each sidedness gets its own hierarchy (levels.py), welded and position-only,
with the positions both share locked so the two never part. clusterlod's
finest clusters are level 0 and keep the bake's own triangles and colours. A
coarser cluster's vertices are a subset of the finer ones; its group's inner
vertices are pushed outward until no finer vertex lies outside it, at most by
the group's error, so a coarser level covers what the finer one covered, short
of rounding to ticks and of a point next to the locked border. Its
border stays where the finer level put it, and neighbours never crack. A
coarse vertex takes its colour from the nearest level-0 point facing its way,
one colour per cluster, and a flat triangle from the nearest to its middle.

A group's error is at least the farthest its coarse clusters lie from the
level-0 triangles under them, either way. clusterlod's own error measures distance off the input's
planes, so an outline that shrinks within its own plane (a thin slab's rim
folding in) costs it nothing.

Group bounds and errors are rounded outward and up, then grown so each holds
every group simplified into it with LOD_ENCLOSE_MARGIN to spare and carries
at least its error. A group that would not fit the format's int16 bounds or
uint16 error is dropped with every group above it: its finer clusters stay
drawn at every distance. Groups join finest first, and the first whose
clusters would take the mesh past the format's vertex, triangle or cluster
count is dropped with every coarser one.
"""

from collections import defaultdict
from typing import NamedTuple

import numpy as np
from scipy.spatial import cKDTree

from r3d.geometry import closest_point_on_triangles
from r3d.lit_mesh import MAX_CLUSTERS, MAX_TRIANGLES, MAX_VERTICES, oriented
from r3d.lod.levels import build_levels
from r3d.mesh_asset import LOD_ENCLOSE_MARGIN, LOD_NONE

INT16_MAX = 32767
ERROR_MAX = LOD_NONE - 1  # an error fits uint16
# Level-0 triangles tried for each sample, nearest middle first.
SAMPLE_CANDIDATES = 16
# Fine vertices measured against the coarse triangles at once.
PUSH_CHUNK = 256
# The push's rounds, and the ticks of push left that end them early.
PUSH_ROUNDS = 4
PUSH_DONE = 0.25
# A movable weight below this cannot lift its point: the sample sits on the border.
PUSH_WEIGHT_MIN = 1e-3


class Baked(NamedTuple):
    """The mesh's vertex rows with the coarser levels' appended, its clusters
    as lit_mesh.py lays them out, and the group rows (centre x y z, radius,
    error; ticks)."""
    q: np.ndarray
    col: object
    entries: list
    groups: np.ndarray


def border_vertices(ids):
    """The welded ids on an edge that bounds the triangles `ids` (T x 3): used
    by one triangle, or by more than two."""
    edges = np.sort(np.stack([ids[:, [0, 1]], ids[:, [1, 2]], ids[:, [2, 0]]], axis=1).reshape(-1, 2), axis=1)
    unique, count = np.unique(edges, axis=0, return_counts=True)
    return np.unique(unique[count != 2])


def push_outward(fine_ids, fine_pos, coarse_ids, cap):
    """Integer corner positions (Nc x 3 x 3) of the coarse triangles
    `coarse_ids` (welded ids) simplified from the fine ones (ids and drawn
    positions, Nf x 3 and Nf x 3 x 3). Every vertex starts where the fine
    level draws it; those off the fine patch's border move along their
    area-weighted normal until no fine vertex lies outside the coarse triangle
    nearest it, or until each has moved `cap`. A fine vertex outside by d over
    a triangle whose movable corners weigh w in it asks each of them for d / w;
    PUSH_ROUNDS rounds follow the triangles as they turn."""
    ids, pos = fine_ids.reshape(-1), fine_pos.reshape(-1, 3).astype(np.float64)
    vertices, first = np.unique(ids, return_index=True)
    fine = pos[first]
    corner = np.searchsorted(vertices, coarse_ids)
    assert np.all(vertices[corner] == coarse_ids), "a coarse vertex is not a fine one"
    movable = ~np.isin(vertices, border_vertices(fine_ids))
    at, moved = fine.copy(), np.zeros(len(vertices))
    for _ in range(PUSH_ROUNDS):
        need = np.minimum(outside_need(fine, at, corner, movable), cap - moved)
        if need.max(initial=0.0) <= PUSH_DONE:
            break
        a, b, c = at[corner[:, 0]], at[corner[:, 1]], at[corner[:, 2]]
        direction = np.zeros_like(at)
        for k in range(3):
            np.add.at(direction, corner[:, k], np.cross(b - a, c - a))
        direction /= np.maximum(np.linalg.norm(direction, axis=1, keepdims=True), 1e-12)
        at += need[:, None] * direction
        moved += need
    return np.rint(at).astype(np.int64)[corner]


def outside_need(fine, at, corner, movable):
    """How far each coarse vertex must move for the fine vertices `fine` to
    lie on or inside the coarse triangles `corner` drawn at `at`: per fine
    vertex outside its nearest triangle by d, d over the weight that
    triangle's movable corners have at its nearest point, the most asked."""
    a, b, c = at[corner[:, 0]], at[corner[:, 1]], at[corner[:, 2]]
    normal = np.cross(b - a, c - a)
    unit = normal / np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
    need = np.zeros(len(at))
    tris = len(corner)
    for start in range(0, len(fine), PUSH_CHUNK):
        q = fine[start:start + PUSH_CHUNK]
        rows = np.repeat(q, tris, axis=0)
        ta, tb, tc = (np.tile(v, (len(q), 1)) for v in (a, b, c))
        bary, distance = closest_point_on_triangles(rows, ta, tb, tc)
        nearest = np.argmin(distance.reshape(len(q), tris), axis=1)
        picked = np.arange(len(q)) * tris + nearest
        closest = bary[picked, :1] * ta[picked] + bary[picked, 1:2] * tb[picked] + bary[picked, 2:] * tc[picked]
        outside = np.maximum(((rows[picked] - closest) * unit[nearest]).sum(axis=1), 0.0)
        weight = (bary[picked] * movable[corner[nearest]]).sum(axis=1)
        share = np.where(weight > PUSH_WEIGHT_MIN, outside / np.maximum(weight, PUSH_WEIGHT_MIN), 0.0)
        for k in range(3):
            np.maximum.at(need, corner[nearest, k], np.where(movable[corner[nearest, k]], share, 0.0))
    return need


class Triangles:
    """Triangles (T x 3 x 3) with a tree of their middles, for the farthest of many points from them."""

    def __init__(self, corners):
        self.corners = np.asarray(corners, dtype=np.float64)
        self.tree = cKDTree(self.corners.mean(axis=1))

    def farthest(self, points):
        """The farthest of `points` (N x 3) from its nearest triangle. Each point tries the SAMPLE_CANDIDATES
        triangles whose middles lie nearest: missing the nearest can only overstate the distance."""
        k = min(SAMPLE_CANDIDATES, len(self.corners))
        near = np.asarray(self.tree.query(points, k=k)[1]).reshape(len(points), k)
        a, b, c = self.corners[:, 0], self.corners[:, 1], self.corners[:, 2]
        farthest = 0.0
        for start in range(0, len(points), PUSH_CHUNK):
            rows = near[start:start + PUSH_CHUNK].reshape(-1)
            q = np.repeat(points[start:start + PUSH_CHUNK], k, axis=0)
            _, distance = closest_point_on_triangles(q, a[rows], b[rows], c[rows])
            farthest = max(farthest, float(distance.reshape(-1, k).min(axis=1).max()))
        return farthest


def surface_gap(level0, coarse, everything):
    """How far coarse triangles (N x 3 x 3) lie from the level-0 triangles under them, either way: every level-0
    vertex to the coarse triangles, and every coarse vertex to `everything`, the side's level-0 Triangles (a
    nearer level-0 triangle elsewhere can only understate a bulge, which the push already bounds)."""
    level0 = np.unique(np.asarray(level0, dtype=np.float64).reshape(-1, 3), axis=0)
    coarse = np.asarray(coarse, dtype=np.float64)
    return max(Triangles(coarse).farthest(level0), everything.farthest(np.unique(coarse.reshape(-1, 3), axis=0)))


class Level0:
    """Colours read off the level-0 triangles: the nearest point to a
    sample, among triangles facing the same way when any of the nearest do."""

    def __init__(self, q, col, tris, face):
        self.col, self.tris, self.face = col, tris, face
        self.a, self.b, self.c = (q[tris[:, k]].astype(np.float64) for k in range(3))
        normal = np.cross(self.b - self.a, self.c - self.a)
        self.normal = normal / np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
        self.tree = cKDTree((self.a + self.b + self.c) / 3.0)

    def nearest(self, points, normals):
        """(triangle, barycentrics) of the nearest level-0 point to each sample."""
        k = min(SAMPLE_CANDIDATES, len(self.tris))
        _, candidates = self.tree.query(points, k=k)
        candidates = candidates.reshape(len(points), k)
        flat = candidates.reshape(-1)
        rows = np.repeat(points, k, axis=0)
        bary, distance = closest_point_on_triangles(rows, self.a[flat], self.b[flat], self.c[flat])
        distance = distance.reshape(len(points), k)
        facing = ((self.normal[flat] * np.repeat(normals, k, axis=0)).sum(axis=1) >= 0).reshape(len(points), k)
        best = np.argmin(np.where(facing | ~facing.any(axis=1, keepdims=True), distance, np.inf), axis=1)
        picked = np.arange(len(points)) * k + best
        return flat[picked], bary[picked]

    def vertex_colours(self, points, normals):
        triangle, bary = self.nearest(points, normals)
        corners = self.col[self.tris[triangle]].astype(np.float64)
        return np.clip(np.rint((bary[:, :, None] * corners).sum(axis=1)), 0, 255).astype(np.int64)

    def face_colours(self, points, normals):
        return self.face[self.nearest(points, normals)[0]]


def quantised_groups(sphere, error, sources):
    """(rows, kept): each group's (centre, radius, error) in ticks, rounded
    outward and up and grown to hold every group in `sources[g]` (all of a
    lower id) with LOD_ENCLOSE_MARGIN to spare, and whether it fits the
    format; a group over a dropped one is dropped."""
    rows = np.zeros((len(sphere), 5), dtype=np.int64)
    kept = np.ones(len(sphere), dtype=bool)
    for g in range(len(sphere)):
        centre = np.rint(sphere[g, :3]).astype(np.int64)
        shift = np.linalg.norm(sphere[g, :3] - centre)
        radius = int(np.ceil(sphere[g, 3] + shift))
        err = int(np.ceil(error[g]))
        for s in sources[g]:
            kept[g] &= kept[s]
            gap = int(np.ceil(np.sqrt(float(((centre - rows[s, :3]) ** 2).sum()))))
            radius = max(radius, gap + int(rows[s, 3]) + LOD_ENCLOSE_MARGIN)
            err = max(err, int(rows[s, 4]))
        kept[g] &= bool(np.all(np.abs(centre) <= INT16_MAX)) and radius <= INT16_MAX and err <= ERROR_MAX
        rows[g] = (*centre, radius, err)
    return rows, kept


def within_format(rows, kept, cost, used):
    """kept, less the groups from the first, finest first (by error, then id,
    so a group comes after its sources), whose `cost` (vertices, triangles,
    clusters) would take the mesh's `used` past the format's counts."""
    order = np.lexsort((np.arange(len(rows)), rows[:, 4]))
    total = used + np.cumsum(cost[order] * kept[order, None], axis=0)
    fits = np.all(total <= (MAX_VERTICES, MAX_TRIANGLES, MAX_CLUSTERS), axis=1)
    kept = kept.copy()
    kept[order[np.argmin(fits):] if not fits.all() else []] = False
    return kept


def bake_levels(q, col, tris, double, face, meshlet_triangles, cluster):
    """The levels of a welded bake (q ticks, col and face colours or None,
    tris rows into q, double per triangle), clusters of at most
    meshlet_triangles; triangles whose corners share a position draw nothing
    and are left out. `cluster`, per row and ascending, is the plain bake's
    cluster each triangle sits in: those clusters, in that order and their
    rows in order, are the finest level."""
    welded, wid = np.unique(q, axis=0, return_inverse=True)
    wid = wid.reshape(-1)
    wt = wid[tris]
    double = np.asarray(double)
    keep = (wt[:, 0] != wt[:, 1]) & (wt[:, 1] != wt[:, 2]) & (wt[:, 0] != wt[:, 2])
    used = np.zeros((2, len(welded)), dtype=bool)
    for side in (0, 1):
        used[side, wt[keep & (double == side)].reshape(-1)] = True
    shared = used[0] & used[1]
    level0 = Level0(q, col, tris[keep], None if face is None else np.asarray(face)[keep])
    entries, coarse, spheres, errors, sources = [], [], [], [], []
    for side in (0, 1):
        member = np.flatnonzero(keep & (double == side))
        if len(member):
            levels = build_levels(welded, wt[member], meshlet_triangles, shared,
                                  np.unique(cluster[member], return_counts=True)[1])
            side_entries, side_coarse, side_groups = side_levels(levels, welded, wt, tris, face, member, side,
                                                                 len(spheres))
            entries += side_entries
            coarse += side_coarse
            spheres += side_groups[0]
            errors += side_groups[1]
            sources += side_groups[2]
    # The plain bake's clusters in its order, both sidednesses together.
    entries = [{k: v for k, v in e.items() if k != "rows"} for e in sorted(entries, key=lambda e: int(e["rows"][0]))]
    rows, kept = quantised_groups(np.array(spheres).reshape(-1, 4), np.array(errors), sources)
    cost = np.zeros((len(rows), 3), dtype=np.int64)
    for e in coarse:
        cost[e["own"]] += (len(np.unique(e["corners"].reshape(-1, 3), axis=0)), len(e["corners"]), 1)
    used = np.sum([(len(np.unique(e["tris"])), len(e["tris"]), 1) for e in entries], axis=0)
    kept = within_format(rows, kept, cost, used)
    return finish(q, col, level0, entries, coarse, rows, kept)


def side_levels(levels, welded, wt, tris, face, member, side, group_base):
    """One sidedness's level-0 entries, its coarse clusters (welded corner ids
    and drawn corner positions), and its groups that have simplified
    clusters: their spheres, errors and source groups, numbered from
    group_base."""
    first, count = levels.cluster_first, levels.cluster_triangles
    corner_ids = [levels.indices[first[c]:first[c] + 3 * count[c]].reshape(-1, 3) for c in range(len(first))]
    by_shape = defaultdict(list)
    for t in member:
        by_shape[oriented(wt[t])].append(t)
    for shapes in by_shape.values():
        shapes.reverse()
    produced = np.zeros(int(levels.cluster_group.max()) + 1, dtype=bool)
    produced[levels.cluster_refined[levels.cluster_refined >= 0]] = True
    number = np.full(len(produced), -1, dtype=np.int64)
    number[produced] = group_base + np.arange(int(produced.sum()))

    def group_id(g):
        return LOD_NONE if g < 0 or number[g] < 0 else int(number[g])

    drawn = [None] * len(first)
    under = [None] * len(first)  # the level-0 clusters each cluster stands for
    entries, coarse = [], []
    for c in np.flatnonzero(levels.cluster_refined < 0):
        # The given cluster's rows in their own order, whatever order clusterlod hands them back in.
        rows = np.sort([by_shape[oriented(t)].pop() for t in corner_ids[c]])
        drawn[c] = welded[corner_ids[c]]
        under[c] = frozenset((int(c),))
        entries.append({"double": side, "rows": rows, "tris": tris[rows],
                        "face": None if face is None else np.asarray(face)[rows],
                        "own": LOD_NONE, "parent": group_id(levels.cluster_group[c])})
    everything = Triangles(np.concatenate([drawn[c] for c in range(len(first)) if under[c] is not None]))
    spheres, errors, sources = [], [], []
    for g in np.flatnonzero(produced):
        inputs = np.flatnonzero(levels.cluster_group == g)
        outputs = np.flatnonzero(levels.cluster_refined == g)
        error = float(levels.own_error[outputs[0]])
        moved = push_outward(np.concatenate([corner_ids[c] for c in inputs]),
                             np.concatenate([drawn[c] for c in inputs]),
                             np.concatenate([corner_ids[c] for c in outputs]), error)
        level0 = frozenset().union(*(under[c] for c in inputs))
        error = max(error, surface_gap(welded[np.concatenate([corner_ids[c] for c in sorted(level0)])], moved,
                                       everything))
        at = 0
        for c in outputs:
            drawn[c] = moved[at:at + len(corner_ids[c])]
            under[c] = level0
            at += len(corner_ids[c])
            coarse.append({"double": side, "corners": drawn[c], "own": group_id(g),
                           "parent": group_id(levels.cluster_group[c])})
        # The push moves a vertex by at most clusterlod's own error, outward of the sphere it measured.
        spheres.append(levels.own_sphere[outputs[0]] + (0, 0, 0, levels.own_error[outputs[0]]))
        errors.append(error)
        sources.append(sorted({group_id(levels.cluster_refined[c]) for c in inputs} - {LOD_NONE}))
    return entries, coarse, (spheres, errors, sources)


def finish(q, col, level0, entries, coarse, rows, kept):
    """The coarse clusters sampled and appended as vertex rows, the dropped
    groups' clusters left out and their numbers closed up."""
    number = np.full(len(rows), LOD_NONE, dtype=np.int64)
    number[kept] = np.arange(int(kept.sum()))

    def renumbered(e):
        return dict(e, own=LOD_NONE if e["own"] == LOD_NONE else int(number[e["own"]]),
                    parent=LOD_NONE if e["parent"] == LOD_NONE else int(number[e["parent"]]))

    coarse = [e for e in coarse if kept[e["own"]]]
    flat, coloured = level0.face is not None, col is not None
    # Each coarse cluster's vertices: its corners' distinct positions, with the
    # area-weighted normal of its triangles around each.
    vertices, normals, corner_of = [], [], []
    for e in coarse:
        unique, inverse = np.unique(e["corners"].reshape(-1, 3), axis=0, return_inverse=True)
        corners = e["corners"].astype(np.float64)
        area = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
        around = np.zeros((len(unique), 3))
        np.add.at(around, inverse.reshape(-1), np.repeat(area, 3, axis=0))
        vertices.append(unique)
        normals.append(around / np.maximum(np.linalg.norm(around, axis=1, keepdims=True), 1e-12))
        corner_of.append(inverse.reshape(-1, 3))
    sampled = face = None
    if coarse and coloured:
        sampled = level0.vertex_colours(np.concatenate(vertices).astype(np.float64), np.concatenate(normals))
    if coarse and flat:
        corners = np.concatenate([e["corners"] for e in coarse]).astype(np.float64)
        area = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
        face = level0.face_colours(corners.mean(axis=1), area / np.maximum(np.linalg.norm(area, axis=1, keepdims=True),
                                                                          1e-12))
    rows_q, rows_col, base, at_vertex, at_face = [q], [col], len(q), 0, 0
    out = [renumbered(e) for e in entries]
    for e, unique, local in zip(coarse, vertices, corner_of):
        rows_q.append(unique)
        if coloured:
            rows_col.append(sampled[at_vertex:at_vertex + len(unique)])
        out.append(renumbered({"double": e["double"], "tris": base + local,
                               "face": face[at_face:at_face + len(local)] if flat else None, "own": e["own"],
                               "parent": e["parent"]}))
        base += len(unique)
        at_vertex += len(unique)
        at_face += len(local)
    return Baked(np.concatenate(rows_q), np.concatenate(rows_col) if coloured else None, out, rows[kept])


def bake_lit_mesh(positions, rgb, tris, double, position_scale=None, leaf_triangles=None,
                  max_depth=None, meshlet_triangles=None, face_rgb=None, lod=True):
    """Assemble a plain mesh or its CPU-built hierarchy from the same welded triangles."""
    from r3d import lit_mesh
    position_scale = lit_mesh.POSITION_SCALE if position_scale is None else position_scale
    leaf_triangles = lit_mesh.LEAF_TRIANGLES if leaf_triangles is None else leaf_triangles
    max_depth = lit_mesh.MAX_DEPTH if max_depth is None else max_depth
    meshlet_triangles = lit_mesh.MESHLET_TRIANGLES if meshlet_triangles is None else meshlet_triangles
    plain = lit_mesh.bake_lit_mesh(positions, rgb, tris, double, position_scale, leaf_triangles,
                                  max_depth, meshlet_triangles, face_rgb)
    if not lod:
        return plain
    q, col, tris, double, face = lit_mesh.finest_triangles(plain)
    counts = [c[3] for c in plain.clusters]
    assert len(tris) == sum(counts), "re-welding the plain bake dropped a triangle"
    levels = bake_levels(q, col, tris, double, face, meshlet_triangles, np.repeat(np.arange(len(counts)), counts))
    if not len(levels.groups):
        return plain
    return lit_mesh.assemble(levels.q, levels.col, levels.entries, position_scale, leaf_triangles,
                             max_depth, levels.groups)

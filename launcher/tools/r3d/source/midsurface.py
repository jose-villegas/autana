"""Midsurface abstraction for cloth built as two skins: one sheet halfway between them, drawn from both sides.

Each face of the near skin casts one ray along its inward normal onto its own shell's far skin, as midsurface
abstraction pairs the opposite faces of a thin wall (Rezayat 1996) and the shape diameter function measures a
part's thickness with inward rays (Shapira et al. 2008). A face whose ray finds the far skin facing back within
reach stays and its vertices move halfway across; a face whose ray misses or grazes is the rim, where the skins
meet, and goes. A shell whose collapse would open a hole inside the sheet or turn a triangle over stays as its two
skins. The far skin leaves the drawing but stays the source of what the sheet shows from behind (back_albedo())."""
from typing import NamedTuple

import numpy as np

from r3d.geometry import (
    WELD_TOLERANCE, edge_uses, face_normals, nearest_triangles, triangle_search, vertex_normals, weld_map,
)

# A pair's skins face each other: the dot of the two normals is below this.
FACING = -0.9
# A pair is at most this many times its shell's median thickness apart; a longer ray crossed a fold.
REACH = 1.5
# A ray that meets the far skin within a weld starts on it: a rim vertex both skins share.
CONTACT = WELD_TOLERANCE


class Midsurface(NamedTuple):
    """points: the source points with the moved vertices after them; source: the source point each of `points`
    comes from, for per-point attributes. tri_v: the triangles, each sheet one indexing the moved vertices.
    drawn: per triangle, False for the far skin and the rim. far: per triangle, the far skin. sheet: per
    triangle, the collapsed sheet. thickness: per triangle, its shell's median thickness (0 off the shells)."""
    points: np.ndarray
    source: np.ndarray
    tri_v: np.ndarray
    drawn: np.ndarray
    far: np.ndarray
    sheet: np.ndarray
    thickness: np.ndarray


class FarSkin(NamedTuple):
    """What collapsed sheets show from behind, for back_albedo(): `query` a ray intersector over the far
    triangles, `corners` (F, 3, 3) and `piece` (F,) their positions and shells; `sheet_corners` (S, 3, 3),
    `sheet_piece` and `sheet_thickness` (S,) the sheet triangles', their shells and those shells' median
    thickness."""
    query: object
    corners: np.ndarray
    piece: np.ndarray
    sheet_corners: np.ndarray
    sheet_piece: np.ndarray
    sheet_thickness: np.ndarray


def inward_hits(far_query, far_normals, far_piece, origins, normals, piece):
    """(paired, distance) of rays from `origins` along -`normals` onto the far skin: a hit beyond CONTACT on a far
    triangle of the origin's own shell (`piece`) that faces back (FACING)."""
    if far_query is None:
        return np.zeros(len(origins), dtype=bool), np.zeros(len(origins))
    hit, distance, tri = far_query.first_hits(origins, -normals)
    at = np.where(hit, tri, 0)
    facing = np.einsum("ij,ij->i", normals, far_normals[at]) < FACING
    return hit & (distance > CONTACT) & facing & (far_piece[at] == piece), distance


def within_reach(paired, distance, piece):
    """`paired` less the pairs farther than REACH times their shell's median thickness, and each shell's median."""
    median = {}
    for shell in np.unique(piece[paired]):
        mine = paired & (piece == shell)
        median[shell] = float(np.median(distance[mine]))
        paired = paired & (~mine | (distance <= REACH * median[shell]))
    return paired, median


def torn_shells(old, new, tris, welded, piece, sheet, dropped, far):
    """The shells whose collapse tears them: a `sheet` triangle turned over (`old` and `new` corner positions of
    `tris`), or near faces `dropped` that touch no far triangle `far` through other dropped faces, so they open a
    hole inside the sheet where the rim would join the far skin."""
    normal_old = np.cross(old[:, 1] - old[:, 0], old[:, 2] - old[:, 0])
    normal_new = np.cross(new[:, 1] - new[:, 0], new[:, 2] - new[:, 0])
    turned = sheet & (np.einsum("ij,ij->i", normal_old, normal_new) < 0)
    torn = set(piece[turned].tolist())
    loose = np.flatnonzero(dropped | far)
    if np.any(dropped):
        from scipy.sparse import coo_matrix
        from scipy.sparse.csgraph import connected_components

        _edges, slot, uses = edge_uses(welded[tris[loose]])
        owner = np.tile(np.arange(len(loose)), 3)
        graph = coo_matrix((np.ones(len(slot)), (owner, len(loose) + slot)), shape=(len(loose) + len(uses),) * 2)
        _count, label = connected_components(graph, directed=False)
        label = label[:len(loose)]
        reaches_far = np.zeros(label.max() + 1, dtype=bool)
        reaches_far[label[far[loose]]] = True
        torn |= set(piece[loose[dropped[loose] & ~reaches_far[label]]].tolist())
    return np.array(sorted(torn), dtype=np.int64)


def collapse(p, tri_v, far, piece, far_query):
    """The Midsurface of the shells `piece` marks (closed_shells(), -1 elsewhere) whose far skins `far` marks.
    far_query(p, tris) is a ray intersector over those triangles (ray_query.RayQuery).

    A near face pairs when the ray from its centre along its inward normal meets its shell's far skin facing back
    within reach; the faces that do not, the hem or rim joining the skins, go. Each vertex of a paired face moves
    halfway along its own inward ray where that pairs too, else by half its shell's median thickness, as a vertex
    on the sheet's border, whose ray grazes the far skin's edge, does. A shell torn by this (torn_shells()) keeps
    both skins as they were."""
    near_rows = np.flatnonzero((piece >= 0) & ~far)
    far_rows = np.flatnonzero(far)
    query = far_query(p, tri_v[far_rows]) if len(far_rows) else None
    far_normals = face_normals(p, tri_v[far_rows])[0] if len(far_rows) else np.zeros((1, 3))
    far_piece = piece[far_rows] if len(far_rows) else np.full(1, -1)
    face_n, _area = face_normals(p, tri_v[near_rows])
    face_paired, face_distance = inward_hits(query, far_normals, far_piece, p[tri_v[near_rows]].mean(axis=1), face_n,
                                             piece[near_rows])
    face_paired, median = within_reach(face_paired, face_distance, piece[near_rows])
    sheet_rows = near_rows[face_paired]
    first, welded = weld_map(p)
    normals = vertex_normals(p, tri_v[sheet_rows], welded, len(first))
    cast = np.unique(welded[tri_v[sheet_rows]])
    vertex_piece = np.full(len(first), -1)
    vertex_piece[welded[tri_v[sheet_rows]].reshape(-1)] = np.repeat(piece[sheet_rows], 3)
    origin = p[first[cast]]
    paired, distance = inward_hits(query, far_normals, far_piece, origin, normals[cast], vertex_piece[cast])
    paired, _median = within_reach(paired, distance, vertex_piece[cast])
    half = np.where(paired, distance, [median.get(shell, 0.0) for shell in vertex_piece[cast]]) / 2
    moved = np.full(len(first), -1)
    moved[cast] = len(p) + np.arange(len(cast))
    points = np.concatenate([p, origin - normals[cast] * half[:, None]])
    moved_tris = moved[welded[tri_v]]
    # A far-facing triangle whose corners all moved lies on the near skin: it moves with them.
    sheet = np.zeros(len(tri_v), dtype=bool)
    sheet[sheet_rows] = True
    sheet |= far & np.all(moved_tris >= 0, axis=1)
    out = np.where(sheet[:, None], moved_tris, tri_v)
    whole = np.isin(piece, torn_shells(p[tri_v], points[out], tri_v, welded, piece, sheet,
                                       (piece >= 0) & ~far & ~sheet, far & ~sheet))
    sheet &= ~whole
    out[whole] = tri_v[whole]
    far_left = far & ~sheet & ~whole
    thickness = np.array([median.get(shell, 0.0) for shell in piece.tolist()])
    thickness[whole] = 0.0
    source = np.concatenate([np.arange(len(p)), first[cast]])
    return Midsurface(points, source, out, (piece < 0) | sheet | whole, far_left, sheet, thickness)


def back_albedo(points, normals, front, far, albedo_at_hit):
    """What sheet vertices show from behind (`far` a FarSkin, or None): a point belongs to the shell whose sheet
    lies within half that shell's thickness of it, and reads that shell's far skin alone, the albedo where a ray
    along -normal meets it within REACH times the thickness (albedo_at_hit(points on the far skin, its triangle
    rows, which of `points` found one)); where the ray misses, as at the sheet's border, whose ray grazes the far
    skin's edge, or for points with no `normals` (None), its nearest point within that reach; else `front`, as on
    a sheet with one skin."""
    back = np.array(front, dtype=np.float64, copy=True)
    if far is None or far.query is None or not len(points):
        return back
    rows, _bary, gap = nearest_triangles(points, triangle_search(far.sheet_corners))
    thickness = far.sheet_thickness[rows]
    owner = np.where(gap <= thickness / 2, far.sheet_piece[rows], -1)
    reach = REACH * thickness
    hit, distance, tri = np.zeros(len(points), dtype=bool), np.zeros(len(points)), np.zeros(len(points), np.int64)
    if normals is not None:
        hit, distance, tri = far.query.first_hits(points, -normals)
        hit &= (owner >= 0) & (distance <= reach) & (far.piece[np.where(hit, tri, 0)] == owner)
    at = np.array(points, dtype=np.float64, copy=True)
    if hit.any():
        at[hit] -= normals[hit] * distance[hit, None]
    for shell in np.unique(owner[~hit & (owner >= 0)]):
        missed = np.flatnonzero(~hit & (owner == shell))
        mine = np.flatnonzero(far.piece == shell)
        nearest, bary, apart = nearest_triangles(points[missed], triangle_search(far.corners[mine]))
        near = apart <= reach[missed]
        at[missed[near]] = (far.corners[mine[nearest[near]]] * bary[near][..., None]).sum(axis=1)
        tri[missed[near]] = mine[nearest[near]]
        hit[missed[near]] = True
    if hit.any():
        back[hit] = albedo_at_hit(at[hit], tri[hit], hit)
    return back

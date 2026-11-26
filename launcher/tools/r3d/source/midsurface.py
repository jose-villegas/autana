"""Midsurface abstraction for cloth built as two skins: one sheet halfway between them, drawn from both sides.

Each face of the near skin casts one ray along its inward normal onto its own shell's far skin, as midsurface
abstraction pairs the opposite faces of a thin wall (Rezayat 1996) and the shape diameter function measures a
part's thickness with inward rays (Shapira et al. 2008). A face whose ray finds the far skin facing back within
reach stays and its vertices move halfway across; a face whose ray misses or grazes is the rim, where the skins
meet, and goes, while an unpaired patch inside the near skin stays on the sheet. A shell whose collapse would turn
a triangle over stays as its two skins. The far skin leaves the drawing but stays the source of what the sheet shows from behind (back_albedo())."""
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


# Rounds of smoothing the moves around turned triangles before their shell keeps both skins instead.
UNTANGLE_ROUNDS = 16


def enclosed(tris, welded, dropped, far):
    """Which `dropped` near faces touch no `far` triangle through other dropped faces (shared welded edges): not
    the rim, which joins the far skin, but a patch inside the near skin whose rays found no pair."""
    loose = np.flatnonzero(dropped | far)
    inside = np.zeros(len(tris), dtype=bool)
    if not np.any(dropped):
        return inside
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    _edges, slot, uses = edge_uses(welded[tris[loose]])
    owner = np.tile(np.arange(len(loose)), 3)
    graph = coo_matrix((np.ones(len(slot)), (owner, len(loose) + slot)), shape=(len(loose) + len(uses),) * 2)
    _count, label = connected_components(graph, directed=False)
    label = label[:len(loose)]
    reaches_far = np.zeros(label.max() + 1, dtype=bool)
    reaches_far[label[far[loose]]] = True
    inside[loose[dropped[loose] & ~reaches_far[label]]] = True
    return inside


def turned(old, new, rows):
    """Which of `rows` (triangle rows, corner positions `old` and `new`) the move turned over."""
    normal_old = np.cross(old[rows, 1] - old[rows, 0], old[rows, 2] - old[rows, 0])
    normal_new = np.cross(new[rows, 1] - new[rows, 0], new[rows, 2] - new[rows, 0])
    return np.einsum("ij,ij->i", normal_old, normal_new) < 0


def untangled(p, tri_v, origin, shift, moved_tris, rows):
    """(points, rows still turned over): `p` with each moved point (row len(p) + k of `moved_tris`, the triangles
    `tri_v` moved) at origin[k] + shift[k]. Where a move turns one of `rows` over, its corners take the mean move
    of the triangles of `rows` around them, as Laplacian smoothing does (Sorkine 2005), for up to UNTANGLE_ROUNDS
    rounds: normals that part faster than the cloth is thick, as across a sliver on a tight curve, turn it over."""
    shift = np.array(shift, dtype=np.float64, copy=True)
    old = p[tri_v]
    corners = moved_tris[rows] - len(p)
    count = np.zeros(len(shift))
    np.add.at(count, corners.reshape(-1), 1)
    for done in range(UNTANGLE_ROUNDS + 1):
        points = np.concatenate([p, origin + shift])
        flipped = rows[turned(old, points[moved_tris], rows)]
        if not len(flipped) or done == UNTANGLE_ROUNDS:
            break
        stuck = np.unique(moved_tris[flipped] - len(p))
        total = np.zeros_like(shift)
        np.add.at(total, corners.reshape(-1), np.repeat(shift[corners].mean(axis=1), 3, axis=0))
        shift[stuck] = total[stuck] / count[stuck, None]
    return points, flipped


def collapse(p, tri_v, far, piece, far_query):
    """The Midsurface of the shells `piece` marks (closed_shells(), -1 elsewhere) whose far skins `far` marks.
    far_query(p, tris) is a ray intersector over those triangles (ray_query.RayQuery).

    A near face pairs when the ray from its centre along its inward normal meets its shell's far skin facing back
    within reach; unpaired faces joined to the far skin through each other are the hem or rim and go, and those
    inside the near skin (enclosed()) stay on the sheet. Each vertex of the sheet moves halfway along its own
    inward ray where that pairs, else by half its shell's median thickness, as a vertex on the sheet's border,
    whose ray grazes the far skin's edge, does. A triangle the move turned over has its corners' moves smoothed
    with their neighbours' (UNTANGLE_ROUNDS); a shell with a triangle still turned over keeps both skins as they
    were."""
    near_rows = np.flatnonzero((piece >= 0) & ~far)
    far_rows = np.flatnonzero(far)
    query = far_query(p, tri_v[far_rows]) if len(far_rows) else None
    far_normals = face_normals(p, tri_v[far_rows])[0] if len(far_rows) else np.zeros((1, 3))
    far_piece = piece[far_rows] if len(far_rows) else np.full(1, -1)
    face_n, _area = face_normals(p, tri_v[near_rows])
    face_paired, face_distance = inward_hits(query, far_normals, far_piece, p[tri_v[near_rows]].mean(axis=1), face_n,
                                             piece[near_rows])
    face_paired, median = within_reach(face_paired, face_distance, piece[near_rows])
    first, welded = weld_map(p)
    sheet = np.zeros(len(tri_v), dtype=bool)
    sheet[near_rows[face_paired]] = True
    dropped = (piece >= 0) & ~far & ~sheet
    sheet |= enclosed(tri_v, welded, dropped, far) & np.isin(piece, list(median))
    sheet_rows = np.flatnonzero(sheet)
    normals = vertex_normals(p, tri_v[sheet_rows], welded, len(first))
    cast = np.unique(welded[tri_v[sheet_rows]])
    vertex_piece = np.full(len(first), -1)
    vertex_piece[welded[tri_v[sheet_rows]].reshape(-1)] = np.repeat(piece[sheet_rows], 3)
    origin = p[first[cast]]
    paired, distance = inward_hits(query, far_normals, far_piece, origin, normals[cast], vertex_piece[cast])
    paired, _median = within_reach(paired, distance, vertex_piece[cast])
    fallback = np.array([median.get(shell, 0.0) for shell in vertex_piece[cast]])
    moved = np.full(len(first), -1)
    moved[cast] = len(p) + np.arange(len(cast))
    moved_tris = moved[welded[tri_v]]
    shift = -normals[cast] * (np.where(paired, distance, fallback) / 2)[:, None]
    points, flipped = untangled(p, tri_v, origin, shift, moved_tris, sheet_rows)
    # A far-facing triangle whose corners all moved lies on the near skin: it moves with them.
    sheet |= far & np.all(moved_tris >= 0, axis=1)
    whole = np.isin(piece, piece[flipped]) & (piece >= 0)
    sheet &= ~whole
    out = np.where(sheet[:, None], moved_tris, tri_v)
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
        if not len(mine):
            continue
        nearest, bary, apart = nearest_triangles(points[missed], triangle_search(far.corners[mine]))
        near = apart <= reach[missed]
        at[missed[near]] = (far.corners[mine[nearest[near]]] * bary[near][..., None]).sum(axis=1)
        tri[missed[near]] = mine[nearest[near]]
        hit[missed[near]] = True
    if hit.any():
        back[hit] = albedo_at_hit(at[hit], tri[hit], hit)
    return back

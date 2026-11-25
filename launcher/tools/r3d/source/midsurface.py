"""Midsurface abstraction for cloth built as two skins: one sheet halfway between them, drawn from both sides.

Each face of the near skin casts one ray along its inward normal onto the far skin, as midsurface abstraction
pairs the opposite faces of a thin wall (Rezayat 1996) and the shape diameter function measures a part's thickness
with inward rays (Shapira et al. 2008). A face whose ray finds the far skin facing back within reach stays and its
vertices move halfway across; a face whose ray misses or grazes is the rim, where the skins meet, and goes. The
far skin leaves the drawing but stays the source of what the sheet shows from behind (back_albedo())."""
from typing import NamedTuple

import numpy as np

from r3d.geometry import face_normals, nearest_triangles, triangle_search, weld_map

# A pair's skins face each other: the dot of the two normals is below this.
FACING = -0.9
# A pair is at most this many times its piece's median thickness apart; a longer ray crossed a fold.
REACH = 1.5
# A ray that meets the far skin nearer than this (model units) starts on it: a rim vertex both skins share.
CONTACT = 1e-6


class Midsurface(NamedTuple):
    """points: the source points with the moved vertices after them. tri_v: the triangles, each near one that
    stays indexing the moved vertices. drawn: per triangle, False for the far skin and the rim. far: per triangle,
    the far skin. reach: the farthest a sheet vertex lies from its far skin, for back_albedo()."""
    points: np.ndarray
    tri_v: np.ndarray
    drawn: np.ndarray
    far: np.ndarray
    reach: float


def vertex_normals(p, tris, welded, count):
    """Area-weighted unit normals of `tris` at each welded vertex (`welded` per point, `count` of them)."""
    cross = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    normals = np.zeros((count, 3))
    for k in range(3):
        np.add.at(normals, welded[tris[:, k]], cross)
    return normals / np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-30)


def inward_hits(far_query, far_normals, origins, normals):
    """(paired, distance) of rays from `origins` along -`normals` onto the far skin: a hit beyond CONTACT whose
    far triangle faces back (FACING)."""
    if far_query is None:
        return np.zeros(len(origins), dtype=bool), np.zeros(len(origins))
    hit, distance, tri = far_query.first_hits(origins, -normals)
    facing = np.einsum("ij,ij->i", normals, far_normals[np.where(hit, tri, 0)]) < FACING
    return hit & (distance > CONTACT) & facing, distance


def within_reach(paired, distance, piece):
    """`paired` less the pairs farther than REACH times their piece's median thickness, and each piece's median
    (NaN for a piece with no pair)."""
    median = {}
    for shell in np.unique(piece[paired]):
        mine = paired & (piece == shell)
        median[shell] = float(np.median(distance[mine]))
        paired = paired & (~mine | (distance <= REACH * median[shell]))
    return paired, median


def collapse(p, tri_v, far, piece, far_query):
    """The Midsurface of the shells `piece` marks (closed_shells(), -1 elsewhere) whose far skins `far` marks.
    far_query(p, tris) is a ray intersector over those triangles (ray_query.RayQuery).

    A near face pairs when the ray from its centre along its inward normal meets the far skin facing back within
    reach; the faces that do not, the hem or rim joining the skins, go. Each vertex of a paired face moves halfway
    along its own inward ray where that pairs too, else by half its piece's median thickness, as a vertex on the
    sheet's border, whose ray grazes the far skin's edge, does."""
    near_rows = np.flatnonzero((piece >= 0) & ~far)
    far_rows = np.flatnonzero(far)
    query = far_query(p, tri_v[far_rows]) if len(far_rows) else None
    far_normals = face_normals(p, tri_v[far_rows])[0] if len(far_rows) else np.zeros((1, 3))
    face_n, _area = face_normals(p, tri_v[near_rows])
    face_paired, face_distance = inward_hits(query, far_normals, p[tri_v[near_rows]].mean(axis=1), face_n)
    face_paired, median = within_reach(face_paired, face_distance, piece[near_rows])
    sheet_rows = near_rows[face_paired]
    first, welded = weld_map(p)
    normals = vertex_normals(p, tri_v[sheet_rows], welded, len(first))
    cast = np.unique(welded[tri_v[sheet_rows]])
    vertex_piece = np.full(len(first), -1)
    vertex_piece[welded[tri_v[sheet_rows]].reshape(-1)] = np.repeat(piece[sheet_rows], 3)
    origin = p[first[cast]]
    paired, distance = inward_hits(query, far_normals, origin, normals[cast])
    paired, _median = within_reach(paired, distance, vertex_piece[cast])
    half = np.where(paired, distance, [median.get(shell, 0.0) for shell in vertex_piece[cast]]) / 2
    moved = np.full(len(first), -1)
    moved[cast] = len(p) + np.arange(len(cast))
    points = np.concatenate([p, origin - normals[cast] * half[:, None]])
    sheet = moved[welded[tri_v]]
    # A far-facing triangle whose corners all moved lies on the near skin: it moves with them.
    on_sheet = np.zeros(len(tri_v), dtype=bool)
    on_sheet[sheet_rows] = True
    on_sheet |= far & np.all(sheet >= 0, axis=1)
    out = np.where(on_sheet[:, None], sheet, tri_v)
    drawn = (piece < 0) | on_sheet
    reach = REACH * max(median.values(), default=0.0)
    return Midsurface(points, out, drawn, far & ~on_sheet, reach)


def back_albedo(points, normals, front, far_query, far_corners, reach, albedo_at_hit):
    """What sheet vertices show from behind: the albedo where a ray along -normal meets the far skin within
    `reach` (albedo_at_hit(points on the far skin, its triangle rows, which of `points` found one)); where the
    ray misses, as at the sheet's border, whose ray grazes the far skin's edge, or for points with no `normals`
    (None), the nearest point of the far skin (`far_corners`, (F, 3, 3)) within reach; else `front`, as on a sheet
    with one skin."""
    back = np.array(front, dtype=np.float64, copy=True)
    if far_query is None or not len(points):
        return back
    hit, distance, tri = np.zeros(len(points), dtype=bool), np.zeros(len(points)), np.zeros(len(points), np.int64)
    if normals is not None:
        hit, distance, tri = far_query.first_hits(points, -normals)
        hit &= distance <= reach
    at = np.array(points, dtype=np.float64, copy=True)
    at[hit] -= normals[hit] * distance[hit, None]
    missed = np.flatnonzero(~hit)
    if len(missed):
        rows, bary, gap = nearest_triangles(points[missed], triangle_search(far_corners))
        near = gap <= reach
        at[missed[near]] = (far_corners[rows[near]] * bary[near][..., None]).sum(axis=1)
        tri[missed[near]] = rows[near]
        hit[missed[near]] = True
    if hit.any():
        back[hit] = albedo_at_hit(at[hit], tri[hit], hit)
    return back

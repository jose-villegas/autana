"""Joins the pieces of a mesh that touch, before it is simplified.

A model built from many pieces that butt against each other reaches the
simplifier as separate open borders, and each border is approximated on its
own. Here a border vertex that lies within `tolerance` of another piece's
border vertex is welded to it, and a border vertex that lies on the inside of
another piece's border edge splits that edge, so the shared edge is one edge
with one set of vertices on both sides. Only positions change: vertices are
never merged, so a colour or normal seam stays a seam."""

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree


def _position_ids(pos, tolerance):
    """One id per distinct position, and the position each id stands for."""
    # A grid a sixteenth of the tolerance keeps only the copies of one point
    # together; a coarser one would split near vertices by which cell they fall in.
    key = np.round(pos / (tolerance / 16.0)).astype(np.int64)
    _, first, ids = np.unique(key, axis=0, return_index=True, return_inverse=True)
    return first, ids.reshape(-1)


def _border_edges(ids, tris):
    """Edges with one triangle on them, as sorted pairs of position ids."""
    t = ids[tris]
    edges = np.sort(np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]]), axis=1)
    edges = edges[edges[:, 0] != edges[:, 1]]
    unique, count = np.unique(edges, axis=0, return_counts=True)
    return unique[count == 1]


def _weld_borders(pos, tris, tolerance):
    """Moves every border vertex to the lowest-numbered one within tolerance
    of it, chains of them included."""
    first, ids = _position_ids(pos, tolerance)
    stand = pos[first]
    border = np.unique(_border_edges(ids, tris))
    pairs = cKDTree(stand[border]).query_pairs(tolerance, output_type="ndarray")
    graph = coo_matrix((np.ones(len(pairs)), (pairs[:, 0], pairs[:, 1])), shape=(len(border), len(border)))
    count, label = connected_components(graph, directed=False)
    lead = np.full(count, len(stand))
    np.minimum.at(lead, label, border)
    target = np.arange(len(stand))
    target[border] = lead[label]
    return stand[target[ids]]


def _t_junctions(stand, edges, tolerance):
    """{(a, b): [(t, vertex), ...]} for each border edge (position ids, a < b)
    that has border vertices strictly inside it, t along it from a."""
    border = np.unique(edges)
    tree = cKDTree(stand[border])
    found = {}
    for a, b in edges:
        d = stand[b] - stand[a]
        length = float(np.linalg.norm(d))
        for k in tree.query_ball_point((stand[a] + stand[b]) / 2.0, length / 2.0 + tolerance):
            v = int(border[k])
            t = float((stand[v] - stand[a]) @ d) / (length * length)
            near = float(np.linalg.norm(stand[a] + t * d - stand[v]))
            if v not in (a, b) and near <= tolerance and tolerance < t * length < length - tolerance:
                found.setdefault((int(a), int(b)), []).append((t, v))
    return found


def _split(piece, k, chain, pid, rgb, new_rgb):
    """Fans a triangle from the corner opposite its edge k over the points on
    that edge (`chain`, ordered from the edge's first corner); each new
    vertex takes the colour the edge has at its position."""
    a, b, c = piece[k], piece[(k + 1) % 3], piece[(k + 2) % 3]

    def colour(i):
        return rgb[i] if i < len(rgb) else new_rgb[i - len(rgb)]

    pieces, previous = [], a
    for t, v in chain:
        index = len(pid)
        pid.append(v)
        new_rgb.append(colour(a) * (1.0 - t) + colour(b) * t)
        pieces.append((previous, index, c))
        previous = index
    pieces.append((previous, b, c))
    return pieces


def repair(pos, rgb, tris, labels, tolerance):
    """Returns pos, rgb, tris, labels with touching border vertices welded
    (within `tolerance`, a distance in the units of `pos`) and the border
    edges that carry another piece's vertex split there. A triangle split
    keeps its label."""
    pos = _weld_borders(np.asarray(pos, dtype=np.float64), np.asarray(tris), tolerance)
    rgb = np.asarray(rgb, dtype=np.float64)
    tris = np.asarray(tris)
    first, ids = _position_ids(pos, tolerance)
    stand = pos[first]
    junctions = _t_junctions(stand, _border_edges(ids, tris), tolerance)
    if not junctions:
        return pos, rgb, tris, np.asarray(labels)

    pid, new_rgb = list(ids), []
    out_tris, out_labels = [], []
    for i, tri in enumerate(tris):
        pending = [tuple(tri)]
        while pending:
            piece = pending.pop()
            for k in range(3):
                a, b = pid[piece[k]], pid[piece[(k + 1) % 3]]
                chain = junctions.get((min(a, b), max(a, b)))
                if chain:
                    along = sorted(chain if a < b else [(1.0 - t, v) for t, v in chain])
                    pending.extend(_split(piece, k, along, pid, rgb, new_rgb))
                    break
            else:
                out_tris.append(piece)
                out_labels.append(labels[i])
    added = np.array(pid[len(ids):], dtype=np.int64)
    return (np.concatenate([pos, stand[added]]), np.concatenate([rgb, np.array(new_rgb).reshape(-1, 3)]),
            np.array(out_tris, dtype=tris.dtype), np.array(out_labels, dtype=np.asarray(labels).dtype))

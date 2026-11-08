"""An object's connected parts: the pieces the object reports and the repeated-unit search work on."""

import numpy as np


def part_labels(p, tri_v, tri_o):
    """Per triangle, its connected part: triangles of one object that share a welded position. Parts are numbered
    by object, then by first triangle. Also per part, whether it shares a welded position with another object."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    from r3d.geometry import weld_keeping

    _, welded = weld_keeping(p, tri_v)
    nodes, node = np.unique(np.column_stack([np.repeat(tri_o, 3), welded.ravel()]), axis=0, return_inverse=True)
    node = node.reshape(-1, 3)
    graph = coo_matrix((np.ones(2 * len(node)), (np.r_[node[:, 0], node[:, 0]], np.r_[node[:, 1], node[:, 2]])),
                       shape=(len(nodes), len(nodes)))
    _, component = connected_components(graph, directed=False)
    raw = component[node[:, 0]]
    _, first = np.unique(raw, return_index=True)
    order = np.lexsort((first, tri_o[first]))
    rank = np.empty(len(order), dtype=np.int64)
    rank[order] = np.arange(len(order))
    labels = rank[np.unique(raw, return_inverse=True)[1]]
    owners = np.unique(nodes[:, 1], return_counts=True)
    shared_positions = owners[0][owners[1] > 1]
    shared = np.zeros(len(order), dtype=bool)
    shared[np.unique(labels[np.isin(welded, shared_positions).any(axis=1)])] = True
    return labels, shared


def part_names(labels, tri_o, object_names):
    """`object` for an object of one part, else `object#k`, k counting its parts from 0; also per part its
    object's index and part count."""
    first = np.unique(labels, return_index=True)[1]
    owner = tri_o[first]
    counts = np.bincount(owner, minlength=len(object_names))
    names, seen = [], {}
    for o in owner:
        k = seen.get(o, 0)
        seen[o] = k + 1
        names.append(object_names[o] if counts[o] == 1 else f"{object_names[o]}#{k}")
    return names, owner, counts[owner]

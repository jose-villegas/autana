"""A cluster tree: clusters split by centre into an octree whose leaves hold runs of clusters."""

import numpy as np


def build_octree(points, weights, leaf_weight, max_depth):
    """Splits the items by point until a node holds one item, or items whose
    weights sum to leaf_weight or less. Only axes at least half as long as
    the node's longest are split, so a long thin node halves along its
    length first."""
    points = np.asarray(points, dtype=np.float64)
    weights = np.asarray(weights)

    def build(members, lo, hi, depth):
        if len(members) == 1 or weights[members].sum() <= leaf_weight or depth >= max_depth:
            return {"leaf": members}
        extent = hi - lo
        axes = [a for a in range(3) if extent[a] >= 0.5 * extent.max()]
        mid = (lo + hi) / 2
        code = np.zeros(len(members), dtype=np.int64)
        for bit, a in enumerate(axes):
            code |= (points[members, a] >= mid[a]).astype(np.int64) << bit
        children = []
        for c in np.unique(code):
            clo, chi = lo.copy(), hi.copy()
            for bit, a in enumerate(axes):
                if (c >> bit) & 1:
                    clo[a] = mid[a]
                else:
                    chi[a] = mid[a]
            children.append(build(members[code == c], clo, chi, depth + 1))
        if len(children) == 1:
            return children[0]
        return {"children": children}

    return build(np.arange(len(points)), points.min(axis=0), points.max(axis=0), 0)


def flatten_octree(root):
    """Returns (order, nodes): the items in leaf order, and the nodes breadth
    first, so each node's children sit together while a subtree's items sit
    together in `order`; a leaf's first and count index `order`."""
    order = []

    def collect(node):
        if "leaf" in node:
            node["first_item"] = len(order)
            order.extend(int(m) for m in node["leaf"])
        else:
            for child in node["children"]:
                collect(child)

    collect(root)
    queue = [root]
    nodes = []
    i = 0
    while i < len(queue):
        node = queue[i]
        if "leaf" in node:
            nodes.append({"leaf": True, "first": node["first_item"], "count": len(node["leaf"])})
        else:
            nodes.append({"leaf": False, "first": len(queue), "count": len(node["children"])})
            queue.extend(node["children"])
        i += 1
    return order, nodes


def node_bounds(nodes, clusters):
    """Tight bounds of what a node holds; children always follow their parent."""
    for node in reversed(nodes):
        if node["leaf"]:
            parts = clusters[node["first"] : node["first"] + node["count"]]
            node["lo"] = np.min([c[4] for c in parts], axis=0)
            node["hi"] = np.max([c[5] for c in parts], axis=0)
        else:
            parts = nodes[node["first"] : node["first"] + node["count"]]
            node["lo"] = np.min([n["lo"] for n in parts], axis=0)
            node["hi"] = np.max([n["hi"] for n in parts], axis=0)

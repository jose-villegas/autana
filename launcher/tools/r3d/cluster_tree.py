"""A cluster tree: clusters split by the surface area heuristic into a tree whose leaves hold runs of clusters."""

import numpy as np


# One node test costs this many cluster tests: the box test, then the depth sort and the push of its children
# (r3d_pipeline.c's push_children). The host count along the fitted scenes' paths is lowest here.
NODE_TEST_COST = 4.0


def surface_area(lo, hi):
    """Surface area of each box of `lo` and `hi`, rows of three; a point or an inverted box has none."""
    e = np.maximum(np.asarray(hi, dtype=np.float64) - lo, 0.0)
    return 2.0 * (e[..., 0] * e[..., 1] + e[..., 1] * e[..., 2] + e[..., 2] * e[..., 0])


def build_tree(lo, hi, weights, leaf_weight, max_items, fanout, node_cost=NODE_TEST_COST):
    """Splits items (boxes `lo`..`hi`, one row each) by the surface area heuristic (MacDonald and Booth):
    a node's chance of being reached is its box's area over its parent's, so a split costs node_cost plus
    each side's items weighted by its area, and a node stays a leaf while testing all its items costs no
    more. A leaf holds at most max_items items and, unless it is one item, weights summing to leaf_weight.
    Each split is the cheapest cut of the items sorted by box centre along one axis; the binary splits
    then collapse into nodes of up to `fanout` children, the largest child opening first."""
    lo = np.asarray(lo, dtype=np.float64)
    hi = np.asarray(hi, dtype=np.float64)
    weights = np.asarray(weights)
    centres = (lo + hi) / 2.0

    def cheapest_cut(members):
        best = None
        parent = surface_area(lo[members].min(axis=0), hi[members].max(axis=0)) or 1.0
        n = len(members)
        sides = np.arange(1, n)
        for axis in range(3):
            order = members[np.argsort(centres[members, axis], kind="stable")]
            left = surface_area(np.minimum.accumulate(lo[order])[:-1], np.maximum.accumulate(hi[order])[:-1])
            right = surface_area(np.minimum.accumulate(lo[order][::-1])[::-1][1:],
                                 np.maximum.accumulate(hi[order][::-1])[::-1][1:])
            costs = node_cost + (left * sides + right * (n - sides)) / parent
            k = int(np.argmin(costs))
            if best is None or costs[k] < best[0]:
                best = (float(costs[k]), order, k + 1)
        return best

    def build(members):
        if len(members) == 1:
            return {"leaf": members}
        cost, order, k = cheapest_cut(members)
        fits = len(members) <= max_items and weights[members].sum() <= leaf_weight
        if fits and len(members) <= cost:
            return {"leaf": members}
        return {"children": [build(order[:k]), build(order[k:])]}

    def box(node):
        if "leaf" in node:
            return lo[node["leaf"]].min(axis=0), hi[node["leaf"]].max(axis=0)
        return node["lo"], node["hi"]

    def collapse(node):
        if "leaf" in node:
            return node
        children = [collapse(child) for child in node["children"]]
        while True:
            inner = [child for child in children if "children" in child]
            if not inner:
                break
            widest = max(inner, key=lambda child: surface_area(*box(child)))
            if len(children) - 1 + len(widest["children"]) > fanout:
                break
            children = [child for child in children if child is not widest] + widest["children"]
        boxes = [box(child) for child in children]
        return {"children": children, "lo": np.min([b[0] for b in boxes], axis=0),
                "hi": np.max([b[1] for b in boxes], axis=0)}

    return collapse(build(np.arange(len(lo))))


def flatten_tree(root):
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

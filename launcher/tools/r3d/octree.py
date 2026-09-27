"""A cluster tree: triangles split by centroid into an octree whose leaves become clusters."""

import numpy as np


def build_octree(positions, tris, leaf_triangles, max_depth):
    """Splits the triangles by centroid until a node holds leaf_triangles or
    fewer. Only axes at least half as long as the node's longest are split,
    so a long thin node halves along its length first."""
    centroid = positions[tris].mean(axis=1)

    def build(members, lo, hi, depth):
        if len(members) <= leaf_triangles or depth >= max_depth:
            return {"leaf": members}
        extent = hi - lo
        axes = [a for a in range(3) if extent[a] >= 0.5 * extent.max()]
        mid = (lo + hi) / 2
        code = np.zeros(len(members), dtype=np.int64)
        for bit, a in enumerate(axes):
            code |= (centroid[members, a] >= mid[a]).astype(np.int64) << bit
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

    return build(np.arange(len(tris)), positions.min(axis=0), positions.max(axis=0), 0)


def flatten_octree(root, tri_double):
    """Nodes breadth first, so each node's children sit together; leaves'
    clusters depth first, so a subtree's clusters sit together too. A leaf
    holding both single- and double-sided triangles owns two clusters."""
    clusters = []

    def collect(node):
        if "leaf" in node:
            node["first_cluster"] = len(clusters)
            for double in (False, True):
                members = node["leaf"][tri_double[node["leaf"]] == int(double)]
                if len(members):
                    clusters.append((double, members))
            node["cluster_count"] = len(clusters) - node["first_cluster"]
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
            nodes.append({"leaf": True, "first": node["first_cluster"], "count": node["cluster_count"]})
        else:
            nodes.append({"leaf": False, "first": len(queue), "count": len(node["children"])})
            queue.extend(node["children"])
        i += 1
    return clusters, nodes


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

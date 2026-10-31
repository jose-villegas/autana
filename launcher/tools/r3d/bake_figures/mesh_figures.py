"""The figures of docs/render/offline/Baked-Mesh.md, from the bake's own functions and a baked mesh.

Clusters and the tree: a baked mesh's clusters in the meshlets view's hues, then the tree level by level, each
cluster in the colour of its node at that level, every box faint but one root-to-leaf path's, orbiting. The
ranges: the first leaves' clusters as runs of the triangle and vertex arrays. The build: the clusterizer the
importer calls (r3d.meshopt.build_meshlets) on a small flat patch, one meshlet per step in the order it emits
them. meshoptimizer has no per-triangle hook and the call reorders each meshlet's triangles, so a step is a
whole meshlet.

    python launcher/tools/r3d/bake_figures/mesh_figures.py --mesh MESH --out DIR

MESH is a baked .mesh (bake.py path OUTPUT). Needs launcher/tools/r3d/requirements.txt.
launcher/tools/render/render_doc_images.sh runs it.
"""

import argparse
import colorsys
import pathlib
import re
import sys

import numpy as np
from matplotlib.collections import PolyCollection
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection
from scipy.spatial import Delaunay

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from docs.plot_style import FAINT, GIF_FPS, INK, LABEL_SIZE, MUTED, TITLE_SIZE, save, save_gif, subplots  # noqa: E402
from r3d.lit_mesh import read_lit_mesh  # noqa: E402
from r3d.meshopt import build_meshlets  # noqa: E402

VIEW = TOOLS.parents[0] / "main/render/view/raster_meshlets.c"
# The tree figure: one panel per level after the clusters, orbiting from VIEW_ELEVATION degrees up.
TREE_PANEL = 3.0  # inches per panel
VIEW_ELEVATION = 55
VIEW_AZIMUTH = -60
ORBIT_FRAMES = 36
BOX_WIDTH = 0.5
BOLD_WIDTH = 2.0  # the one root-to-leaf path's boxes
OUTSIDE_FADE = 0.75  # how far triangles outside the bold box fade toward white
# The ranges figure.
STRIP_SIZE = (8.0, 2.4)
STRIP_HEIGHT = 0.8
RANGE_LEAVES = 3
# The build figure: a flat patch of BUILD_POINTS jittered points in a unit disc, cut at BUILD_TRIANGLES.
BUILD_POINTS = 180
RIM_POINTS = 40  # evenly round the edge, so the patch is a disc
BUILD_SEED = 7
BUILD_TRIANGLES = 16
BUILD_SIZE = (4.2, 4.6)
STEP_FRAMES = 3  # frames each meshlet holds as the newest
END_FRAMES = 12
EDGE_WIDTH = 0.4
NEW_EDGE_WIDTH = 1.6
OLD_FADE = 0.55
# Many cluster hues plus their fades: the 64 of an orbit tint the titles, so these GIFs take the format's most.
HUE_GIF_COLORS = 256


def view_hash():
    """The multiplier the meshlets view hashes a cluster id by, read from its source."""
    return int(re.search(r"id \* (\d+)U", VIEW.read_text())[1])


def hue(index, multiplier):
    """The meshlets view's colour of cluster id `index` (ids start at 1), as RGB 0..1."""
    return colorsys.hsv_to_rgb(((index * multiplier) & 0xFFFFFFFF) / 2**32, 1.0, 1.0)


def faded(rgb, amount):
    return tuple(c + (1.0 - c) * amount for c in rgb)


def up(points):
    """Mesh axes (y up) to the plot's (z up)."""
    return points[..., [0, 2, 1]]


def node_depths(nodes):
    """Each node's depth from the root."""
    depth = [0] * len(nodes)
    for i, node in enumerate(nodes):
        if not node["leaf"]:
            for child in range(node["first"], node["first"] + node["count"]):
                depth[child] = depth[i] + 1
    return depth


def cluster_nodes(nodes):
    """For each cluster, the chain of nodes from the root to its leaf."""
    chains = {}

    def walk(i, chain):
        node = nodes[i]
        for child in range(node["first"], node["first"] + node["count"]):
            if node["leaf"]:
                chains[child] = chain + [i]
            else:
                walk(child, chain + [i])

    walk(0, [])
    return [chains[c] for c in range(len(chains))]


def box_edges(lo, hi):
    corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
    pairs = [(a, b) for a in range(8) for b in range(a + 1, 8) if bin(a ^ b).count("1") == 1]
    return [up(corners[[a, b]]) for a, b in pairs]


def tree(mesh, path):
    """Orbit: the clusters, then each level of the tree: its boxes faint but for one root-to-leaf path, bold, and
    that path's triangles in full colour."""
    multiplier = view_hash()
    depth = node_depths(mesh.nodes)
    chains = cluster_nodes(mesh.nodes)
    levels = max(depth)
    triangles = up(mesh.pos[mesh.tris].astype(float))
    owner = np.empty(len(mesh.tris), dtype=np.int64)
    for c, (_, _, tbase, tcount, *_rest) in enumerate(mesh.clusters):
        owner[tbase : tbase + tcount] = c
    # The path shown bold: down to the deepest leaf holding the most triangles.
    sizes = np.bincount(owner, minlength=len(mesh.clusters))
    path_chain = max(chains, key=lambda chain: (len(chain), sum(sizes[c] for c in range(
        mesh.nodes[chain[-1]]["first"], mesh.nodes[chain[-1]]["first"] + mesh.nodes[chain[-1]]["count"]))))
    panels = [("clusters", [hue(c + 1, multiplier) for c in owner], [])]
    for level in range(1, levels + 1):
        node_of = np.array([chain[min(level, len(chain) - 1)] for chain in chains])
        shown = sorted(set(node_of))
        bold = path_chain[level]
        colour = {n: hue(n + 1, multiplier) for n in shown}
        boxes = [(edge, colour[n] if n == bold else FAINT, BOLD_WIDTH if n == bold else BOX_WIDTH)
                 for n in shown for edge in box_edges(mesh.nodes[n]["lo"], mesh.nodes[n]["hi"])]
        name = "leaves" if level == levels else f"level {level}"
        panels.append((f"{name}: {len(shown)} boxes, one bold",
                       [colour[bold] if node_of[c] == bold else faded(colour[node_of[c]], OUTSIDE_FADE)
                        for c in owner], boxes))
    figure, axes = subplots(1, len(panels), (TREE_PANEL * len(panels), TREE_PANEL),
                            subplot_kw={"projection": "3d", "computed_zorder": False})
    lo, hi = triangles.reshape(-1, 3).min(axis=0), triangles.reshape(-1, 3).max(axis=0)
    for axis, (title, colours, boxes) in zip(axes, panels):
        axis.add_collection3d(Poly3DCollection(triangles, facecolors=colours, edgecolors="none", zorder=1))
        # The boxes over the mesh, the faint boxes first, so the bold one draws over them.
        for edges in (sorted(boxes, key=lambda box: box[2])):
            axis.add_collection3d(Line3DCollection([edges[0]], colors=[edges[1]], linewidths=edges[2], zorder=2))
        axis.set(xlim=(lo[0], hi[0]), ylim=(lo[1], hi[1]), zlim=(lo[2], hi[2]))
        axis.set_box_aspect(hi - lo)
        axis.set_axis_off()
        axis.set_title(title, fontsize=TITLE_SIZE, color=INK)

    def frame(index):
        for axis in axes:
            axis.view_init(VIEW_ELEVATION, VIEW_AZIMUTH + 360.0 * index / ORBIT_FRAMES)

    if path.suffix == ".png":
        frame(0)
        save(figure, path)
    else:
        save_gif(figure, frame, ORBIT_FRAMES, path, GIF_FPS, colors=HUE_GIF_COLORS)


def ranges(mesh, path):
    """The first RANGE_LEAVES leaves' clusters: each a run of the triangle array and of the vertex array."""
    multiplier = view_hash()
    leaves = [n for n in mesh.nodes if n["leaf"]][:RANGE_LEAVES]
    shown = [c for leaf in leaves for c in range(leaf["first"], leaf["first"] + leaf["count"])]
    figure, axis = subplots(1, 1, STRIP_SIZE)
    rows = (("triangles", 2, 2), ("vertices", 0, 0))
    for label, y, field in rows:
        start = mesh.clusters[shown[0]][field]
        last = mesh.clusters[shown[-1]]
        span = last[field] + last[field + 1] - start
        for c in shown:
            first, count = mesh.clusters[c][field], mesh.clusters[c][field + 1]
            axis.broken_barh([((first - start) / span, count / span)], (y, STRIP_HEIGHT),
                             facecolors=hue(c + 1, multiplier), edgecolors="white", linewidths=EDGE_WIDTH)
        axis.text(-0.01, y + STRIP_HEIGHT / 2, f"{label}\n{start}..{start + span}", ha="right", va="center",
                  fontsize=LABEL_SIZE, color=MUTED)
    start = mesh.clusters[shown[0]][2]
    last = mesh.clusters[shown[-1]]
    span = last[2] + last[3] - start
    for i, leaf in enumerate(leaves):
        first = mesh.clusters[leaf["first"]][2]
        end_cluster = mesh.clusters[leaf["first"] + leaf["count"] - 1]
        a, b = (first - start) / span, (end_cluster[2] + end_cluster[3] - start) / span
        axis.plot([a, a, b, b], [3.0, 3.2, 3.2, 3.0], color=INK, linewidth=1.0)
        axis.text((a + b) / 2, 3.3, str(leaf["count"]), ha="center", va="bottom", fontsize=LABEL_SIZE, color=MUTED)
    axis.text(-0.01, 3.1, "leaves\n(clusters)", ha="right", va="center", fontsize=LABEL_SIZE, color=MUTED)
    vertex_start = mesh.clusters[shown[0]][0]
    vertex_span = mesh.clusters[shown[-1]][0] + mesh.clusters[shown[-1]][1] - vertex_start
    for c in shown:
        vbase, vcount, tbase, tcount = mesh.clusters[c][:4]
        top = ((tbase - start) / span, (tbase + tcount - start) / span)
        bottom = ((vbase - vertex_start) / vertex_span, (vbase + vcount - vertex_start) / vertex_span)
        axis.fill([top[0], top[1], bottom[1], bottom[0]], [2, 2, STRIP_HEIGHT, STRIP_HEIGHT],
                  color=faded(hue(c + 1, multiplier), OLD_FADE), linewidth=0)
    axis.set(xlim=(0, 1), ylim=(-0.2, 3.8))
    axis.set_axis_off()
    axis.set_title(f"The first {len(shown)} of {len(mesh.clusters)} clusters: each a run of both arrays",
                   fontsize=TITLE_SIZE, color=INK)
    save(figure, path)


def patch():
    """A flat patch: jittered points in a unit disc and round its rim, Delaunay-triangulated."""
    rng = np.random.default_rng(BUILD_SEED)
    angle = rng.uniform(0, 2 * np.pi, BUILD_POINTS)
    radius = np.sqrt(rng.uniform(0, 1, BUILD_POINTS))
    flat = np.column_stack([radius * np.cos(angle), radius * np.sin(angle)])
    rim = np.linspace(0, 2 * np.pi, RIM_POINTS, endpoint=False)
    flat = np.vstack([flat, np.column_stack([np.cos(rim), np.sin(rim)])])
    tris = Delaunay(flat).simplices
    return np.column_stack([flat, np.zeros(len(flat))]), tris


def build(path):
    """One meshlet per step in the clusterizer's order: the newest outlined, the earlier ones faded."""
    multiplier = view_hash()
    pos, tris = patch()
    meshlets = build_meshlets(pos, tris, BUILD_TRIANGLES)
    flat = pos[:, :2]
    figure, axis = subplots(1, 1, BUILD_SIZE)
    axis.set_aspect("equal")
    axis.set_axis_off()
    axis.add_collection(PolyCollection(flat[tris], facecolors="white", edgecolors=FAINT, linewidths=EDGE_WIDTH))
    drawn = []
    steps = len(meshlets)
    frames = steps * STEP_FRAMES + END_FRAMES

    def frame(index):
        step = min(index // STEP_FRAMES, steps - 1)
        for artist in drawn:
            artist.remove()
        drawn.clear()
        for k in range(step + 1):
            colour = hue(k + 1, multiplier)
            newest = k == step and index < steps * STEP_FRAMES
            drawn.append(axis.add_collection(PolyCollection(
                flat[meshlets[k]], facecolors=colour if newest else faded(colour, OLD_FADE),
                edgecolors=INK if newest else "white", linewidths=NEW_EDGE_WIDTH if newest else EDGE_WIDTH)))
        current = meshlets[step]
        verts = len(np.unique(current))
        axis.set_title(f"meshlet {step + 1} of {steps}: {len(current)} of {BUILD_TRIANGLES} triangles, "
                       f"{verts} vertices", fontsize=TITLE_SIZE, color=INK)
        axis.set(xlim=(-1.05, 1.05), ylim=(-1.05, 1.05))

    if path.suffix == ".png":
        frame(frames // 2)
        save(figure, path)
    else:
        save_gif(figure, frame, frames, path, GIF_FPS, colors=HUE_GIF_COLORS)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mesh", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--still", action="store_true", help="PNG stills in place of the GIFs")
    args = parser.parse_args(argv)
    mesh = read_lit_mesh(args.mesh)
    moving = ".png" if args.still else ".gif"
    tree(mesh, args.out / f"baked-mesh-tree{moving}")
    ranges(mesh, args.out / "baked-mesh-ranges.png")
    build(args.out / f"meshlet-build{moving}")


if __name__ == "__main__":
    main()

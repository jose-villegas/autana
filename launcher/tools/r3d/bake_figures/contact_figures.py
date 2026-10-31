"""The contact figure of docs/render/offline/Mesh-Import.md, from the importer's own contact graph and simplify pass.

A pillar standing on a floor, two objects that share no vertex, carved as a mesh with each seam policy in turn and
simplified to the same budgets. Each panel shows the base from above at an angle: the simplified wire, the source
footprint, and each contact vertex pinned where the source had it or moved by the pass.

    python launcher/tools/r3d/bake_figures/contact_figures.py --out DIR

Needs launcher/tools/r3d/requirements.txt. launcher/tools/render/render_doc_images.sh runs it.
"""

import argparse
import pathlib
import sys

import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, figure, save  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection  # noqa: E402
from r3d.import_settings import SEAM_POLICIES  # noqa: E402
from r3d.lit_mesh import POSITION_SCALE  # noqa: E402
from r3d.objects.contact import contact_graph  # noqa: E402
from r3d.simplify import Allocation, simplify  # noqa: E402

FIGURE_NAME = "import-mesh-contact.png"
OBJECT_NAMES = ("floor", "pillar")
FLOOR, PILLAR = range(len(OBJECT_NAMES))
# The pillar: a plinth stepping in to the shaft, round in PILLAR_SIDES; each ring a height and a radius.
PILLAR_SIDES = 16
PILLAR_RINGS = ((0.0, 1.3), (0.12, 1.3), (0.25, 1.1), (0.4, 1.0), (0.8, 1.0), (1.2, 1.0), (1.6, 1.0))
# The floor: a square plate of FLOOR_CELLS by FLOOR_CELLS quads, FLOOR_HALF either side of the pillar's axis.
FLOOR_CELLS = 10
FLOOR_HALF = 2.0
# Budgets: the pillar's as a carved mesh, the rest's (the floor) as the variant's.
PILLAR_TRIANGLES = 120
REST_TRIANGLES = 80
SOURCE_COLOUR = 100
COLOUR_DEVIATION = 1
# Two contact vertices are the same point when they agree to this many decimals.
SAME_POINT_DECIMALS = 5
# The view: from VIEW_ELEVATION degrees up, turned VIEW_AZIMUTH about the pillar's axis.
VIEW_ELEVATION = 32
VIEW_AZIMUTH = -58
PANEL_SIZE = (4.0, 3.9)
FLOOR_ALPHA = 0.55
PILLAR_ALPHA = 0.45
WIRE_WIDTH = 0.5
FOOTPRINT_WIDTH = 1.4
MARKER_SIZE = 26
MOVED_EDGE = 1.3
PINNED_COLOUR = SLOTS[2]
MOVED_COLOUR = SLOTS[7]
PILLAR_COLOUR = SLOTS[0]


def pillar():
    points = [(radius * np.cos(angle), height, radius * np.sin(angle))
              for height, radius in PILLAR_RINGS
              for angle in np.linspace(0, 2 * np.pi, PILLAR_SIDES, endpoint=False)]
    faces = []
    for ring in range(len(PILLAR_RINGS) - 1):
        for side in range(PILLAR_SIDES):
            a = ring * PILLAR_SIDES + side
            b = ring * PILLAR_SIDES + (side + 1) % PILLAR_SIDES
            faces += [(a, a + PILLAR_SIDES, b), (b, a + PILLAR_SIDES, b + PILLAR_SIDES)]
    return np.asarray(points), np.asarray(faces)


def floor():
    ticks = np.linspace(-FLOOR_HALF, FLOOR_HALF, FLOOR_CELLS + 1)
    points = [(x, 0.0, z) for z in ticks for x in ticks]
    row = FLOOR_CELLS + 1
    faces = []
    for j in range(FLOOR_CELLS):
        for i in range(FLOOR_CELLS):
            a = j * row + i
            faces += [(a, a + 1, a + row), (a + 1, a + row + 1, a + row)]
    return np.asarray(points), np.asarray(faces)


def source():
    """The floor, then the pillar on it; each triangle's owner."""
    floor_points, floor_faces = floor()
    pillar_points, pillar_faces = pillar()
    points = np.vstack([floor_points, pillar_points])
    faces = np.vstack([floor_faces, pillar_faces + len(floor_points)])
    owners = np.r_[np.full(len(floor_faces), FLOOR), np.full(len(pillar_faces), PILLAR)]
    return points, faces, owners


def simplified(points, faces, owners, policy):
    allocation = Allocation(tri_o=owners, object_names=OBJECT_NAMES, material_names=("plain",), report={},
                            meshes=({"match": {"object": OBJECT_NAMES[PILLAR]}, "triangles": PILLAR_TRIANGLES,
                                     "seam": policy},))
    kept, _, kept_faces, _ = simplify(points, np.full_like(points, SOURCE_COLOUR), faces,
                                      np.zeros(len(faces), dtype=int), REST_TRIANGLES,
                                      colour_deviation=COLOUR_DEVIATION, allocation=allocation)
    return kept, kept_faces


def pinned(points, contact, kept):
    """Each contact vertex: whether the pass kept a vertex where the source had it."""
    kept_points = {tuple(p) for p in np.round(kept, SAME_POINT_DECIMALS)}
    return np.array([tuple(p) in kept_points for p in np.round(points[contact], SAME_POINT_DECIMALS)])


def to_axes(p):
    """Engine space has y up; the plot's third axis is up."""
    return p[..., [0, 2, 1]]


def draw_panel(axis, policy, points, contact, kept, kept_faces):
    axis.computed_zorder = False  # the markers over the faces, in drawing order
    corners = kept[kept_faces]
    on_floor = np.all(np.abs(corners[:, :, 1]) < 10 ** -SAME_POINT_DECIMALS, axis=1)
    for chosen, colour, alpha in ((on_floor, FAINT, FLOOR_ALPHA), (~on_floor, PILLAR_COLOUR, PILLAR_ALPHA)):
        axis.add_collection3d(Poly3DCollection(to_axes(corners[chosen]), facecolor=colour, alpha=alpha,
                                               edgecolor=INK, linewidth=WIRE_WIDTH))
    base = to_axes(np.array([(PILLAR_RINGS[0][1] * np.cos(a), 0.0, PILLAR_RINGS[0][1] * np.sin(a))
                             for a in np.linspace(0, 2 * np.pi, PILLAR_SIDES + 1)]))
    axis.add_collection3d(Line3DCollection([base], colors=MUTED, linewidths=FOOTPRINT_WIDTH, linestyles="dashed"))
    held = pinned(points, contact, kept)
    shown = to_axes(points[contact])
    axis.scatter(*shown[held].T, s=MARKER_SIZE, color=PINNED_COLOUR, depthshade=False)
    axis.scatter(*shown[~held].T, s=MARKER_SIZE, facecolors="none", edgecolors=MOVED_COLOUR, linewidths=MOVED_EDGE,
                 depthshade=False)
    axis.set_title(f"{policy}: {held.sum()} of {len(held)} contact vertices pinned", fontsize=TITLE_SIZE, color=INK)
    top = PILLAR_RINGS[-1][0]
    axis.set_xlim(-FLOOR_HALF, FLOOR_HALF)
    axis.set_ylim(-FLOOR_HALF, FLOOR_HALF)
    axis.set_zlim(0, top)
    axis.set_box_aspect((2 * FLOOR_HALF, 2 * FLOOR_HALF, top))
    axis.view_init(VIEW_ELEVATION, VIEW_AZIMUTH)
    axis.set_axis_off()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    points, faces, owners = source()
    edges = contact_graph(points, faces, owners, POSITION_SCALE)
    if len(edges) != 1:
        raise SystemExit(f"contact_figures: the pillar should touch the floor once, not {len(edges)} times")
    contact = np.unique(np.concatenate([edges[0].vertices_a, edges[0].vertices_b]))
    fig = figure((PANEL_SIZE[0] * len(SEAM_POLICIES), PANEL_SIZE[1]))
    for index, policy in enumerate(SEAM_POLICIES):
        axis = fig.add_subplot(1, len(SEAM_POLICIES), index + 1, projection="3d")
        draw_panel(axis, policy, points, contact, *simplified(points, faces, owners, policy))
    legend = [Line2D([], [], color=INK, linewidth=WIRE_WIDTH, label="simplified wire"),
              Line2D([], [], color=MUTED, linewidth=FOOTPRINT_WIDTH, linestyle="dashed", label="source footprint"),
              Line2D([], [], marker="o", linestyle="none", color=PINNED_COLOUR, label="contact vertex pinned"),
              Line2D([], [], marker="o", linestyle="none", markerfacecolor="none", markeredgecolor=MOVED_COLOUR,
                     label="contact vertex moved (its source place)")]
    fig.legend(handles=legend, loc="lower center", ncol=len(legend), fontsize=LABEL_SIZE, frameon=False)
    args.out.mkdir(parents=True, exist_ok=True)
    save(fig, args.out / FIGURE_NAME)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

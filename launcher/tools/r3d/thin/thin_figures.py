#!/usr/bin/env python3
"""Thin-curve figures for docs/render/Mesh-Rendering.md and docs/render/offline/Mesh-Import.md; --cc CC --out DIR.

curves-ribbon.png: four pictures drawn by r3d_curve_draw() through a probe. curves-detection.gif: a small scene
orbiting, its triangles coloured by what thin_parts_at() takes for curves."""
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from docs.figures import probe_into_folder  # noqa: E402
from docs.producer import Producer  # noqa: E402
from docs.plot_style import FAINT, GIF_FPS, GIF_FRAMES, INK, LABEL_SIZE, SLOTS, save, save_gif, subplots  # noqa: E402
from r3d.thin.thin_parts import budget_side, find_thin_parts, thin_parts_at  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[4]
SOURCES = (pathlib.Path(__file__).with_name("thin_figures_probe.c"),
           ROOT / "launcher/main/render/curve/r3d_curve.c", ROOT / "launcher/main/render/r3d_span.c")
PRODUCER = Producer(area="render", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("curves-ribbon.png", "curves-detection.gif"))

PANEL_COLUMNS = 2
PANEL_SIZE = (3.4, 2.4)  # inches a picture takes, title included
RGB_BYTE = 0xFF
RGB_SHIFTS = (16, 8, 0)

# The detection scene, in metres: two posts, a cable hung between them, a block with a rail lying on it.
BUDGET = 40  # start triangles: the side the detector measures against is sqrt(area / BUDGET)
POST_HEIGHT = 3.0
POST_WIDTH = 0.08
POST_X = 2.0
CABLE_RADIUS = 0.02
CABLE_GAP = 0.15  # metres between the cable's ends and the posts, so the two stay separate parts
CABLE_SAG = 0.6
CABLE_KNOTS = 24
TUBE_SIDES = 6
BLOCK = (2.0, 1.0, 1.0)
RAIL = (1.8, 0.08, 0.08)
CURVE_COLOUR = SLOTS[1]
MESH_COLOUR = "#b8bcc4"
RESTING_COLOUR = SLOTS[0]
FACE_ALPHA = 0.9
VIEW_ELEVATION = 20
ORBIT_AZIMUTH = -60
ORBIT_TURN = 360
GIF_SIZE = (5.0, 3.6)


def rgb_picture(picture, width, height):
    packed = np.asarray(picture["rgb"], dtype=np.uint32).reshape(height, width)
    return np.stack([(packed >> shift) & RGB_BYTE for shift in RGB_SHIFTS], axis=-1).astype(np.uint8)


def ribbon_figure(data, output):
    pictures = data["pictures"]
    rows = (len(pictures) + PANEL_COLUMNS - 1) // PANEL_COLUMNS
    fig, axes = subplots(rows, PANEL_COLUMNS, (PANEL_SIZE[0] * PANEL_COLUMNS, PANEL_SIZE[1] * rows))
    for axis, picture in zip(np.ravel(axes), pictures):
        axis.imshow(rgb_picture(picture, data["width"], data["height"]), interpolation="nearest")
        axis.set_title(picture["title"])
        axis.set_xticks([])
        axis.set_yticks([])
    fig.suptitle(f"Curves as drawn: at least {data['min_width_px']:g} pixel wide, fading by coverage below it")
    save(fig, output)


def box(size, centre):
    corners = np.array([[x, y, z] for x in (-0.5, 0.5) for y in (-0.5, 0.5) for z in (-0.5, 0.5)])
    points = corners * size + centre
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    return points, np.array([tri for a, b, c, d in faces for tri in ((a, b, c), (a, c, d))])


def tube(centres, radius):
    """A closed-sided tube of TUBE_SIDES around a polyline lying in the xy plane."""
    angles = np.linspace(0, 2 * np.pi, TUBE_SIDES, endpoint=False)
    rings = []
    for k, centre in enumerate(centres):
        along = centres[min(k + 1, len(centres) - 1)] - centres[max(k - 1, 0)]
        along /= np.linalg.norm(along)
        across = np.array([-along[1], along[0], 0.0])
        rings.append([centre + radius * (np.cos(a) * across + np.sin(a) * np.array([0, 0, 1])) for a in angles])
    points = np.array(rings).reshape(-1, 3)
    tris = []
    for k in range(len(centres) - 1):
        for s in range(TUBE_SIDES):
            a, b = k * TUBE_SIDES + s, k * TUBE_SIDES + (s + 1) % TUBE_SIDES
            tris += [(a, b, b + TUBE_SIDES), (a, b + TUBE_SIDES, a + TUBE_SIDES)]
    return points, np.array(tris)


def scene():
    """(positions, triangles, part names, each triangle's part)."""
    t = np.linspace(-1, 1, CABLE_KNOTS)
    span = POST_X - CABLE_GAP
    cable = np.stack([t * span, POST_HEIGHT - CABLE_SAG * (1 - t * t), np.zeros_like(t)], axis=1)
    parts = [("post", box((POST_WIDTH, POST_HEIGHT, POST_WIDTH), (-POST_X, POST_HEIGHT / 2, 0))),
             ("post", box((POST_WIDTH, POST_HEIGHT, POST_WIDTH), (POST_X, POST_HEIGHT / 2, 0))),
             ("cable", tube(cable, CABLE_RADIUS)),
             ("block", box(BLOCK, (0, BLOCK[1] / 2, -1.5))),
             ("rail on the block", box(RAIL, (0, BLOCK[1] + RAIL[1] / 2, -1.5)))]
    positions, triangles, owner = [], [], []
    for index, (_, (points, tris)) in enumerate(parts):
        triangles.append(tris + sum(len(p) for p in positions))
        positions.append(points)
        owner += [index] * len(tris)
    return np.concatenate(positions), np.concatenate(triangles), [name for name, _ in parts], np.array(owner)


def detection_figure(output):
    from matplotlib.patches import Patch
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    positions, triangles, names, owner = scene()
    curve = np.zeros(len(triangles), dtype=bool)
    for part in thin_parts_at(positions, triangles, BUDGET):
        curve[part.triangles] = True
    # An object the detector takes when it stands alone, but not in the scene, rests on a surface.
    side = budget_side(positions, triangles, BUDGET)
    resting = np.zeros(len(triangles), dtype=bool)
    for index in np.unique(owner):
        rows = np.flatnonzero(owner == index)
        if find_thin_parts(positions, triangles[rows], side) and not curve[rows].any():
            resting[rows] = True
    colours = np.where(curve, CURVE_COLOUR, np.where(resting, RESTING_COLOUR, MESH_COLOUR))
    fig = __import__("matplotlib.pyplot", fromlist=["figure"]).figure(figsize=GIF_SIZE)
    axis = fig.add_subplot(projection="3d")
    axis.add_collection3d(Poly3DCollection(positions[triangles], facecolors=colours,
                                           edgecolor=FAINT, linewidths=0.2, alpha=FACE_ALPHA))
    low, high = positions.min(axis=0), positions.max(axis=0)
    for setter, a, b in zip((axis.set_xlim, axis.set_ylim, axis.set_zlim), low, high):
        setter(a, b)
    axis.set_box_aspect(high - low)
    axis.set_axis_off()
    def named(mask):
        return ", ".join(sorted({names[i] for i in np.unique(owner[mask])}))

    fig.legend(handles=[Patch(color=CURVE_COLOUR, label="curve: " + named(curve)),
                        Patch(color=RESTING_COLOUR, label="thin, but rests on a surface: mesh: " + named(resting)),
                        Patch(color=MESH_COLOUR, label="mesh: " + named(~curve & ~resting))],
               loc="lower center", fontsize=LABEL_SIZE, frameon=False, labelcolor=INK)
    fig.suptitle("What geometry.curves takes out of the start")

    def draw(frame):
        axis.view_init(elev=VIEW_ELEVATION, azim=ORBIT_AZIMUTH + ORBIT_TURN * frame / GIF_FRAMES, vertical_axis="y")

    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS)


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    ribbon_figure(data, out / "curves-ribbon.png")
    detection_figure(out / "curves-detection.gif")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

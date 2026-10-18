#!/usr/bin/env python3
"""docs/math/README.md's figures: one shape through scale, rotate and translate,
and one camera point on its way to a pixel. launcher/tools/render/render_doc_images.sh
runs it; the numbers come from math_figures_probe.c, which calls the library.

    python launcher/tools/math/math_figures.py --cc CC --trs PNG --pixel PNG

--trs gets the scale, rotate, translate panels; --pixel the camera point's way
to a pixel. The viewport is the panel's, read from the board's display header.
"""

import argparse
import json
import math
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define
from build.packages import include_roots

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

PROBE = ROOT / "launcher/tools/math/math_figures_probe.c"
PROBE_SOURCES = (PROBE, ROOT / "launcher/main/render/render_view.c")
DISPLAY = ROOT / "launcher/components/esp32_s3_touch_amoled_1_8/include/bsp/display.h"

INK, MUTED, FAINT = "#0b0b0b", "#52514e", "#c9c7c2"
SHAPE_FILL, POINT, PIXEL_FILL = "#2a78d6", "#eb6834", "#1baf7a"
TRS_MARGIN = 0  # units of room beyond the farthest corner in the 3D panels
TRS_PANEL_SIZE = 3.2  # the 3D panels need more room
PANEL_SIZE = 3.0  # inches per panel, wide and high
PANEL_TITLE_ROOM = 0.3  # inches above each row for the panel titles
VIEW_ELEVATION, VIEW_AZIMUTH = 25, -60  # degrees, the same for every panel
DPI = 110
TITLE_SIZE = 10
LABEL_SIZE = 8
SHAPE_ALPHA = 0.3
GHOST_WIDTH = 1.0  # the previous panel's dashed outline
POINT_SIZE = 40
PIXEL_PANELS = 4  # camera, offset, picture, whole pixel
PIXEL_WINDOW = 3  # whole pixels shown on each side of the chosen one


def probe(cc, width, height):
    """Build and run the probe; its JSON is every number the figures show."""
    with tempfile.TemporaryDirectory() as work:
        binary = pathlib.Path(work) / "math_figures_probe"
        includes = [flag for root in include_roots() for flag in ("-I", str(root))]
        subprocess.run([cc, "-std=gnu2x", "-O1", *includes, *map(str, PROBE_SOURCES), "-lm", "-o", str(binary)],
                       check=True)
        out = subprocess.run([str(binary), str(width), str(height)], check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def square_axes(axis, title):
    axis.set_title(title, fontsize=TITLE_SIZE, color=INK)
    axis.set_aspect("equal")
    axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
    for spine in axis.spines.values():
        spine.set_color(FAINT)


def trs_figure(trs, output):
    stages = (("local", "local"), ("scaled", "1. scale {sx:g} x {sy:g} x {sz:g}"),
              ("rotated", "2. rotate {angle:g} deg about ({ax:.2f}, {ay:.2f}, {az:.2f})"),
              ("placed", "3. translate to ({tx:g}, {ty:g}, {tz:g})"))
    values = dict(zip(("sx", "sy", "sz"), trs["scale"]), angle=trs["angle_degrees"],
                  **dict(zip(("ax", "ay", "az"), trs["axis"])), **dict(zip(("tx", "ty", "tz"), trs["position"])))
    corners = [p for name, _ in stages for p in trs[name]]
    high = math.ceil(max(abs(v) for c in corners for v in c)) + TRS_MARGIN
    low = -high
    count = trs["outline_corners"]
    size = (TRS_PANEL_SIZE * len(stages), TRS_PANEL_SIZE + PANEL_TITLE_ROOM)
    figure = plt.figure(figsize=size, layout="constrained")
    previous = None
    for index, (name, title) in enumerate(stages):
        axis = figure.add_subplot(1, len(stages), index + 1, projection="3d")
        axis.set_title(title.format(**values), fontsize=TITLE_SIZE, color=INK)
        axis.set_xlim(low, high)
        axis.set_ylim(low, high)
        axis.set_zlim(low, high)
        axis.set_box_aspect((1, 1, 1))
        axis.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH)
        axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
        axis.set_xticks(range(low, high + 1))
        axis.set_yticks(range(low, high + 1))
        axis.set_zticks(range(low, high + 1))
        for label, direction in zip("xyz", ((1, 0, 0), (0, 1, 0), (0, 0, 1))):
            end = [high * d for d in direction]
            axis.plot([-end[0], end[0]], [-end[1], end[1]], [-end[2], end[2]], color=MUTED, linewidth=1)
            axis.text(*[(high + 0.2) * d for d in direction], label, fontsize=LABEL_SIZE, color=MUTED)
        if previous is not None:
            for ring in prism_edges(previous, count):
                axis.plot(*zip(*ring), color=INK, linestyle="--", linewidth=GHOST_WIDTH)
        axis.add_collection3d(Poly3DCollection(prism_faces(trs[name], count), facecolor=SHAPE_FILL,
                                               alpha=SHAPE_ALPHA, edgecolor=SHAPE_FILL))
        previous = trs[name]
    figure.savefig(output, dpi=DPI, metadata={"Software": None})
    plt.close(figure)


def prism_faces(points, count):
    """The block's faces: both L outlines and one quad per side."""
    back, front = points[:count], points[count:]
    sides = [[back[i], back[(i + 1) % count], front[(i + 1) % count], front[i]] for i in range(count)]
    return [back, front, *sides]


def prism_edges(points, count):
    """The block's edges as polylines: both outlines and the joins between them."""
    back, front = points[:count], points[count:]
    return [back + back[:1], front + front[:1], *([back[i], front[i]] for i in range(count))]


def pixel_figure(px, output):
    width, height = px["viewport"]
    cx, cy = px["cx"], px["cy"]
    x, y, z = px["camera"]
    ox, oy = px["offset"]
    sx, sy = px["screen"]
    pixel_x, pixel_y = px["pixel"]
    size = (PANEL_SIZE * PIXEL_PANELS, PANEL_SIZE + PANEL_TITLE_ROOM)
    figure, axes = plt.subplots(1, PIXEL_PANELS, figsize=size, layout="constrained")

    # Camera space, seen from above: the point and the ray back to the camera.
    axis = axes[0]
    square_axes(axis, f"camera ({x:g}, {y:g}, {z:g})")
    half_width = z * (width - cx) / px["fx"]
    axis.plot([0, -half_width], [0, z], color=FAINT)
    axis.plot([0, half_width], [0, z], color=FAINT)
    axis.axhline(px["near_z"], color=MUTED, linestyle=":", linewidth=0.8)
    axis.text(half_width, px["near_z"], " near_z", fontsize=LABEL_SIZE, color=MUTED, va="bottom", ha="right")
    axis.plot([0, x], [0, z], color=POINT, linewidth=0.8)
    axis.scatter([x], [z], s=POINT_SIZE, color=POINT, zorder=3)
    axis.set_xlabel("x, +x right", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel("z, forward", fontsize=LABEL_SIZE, color=MUTED)

    # projectf_offset: pixels from the optical centre, +y up.
    axis = axes[1]
    square_axes(axis, f"projectf_offset ({ox:g}, {oy:g})")
    axis.add_patch(Rectangle((-cx, -(height - cy)), width, height, fill=False, edgecolor=FAINT))
    axis.axhline(0, color=FAINT, linewidth=0.8)
    axis.axvline(0, color=FAINT, linewidth=0.8)
    axis.annotate("", (ox, oy), (0, 0), arrowprops=dict(arrowstyle="->", color=POINT))
    axis.scatter([ox], [oy], s=POINT_SIZE, color=POINT, zorder=3)
    axis.set_xlim(-cx, width - cx)
    axis.set_ylim(-(height - cy), cy)
    axis.set_xlabel(f"fx x / z, fx = {px['fx']:g}", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel("fy y / z, +y up", fontsize=LABEL_SIZE, color=MUTED)

    # projectf_screen: the picture, y down from the top-left corner.
    axis = axes[2]
    square_axes(axis, f"projectf_screen ({sx:g}, {sy:g})")
    axis.add_patch(Rectangle((0, 0), width, height, fill=False, edgecolor=MUTED))
    axis.scatter([cx], [cy], s=POINT_SIZE / 2, marker="+", color=MUTED)
    axis.text(cx, cy, f" centre ({cx:g}, {cy:g})", fontsize=LABEL_SIZE, color=MUTED, va="top")
    axis.annotate("", (sx, sy), (cx, cy), arrowprops=dict(arrowstyle="->", color=POINT))
    axis.scatter([sx], [sy], s=POINT_SIZE, color=POINT, zorder=3)
    axis.set_xlim(0, width)
    axis.set_ylim(height, 0)
    axis.set_xlabel("x, right", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel("y, down", fontsize=LABEL_SIZE, color=MUTED)

    # The caller's snap: each offset truncated toward the centre, then the flip.
    axis = axes[3]
    square_axes(axis, f"whole pixel ({pixel_x}, {pixel_y})")
    left, top = pixel_x - PIXEL_WINDOW, pixel_y - PIXEL_WINDOW
    span = 2 * PIXEL_WINDOW + 1
    for column in range(span):
        for row in range(span):
            axis.add_patch(Rectangle((left + column, top + row), 1, 1, fill=False, edgecolor=FAINT, linewidth=0.6))
    axis.add_patch(Rectangle((pixel_x, pixel_y), 1, 1, facecolor=PIXEL_FILL, alpha=SHAPE_ALPHA, edgecolor=PIXEL_FILL))
    axis.scatter([sx], [sy], s=POINT_SIZE, color=POINT, zorder=3)
    axis.set_xlim(left, left + span)
    axis.set_ylim(top + span, top)
    axis.set_xlabel(f"cx + trunc({ox:g})", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel(f"cy - trunc({oy:g})", fontsize=LABEL_SIZE, color=MUTED)

    figure.savefig(output, dpi=DPI, metadata={"Software": None})
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", required=True, help="host C compiler")
    parser.add_argument("--trs", required=True, type=pathlib.Path, help="PNG for the TRS panels")
    parser.add_argument("--pixel", required=True, type=pathlib.Path, help="PNG for the camera-to-pixel panels")
    args = parser.parse_args()
    width = read_define(DISPLAY, "BSP_LCD_H_RES")
    height = read_define(DISPLAY, "BSP_LCD_V_RES")
    numbers = probe(args.cc, width, height)
    trs_figure(numbers["trs"], args.trs)
    pixel_figure(numbers["pixel"], args.pixel)
    return 0


if __name__ == "__main__":
    sys.exit(main())

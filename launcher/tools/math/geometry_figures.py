#!/usr/bin/env python3
"""Plot library-derived geometry examples for docs/math/Geometry.md.

    python launcher/tools/math/geometry_figures.py --cc CC --out DIR
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import (PANEL_SIZE, PANEL_TITLE_ROOM, PIXEL_FILL, POINT, POINT_SIZE, SHAPE_ALPHA, SHAPE_FILL,
                          run_probe)
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, TITLE_SIZE, style_axes, figure as new_figure, save
import numpy as np
from matplotlib.patches import Rectangle
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

PROBE = pathlib.Path(__file__).with_name("geometry_figures_probe.c")
PLANE_NAMES = ("near", "left", "right", "top", "bottom")
PLANE_COLORS = (INK, SHAPE_FILL, POINT, "#8655ad", PIXEL_FILL)
PANELS = 2
BAND_COUNT = 9
NORMAL_ARROW_LENGTH = 0.8
INSIDE_SHADE_DEPTH = 0.6
RECT_LABEL_OFFSET = (4, -13)


def panels(projection=None):
    figure = new_figure((PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    axes = [figure.add_subplot(1, PANELS, i + 1, projection=projection) for i in range(PANELS)]
    return figure, axes


def rect_figure(data, output):
    figure, (axis, empty_axis) = panels()
    style_axes(axis, "Half-open pixel rectangles", square=True)
    for key, color, fill, style in (("a", SHAPE_FILL, False, "-"), ("b", POINT, False, "-"),
                                     ("intersect", PIXEL_FILL, True, "-"), ("union", MUTED, False, "--")):
        x, y, w, h = data[key]
        axis.add_patch(Rectangle((x, y), w, h, facecolor=color if fill else "none",
                                 edgecolor=color, alpha=SHAPE_ALPHA if fill else 1, linestyle=style,
                                 label=key))
    for x, y, inside in data["samples"]:
        axis.scatter(x, y, color=PIXEL_FILL if inside else POINT, marker="o" if inside else "x",
                     s=POINT_SIZE, zorder=3)
        axis.annotate(f"({x}, {y}) {'in' if inside else 'out'}", (x, y), xytext=RECT_LABEL_OFFSET,
                      textcoords="offset points", fontsize=LABEL_SIZE)
    x, y, w, h = data["union"]
    axis.set_xticks(range(x, x + w + 1))
    axis.set_yticks(range(y, y + h + 1))
    axis.set_xlim(x - 1, x + w + 1)
    axis.set_ylim(y + h + 1, y - 1)
    axis.grid(color=FAINT, linewidth=0.5)
    axis.legend(fontsize=LABEL_SIZE, loc="upper left", bbox_to_anchor=(0, -0.13), ncol=2)
    style_axes(empty_axis, "Crossed edges", square=True)
    left, top, right, bottom = data["edges"]
    empty_axis.plot([left, right], [top, bottom], "--", color=MUTED)
    empty_axis.scatter([left, right], [top, bottom], color=POINT)
    empty_axis.annotate(f"left, top ({left}, {top})", (left, top), xytext=(-4, -15),
                        textcoords="offset points", ha="right", fontsize=LABEL_SIZE)
    empty_axis.annotate(f"right, bottom ({right}, {bottom})", (right, bottom), xytext=(4, 8),
                        textcoords="offset points", fontsize=LABEL_SIZE)
    empty_axis.margins(0.8)
    empty_axis.invert_yaxis()
    empty_axis.set_xlabel(f"from_edges = {tuple(data['crossed'])}; empty = {data['empty']}",
                         fontsize=LABEL_SIZE)
    save(figure, output)


def distance_figure(data, output):
    figure, axes = panels()
    grid = np.array(data["grid"])
    x, z = np.unique(grid[:, 0]), np.unique(grid[:, 1])
    values = [grid[:, i].reshape(len(z), len(x)) for i in (2, 3)]
    span = min(abs(v).max() for v in values)
    levels = np.linspace(-span, span, BAND_COUNT)
    for axis, value, title in zip(axes, values, ("Before normalize", "After normalize")):
        style_axes(axis, title, square=True)
        bands = axis.contourf(x, z, value, levels=levels, cmap="RdBu", extend="both")
        axis.contour(x, z, value, levels=[0], colors=INK)
        axis.set_xlabel("x", fontsize=LABEL_SIZE)
        axis.set_ylabel("z", fontsize=LABEL_SIZE)
    figure.colorbar(bands, ax=axes, label="planef_distance", shrink=0.7)
    figure.suptitle(f"Same zero line; normal length = {data['normal_length']:.3g}", fontsize=TITLE_SIZE)
    save(figure, output)


def space_axes(axis, title):
    axis.set_title(title, fontsize=TITLE_SIZE)
    axis.set_xlabel("x", fontsize=LABEL_SIZE, labelpad=0)
    axis.set_ylabel("z", fontsize=LABEL_SIZE, labelpad=0)
    axis.set_zlabel("y", fontsize=LABEL_SIZE, labelpad=0)
    axis.tick_params(labelsize=LABEL_SIZE, pad=0)
    axis.view_init(elev=24, azim=-60)


def points_figure(data, output):
    figure, axes = panels("3d")
    points = np.array(data["points"])[:, [0, 2, 1]]
    centroid = np.array(data["centroid"])[[0, 2, 1]]
    for axis, normal, order, title in zip(axes, data["normals"], ((0, 1, 2), (0, 2, 1)),
                                          ("from_points(a, b, c)", "from_points(a, c, b)")):
        normal = np.array(normal, dtype=float)[[0, 2, 1]]
        space_axes(axis, title + f"\nnormal = ({normal[0]:g}, {normal[2]:g}, {normal[1]:g})")
        normal /= np.linalg.norm(normal)
        axis.add_collection3d(Poly3DCollection([points], facecolor=SHAPE_FILL, alpha=SHAPE_ALPHA))
        inside = points + normal * INSIDE_SHADE_DEPTH
        axis.add_collection3d(Poly3DCollection([inside], facecolor=PIXEL_FILL, alpha=SHAPE_ALPHA))
        for i, j in zip(order, (*order[1:], order[0])):
            start, delta = points[i], (points[j] - points[i])
            axis.quiver(*start, *delta, color=POINT, arrow_length_ratio=0.15)
        axis.quiver(*centroid, *normal, color=INK, length=NORMAL_ARROW_LENGTH)
        axis.text(*inside[0], "inside", fontsize=LABEL_SIZE, color=PIXEL_FILL)
        for name, point in zip("abc", points):
            axis.text(*point, name, fontsize=LABEL_SIZE)
        bound = np.vstack([points, inside, centroid + normal])
        axis.auto_scale_xyz(bound[:, 0], bound[:, 1], bound[:, 2])
        axis.set_zlim(-NORMAL_ARROW_LENGTH * 2, NORMAL_ARROW_LENGTH * 2)
    save(figure, output)


def zero_line(axis, plane, fixed_y, color, label=None):
    a, b, c, d = plane
    xmin, xmax = axis.get_xlim()
    zmin, zmax = axis.get_ylim()
    candidates = []
    if abs(c) > np.finfo(float).eps:
        candidates.extend((x, -(a * x + b * fixed_y + d) / c) for x in (xmin, xmax))
    if abs(a) > np.finfo(float).eps:
        candidates.extend((-(c * z + b * fixed_y + d) / a, z) for z in (zmin, zmax))
    points = np.array([(x, z) for x, z in candidates if xmin <= x <= xmax and zmin <= z <= zmax])
    if len(points) < 2:
        return
    axis.plot(points[:, 0], points[:, 1], color=color, label=label)
    middle = points.mean(axis=0)
    normal = np.array([a, c], dtype=float)
    normal *= NORMAL_ARROW_LENGTH / np.linalg.norm(normal)
    axis.annotate("", middle + normal, middle, arrowprops=dict(arrowstyle="->", color=color))


def rows_figure(data, output):
    figure, axes = panels()
    for axis, key, title, distance in zip(axes, ("lens", "world"), ("Lens space", "World space"), data["distances"]):
        point = np.array(data[key + "_sample"])
        plane = data[key + "_plane"]
        style_axes(axis, title, square=True)
        radius = np.linalg.norm(point) + NORMAL_ARROW_LENGTH
        axis.set_xlim(-radius, radius)
        axis.set_ylim(-radius, radius)
        if key == "lens":
            depth_max = radius
            side_span = abs(plane[2] / plane[0]) * depth_max
            axis.set_xlim(-side_span, side_span)
            axis.set_aspect("auto")
            axis.set_box_aspect(1)
        zero_line(axis, plane, point[1], SHAPE_FILL)
        axis.scatter(point[0], point[2], color=POINT, s=POINT_SIZE)
        axis.set_xlabel("x, lens units (pixels times depth)" if key == "lens" else "x", fontsize=LABEL_SIZE)
        axis.set_ylabel("z, depth" if key == "lens" else "z", fontsize=LABEL_SIZE)
        axis.text(0.03, 0.97, f"left plane\ny = {point[1]:.3g}\ndistance = {distance:g}",
                  transform=axis.transAxes, va="top", fontsize=LABEL_SIZE, bbox=dict(facecolor="white", alpha=0.8, edgecolor="none"))
    save(figure, output)


def frustum_figure(data, output):
    figure = new_figure((PANEL_SIZE * PANELS * 1.3, PANEL_SIZE * 1.3 + PANEL_TITLE_ROOM))
    axis = figure.add_subplot(1, PANELS, 1, projection="3d")
    slice_axis = figure.add_subplot(1, PANELS, 2)
    space_axes(axis, f"Frustum; shown to depth {data['shown_depth']:g}")
    corners = np.array(data["corners"])[:, [0, 2, 1]]
    near, distant = corners[:4], corners[4:]
    faces = [near]
    for i in range(len(near)):
        following = (i + 1) % len(near)
        faces.append([near[i], near[following], distant[following], distant[i]])
    axis.add_collection3d(Poly3DCollection(faces, facecolor=SHAPE_FILL, edgecolor=SHAPE_FILL,
                                         alpha=SHAPE_ALPHA))
    camera = np.array(data["camera"])[[0, 2, 1]]
    for corner in near:
        axis.plot(*np.array([camera, corner]).T, color=MUTED, alpha=SHAPE_ALPHA)
    axis.scatter(*camera, color=POINT, s=POINT_SIZE)
    axis.text(*camera, " camera", fontsize=LABEL_SIZE)
    grid = data["grid"]
    x, z = np.array([(p[0], p[1]) for p in grid]).T
    colors = [PIXEL_FILL if p[3] else FAINT for p in grid]
    axis.scatter(x, z, np.zeros_like(x), c=colors, s=6, depthshade=False)
    bounds = np.vstack([corners, camera, np.column_stack([x, z, np.zeros_like(x)])])
    for setter, dim in ((axis.set_xlim, 0), (axis.set_ylim, 1), (axis.set_zlim, 2)):
        setter(bounds[:, dim].min(), bounds[:, dim].max())
    style_axes(slice_axis, "slice y = 0 (ground)", square=True)
    slice_axis.scatter(x, z, c=colors, s=8)
    slice_axis.set_xlim(x.min(), x.max())
    slice_axis.set_ylim(z.min(), z.max())
    for plane, name, color in zip(data["planes"], PLANE_NAMES, PLANE_COLORS):
        zero_line(slice_axis, plane, 0, color, name)
    slice_axis.legend(fontsize=LABEL_SIZE, loc="lower right")
    slice_axis.set_xlabel("x", fontsize=LABEL_SIZE)
    slice_axis.set_ylabel("z", fontsize=LABEL_SIZE)
    save(figure, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = run_probe(args.cc, [PROBE])
    for name, key, plot in (("rect-queries", "rect", rect_figure),
                            ("plane-distance", "distance", distance_figure),
                            ("plane-from-points", "triangle", points_figure),
                            ("plane-from-rows", "rows", rows_figure),
                            ("frustum", "frustum", frustum_figure)):
        plot(data[key], args.out / (name + ".png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

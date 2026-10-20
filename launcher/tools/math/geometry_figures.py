#!/usr/bin/env python3
"""Plot library-derived geometry examples for docs/math/Geometry.md.

    python launcher/tools/math/geometry_figures.py --cc CC --out DIR
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import (PANEL_SIZE, PANEL_TITLE_ROOM, PIXEL_FILL, POINT, POINT_SIZE, SHAPE_ALPHA, SHAPE_FILL,
                          run_probe, square_axes, VIEW_ELEVATION, prism_faces)
from docs.plot_style import (FAINT, INK, LABEL_SIZE, MUTED, TITLE_SIZE, SLOTS,
                             GIF_FRAMES, GIF_FPS, ORBIT_GIF_COLORS, wire_sphere,
                             figure as new_figure, save, save_gif)
import numpy as np
from matplotlib.patches import Patch, Rectangle
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d import proj3d
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

PROBE = pathlib.Path(__file__).with_name("geometry_figures_probe.c")
PLANE_NAMES = ("near", "left", "right", "top", "bottom")
PLANE_COLORS = (INK, SHAPE_FILL, POINT, SLOTS[6], PIXEL_FILL)
PANELS = 2
NORMAL_ARROW_LENGTH = 0.8
RECT_LABEL_OFFSET = (4, -13)


def panels(projection=None):
    figure = new_figure((PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    axes = [figure.add_subplot(1, PANELS, i + 1, projection=projection) for i in range(PANELS)]
    return figure, axes


def rect_figure(data, output):
    figure, (axis, empty_axis) = panels()
    square_axes(axis, "Half-open pixel rectangles")
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
    square_axes(empty_axis, "Crossed edges")
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


ORBIT_AZIMUTH = -85
ORBIT_TURN = 360
ANIMATION_DPI = 80
FIGURE_WIDTH = 10
FIGURE_HEIGHT = 5.8
AXIS_MARGIN = 1.08
FACE_ALPHA = 0.14
ARROW_RATIO = 0.2
GRID_WIDTH = 0.4
GRID_STEPS = 6
LABEL_OFFSET = (5, 5)
STILL_QUARTERS = 4
PLANE_AZIMUTH = -125
LABEL_CLEARANCE = 10
POINTS_PER_INCH = 72
LABEL_LEADER_WIDTH = 0.5
NORMAL_DEPTH_RATIO = 0.25
SAMPLE_SCALE = 2
NORMAL_COLORS = (SLOTS[0], SLOTS[7])
LABEL_ALPHA = 0.75
LABEL_PADDING = 0.3
AXIS_ZOOM = 1.03
SUBPLOT_RIGHT = 0.98
SUBPLOT_BOTTOM = 0.12
SUBPLOT_TOP = 0.9
SUBPLOT_GAP = 0.08
BOX_COLORS = (SLOTS[5], SLOTS[3], MUTED)
SAMPLE_COLORS = (SLOTS[1], MUTED, SLOTS[5])
BOX_RING = ((0, 0), (1, 0), (1, 1), (0, 1))
FRUSTUM_FACE_ORDER = (0, 5, 3, 2, 4)
LEGEND_Y = 0.015


def space_panels(titles):
    fig = new_figure((FIGURE_WIDTH if len(titles) > 1 else FIGURE_HEIGHT, FIGURE_HEIGHT))
    fig.set_dpi(ANIMATION_DPI)
    axes = [fig.add_subplot(1, len(titles), i + 1, projection="3d")
            for i in range(len(titles))]
    for axis, title in zip(axes, titles):
        axis.set_title(title, fontsize=TITLE_SIZE)
        axis.set_axis_off()
        axis.view_init(elev=VIEW_ELEVATION, azim=ORBIT_AZIMUTH, vertical_axis="y")
    fig.set_layout_engine(None)
    fig.subplots_adjust(left=0, right=SUBPLOT_RIGHT, bottom=SUBPLOT_BOTTOM,
                        top=SUBPLOT_TOP, wspace=SUBPLOT_GAP)
    return fig, axes


def fit_space(axis, points):
    points = np.asarray(points)
    low, high = points.min(axis=0), points.max(axis=0)
    centre = (low + high) / 2
    extent = np.maximum(high - low, max(high - low) / GRID_STEPS) * AXIS_MARGIN
    low, high = centre - extent / 2, centre + extent / 2
    for setter, lower, upper in zip((axis.set_xlim, axis.set_ylim, axis.set_zlim), low, high):
        setter(lower, upper)
    axis.set_box_aspect(extent, zoom=AXIS_ZOOM)
    for x in np.linspace(low[0], high[0], GRID_STEPS):
        axis.plot([x, x], [low[1], low[1]], [low[2], high[2]], color=FAINT,
                  alpha=FACE_ALPHA, linewidth=GRID_WIDTH)
    for z in np.linspace(low[2], high[2], GRID_STEPS):
        axis.plot([low[0], high[0]], [low[1], low[1]], [z, z], color=FAINT,
                  alpha=FACE_ALPHA, linewidth=GRID_WIDTH)


def sheet(axis, vertices, color=SHAPE_FILL):
    axis.add_collection3d(Poly3DCollection([vertices], facecolor=color, edgecolor=color,
                                         alpha=FACE_ALPHA))


def arrow(axis, origin, normal, color=INK, length=NORMAL_ARROW_LENGTH):
    normal = np.asarray(normal, dtype=float)
    normal *= length / np.linalg.norm(normal)
    axis.quiver(*origin, *normal, color=color, arrow_length_ratio=ARROW_RATIO)
    return np.asarray(origin) + normal


def legend(fig, entries, patches=False):
    handles = [Patch(facecolor=color, edgecolor=color, alpha=FACE_ALPHA, label=label)
               if patches else Line2D([], [], color=color, marker=marker, linestyle=style, label=label)
               for label, color, marker, style in entries]
    fig.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, LEGEND_Y),
               fontsize=LABEL_SIZE, ncol=PANELS, frameon=False)


def orbit(fig, axes, output, stills, labels=()):
    annotations = []

    def draw(frame):
        for annotation in annotations:
            annotation.remove()
        annotations.clear()
        for axis in axes:
            axis.view_init(elev=VIEW_ELEVATION,
                           azim=(PLANE_AZIMUTH if output.suffix == ".png" else
                                 ORBIT_AZIMUTH + ORBIT_TURN * frame / GIF_FRAMES),
                           vertical_axis="y")
        fig.canvas.draw()
        renderer = fig.canvas.get_renderer()
        for axis in axes:
            placed = []
            for owner, points, texts, *clearance in labels:
                if owner is not axis:
                    continue
                projected = [proj3d.proj_transform(*point, axis.get_proj())[:2] for point in points]
                for point, text in zip(projected, texts):
                    offset = list(LABEL_OFFSET)
                    align = "left"
                    if clearance:
                        outline = np.array([proj3d.proj_transform(*vertex, axis.get_proj())[:2]
                                            for vertex in clearance[0]])
                        low, high = axis.transData.transform(outline).min(axis=0), axis.transData.transform(outline).max(axis=0)
                        anchor = axis.transData.transform(point)
                        on_right = anchor[0] >= (low[0] + high[0]) / 2
                        edge = high[0] + LABEL_CLEARANCE if on_right else low[0] - LABEL_CLEARANCE
                        offset[0] = (edge - anchor[0]) * POINTS_PER_INCH / fig.dpi
                        align = "left" if on_right else "right"
                    annotation = axis.annotate(text, point, xytext=tuple(offset),
                        textcoords="offset points", fontsize=LABEL_SIZE, color=INK,
                        va="bottom", ha=align, annotation_clip=False,
                        arrowprops=dict(arrowstyle="-", color=MUTED, linewidth=LABEL_LEADER_WIDTH),
                        bbox=dict(facecolor="white", edgecolor="none", alpha=LABEL_ALPHA, pad=LABEL_PADDING))
                    for attempt in range(len(placed) + 1):
                        annotation.update_positions(renderer)
                        annotation.update_bbox_position_size(renderer)
                        bounds = annotation.get_bbox_patch().get_window_extent(renderer).padded(LABEL_CLEARANCE / 2)
                        overlaps = [other for other in placed if bounds.overlaps(other)]
                        if not overlaps:
                            break
                        shift = max(other.y1 for other in overlaps) - bounds.y0 + LABEL_CLEARANCE
                        offset[1] += shift * POINTS_PER_INCH / fig.dpi
                        annotation.set_position(tuple(offset))
                    placed.append(bounds)
                    annotations.append(annotation)

    if output.suffix == ".png":
        draw(0)
        save(fig, output, dpi=ANIMATION_DPI)
        return

    if stills is not None:
        stills.mkdir(parents=True, exist_ok=True)
        for quarter in range(STILL_QUARTERS):
            draw(quarter * GIF_FRAMES // STILL_QUARTERS)
            fig.savefig(stills / f"{output.stem}-f{quarter}.png", dpi=ANIMATION_DPI,
                        facecolor="white", metadata={"Software": None})
    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def distance_figure(data, output, stills):
    fig, axes = space_panels(("Before normalize", "After normalize"))
    points = [sample[0] for sample in data["samples"]]
    labels = []
    for column, axis in enumerate(axes):
        sheet(axis, data["sheet"])
        for vertices, value in zip(data["level_sheets"][column], (-1, 1)):
            sheet(axis, vertices, SAMPLE_COLORS[value + 1])
            labels.append((axis, [vertices[0 if value < 0 else 2]], [f"value {value:+d}"]))
        arrow(axis, data["point"], data["planes"][column][:3])
        for point, *values in data["samples"]:
            value = values[column]
            axis.scatter(*point, color=SAMPLE_COLORS[int(np.sign(value)) + 1], s=POINT_SIZE,
                         depthshade=False)
        fit_space(axis, [*data["sheet"], *points,
                         *np.asarray(data["level_sheets"][column]).reshape(-1, 3)])
        labels.append((axis, points, [f"{sample[column + 1]:.3g}" for sample in data["samples"]]))
    legend(fig, [("Zero sheet", SHAPE_FILL, "", "-"),
                 ("Value −1 sheet", SAMPLE_COLORS[0], "", "-"),
                 ("Value +1 sheet", SAMPLE_COLORS[2], "", "-")], patches=True)
    orbit(fig, axes, output, stills, labels)


def points_figure(data, output, stills):
    fig, axes = space_panels(("Winding a → b → c", "Winding a → c → b"))
    points = np.array(data["points"])
    labels = []
    for axis, normal, order, color in zip(axes, data["normals"], ((0, 1, 2), (0, 2, 1)), NORMAL_COLORS):
        sheet(axis, points)
        for first, second in zip(order, (*order[1:], order[0])):
            axis.quiver(*points[first], *(points[second] - points[first]), color=POINT,
                        arrow_length_ratio=ARROW_RATIO)
        tip = arrow(axis, data["centroid"], normal, color)
        fit_space(axis, [*points, tip])
        labels.append((axis, points, list("abc")))
    legend(fig, [(f"normal {tuple(normal)}", color, "", "-")
                 for normal, color in zip(data["normals"], NORMAL_COLORS)])
    orbit(fig, axes, output, stills, labels)


def rows_figure(data, output, stills):
    titles = [f"{space.title()} space: signed value {distance:g}"
              for space, distance in zip(("lens", "world"), data["distances"])]
    fig, axes = space_panels(titles)
    labels = []
    for axis, key, vertices, foot, value in zip(axes, ("lens", "world"), data["sheets"],
                                                data["feet"], data["distances"]):
        point = data[key + "_sample"]
        sheet(axis, vertices)
        arrow(axis, np.mean(vertices, axis=0), data[key + "_plane"][:3])
        axis.scatter(*point, color=POINT, s=POINT_SIZE, depthshade=False)
        axis.plot(*np.array([point, foot]).T, color=POINT, linestyle="--")
        labels.append((axis, [np.mean([point, foot], axis=0)], [f"signed value = {value:g}"]))
        fit_space(axis, [*vertices, point, foot])
    legend(fig, [("Translucent sheet: plane; arrow: normal", SHAPE_FILL, "", "-"),
                 ("Corresponding sample", POINT, "o", "")])
    orbit(fig, axes, output, stills, labels)


def draw_frustum(axis, data, normals=True, labels=None):
    corners = np.asarray(data["corners"])
    near, distant = corners[:4], corners[4:]
    prism = prism_faces(corners, len(near))
    faces = [np.asarray(prism[index]) for index in FRUSTUM_FACE_ORDER]
    for face, plane, color in zip(faces, data["planes"], PLANE_COLORS):
        sheet(axis, face, color)
        if normals:
            tip = arrow(axis, face.mean(axis=0), plane[:3], color,
                        length=data["shown_depth"] * NORMAL_DEPTH_RATIO)
            if labels is not None:
                labels.append((axis, [tip], [PLANE_NAMES[len(labels)]]))
    axis.plot(*np.vstack([distant, distant[0]]).T, color=MUTED, linestyle="--")
    camera = data["camera"]
    axis.scatter(*camera, color=INK, marker="^", s=POINT_SIZE)
    for point in distant:
        axis.plot(*np.array([camera, point]).T, color=MUTED, linewidth=1)
    return [*corners, camera]


def frustum_figure(data, output, stills):
    fig, (axis,) = space_panels(("Five inward frustum planes",))
    labels = []
    bounds = draw_frustum(axis, data, labels=labels)
    for point, inside in data["samples"]:
        axis.scatter(*point, color=BOX_COLORS[0 if inside else 2], s=POINT_SIZE * SAMPLE_SCALE, depthshade=False)
        labels.append((axis, [point], ["inside" if inside else "outside"]))
        bounds.append(point)
    fit_space(axis, bounds)
    legend(fig, [(name, color, "", "-") for name, color in zip(PLANE_NAMES, PLANE_COLORS)] +
           [("drawn depth (not a plane)", MUTED, "", "--"),
            ("camera", INK, "^", ""), ("inside / outside samples", BOX_COLORS[0], "o", "")])
    orbit(fig, [axis], output, stills, labels)


def draw_box(axis, bounds, color=SHAPE_FILL):
    low, high = np.array(bounds)
    corners = np.array([((low[0], high[0])[x], (low[1], high[1])[y], z)
                        for z in (low[2], high[2]) for x, y in BOX_RING])
    axis.add_collection3d(Poly3DCollection(prism_faces(corners, len(BOX_RING)),
                                         facecolor=color, edgecolor=color, alpha=FACE_ALPHA))
    return corners


def box_frustum_figure(data, output, stills):
    fig, (axis,) = space_panels(("Closed boxes against the same frustum",))
    bounds = draw_frustum(axis, data["frustum"], normals=False)
    centres, texts = [], []
    for index, (box, outside, mask) in enumerate(data["boxes"]):
        color = BOX_COLORS[2 if outside else (1 if mask else 0)]
        bounds.extend(draw_box(axis, box, color))
        centres.append(np.asarray(box)[1])
        texts.append(str(index + 1))
    fit_space(axis, bounds)
    legend(fig, [(label, color, "s", "") for label, color in
                 zip(("inside (mask = 0)", "crossing", "outside"), BOX_COLORS)] +
           [("8: loose corner (not rejected)", BOX_COLORS[1], "", "-")])
    orbit(fig, [axis], output, stills, [(axis, centres, texts)])


def sphere_queries_figure(data, output, stills):
    fig, (left, right) = space_panels(("Sphere / box closest points", "Segment entry parameters"))
    labels = []
    sphere_labels = []
    bounds = list(draw_box(left, data["box"]))
    entries = []
    for (sphere, closest, hit), name, color in zip(data["spheres"], ("overlap", "touch", "miss"), BOX_COLORS):
        centre, radius = sphere
        wire_sphere(left, centre, radius, color)
        left.plot(*np.array([centre, closest]).T, color=color, linestyle="--")
        left.scatter(*closest, color=color, s=POINT_SIZE, depthshade=False)
        bounds.extend([np.array(centre) - radius, np.array(centre) + radius])
        sphere_labels.append((left, [np.asarray(centre) + [0, radius, 0]], [name]))
    fit_space(left, bounds)
    bounds = list(draw_box(right, data["box"]))
    clearance = list(bounds)
    centre, radius = data["segment_sphere"]
    wire_sphere(right, centre, radius, POINT)
    clearance.extend(np.asarray(centre) + radius * np.array([
        (x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]))
    entry_labels = []
    bounds.extend([np.array(centre) - radius, np.array(centre) + radius])
    entries.extend([("Closest point / entry", INK, "o", ""),
                    ("Centre to closest point", MUTED, "", "--"),
                    ("Segment target", POINT, "", "-")])
    for index, (a, c, results) in enumerate(data["segments"]):
        color = SLOTS[index]
        right.plot(*np.array([a, c]).T, color=color,
                   linestyle="-" if any(result[0] for result in results) else "--")
        bounds.extend([a, c])
        for target, (hit, t, point) in enumerate(results):
            if hit:
                right.scatter(*point, color=color, s=POINT_SIZE, depthshade=False)
                entry_labels.append((point, f"t={t:.3g}"))
        labels.append((a, str(index + 1)))
    fit_space(right, bounds)
    legend(fig, entries)
    orbit(fig, [left, right], output, stills,
          [*sphere_labels, (right, [point for point, _ in labels], [text for _, text in labels]),
           (right, [point for point, _ in entry_labels], [text for _, text in entry_labels], clearance)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--stills", type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = run_probe(args.cc, [PROBE])
    rect_figure(data["rect"], args.out / "rect-queries.png")
    data["box_frustum"]["frustum"] = data["frustum"]
    for name, key, plot in (("plane-distance", "distance", distance_figure),
                            ("plane-from-points", "triangle", points_figure),
                            ("plane-from-rows", "rows", rows_figure),
                            ("frustum", "frustum", frustum_figure),
                            ("box3-frustum", "box_frustum", box_frustum_figure),
                            ("box3-sphere-queries", "queries", sphere_queries_figure)):
        extension = ".png" if key in ("distance", "triangle", "rows") else ".gif"
        plot(data[key], args.out / (name + extension), args.stills)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
from matplotlib.patches import Rectangle
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


ORBIT_AZIMUTH = -115
ORBIT_TURN = 360
ANIMATION_DPI = 80
FIGURE_WIDTH = 10
FIGURE_HEIGHT = 5.8
AXIS_MARGIN = 1.15
FACE_ALPHA = 0.14
ARROW_RATIO = 0.2
LEADER_WIDTH = 0.6
AXIS_ZOOM = 1.2
SUBPLOT_RIGHT = 0.80
SUBPLOT_BOTTOM = 0.22
SUBPLOT_TOP = 0.9
SUBPLOT_GAP = 0.3
BOX_COLORS = (SLOTS[5], SLOTS[3], MUTED)
SAMPLE_COLORS = (SLOTS[1], MUTED, SLOTS[5])
BOX_RING = ((0, 0), (1, 0), (1, 1), (0, 1))
FRUSTUM_FACE_ORDER = (0, 5, 3, 2, 4)
CALLOUT_X = 1.02
CALLOUT_TOP = 0.9
CALLOUT_BOTTOM = 0.1
LEGEND_Y = 0.04


def space_panels(titles):
    fig = new_figure((FIGURE_WIDTH, FIGURE_HEIGHT))
    fig.set_dpi(ANIMATION_DPI)
    axes = [fig.add_subplot(1, len(titles), i + 1, projection="3d")
            for i in range(len(titles))]
    for axis, title in zip(axes, titles):
        axis.set_title(title, fontsize=TITLE_SIZE)
        axis.set_axis_off()
    fig.set_layout_engine(None)
    fig.subplots_adjust(left=0, right=SUBPLOT_RIGHT, bottom=SUBPLOT_BOTTOM,
                        top=SUBPLOT_TOP, wspace=SUBPLOT_GAP)
    return fig, axes


def fit_space(axis, points):
    points = np.asarray(points)
    low, high = points.min(axis=0), points.max(axis=0)
    centre = (low + high) / 2
    radius = max(high - low) * AXIS_MARGIN / 2
    for setter, middle in zip((axis.set_xlim, axis.set_ylim, axis.set_zlim), centre):
        setter(middle - radius, middle + radius)
    axis.set_box_aspect((1, 1, 1), zoom=AXIS_ZOOM)


def sheet(axis, vertices, color=SHAPE_FILL):
    axis.add_collection3d(Poly3DCollection([vertices], facecolor=color, edgecolor=color,
                                         alpha=FACE_ALPHA))


def arrow(axis, origin, normal, color=INK):
    normal = np.asarray(normal, dtype=float)
    normal *= NORMAL_ARROW_LENGTH / np.linalg.norm(normal)
    axis.quiver(*origin, *normal, color=color, arrow_length_ratio=ARROW_RATIO)


def legend(fig, entries):
    handles = [Line2D([], [], color=color, marker=marker, linestyle=style, label=label)
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
                           azim=ORBIT_AZIMUTH + ORBIT_TURN * frame / GIF_FRAMES,
                           vertical_axis="y")
        for axis, points, texts in labels:
            projected = [proj3d.proj_transform(*point, axis.get_proj())[:2] for point in points]
            order = np.argsort([point[1] for point in projected])[::-1]
            for index, height in zip(order, np.linspace(CALLOUT_TOP, CALLOUT_BOTTOM, len(order))):
                annotations.append(axis.annotate(texts[index], projected[index],
                    xytext=(CALLOUT_X, height), textcoords="axes fraction", fontsize=LABEL_SIZE,
                    color=INK, va="center", annotation_clip=False,
                    arrowprops=dict(arrowstyle="-", color=MUTED, linewidth=LEADER_WIDTH)))

    draw(0)
    if stills is not None:
        stills.mkdir(parents=True, exist_ok=True)
        fig.savefig(stills / (output.stem + "-still.png"), dpi=ANIMATION_DPI,
                    facecolor="white", metadata={"Software": None})
    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def distance_figure(data, output, stills):
    fig, axes = space_panels(("Before normalize", "After normalize"))
    points = [sample[0] for sample in data["samples"]]
    for column, axis in enumerate(axes):
        sheet(axis, data["sheet"])
        arrow(axis, data["point"], data["planes"][column][:3])
        for point, *values in data["samples"]:
            value = values[column]
            axis.scatter(*point, color=SAMPLE_COLORS[int(np.sign(value)) + 1], s=POINT_SIZE,
                         depthshade=False)
        fit_space(axis, [*data["sheet"], *points])
    entries = [(f"sample {i + 1}: {a:.3g} → {b:.3g}", SAMPLE_COLORS[int(np.sign(a)) + 1], "o", "")
               for i, (_, a, b) in enumerate(data["samples"])]
    entries.append(("Sheet: same zero set; arrow: normal", SHAPE_FILL, "", "-"))
    legend(fig, entries)
    orbit(fig, axes, output, stills,
          [(axis, points, [str(i + 1) for i in range(len(points))]) for axis in axes])


def points_figure(data, output, stills):
    fig, axes = space_panels(("Winding a → b → c", "Winding a → c → b"))
    points = np.array(data["points"])
    labels = []
    for axis, normal, order in zip(axes, data["normals"], ((0, 1, 2), (0, 2, 1))):
        sheet(axis, points)
        for first, second in zip(order, (*order[1:], order[0])):
            axis.quiver(*points[first], *(points[second] - points[first]), color=POINT,
                        arrow_length_ratio=ARROW_RATIO)
        arrow(axis, data["centroid"], normal)
        fit_space(axis, points)
        labels.append((axis, points, list("abc")))
    legend(fig, [(f"normal {tuple(normal)}", INK, "", "-") for normal in data["normals"]])
    orbit(fig, axes, output, stills, labels)


def rows_figure(data, output, stills):
    titles = [f"{space.title()} space: signed value {distance:g}"
              for space, distance in zip(("lens", "world"), data["distances"])]
    fig, axes = space_panels(titles)
    for axis, key, vertices in zip(axes, ("lens", "world"), data["sheets"]):
        point = data[key + "_sample"]
        sheet(axis, vertices)
        arrow(axis, np.mean(vertices, axis=0), data[key + "_plane"][:3])
        axis.scatter(*point, color=POINT, s=POINT_SIZE, depthshade=False)
        fit_space(axis, [*vertices, point])
    legend(fig, [("Translucent sheet: plane; arrow: normal", SHAPE_FILL, "", "-"),
                 ("Corresponding sample", POINT, "o", "")])
    orbit(fig, axes, output, stills)


def draw_frustum(axis, data, normals=True):
    corners = np.asarray(data["corners"])
    near, distant = corners[:4], corners[4:]
    prism = prism_faces(corners, len(near))
    faces = [np.asarray(prism[index]) for index in FRUSTUM_FACE_ORDER]
    for face, plane, color in zip(faces, data["planes"], PLANE_COLORS):
        sheet(axis, face, color)
        if normals:
            arrow(axis, face.mean(axis=0), plane[:3], color)
    axis.plot(*np.vstack([distant, distant[0]]).T, color=MUTED, linestyle="--")
    camera = data["camera"]
    axis.scatter(*camera, color=INK, marker="^", s=POINT_SIZE)
    for point in near:
        axis.plot(*np.array([camera, point]).T, color=FAINT, linestyle=":")
    return [*corners, camera]


def frustum_figure(data, output, stills):
    fig, (axis,) = space_panels(("Five inward frustum planes",))
    bounds = draw_frustum(axis, data)
    for point, inside in data["samples"]:
        axis.scatter(*point, color=BOX_COLORS[0 if inside else 2], s=POINT_SIZE, depthshade=False)
        bounds.append(point)
    fit_space(axis, bounds)
    legend(fig, [(name, color, "", "-") for name, color in zip(PLANE_NAMES, PLANE_COLORS)] +
           [("drawn depth (not a plane)", MUTED, "", "--"),
            ("camera", INK, "^", ""), ("inside / outside samples", BOX_COLORS[0], "o", "")])
    orbit(fig, [axis], output, stills)


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
        centres.append(np.mean(box, axis=0))
        texts.append(f"{index + 1}: {'outside' if outside else 'crossing' if mask else 'inside'}")
    fit_space(axis, bounds)
    legend(fig, [(label, color, "s", "") for label, color in
                 zip(("inside (mask = 0)", "crossing", "outside"), BOX_COLORS)] +
           [("8: loose corner (not rejected)", BOX_COLORS[1], "", "-")])
    orbit(fig, [axis], output, stills, [(axis, centres, texts)])


def sphere_queries_figure(data, output, stills):
    fig, (left, right) = space_panels(("Sphere / box closest points", "Segment entry parameters"))
    labels = []
    bounds = list(draw_box(left, data["box"]))
    entries = []
    for (sphere, closest, hit), name, color in zip(data["spheres"], ("overlap", "touch", "miss"), BOX_COLORS):
        centre, radius = sphere
        wire_sphere(left, centre, radius, color)
        left.plot(*np.array([centre, closest]).T, color=color, linestyle="--")
        left.scatter(*closest, color=color, s=POINT_SIZE, depthshade=False)
        bounds.extend([np.array(centre) - radius, np.array(centre) + radius])
        entries.append((f"{name}: {hit}", color, "o", "--"))
    fit_space(left, bounds)
    bounds = list(draw_box(right, data["box"]))
    centre, radius = data["segment_sphere"]
    wire_sphere(right, centre, radius, POINT)
    bounds.extend([np.array(centre) - radius, np.array(centre) + radius])
    entries.append(("Orange wire sphere: segment target", POINT, "", "-"))
    for index, (a, c, results) in enumerate(data["segments"]):
        color = SLOTS[index]
        right.plot(*np.array([a, c]).T, color=color,
                   linestyle="-" if any(result[0] for result in results) else "--")
        bounds.extend([a, c])
        hits = []
        for target, (hit, t, point) in enumerate(results):
            if hit:
                right.scatter(*point, color=color, s=POINT_SIZE, depthshade=False)
                hits.append(f"{'box' if target == 0 else 'sphere'} t={t:.3g}")
        entries.append((f"Segment {index + 1}: {', '.join(hits) or 'miss'}", color, "o", "-"))
        labels.append((np.mean([a, c], axis=0), str(index + 1)))
    fit_space(right, bounds)
    legend(fig, entries)
    orbit(fig, [left, right], output, stills,
          [(right, [point for point, _ in labels], [text for _, text in labels])])


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
        plot(data[key], args.out / (name + ".gif"), args.stills)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

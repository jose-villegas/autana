#!/usr/bin/env python3
"""Plot library-derived geometry examples for docs/math/Geometry.md.

    python launcher/tools/math/geometry_figures.py --cc CC --out DIR
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import (PANEL_SIZE, PANEL_TITLE_ROOM, PIXEL_FILL, POINT, POINT_SIZE, SHAPE_ALPHA, SHAPE_FILL,
                          run_probe, VIEW_ELEVATION, prism_faces)
from docs.plot_style import (FAINT, INK, LABEL_SIZE, MUTED, TITLE_SIZE, SLOTS,
                             GIF_FRAMES, GIF_FPS, ORBIT_GIF_COLORS, wire_sphere,
                             figure as new_figure, save, save_gif, style_axes)
import numpy as np
from matplotlib.patches import Circle, Patch, Rectangle
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d import proj3d
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

PROBE = pathlib.Path(__file__).with_name("geometry_figures_probe.c")
PLANE_NAMES = ("near", "left", "right", "top", "bottom")
PLANE_COLORS = (INK, SHAPE_FILL, POINT, SLOTS[6], PIXEL_FILL)
PANELS = 2
NORMAL_ARROW_LENGTH = 0.8
RECT_LABEL_OFFSET = (4, -13)
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
FIELD_AZIMUTH = -125
FIELD_ELEVATION = 35
FRUSTUM_AZIMUTH = -30
FRUSTUM_ELEVATION = 15
CAMERA_ZORDER = 10
GROUND_HEIGHT = 0
FIELD_FIGURE_SIZE = (8, 6.8)
FIELD_ZOOM = 1.12
GROUND_ALPHA = 0.65
DISTANCE_RULER_Y = 0
DISTANCE_PLANE_EXTENT = 1.6
DISTANCE_TICK_HEIGHT = 0.12
DISTANCE_SAMPLE_OFFSET = (5, 9)
DISTANCE_VALUE_OFFSET = (0, -16)
DISTANCE_UPPER_TICK_OFFSET = (0, 10)
DISTANCE_ZERO_OFFSET = (10, -16)
DISTANCE_NEGATIVE_SAMPLE_OFFSET = (5, 22)
DISTANCE_BRACKET_LABEL_OFFSET = (0, -16)
DISTANCE_STEP_Y = -1.35
DISTANCE_MARGIN = 0.8
SEGMENT_ZOOM = 0.9
CIRCLE_MARGIN = 1
CIRCLE_STATE_OFFSET = (12, 10)
CIRCLE_SEGMENT_OFFSET = (0, 24)
CIRCLE_LEGEND_OFFSET = -0.12


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


def circle_figure(data, output):
    fig, (axis, segment_axis) = panels()
    fig.set_size_inches(FIGURE_WIDTH, FIGURE_HEIGHT)
    style_axes(axis, "Circle / rect closest point", square=True)
    x, y, w, h = data["rect"]
    last_x, last_y = data["last"]
    for cell_y in range(y, y + h):
        for cell_x in range(x, x + w):
            axis.add_patch(Rectangle((cell_x, cell_y), 1, 1, facecolor=PIXEL_FILL,
                                     edgecolor=FAINT, alpha=SHAPE_ALPHA))
    axis.add_patch(Rectangle((x, y), w, h, facecolor="none", edgecolor=SHAPE_FILL))
    axis.plot([last_x, last_x], [y, last_y], color=INK, linestyle=":")
    axis.set_xlabel(f"right−1 = {last_x}; right = {x + w} (excluded)", fontsize=LABEL_SIZE)
    for centre, radius, closest, hit, state in data["circles"]:
        color = BOX_COLORS[("overlap", "touch", "miss").index(state)]
        axis.add_patch(Circle(centre, radius, facecolor=color, edgecolor=color, alpha=SHAPE_ALPHA))
        axis.plot(*np.array([centre, closest]).T, color=MUTED, linestyle="--")
        axis.scatter(*centre, color=color, s=POINT_SIZE)
        axis.scatter(*closest, color=INK, s=POINT_SIZE)
        axis.annotate(f"{state} ({'true' if hit else 'false'})", centre,
                      xytext=CIRCLE_STATE_OFFSET, textcoords="offset points",
                      fontsize=LABEL_SIZE, color=color)
    axis.set_xlim(x - CIRCLE_MARGIN, max(c[0][0] + c[1] for c in data["circles"]) + CIRCLE_MARGIN)
    axis.set_ylim(y + h + CIRCLE_MARGIN * 3,
                  min(y, min(c[0][1] - c[1] for c in data["circles"])) - CIRCLE_MARGIN)
    axis.set_xticks(range(x, x + w + CIRCLE_MARGIN * 3))
    axis.set_yticks(range(y, y + h + 1))
    state_handles = [Line2D([], [], color=color, marker="o", linestyle="", label=state)
                     for state, color in zip(("overlap", "touch", "miss"), BOX_COLORS)]
    axis.legend(handles=state_handles + [Line2D([], [], color=INK, marker="o", linestyle="", label="closest covered point"),
                         Line2D([], [], color=MUTED, linestyle="--", label="centre to closest")],
                fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=(0.5, CIRCLE_LEGEND_OFFSET),
                frameon=False, ncol=2)
    style_axes(segment_axis, "Segment / circle", square=True)
    centre, radius = data["segment_circle"]
    segment_axis.add_patch(Circle(centre, radius, facecolor=POINT, edgecolor=POINT, alpha=SHAPE_ALPHA))
    for (a, b, hit, t, point), color in zip(data["segments"], SLOTS):
        segment_axis.plot(*np.array([a, b]).T, color=color, linestyle="-" if hit else "--")
        if hit:
            segment_axis.scatter(*point, color=color, s=POINT_SIZE)
            label = "inside start t = 0" if t == 0 else f"entry t = {t:.3g}"
        else:
            label = "miss"
        segment_axis.annotate(label, a, xytext=CIRCLE_SEGMENT_OFFSET,
                              textcoords="offset points", color=color, fontsize=LABEL_SIZE)
    segment_axis.margins(CIRCLE_MARGIN / radius)
    segment_axis.set_xlabel("hit point marked; miss dashed", fontsize=LABEL_SIZE)
    save(fig, output)


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


def fit_space(axis, points, ground=None, zoom=AXIS_ZOOM):
    points = np.asarray(points)
    low, high = points.min(axis=0), points.max(axis=0)
    centre = (low + high) / 2
    extent = np.maximum(high - low, max(high - low) / GRID_STEPS) * AXIS_MARGIN
    low, high = centre - extent / 2, centre + extent / 2
    for setter, lower, upper in zip((axis.set_xlim, axis.set_ylim, axis.set_zlim), low, high):
        setter(lower, upper)
    axis.set_box_aspect(extent, zoom=zoom)
    grid_alpha = FACE_ALPHA if ground is None else GROUND_ALPHA
    ground = low[1] if ground is None else ground
    for x in np.linspace(low[0], high[0], GRID_STEPS):
        axis.plot([x, x], [ground, ground], [low[2], high[2]], color=FAINT,
                  alpha=grid_alpha, linewidth=GRID_WIDTH)
    for z in np.linspace(low[2], high[2], GRID_STEPS):
        axis.plot([low[0], high[0]], [ground, ground], [z, z], color=FAINT,
                  alpha=grid_alpha, linewidth=GRID_WIDTH)


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


def orbit(fig, axes, output, labels=(), azimuth=None, elevation=VIEW_ELEVATION, label_colors=None):
    annotations = []

    def draw(frame):
        for annotation in annotations:
            annotation.remove()
        annotations.clear()
        for axis in axes:
            axis.view_init(elev=elevation,
                           azim=azimuth if azimuth is not None else (PLANE_AZIMUTH if output.suffix == ".png" else
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
                        outline = axis.transData.transform(outline)
                        low, high = outline.min(axis=0), outline.max(axis=0)
                        anchor = axis.transData.transform(point)
                        on_right = anchor[0] >= (low[0] + high[0]) / 2
                        edge = high[0] + LABEL_CLEARANCE if on_right else low[0] - LABEL_CLEARANCE
                        offset[0] = (edge - anchor[0]) * POINTS_PER_INCH / fig.dpi
                        align = "left" if on_right else "right"
                    annotation = axis.annotate(text, point, xytext=tuple(offset),
                        textcoords="offset points", fontsize=LABEL_SIZE,
                        color=label_colors.get(text, INK) if label_colors else INK,
                        va="bottom", ha=align, annotation_clip=False,
                        arrowprops=dict(arrowstyle="-", color=MUTED, linewidth=LABEL_LEADER_WIDTH),
                        bbox=dict(facecolor="white", edgecolor="none", alpha=LABEL_ALPHA, pad=LABEL_PADDING))
                    for _ in range(len(placed) + 1):
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

    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def distance_figure(data, output):
    fig, axes = panels()
    length = data["normal_length"]
    for column, axis in enumerate(axes):
        style_axes(axis, "Before normalize" if column == 0 else "After normalize", square=True)
        axis.spines[["top", "right", "left", "bottom"]].set_visible(False)
        axis.set_xticks([])
        axis.set_yticks([])
        axis.axhline(DISTANCE_RULER_Y, color=MUTED, linewidth=1)
        axis.plot([0, 0], [-DISTANCE_PLANE_EXTENT / 2, DISTANCE_PLANE_EXTENT], color=SHAPE_FILL)
        axis.annotate("plane (edge-on)", (0, DISTANCE_PLANE_EXTENT),
                      xytext=DISTANCE_SAMPLE_OFFSET, textcoords="offset points", fontsize=LABEL_SIZE)
        for position, value in [*data["rulers"][column], (0, 0)]:
            axis.plot([position, position], [-DISTANCE_TICK_HEIGHT, DISTANCE_TICK_HEIGHT], color=INK)
            tick_offset = DISTANCE_UPPER_TICK_OFFSET if column == 0 else DISTANCE_VALUE_OFFSET
            axis.annotate(f"{value:+g}" if value else "0", (position, DISTANCE_RULER_Y),
                          xytext=tick_offset if value else DISTANCE_ZERO_OFFSET,
                          textcoords="offset points", ha="center", fontsize=LABEL_SIZE)
        for _point, raw, distance, height in data["samples"]:
            if distance == 0:
                continue
            value = raw if column == 0 else distance
            color = SAMPLE_COLORS[int(np.sign(value)) + 1]
            axis.plot([distance, distance], [DISTANCE_RULER_Y, height], color=color, linestyle="--")
            axis.scatter(distance, height, color=color, s=POINT_SIZE)
            axis.annotate(f"value = {value:.3g}", (distance, height),
                          xytext=DISTANCE_NEGATIVE_SAMPLE_OFFSET if value < 0 else DISTANCE_SAMPLE_OFFSET,
                          textcoords="offset points", fontsize=LABEL_SIZE, color=color)
        low, high = min(sample[2] for sample in data["samples"]), max(sample[2] for sample in data["samples"])
        axis.set_xlim(low - DISTANCE_MARGIN, high + DISTANCE_MARGIN)
        axis.set_ylim(-DISTANCE_PLANE_EXTENT - DISTANCE_MARGIN, DISTANCE_PLANE_EXTENT + DISTANCE_MARGIN)
        step = data["rulers"][column][1][0]
        axis.annotate("", (0, DISTANCE_STEP_Y), (step, DISTANCE_STEP_Y),
                      arrowprops=dict(arrowstyle="|-|", color=INK))
        axis.annotate(f"1 / |n| = {step:.3g} unit" if column == 0 else f"{step:g} unit",
                      (step / 2, DISTANCE_STEP_Y), xytext=DISTANCE_BRACKET_LABEL_OFFSET,
                      textcoords="offset points", ha="center", va="top", fontsize=LABEL_SIZE)
        axis.set_xlabel("signed distance along the normal", fontsize=LABEL_SIZE)
    fig.suptitle(f"Signed value ÷ |n| = signed distance     (|n| = {length:g})", fontsize=TITLE_SIZE)
    save(fig, output)


def points_figure(data, output):
    fig, axes = space_panels(["Winding " + " → ".join("abc"[i] for i in order) for order in data["orders"]])
    points = np.array(data["points"])
    labels = []
    for axis, normal, order, color in zip(axes, data["normals"], data["orders"], NORMAL_COLORS):
        sheet(axis, points)
        for first, second in zip(order, (*order[1:], order[0])):
            axis.quiver(*points[first], *(points[second] - points[first]), color=POINT,
                        arrow_length_ratio=ARROW_RATIO)
        tip = arrow(axis, data["centroid"], normal, color)
        fit_space(axis, [*points, tip])
        labels.append((axis, points, list("abc")))
    legend(fig, [(f"normal {tuple(normal)}", color, "", "-")
                 for normal, color in zip(data["normals"], NORMAL_COLORS)])
    orbit(fig, axes, output, labels)


def rows_figure(data, output):
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
    orbit(fig, axes, output, labels)


def draw_frustum(axis, data, normals=True, labels=None):
    assert len(FRUSTUM_FACE_ORDER) == data["plane_count"]
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
    axis.plot(*np.array([camera]).T, color=INK, marker="^", linestyle="",
              markersize=np.sqrt(POINT_SIZE), zorder=CAMERA_ZORDER)
    for point in distant:
        axis.plot(*np.array([camera, point]).T, color=MUTED, linewidth=1)
    return [*corners, camera]


def frustum_figure(data, output):
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
    orbit(fig, [axis], output, labels, azimuth=FRUSTUM_AZIMUTH, elevation=FRUSTUM_ELEVATION)


def draw_box(axis, bounds, color=SHAPE_FILL, alpha=FACE_ALPHA):
    low, high = np.array(bounds)
    corners = np.array([((low[0], high[0])[x], (low[1], high[1])[y], z)
                        for z in (low[2], high[2]) for x, y in BOX_RING])
    axis.add_collection3d(Poly3DCollection(prism_faces(corners, len(BOX_RING)),
                                         facecolor=color, edgecolor=color, alpha=alpha))
    return corners


def box_frustum_figure(data, output):
    fig, (axis,) = space_panels(("Boxes against a moving camera frustum",))
    fig.set_size_inches(*FIELD_FIGURE_SIZE)
    frames = data["box_frustum"]
    bounds = [point for frame in frames for point in frame["frustum"]["corners"]]
    bounds.extend(point for box, _outside, _mask in frames[0]["boxes"] for point in box)
    legend(fig, [(label, color, "s", "") for label, color in
                 zip(("inside (mask = 0)", "intersecting", "outside"), BOX_COLORS)] +
           [("camera", INK, "^", ""), ("drawn depth (not a plane)", MUTED, "", "--")])

    def draw(frame):
        axis.clear()
        axis.set_axis_off()
        axis.set_title("Boxes against a moving camera frustum", fontsize=TITLE_SIZE)
        axis.view_init(elev=FIELD_ELEVATION, azim=FIELD_AZIMUTH, vertical_axis="y")
        fit_space(axis, bounds, ground=GROUND_HEIGHT, zoom=FIELD_ZOOM)
        draw_frustum(axis, frames[frame]["frustum"], normals=False)
        for box, outside, mask in frames[frame]["boxes"]:
            draw_box(axis, box, BOX_COLORS[2 if outside else (1 if mask else 0)], alpha=SHAPE_ALPHA)

    save_gif(fig, draw, len(frames), output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def sphere_overlap_figure(data, output):
    fig, (axis,) = space_panels(("Sphere / box closest point",))
    frames = data["spheres"]
    bounds = list(data["box"])
    for (centre, radius), _closest, _hit, _state in frames:
        bounds.extend((np.array(centre) - radius, np.array(centre) + radius))
    legend(fig, [(label, color, "o", "") for label, color in zip(("overlap", "touch", "miss"), BOX_COLORS)] +
           [("closest point", INK, "o", ""), ("centre to closest", MUTED, "", "--")])

    def draw(frame):
        axis.clear()
        axis.set_axis_off()
        sphere, closest, _hit, state = frames[frame]
        color = BOX_COLORS[("overlap", "touch", "miss").index(state)]
        axis.set_title(f"Sphere / box: {state}", fontsize=TITLE_SIZE, color=color)
        axis.view_init(elev=VIEW_ELEVATION, azim=PLANE_AZIMUTH, vertical_axis="y")
        fit_space(axis, bounds)
        draw_box(axis, data["box"])
        centre, radius = sphere
        wire_sphere(axis, centre, radius, color)
        axis.plot(*np.array([centre, closest]).T, color=MUTED, linestyle="--")
        axis.scatter(*closest, color=INK, s=POINT_SIZE, depthshade=False)
        axis.scatter(*centre, color=color, s=POINT_SIZE, depthshade=False)

    save_gif(fig, draw, len(frames), output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def sphere_segments_figure(data, output):
    fig, axes = space_panels(("Segment / closed box", "Segment / closed sphere"))
    labels = []
    label_colors = {}
    for target, axis in enumerate(axes):
        bounds = list(data["box"])
        if target:
            centre, radius = data["segment_sphere"]
            wire_sphere(axis, centre, radius, POINT)
            bounds.extend((np.array(centre) - radius, np.array(centre) + radius))
        else:
            draw_box(axis, data["box"])
        for number, ((a, c, results), color) in enumerate(zip(data["segments"], SLOTS), start=1):
            hit, t, point = results[target]
            axis.plot(*np.array([a, c]).T, color=color, linestyle="-" if hit else "--")
            bounds.extend((a, c))
            if hit:
                axis.scatter(*point, color=color, s=POINT_SIZE, depthshade=False)
                result = "inside start t = 0" if t == 0 else f"entry t = {t:.3g}"
            else:
                result = "miss"
            label = f"{number}: {result}"
            labels.append((axis, [a], [label]))
            label_colors[label] = color
        fit_space(axis, bounds, zoom=SEGMENT_ZOOM)
    legend(fig, [("entry point", INK, "o", ""), ("miss (dashed)", MUTED, "", "--")])
    orbit(fig, axes, output, labels, label_colors=label_colors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = run_probe(args.cc, [PROBE], GIF_FRAMES)
    rect_figure(data["rect"], args.out / "rect-queries.png")
    circle_figure(data["circle"], args.out / "circle-rect-queries.png")
    for name, values, plot in (("plane-distance.png", data["distance"], distance_figure),
                              ("plane-from-points.png", data["triangle"], points_figure),
                              ("plane-from-rows.png", data["rows"], rows_figure),
                              ("frustum.png", data["frustum"], frustum_figure),
                              ("box3-frustum.gif", data, box_frustum_figure),
                              ("box3-sphere-overlap.gif", data["queries"], sphere_overlap_figure),
                              ("box3-sphere-segments.png", data["queries"], sphere_segments_figure)):
        plot(values, args.out / name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

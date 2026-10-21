#!/usr/bin/env python3
"""Plot library trajectories for docs/math/Motion.md; --cc CC --out DIR."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import (DISPLAY, PANEL_SIZE, PANEL_TITLE_ROOM, ROOT, SHAPE_ALPHA,
                          VIEW_AZIMUTH, VIEW_ELEVATION, read_define, run_probe)
from docs.plot_style import (FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE,
                             figure, save, save_gif, subplots, wire_sphere, GIF_FRAMES, GIF_FPS,
                             ORBIT_GIF_COLORS, WIRE_WIDTH, WIRE_SAMPLES)
import numpy as np
from matplotlib.lines import Line2D
from mpl_toolkits.mplot3d.art3d import Line3DCollection, Poly3DCollection
from mpl_toolkits.mplot3d import proj3d

PROBE = pathlib.Path(__file__).with_name("motion_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/packages/math/src/motion/orbit_motion.c")
PANELS = 2
SAMPLE_MARKER_INTERVAL = 5
FIT_CONE_EXTENSION = 1.1
FIT_VIEW = (24, -125)
FIT_PLANE_ALPHA = 0.09
FIT_SPHERE_ALPHA = 0.32
FIT_SURFACE_SAMPLES = 65
FIT_LEGEND_ANCHOR = (0.5, -0.06)
FIT_ZOOM = 1.05
FIT_TOUCH_LABEL = (0.01, 0.85)
FIT_ROOM_LABEL = (0.62, 0.93)
FIT_LENS_LABEL = (0.12, 0.16)
FIT_CAMERA_LABEL_OFFSET = (-8, -16)
FIT_DIMENSION_CAP = 0.05
WAVE_MARGIN = 1.2
LEGEND_ANCHOR = (0.5, -0.22)
DECAY_MARKER_SIZE = 4
BUMP_MARKER_SIZE = 3
COLORBAR_SHRINK = 0.7
COLUMN_EDGE = 0.5
OFFSET_COLORMAP = "RdBu_r"
TRAIL_SAMPLES = 5
ORBIT_MARGIN = 1.3
FRAME_AXIS_LENGTH = 0.45
ORBIT_ZOOM = 1.5
ORBIT_DPI = 70
FRAME_AXIS_WIDTH = 2
ARROW_HEAD_RATIO = 0.3
ORBIT_PANEL_RATIOS = (3, 1)
PHASE_LABEL_HEIGHT = 1.04
SPEED_MARGIN = 1.15
SPEED_LEGEND_ANCHOR = (0.5, -0.65)
ORBIT_LEGEND_ANCHOR = (0.5, 0.01)
ANIMATION_DPI = 80
PANEL_LABEL_PADDING = 0.15
WAVE_COLOR_PERCENTILE = 98
WAVE_PUSH_TICKS = 5


def orbit_animation(data, output):
    samples = data["orbit"]
    times = np.array([row["time"] for row in samples])
    positions = np.array([row["position"] for row in samples])
    speeds = np.array([row["speed"] for row in samples])
    target = np.array(data["target"])
    radius = np.linalg.norm(positions[0] - target)
    fig = figure((PANEL_SIZE * PANELS, PANEL_SIZE * PANELS))
    fig.set_dpi(ORBIT_DPI)
    grid = fig.add_gridspec(PANELS, 1, height_ratios=ORBIT_PANEL_RATIOS)
    top = fig.add_subplot(grid[0], projection="3d")
    strip = fig.add_subplot(grid[1])
    wire_sphere(top, target, radius)
    yaw = np.linspace(0, 2 * np.pi, WIRE_SAMPLES)
    for pitch in (-data["pitch_limit"], data["pitch_limit"]):
        top.plot(target[0] + radius * np.cos(pitch) * np.cos(yaw),
                 target[1] + radius * np.sin(pitch) * np.ones_like(yaw),
                 target[2] + radius * np.cos(pitch) * np.sin(yaw), color=MUTED, linestyle="--")
    top.scatter(*target, color=INK)
    trail = Line3DCollection([], colors=SLOTS[0], linewidths=2)
    top.add_collection3d(trail, autolim=False)
    arrows = []
    bound = radius * ORBIT_MARGIN
    top.set(xlim=(target[0] - bound, target[0] + bound),
            ylim=(target[1] - bound, target[1] + bound),
            zlim=(target[2] - bound, target[2] + bound))
    top.set_box_aspect((1, 1, 1), zoom=ORBIT_ZOOM)
    top.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH, vertical_axis="y")
    top.set_axis_off()
    handles = [Line2D([], [], color=color, marker="s", linewidth=FRAME_AXIS_WIDTH, label=label) for color, label in
               zip(SLOTS, ("+x stays level", "+y", "+z faces target"))]
    handles.extend((Line2D([], [], marker="o", linestyle="none", color=INK, label="target"),
                    Line2D([], [], linestyle="--", color=MUTED, label="±pitch limit")))
    top.legend(handles=handles, fontsize=LABEL_SIZE, loc="lower center",
               bbox_to_anchor=ORBIT_LEGEND_ANCHOR, ncol=3, frameon=False)
    for column, label, color in zip(speeds.T, ("yaw", "pitch"), SLOTS):
        strip.plot(times, column, color=color, label=label)
    boundaries = (0, data["release"], data["reset_start"], times[-1])
    for start, end, label in zip(boundaries, boundaries[1:], ("held", "fling", "reset")):
        strip.axvline(start, color=MUTED, linewidth=WIRE_WIDTH)
        strip.text((start + end) / 2, PHASE_LABEL_HEIGHT, label, transform=strip.get_xaxis_transform(),
                   ha="center", fontsize=LABEL_SIZE)
    for time in np.arange(data["release"] + data["half_life"], data["reset_start"], data["half_life"]):
        strip.axvline(time, color=FAINT, linestyle=":")
    cursor = strip.axvline(0, color=INK)
    bound = abs(speeds).max() * SPEED_MARGIN
    strip.axhline(0, color=FAINT, linewidth=WIRE_WIDTH)
    strip.set(xlabel="Time (s); dotted marks: fling half-lives", ylabel="Speed (rad/s)",
              xlim=(0, times[-1]), ylim=(-bound, bound))
    strip.tick_params(labelsize=LABEL_SIZE)
    strip.legend(fontsize=LABEL_SIZE, ncol=2, loc="upper center", bbox_to_anchor=SPEED_LEGEND_ANCHOR)

    def draw(frame):
        for arrow in arrows:
            arrow.remove()
        arrows.clear()
        row = samples[frame]
        points = positions[max(0, frame - TRAIL_SAMPLES):frame + 1]
        trail.set_segments(np.stack((points[:-1], points[1:]), axis=1))
        if len(points) > 1:
            trail.set_alpha(np.linspace(0, 1, len(points) - 1))
        for direction, color in zip(row["axes"], SLOTS):
            arrows.append(top.quiver(*positions[frame], *direction, color=color,
                                     length=radius * FRAME_AXIS_LENGTH, linewidth=FRAME_AXIS_WIDTH,
                                     arrow_length_ratio=ARROW_HEAD_RATIO))
        time = row["time"]
        cursor.set_xdata([time, time])
        phase = "held" if time < data["release"] else "fling" if time < data["reset_start"] else "reset"
        if time >= data["reset_start"] and not row["resetting"]:
            phase = "reset settled — home"
        top.set_title(f"3D orbit: {phase} — {time:.1f} s", fontsize=TITLE_SIZE)

    draw(0)
    fig.canvas.draw()
    fig.set_layout_engine("none")
    save_gif(fig, draw, len(samples), output, GIF_FPS, colors=ORBIT_GIF_COLORS)


def decay_figure(data, output):
    fig, axes = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    fig.get_layout_engine().set(w_pad=PANEL_LABEL_PADDING)
    for index, (run, color) in enumerate(zip(data, SLOTS)):
        samples = np.array(run["samples"])
        for axis, column in zip(axes, (1, 2)):
            axis.plot(samples[:, 0], samples[:, column], color=color, linestyle="-" if index == 0 else "none",
                      marker=None if index == 0 else "o", markerfacecolor="none", markersize=DECAY_MARKER_SIZE,
                      markevery=SAMPLE_MARKER_INTERVAL, label=f"dt = {run['dt'] * 1000:g} ms")
    axes[1].axhline(data[0]["limit"], color=MUTED, linestyle="--", label=r"limit $v_0 h / \ln 2$")
    for axis, title, ylabel in zip(axes, ("Fling speed", "Angle covered"), ("rad/s", "rad")):
        axis.set(title=title, xlabel="Time after release (s)", ylabel=ylabel, ylim=(0, None))
        axis.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
    save(fig, output)


def reset_figure(data, output):
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    for run, color in zip(data, SLOTS):
        samples = np.array(run["samples"])
        axis.plot(*samples.T, color=color, label=f"v₀ = {run['speed']:g} rad/s; settled {run['settle']:.2f} s")
        axis.axvline(run["settle"], color=color, linestyle=":")
        axis.scatter([run["settle"]], [0], color=color)
    axis.axhline(0, color=FAINT)
    axis.set(title="Critically damped reset: initial velocity matters", xlabel="Time (s)", ylabel="Yaw offset from home (rad)")
    axis.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
    save(fig, output)


def fit_figure(data, output):
    fig = figure((PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    diagram = fig.add_subplot(1, PANELS, 1, projection="3d")
    axis = fig.add_subplot(1, PANELS, 2)
    distance, radius = data["distance"], data["radius"]
    width = read_define(DISPLAY, "BSP_LCD_H_RES")
    height = read_define(DISPLAY, "BSP_LCD_V_RES")
    short_tangent = data["padded_tangent"]
    half_tangents = np.array((width, height)) / min(width, height) * short_tangent
    depth = distance + radius * FIT_CONE_EXTENSION
    corners = np.array([(sx * depth * half_tangents[0], sy * depth * half_tangents[1], depth)
                        for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1), (-1, -1))])
    camera = np.zeros(3)
    center = np.array((0, 0, distance))
    short_axis = int(width >= height)
    long_axis = 1 - short_axis
    for first, second in zip(corners, corners[1:]):
        face = np.array((camera, first, second))
        if first[short_axis] == second[short_axis]:
            diagram.add_collection3d(Poly3DCollection([face], facecolor=SLOTS[1], alpha=FIT_PLANE_ALPHA))
    for corner in corners[:-1]:
        diagram.plot(*np.array((camera, corner)).T, color=MUTED, linewidth=WIRE_WIDTH)
    diagram.plot(*corners.T, color=SLOTS[1])
    diagram.plot(*np.array((camera, center)).T, color=INK, linestyle=":")
    yaw = np.linspace(0, 2 * np.pi, FIT_SURFACE_SAMPLES)
    pitch = np.linspace(-np.pi / 2, np.pi / 2, FIT_SURFACE_SAMPLES)
    diagram.plot_surface(radius * np.outer(np.cos(yaw), np.cos(pitch)),
                         radius * np.outer(np.ones_like(yaw), np.sin(pitch)),
                         distance + radius * np.outer(np.sin(yaw), np.cos(pitch)),
                         color=SLOTS[0], alpha=FIT_SPHERE_ALPHA, linewidth=0, shade=True)
    diagram.scatter(*camera, color=INK)
    tangent_z = distance / (1 + short_tangent ** 2)
    contacts = []
    for sign in (-1, 1):
        contact = np.array((0, 0, tangent_z))
        contact[short_axis] = sign * tangent_z * short_tangent
        contacts.append(contact)
        diagram.scatter(*contact, color=SLOTS[1], depthshade=False)
    long_normal = np.zeros(3)
    long_normal[long_axis] = 1
    long_normal[2] = -half_tangents[long_axis]
    long_normal /= np.linalg.norm(long_normal)
    plane_distance = -np.dot(center, long_normal)
    gap = np.array((center + radius * long_normal, center + plane_distance * long_normal))
    diagram.plot(*gap.T, color=INK)
    for endpoint in gap:
        cap = np.zeros(3)
        cap[short_axis] = radius * FIT_DIMENSION_CAP
        diagram.plot(*np.array((endpoint - cap, endpoint + cap)).T, color=INK)
    diagram.set_title("Fit: the short axis binds", fontsize=TITLE_SIZE)
    diagram.set(xlim=(-corners[:, 0].max(), corners[:, 0].max()),
                ylim=(-corners[:, 1].max(), corners[:, 1].max()), zlim=(0, depth))
    diagram.set_box_aspect((np.ptp(corners[:, 0]), np.ptp(corners[:, 1]), depth), zoom=FIT_ZOOM)
    diagram.view_init(elev=FIT_VIEW[0], azim=FIT_VIEW[1], vertical_axis="y")
    diagram.set_axis_off()
    x, y, _ = proj3d.proj_transform(*camera, diagram.get_proj())
    diagram.annotate("camera", xy=(x, y), xytext=FIT_CAMERA_LABEL_OFFSET,
                     textcoords="offset points", fontsize=LABEL_SIZE, ha="right")
    for points, label, location in ((contacts, "touches (short axis)", FIT_TOUCH_LABEL),
                                    ([gap.mean(axis=0)], "room (long axis)", FIT_ROOM_LABEL),
                                    ([center / 2], "lens axis (+z)", FIT_LENS_LABEL)):
        for index, point in enumerate(points):
            x, y, _ = proj3d.proj_transform(*point, diagram.get_proj())
            diagram.annotate(label if index == 0 else "", xy=(x, y), xytext=location, textcoords="axes fraction",
                             fontsize=LABEL_SIZE, arrowprops=dict(arrowstyle="-", color=MUTED))
    handles = [Line2D([], [], color=color, marker=marker, linestyle=style, label=label)
               for color, marker, style, label in ((INK, "o", "none", "camera"),
                                                   (SLOTS[0], "o", "none", "sphere"),
                                                   (SLOTS[1], None, "-", "padded picture"))]
    diagram.legend(handles=handles, fontsize=LABEL_SIZE, loc="upper center",
                   bbox_to_anchor=FIT_LEGEND_ANCHOR, ncol=3, frameon=False)
    samples = np.array(data["samples"])
    for column, label, color in ((1, f"padding p = {data['padding']:g}", SLOTS[0]), (2, "padding p = 0", SLOTS[1])):
        axis.plot(samples[:, 0], samples[:, column], color=color, label=label)
    axis.set(title="Framing a sphere", xlabel="Lens half field of view (degrees)", ylabel="Fit distance / radius", ylim=(0, None))
    axis.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
    save(fig, output)


def wave_figures(data, output):
    runs = data["samples"]
    offsets = np.array([row["offset"] for row in runs])
    columns = np.arange(offsets.shape[1])
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    line, = axis.plot(columns, offsets[0], color=SLOTS[0])
    shade = None
    axis.set(xlabel="Column (free ends)", ylabel="Offset (px)", ylim=(offsets.min() * WAVE_MARGIN, offsets.max() * WAVE_MARGIN))
    indices = np.linspace(0, len(runs) - 1, GIF_FRAMES).astype(int)

    def draw(frame):
        nonlocal shade
        index = indices[frame]
        row = runs[index]
        if shade is not None:
            shade.remove()
        shade = None if row["rest"] else axis.axvspan(row["lo"], row["hi"], color=SLOTS[2], alpha=SHAPE_ALPHA)
        line.set_ydata(offsets[index])
        axis.set_title(f"Tick {row['tick']} — {'at rest' if row['rest'] else 'active range shaded'}", fontsize=TITLE_SIZE)

    save_gif(fig, draw, GIF_FRAMES, output / "spring-line-wave.gif", GIF_FPS)
    fig, (heat, bump) = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    fig.get_layout_engine().set(w_pad=PANEL_LABEL_PADDING)
    bound = np.percentile(abs(offsets[WAVE_PUSH_TICKS:]), WAVE_COLOR_PERCENTILE)
    image = heat.imshow(offsets, aspect="auto", origin="upper", cmap=OFFSET_COLORMAP, vmin=-bound, vmax=bound,
                        extent=(-COLUMN_EDGE, offsets.shape[1] - COLUMN_EDGE, len(runs) - COLUMN_EDGE, -COLUMN_EDGE))
    active = [row for row in runs if not row["rest"]]
    for key, label, style in (("lo", "active_lo", "-"), ("hi", "active_hi (exclusive)", "--")):
        heat.plot([row[key] for row in active], [row["tick"] for row in active], color=INK,
                  linestyle=style, label=label)
    heat.set(xlabel="Column", ylabel=f"Tick ({data['tick_ms']} ms)", title="Offset and active range")
    heat.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
    fig.colorbar(image, ax=heat, shrink=COLORBAR_SHRINK, label="Offset (px, clipped)")
    bump.plot(columns, offsets[0], "o-", color=SLOTS[0], markersize=BUMP_MARKER_SIZE)
    bump.set(xlabel="Column", ylabel="Offset (px)", ylim=(0, None), title=r"Nudge: $(1-t^2)^2$")
    save(fig, output / "spring-line-spacetime.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = run_probe(args.cc, SOURCES)
    orbit_animation(data, args.out / "orbit-fling.gif")
    decay_figure(data["decay"], args.out / "orbit-fling-decay.png")
    reset_figure(data["reset"], args.out / "orbit-reset-spring.png")
    fit_figure(data["fit"], args.out / "orbit-fit-distance.png")
    wave_figures(data["line"], args.out)
    for run in data["reset"]:
        print(f"reset v0={run['speed']:g}: minimum={run['minimum']:.9g} rad, settle={run['settle']:.3f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

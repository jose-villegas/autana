#!/usr/bin/env python3
"""Plot library trajectories for docs/math/Motion.md; --cc CC --out DIR."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, ROOT, SHAPE_ALPHA, run_probe, square_axes
from docs.plot_style import (FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE,
                             save, save_gif, subplots)
import numpy as np
from matplotlib.patches import Circle

PROBE = pathlib.Path(__file__).with_name("motion_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/packages/math/src/motion/orbit_motion.c")
GIF_FRAMES = 60
GIF_FPS = 10
PANELS = 2
SAMPLE_MARKER_INTERVAL = 5
FIT_CONE_EXTENSION = 1.3
WAVE_MARGIN = 1.2
LEGEND_ANCHOR = (0.5, -0.22)
DECAY_MARKER_SIZE = 2
BUMP_MARKER_SIZE = 3
COLORBAR_SHRINK = 0.7
COLUMN_EDGE = 0.5
OFFSET_COLORMAP = "RdBu_r"


def orbit_animation(data, output):
    samples = np.array(data["orbit"])
    fig, (top, strip) = subplots(PANELS, 1, (PANEL_SIZE * PANELS, PANEL_SIZE * PANELS))
    square_axes(top, "Held steering, then fling (top view)")
    top.plot(samples[:, 1], samples[:, 2], color=FAINT)
    top.scatter([0], [0], color=INK, label="target")
    dot, = top.plot([], [], "o", color=SLOTS[0])
    top.set(xlabel="x", ylabel="z")
    strip.plot(samples[:, 0], samples[:, 3], color=SLOTS[0])
    strip.axvline(data["release"], color=INK, label="release")
    for time in np.arange(data["release"] + data["half_life"], samples[-1, 0], data["half_life"]):
        strip.axvline(time, color=FAINT, linestyle=":")
    cursor = strip.axvline(0, color=SLOTS[1])
    strip.set(xlabel="Time (s); dotted marks: fling half-lives", ylabel="Yaw speed (rad/s)", ylim=(0, None))
    strip.legend(fontsize=LABEL_SIZE)

    def draw(index):
        time, x, z, _ = samples[index]
        dot.set_data([x], [z])
        cursor.set_xdata([time, time])
        top.set_title(f"{'held' if time < data['release'] else 'fling'} — {time:.1f} s", fontsize=TITLE_SIZE)

    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS)


def decay_figure(data, output):
    fig, axes = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    for run, color in zip(data, SLOTS):
        samples = np.array(run["samples"])
        for axis, column in zip(axes, (1, 2)):
            axis.plot(samples[:, 0], samples[:, column], color=color, marker="." if column == 1 else "o",
                      markersize=DECAY_MARKER_SIZE, markevery=SAMPLE_MARKER_INTERVAL, label=f"dt = {run['dt'] * 1000:g} ms")
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
    fig, (diagram, axis) = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    square_axes(diagram, "Sphere tangent to padded cone")
    distance, radius = data["distance"], data["radius"]
    x, y = data["tangent"]
    diagram.add_patch(Circle((distance, 0), radius, fill=False, edgecolor=SLOTS[0]))
    diagram.plot([0, x * FIT_CONE_EXTENSION], [0, y * FIT_CONE_EXTENSION], color=MUTED)
    diagram.plot([0, x * FIT_CONE_EXTENSION], [0, -y * FIT_CONE_EXTENSION], color=MUTED)
    diagram.plot([distance, x], [0, y], color=SLOTS[1], label="radius r")
    diagram.plot([0, distance], [0, 0], ":", color=INK, label="fit distance d")
    diagram.scatter([x], [y], color=SLOTS[1])
    diagram.set(xlabel="Distance along lens axis", ylabel="Transverse distance")
    diagram.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
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
        shade = axis.axvspan(row["lo"], row["hi"], color=SLOTS[2], alpha=SHAPE_ALPHA)
        line.set_ydata(offsets[index])
        axis.set_title(f"Tick {row['tick']} — {'at rest' if row['rest'] else 'active range shaded'}", fontsize=TITLE_SIZE)

    save_gif(fig, draw, GIF_FRAMES, output / "spring-line-wave.gif", GIF_FPS)
    fig, (heat, bump) = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    bound = abs(offsets).max()
    image = heat.imshow(offsets, aspect="auto", origin="upper", cmap=OFFSET_COLORMAP, vmin=-bound, vmax=bound,
                        extent=(-COLUMN_EDGE, offsets.shape[1] - COLUMN_EDGE, len(runs) - COLUMN_EDGE, -COLUMN_EDGE))
    active = [row for row in runs if not row["rest"]]
    for key, label, style in (("lo", "active_lo", "-"), ("hi", "active_hi (exclusive)", "--")):
        heat.plot([row[key] for row in active], [row["tick"] for row in active], color=INK,
                  linestyle=style, label=label)
    heat.set(xlabel="Column", ylabel=f"Tick ({data['tick_ms']} ms)", title="Offset and active range")
    heat.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR)
    fig.colorbar(image, ax=heat, shrink=COLORBAR_SHRINK)
    bump.plot(columns, offsets[0], "o-", color=SLOTS[0], markersize=BUMP_MARKER_SIZE)
    bump.set(xlabel="Column", ylabel="Offset (px)", ylim=(0, None), title=r"Nudge on still line: $(1-t^2)^2$")
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

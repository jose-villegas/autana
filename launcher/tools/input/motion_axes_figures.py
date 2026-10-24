"""Animate what the motion readers track on a recorded capture; --cc CC --samples FILE --out DIR.

The device is drawn as the panel, sized from its resolution, turned so the
recorded gravity points down. Gravity is each sample's acceleration less its
linear part; shake is the real reader run over the linear part. Writes
motion-tilt.gif from the --tilt-step span where gravity turns furthest and
motion-shake.gif from the --shake-step span shake_figures.py plots.
FILE holds one "step ms ax ay az lx ly lz" line in mg per motion sample.
"""
import argparse
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shake_figures import MS_PER_S, PROBE, SOURCES, window
from math_figures import DISPLAY, ROOT, read_define, run_probe
from docs.plot_style import (FAINT, GIF_FPS, INK, LABEL_SIZE, MUTED, ORBIT_GIF_COLORS, SLOTS, TITLE_SIZE, figure,
                             save_gif, style_axes)
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

WIDTH = read_define(DISPLAY, "BSP_LCD_H_RES")
HEIGHT = read_define(DISPLAY, "BSP_LCD_V_RES")
THICKNESS = WIDTH / 8  # drawn only; the slab's depth is not measured
G_LENGTH = HEIGHT * 0.6  # px drawn per g
AXIS_LENGTH = WIDTH * 0.45
BOUND = HEIGHT * 0.75
# Screen axes run x right, y down, z out of the screen; the plot's z is up.
SCREEN_TO_PLOT = np.diag((1.0, -1.0, 1.0))
DOWN = np.array((0.0, 0.0, -1.0))
TILT_SPAN_MS = 6000
SHAKE_FRAMES = 60
TILT_FRAMES = 60
SHAKE_FPS = 15  # with SHAKE_FRAMES over the span, the clip plays slowed
FIGURE_SIZE = (8, 4.2)
GIF_DPI = 80
VIEW = (18, -60)
SCENE_ZOOM = 1.4
FACE_ALPHA = 0.2
AXIS_WIDTH = 2
LINEAR_WIDTH = 3
LEGEND_ANCHOR = (0.5, -0.2)  # below the strip's time axis
ARROW_HEAD = 0.2


def rotation_to_down(gravity):
    """The smallest rotation taking gravity, in plot axes, onto straight down (Rodrigues)."""
    v = gravity / np.linalg.norm(gravity)
    axis = np.cross(v, DOWN)
    sine, cosine = np.linalg.norm(axis), float(np.dot(v, DOWN))
    if sine < 1e-9:
        return np.eye(3) if cosine > 0 else np.diag((1.0, -1.0, -1.0))
    k = axis / sine
    cross = np.array(((0, -k[2], k[1]), (k[2], 0, -k[0]), (-k[1], k[0], 0)))
    return np.eye(3) + sine * cross + (1 - cosine) * cross @ cross


def slab():
    """The panel's eight corners in screen axes, centred, and its six faces; face 0 is the screen."""
    x, y, z = WIDTH / 2, HEIGHT / 2, THICKNESS / 2
    corners = np.array([(sx * x, sy * y, sz * z) for sz in (1, -1) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
    faces = ((0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7))
    return corners, faces


def samples(data, path):
    """Each probe trace row beside its sample's acceleration and linear part, in g."""
    lines = path.read_text(encoding="utf-8").splitlines()
    return [(row, np.array(line.split()[2:], dtype=float) / MS_PER_S) for row, line in zip(data["trace"], lines)]


def tilt_span(pairs, step):
    """The --tilt-step span over which gravity's direction turns furthest, judged on TILT_FRAMES bin means."""
    rows = [(row, values) for row, values in pairs if row[0] == step]
    if not rows:
        raise SystemExit(f"motion_axes_figures: no samples for step {step}")
    best, best_turn = rows, -1.0
    for first in range(0, len(rows), max(1, len(rows) // TILT_FRAMES)):
        span = [pair for pair in rows[first:] if pair[0][1] - rows[first][0][1] < TILT_SPAN_MS]
        if span[-1] is rows[-1]:
            break
        gravity = np.array([values[:3] - values[3:] for _, values in span])
        down = [unit(part.mean(axis=0)) for part in np.array_split(gravity, TILT_FRAMES)]
        turn = sum(np.arccos(np.clip(np.dot(a, b), -1, 1)) for a, b in zip(down, down[1:]))
        if turn > best_turn:
            best, best_turn = span, turn
    return best


def unit(vector):
    return vector / np.linalg.norm(vector)


def tilt_degrees(gravity):
    """Angle from lying flat screen up, where gravity reads -1 g on z."""
    return float(np.degrees(np.arccos(np.clip(-unit(gravity)[2], -1, 1))))


def animate(span, frames, fps, title, strip_setup, caption, output):
    corners, faces = slab()
    fig = figure(FIGURE_SIZE)
    fig.set_dpi(GIF_DPI)
    grid = fig.add_gridspec(1, 2, width_ratios=(1, 1))
    scene = fig.add_subplot(grid[0], projection="3d")
    strip = fig.add_subplot(grid[1])
    seconds = np.array([(row[1] - span[0][0][1]) / MS_PER_S for row, _ in span])
    strip_setup(strip, seconds)
    cursor = strip.axvline(0, color=INK)
    picks = np.linspace(0, len(span) - 1, frames).round().astype(int)
    drawn = []

    def draw(frame):
        for artist in drawn:
            artist.remove()
        drawn.clear()
        row, values = span[picks[frame]]
        acc, linear = values[:3], values[3:]
        gravity = acc - linear
        turn = rotation_to_down(SCREEN_TO_PLOT @ gravity) @ SCREEN_TO_PLOT
        placed = corners @ turn.T
        colours = [SLOTS[0]] + [FAINT] * (len(faces) - 1)
        drawn.append(scene.add_collection3d(Poly3DCollection([placed[list(face)] for face in faces],
                                                             facecolors=colours, edgecolors=MUTED,
                                                             alpha=FACE_ALPHA)))
        for index, (name, colour) in enumerate(zip("xyz", SLOTS[1:4])):
            direction = turn[:, index] * AXIS_LENGTH
            drawn.append(scene.quiver(0, 0, 0, *direction, color=colour, linewidth=AXIS_WIDTH,
                                      arrow_length_ratio=ARROW_HEAD))
            drawn.append(scene.text(*(direction * 1.12), name, color=colour, fontsize=LABEL_SIZE))
        g = DOWN * G_LENGTH
        drawn.append(scene.quiver(0, 0, 0, *g, color=INK, linewidth=AXIS_WIDTH, arrow_length_ratio=ARROW_HEAD))
        drawn.append(scene.text(*(g * 1.08), "gravity", color=INK, fontsize=LABEL_SIZE))
        push = turn @ linear * G_LENGTH
        drawn.append(scene.quiver(0, 0, 0, *push, color=SLOTS[6], linewidth=LINEAR_WIDTH,
                                  arrow_length_ratio=ARROW_HEAD))
        time = (row[1] - span[0][0][1]) / MS_PER_S
        cursor.set_xdata([time, time])
        scene.set_title(caption(row, gravity, linear), fontsize=LABEL_SIZE, color=INK, family="monospace")

    scene.set(xlim=(-BOUND, BOUND), ylim=(-BOUND, BOUND), zlim=(-BOUND, BOUND))
    scene.set_box_aspect((1, 1, 1), zoom=SCENE_ZOOM)
    scene.view_init(*VIEW)
    scene.set_axis_off()
    fig.suptitle(title, fontsize=TITLE_SIZE, color=INK)
    draw(0)
    fig.canvas.draw()
    fig.set_layout_engine("none")
    save_gif(fig, draw, frames, output, fps, colors=ORBIT_GIF_COLORS)


def tilt_animation(pairs, step, output):
    span = tilt_span(pairs, step)
    down = np.array([unit(values[:3] - values[3:]) for _, values in span])

    def setup(strip, seconds):
        for column, name, colour in zip(down.T, "xyz", SLOTS[1:4]):
            strip.plot(seconds, column, color=colour, label=f"down {name}")
        strip.axhline(0, color=MUTED, linewidth=0.7)
        strip.set(xlim=(0, seconds[-1]), ylim=(-1, 1), xlabel="Time (s)", ylabel="Gravity direction, screen axes")
        style_axes(strip, "Tilt")
        strip.grid(color=FAINT, alpha=0.5)
        strip.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR, ncol=3, frameon=False)

    def caption(row, gravity, linear):
        x, y, z = unit(gravity)
        return f"down x {x:+.2f}  y {y:+.2f}  z {z:+.2f}\ntilt {tilt_degrees(gravity):5.1f} deg"

    animate(span, TILT_FRAMES, GIF_FPS, "Tilt: the device turning, gravity fixed", setup, caption, output)


def shake_animation(data, pairs, step, output):
    rows = window(data["trace"], step)
    first, last = rows[0][1], rows[-1][1]
    span = [pair for pair in pairs if pair[0][0] == step and first <= pair[0][1] <= last]

    def setup(strip, seconds):
        strip.plot(seconds, [row[2] for row, _ in span], color=FAINT, linewidth=0.7, label="size")
        strip.plot(seconds, [row[3] for row, _ in span], color=SLOTS[0], label=f"smoothed, {data['tau_ms']} ms")
        for name, colour in (("start_mg", SLOTS[1]), ("full_mg", SLOTS[3])):
            strip.axhline(data[name], color=colour, linestyle="--", linewidth=1,
                          label=f"{name.removesuffix('_mg').upper()} {data[name]} mg")
        strip.set(xlim=(0, seconds[-1]), ylim=(0, None), xlabel="Time (s)", ylabel="Linear acceleration (mg)")
        style_axes(strip, "Shake")
        strip.grid(color=FAINT, alpha=0.5)
        strip.legend(fontsize=LABEL_SIZE, loc="upper left", frameon=False)

    def caption(row, gravity, linear):
        return f"linear {row[2]:6.0f} mg  smoothed {row[3]:6.0f} mg\nshake level {row[4]:.2f}"

    animate(span, SHAKE_FRAMES, SHAKE_FPS, "Shake: linear acceleration (purple) on top of gravity", setup, caption,
            output)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--tilt-step", required=True)
    parser.add_argument("--shake-step", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    data = run_probe(args.cc, SOURCES, args.samples)
    pairs = samples(data, args.samples)
    tilt_animation(pairs, args.tilt_step, args.out / "motion-tilt.gif")
    shake_animation(data, pairs, args.shake_step, args.out / "motion-shake.gif")


if __name__ == "__main__":
    main()

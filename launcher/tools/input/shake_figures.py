"""Plot the shake reader's response; --cc CC [--out PNG].

With --samples FILE --trace PNG it also runs a recorded capture's linear
acceleration through the reader and plots, for each --step NAME:LABEL, the
size, the smoothed size and the level against the START and FULL thresholds.
--animation GIF plays the same windows on the device model, posed
from the capture's gravity, with its acceleration and the reader beside it.
FILE holds one "step ms ax ay az lx ly lz" line in mg per motion sample.
"""
import argparse
from pathlib import Path

import numpy as np
from matplotlib.lines import Line2D

from input_figures import ROOT, run_probe
from device_model import DeviceAnimation, DeviceModel, facing_yaw, pose_from_down
from docs.plot_style import FAINT, GIF_FPS, INK, LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots

PROBE = Path(__file__).with_name("shake_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/main/input/motion/shake.c")
MS_PER_S = 1000
WINDOW_MS = 3000  # of each step: the span whose mean smoothed size is the step's typical one
LEVEL_ROOM = 1.1  # level axis top, past full
SIZE_ROOM = 1.15  # mg axis top, past the window's largest size
TRACE_SIZE = (10, 4.6)
LEVEL_ROW_SHARE = 0.45  # of the size row's height
CLIP_STRIDE = 8  # samples per animation frame
# The acceleration arrow is drawn at this many g per down arrow's length, so hard shaking stays in the scene.
ACCELERATION_G_PER_UNIT = 2
DOWN = INK
ACCELERATION = SLOTS[5]
LEVEL = SLOTS[6]
VECTOR_WIDTH = 2.5


def step(text):
    name, _, label = text.partition(":")
    return name, label or name


def response(data, output):
    fig, axis = subplots(1, 1, (7, 3))
    axis.plot(*zip(*data["points"]), color=SLOTS[0])
    for name in ("start_mg", "full_mg"):
        mg = data[name]
        axis.axvline(mg, color=SLOTS[1], linestyle=":")
        axis.text(mg, 0.1, name.removesuffix("_mg").upper(), ha="right")
    axis.set(xlabel="Smoothed linear acceleration (mg)", ylabel="Shake level", ylim=(0, LEVEL_ROOM), xlim=(0, None))
    style_axes(axis, "Deliberate shake")
    save(fig, output)


def window(trace, name):
    rows = [row for row in trace if row[0] == name]
    if not rows:
        raise SystemExit(f"shake_figures: no samples for step {name}")
    spans = []
    for first, row in enumerate(rows):
        span = [later for later in rows[first:] if later[1] - row[1] < WINDOW_MS]
        if span[-1] is rows[-1]:
            break
        spans.append((sum(later[3] for later in span) / len(span), span))
    spans.sort(key=lambda pair: pair[0])
    return spans[len(spans) // 2][1]


def trace_figure(data, steps, output):
    windows = [(label, window(data["trace"], name)) for name, label in steps]
    top = max(row[2] for _, rows in windows for row in rows) * SIZE_ROOM
    fig, axes = subplots(2, len(windows), TRACE_SIZE, sharey="row", squeeze=False,
                         gridspec_kw={"height_ratios": (1, LEVEL_ROW_SHARE)})
    for column, (label, rows) in enumerate(windows):
        seconds = [(row[1] - rows[0][1]) / MS_PER_S for row in rows]
        size_axis, level_axis = axes[0][column], axes[1][column]
        size_axis.plot(seconds, [row[2] for row in rows], color=FAINT, linewidth=0.7, label="size")
        size_axis.plot(seconds, [row[3] for row in rows], color=SLOTS[0],
                       label=f"smoothed, {data['tau_ms']} ms time constant")
        for name, colour in (("start_mg", SLOTS[1]), ("full_mg", SLOTS[3])):
            size_axis.axhline(data[name], color=colour, linestyle="--", linewidth=1,
                              label=f"{name.removesuffix('_mg').upper()} {data[name]} mg")
        size_axis.set(ylim=(0, top), xlim=(0, WINDOW_MS / MS_PER_S))
        style_axes(size_axis, label)
        level_axis.plot(seconds, [row[4] for row in rows], color=SLOTS[2])
        level_axis.set(ylim=(0, LEVEL_ROOM), xlim=(0, WINDOW_MS / MS_PER_S), xlabel="Time (s)")
        style_axes(level_axis, "")
        for axis in (size_axis, level_axis):
            axis.grid(color=FAINT, alpha=0.5)
    axes[0][0].set_ylabel("Linear acceleration (mg)", fontsize=LABEL_SIZE, color=MUTED)
    axes[1][0].set_ylabel("Shake level", fontsize=LABEL_SIZE, color=MUTED)
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncol=len(labels), fontsize=LABEL_SIZE, frameon=False)
    save(fig, output)


def clips(data, samples, steps):
    """Per step, its label and its window as (trace row, acceleration, linear) in g."""
    lines = samples.read_text(encoding="utf-8").splitlines()
    if len(lines) != len(data["trace"]):
        raise SystemExit("shake_figures: the trace and the samples file differ in length")
    vectors = {}
    for row, line in zip(data["trace"], lines):
        name, ms, *values = line.split()
        if (name, int(ms)) != (row[0], row[1]):
            raise SystemExit(f"shake_figures: sample {name} {ms} is not trace row {row[0]} {row[1]}")
        vectors[name, row[1]] = np.array(values, dtype=float) / MS_PER_S
    picked = []
    for name, label in steps:
        picked.append((label, [(row, *np.split(vectors[row[0], row[1]], 2))
                               for row in window(data["trace"], name)]))
    return picked


def animation_figure(data, model, samples, steps, output):
    picked = clips(data, samples, steps)
    sample_s = data["sample_us"] / (MS_PER_S * MS_PER_S)
    frames, times, edges = [], [], [0.0]
    for label, rows in picked:
        # One turn about down per clip, from its mean down, so the glass faces the camera without spinning.
        yaw = facing_yaw(model.to_device(np.mean([acceleration - linear for _, acceleration, linear in rows], 0)))
        for index, item in enumerate(rows):
            times.append(edges[-1] + (item[0][1] - rows[0][0][1]) / MS_PER_S)
            if index % CLIP_STRIDE == 0:
                frames.append((label, yaw, times[-1], *item))
        edges.append(times[-1] + sample_s)
    trace = [item[0] for _, rows in picked for item in rows]
    animation = DeviceAnimation(model, plots=2)
    size, level = animation.plots
    size.plot(times, [row[2] for row in trace], color=FAINT, linewidth=0.7, label="size")
    size.plot(times, [row[3] for row in trace], color=SLOTS[0], label="smoothed")
    for name, colour in (("start_mg", SLOTS[1]), ("full_mg", SLOTS[3])):
        size.axhline(data[name], color=colour, linestyle="--", linewidth=1, label=name.removesuffix("_mg").upper())
    level.plot(times, [row[4] for row in trace], color=LEVEL)
    top = max(row[2] for row in trace) * SIZE_ROOM
    for axis, title, ylabel, ylim in ((size, "Linear acceleration", "mg", top),
                                      (level, "shake_level()", "level", LEVEL_ROOM)):
        axis.set(xlim=(0, edges[-1]), ylim=(0, ylim))
        axis.set_ylabel(ylabel, fontsize=LABEL_SIZE)
        style_axes(axis, title)
        for edge in edges[1:-1]:
            axis.axvline(edge, color=MUTED, linewidth=0.7)
    middles = [(left + right) / 2 for left, right in zip(edges, edges[1:])]
    size.set_xticks(middles, [])
    level.set_xticks(middles, [label for label, _ in picked], fontsize=LABEL_SIZE)
    slowed = 1 / (GIF_FPS * CLIP_STRIDE * sample_s)
    animation.legend([Line2D([], [], color=DOWN, label="down (1 g)"),
                      Line2D([], [], color=ACCELERATION,
                             label=f"device acceleration (1 g = 1/{ACCELERATION_G_PER_UNIT} of down)")]
                     + size.get_legend_handles_labels()[0])

    def draw(index):
        label, yaw, time, row, acceleration, linear = frames[index]
        down = model.to_device(acceleration - linear)
        pose = pose_from_down(down, yaw)
        # The code's vectors point the way of down, so the device's own acceleration is the linear part negated.
        push = -pose @ model.to_device(linear) / ACCELERATION_G_PER_UNIT * animation.unit
        animation.frame(pose, [(animation.beside, pose @ down / np.linalg.norm(down) * animation.unit, DOWN,
                                VECTOR_WIDTH), (animation.left, push, ACCELERATION, VECTOR_WIDTH)], time,
                        f"{label}    smoothed {row[3]:5.0f} mg    level {row[4]:.2f}    played {slowed:.1f}x slower",
                        chip_axes=False, screen_axes=False)

    animation.save(output, len(frames), draw)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--samples", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--animation", type=Path)
    parser.add_argument("--step", type=step, action="append", default=[], metavar="NAME:LABEL")
    args = parser.parse_args()
    if (args.samples is None) != (args.trace is None) or (args.trace and not args.step):
        parser.error("--samples, --trace and at least one --step go together")
    if args.animation and not args.trace:
        parser.error("--animation needs --samples, --trace and --step")
    if args.out is None and args.trace is None:
        parser.error("--out or --trace is required")
    data = run_probe(args.cc, SOURCES, *([args.samples] if args.samples else []))
    if args.out:
        response(data, args.out)
    if args.trace:
        trace_figure(data, args.step, args.trace)
    if args.animation:
        animation_figure(data, DeviceModel(args.cc), args.samples, args.step, args.animation)


if __name__ == "__main__":
    main()

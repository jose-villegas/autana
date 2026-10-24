"""Plot the shake reader's response; --cc CC [--out PNG].

With --samples FILE --trace PNG it also runs a recorded capture's linear
acceleration through the reader and plots, for each --step NAME:LABEL, the
size, the smoothed size and the level against the START and FULL thresholds.
FILE holds one "step ms ax ay az lx ly lz" line in mg per motion sample.
"""
import argparse
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "math"))
from math_figures import ROOT, run_probe
from docs.plot_style import FAINT, LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots

PROBE = Path(__file__).with_name("shake_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/main/input/motion/shake.c")
MS_PER_S = 1000
WINDOW_MS = 3000  # of each step: the span whose mean smoothed size is the step's typical one
LEVEL_ROOM = 1.1  # level axis top, past full
SIZE_ROOM = 1.15  # mg axis top, past the window's largest size
TRACE_SIZE = (10, 4.6)
LEVEL_ROW_SHARE = 0.45  # of the size row's height


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
    fig, axes = subplots(2, len(windows), TRACE_SIZE, sharey="row",
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--samples", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--step", type=step, action="append", default=[], metavar="NAME:LABEL")
    args = parser.parse_args()
    if (args.samples is None) != (args.trace is None) or (args.trace and not args.step):
        parser.error("--samples, --trace and at least one --step go together")
    if args.out is None and args.trace is None:
        parser.error("--out or --trace is required")
    data = run_probe(args.cc, SOURCES, *([args.samples] if args.samples else []))
    if args.out:
        response(data, args.out)
    if args.trace:
        trace_figure(data, args.step, args.trace)


if __name__ == "__main__":
    main()

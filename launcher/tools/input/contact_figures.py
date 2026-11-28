"""Plot the touch and BOOT contact scripts against the firmware's input events."""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

from input_figures import ROOT, run_probe
from docs.plot_style import LABEL_SIZE, MUTED, SLOTS, figure, save, style_axes, subplots

INPUT = ROOT / "launcher/main/input"
SOURCES = (
    Path(__file__).with_name("contact_figures_probe.c"),
    INPUT / "touch/touch_fsm.c",
    INPUT / "touch/gesture.c",
    INPUT / "buttons/button_fsm.c",
)
TOUCH_SIZE = (10, 4.5)
BUTTON_SIZE = (10, 3.6)
PRESS = ("Pressed", "o", SLOTS[0], 9)
RELEASE = ("Released", "x", SLOTS[7], 8)
SWIPE = ("Home swipe", "*", SLOTS[3], 11)
HOLD = ("Held", "*", SLOTS[3], 11)
WINDOW_ALPHA = 0.17
TRACE_LIMITS = (-0.33, 1.85)
ANNOTATION_HEIGHT = 1.55
CONTACT_DOT_SIZE = 3.5
LIFT_LEVEL = 2


def event_markers(axis, x, y, events, styles):
    for mask, (label, marker, colour, size) in zip(events, styles):
        axis.plot(x[mask], y[mask], linestyle="none", marker=marker, color=colour, markersize=size,
                  markerfacecolor="none" if marker == "o" else colour, label=label)


def binary_traces(axes, time, raw, down, titles, raw_labels, xlabel, *, raw_samples=False):
    for axis, values, colour, title, labels in (
        (axes[0], raw, SLOTS[0], titles[0], raw_labels),
        (axes[1], down, SLOTS[2], titles[1], ("Up", "Down")),
    ):
        if raw_samples and axis is axes[0]:
            axis.plot(time, values, ".", color=colour, markersize=CONTACT_DOT_SIZE)
        else:
            axis.step(time, values, where="post", color=colour)
        axis.set(yticks=(0, 1), yticklabels=labels, ylim=TRACE_LIMITS, xlim=(time[0], time[-1]))
        axis.grid(axis="x", alpha=0.24)
        style_axes(axis, title)
    axes[0].tick_params(labelbottom=False)
    axes[1].set_xlabel(xlabel)


def touch_figure(data, us_per_ms, render, out):
    us, contact, lifted, down, x, y, *flags = np.array(data["rows"]).T
    events = [flag.astype(bool) for flag in flags]
    time = us / us_per_ms
    width, height, zone = (data[key] for key in ("width", "height", "zone"))
    fig = figure(TOUCH_SIZE)
    grid = fig.add_gridspec(1, 2, width_ratios=(0.55, 1))
    panel = fig.add_subplot(grid[0])
    right = grid[1].subgridspec(2, 1)
    raw = fig.add_subplot(right[0])
    fsm = fig.add_subplot(right[1], sharex=raw)
    with Image.open(render) as screen:
        if screen.size != (width, height):
            raise ValueError(f"{render} has size {screen.size}, expected {(width, height)}")
        panel.imshow(screen.convert("RGB"))
    panel.axhspan(height - zone - 0.5, height - 0.5, color=SLOTS[0], alpha=0.34)
    panel.text(width - zone / 4, height - zone / 2, "Home zone", ha="right", va="center",
               color="white", fontsize=LABEL_SIZE)
    samples = contact.astype(bool)
    panel.plot(x[samples], y[samples], ".", color=SLOTS[2], markersize=CONTACT_DOT_SIZE, label="Controller sample")
    styles = (PRESS, RELEASE, SWIPE)
    event_markers(panel, x, y, events, styles)
    style_axes(panel, "Touch screen")
    panel.set_axis_off()

    raw_samples = np.where(lifted, LIFT_LEVEL, contact)
    binary_traces((raw, fsm), time, raw_samples, down, ("Controller contact", "Touch FSM · down"), ("No point", "Point"),
                  f"Time (ms) · {data['poll_hz']} Hz polling", raw_samples=True)
    raw.set(yticks=(0, 1, LIFT_LEVEL), yticklabels=("No point", "Point", "Lift flag"),
            ylim=(TRACE_LIMITS[0], LIFT_LEVEL - TRACE_LIMITS[0]))
    quiet = data["quiet_us"] / us_per_ms
    last_contact = 0
    for t, have_point, lift_flag, released in zip(time, contact, lifted, events[1]):
        if have_point:
            last_contact = t
        if released and not lift_flag:
            for axis in (raw, fsm):
                axis.axvspan(last_contact, last_contact + quiet, color=SLOTS[0], alpha=WINDOW_ALPHA)
            raw.text(last_contact + quiet / 2, ANNOTATION_HEIGHT, f"Quiet {quiet:g} ms", ha="center",
                     va="center", color=MUTED, fontsize=LABEL_SIZE)
    event_markers(fsm, time, down, events, styles)
    handles, labels = panel.get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncols=len(labels), fontsize=LABEL_SIZE, frameon=False)
    save(fig, out)


def button_figure(data, us_per_ms, out):
    us, raw_down, down, pressed, released, held, debounce_start = np.array(data["rows"]).T
    time = us / us_per_ms
    fig, (raw, fsm) = subplots(2, 1, BUTTON_SIZE, sharex=True)
    binary_traces((raw, fsm), time, 1 - raw_down, down, ("BOOT GPIO level", "Debounced BOOT level"),
                  ("0 · down", "1 · up"), "Time (ms)")
    debounce = data["debounce_us"] / us_per_ms
    for start in debounce_start[debounce_start >= 0] / us_per_ms:
        for axis in (raw, fsm):
            axis.axvspan(start, start + debounce, color=SLOTS[0], alpha=WINDOW_ALPHA)
    hold_end, = time[held.astype(bool)]
    hold_start = time[(pressed != 0) & (time < hold_end)][-1]
    fsm.annotate("", (hold_end, ANNOTATION_HEIGHT), (hold_start, ANNOTATION_HEIGHT),
                 arrowprops={"arrowstyle": "|-|", "shrinkA": 0, "shrinkB": 0, "color": MUTED})
    fsm.text((hold_start + hold_end) / 2, ANNOTATION_HEIGHT, f" Hold {data['hold_us'] / us_per_ms:g} ms ",
             ha="center", va="center", color=MUTED, fontsize=LABEL_SIZE,
             bbox={"facecolor": "white", "edgecolor": "none", "pad": 1})
    event_markers(fsm, time, down, [flag.astype(bool) for flag in (pressed, released, held)], (PRESS, RELEASE, HOLD))
    handles, labels = fsm.get_legend_handles_labels()
    handles.append(raw.patches[0])
    labels.append(f"Debounce {debounce:g} ms")
    fig.legend(handles, labels, loc="outside lower center", ncols=len(labels), fontsize=LABEL_SIZE, frameon=False)
    save(fig, out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--random-tap", type=Path)
    parser.add_argument("--tap", metavar="X,Y", help="tap point in screen pixels")
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--touch", type=Path)
    output.add_argument("--button", type=Path)
    args = parser.parse_args()
    if args.button:
        data = run_probe(args.cc, SOURCES)
        button_figure(data["trace"], data["us_per_ms"], args.button)
        return
    if args.random_tap is None or args.tap is None:
        parser.error("--touch requires --random-tap and --tap")
    try:
        x, y = map(int, args.tap.split(","))
    except ValueError:
        parser.error("--tap must be two integers separated by a comma")
    data = run_probe(args.cc, SOURCES, x, y)
    touch_figure(data["trace"], data["us_per_ms"], args.random_tap, args.touch)


if __name__ == "__main__":
    main()

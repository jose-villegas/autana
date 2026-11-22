#!/usr/bin/env python3
"""Plot the rig paths and hand-off for docs/kinematics/Rig.md; --cc CC --out DIR."""
import pathlib

import numpy as np
from matplotlib.ticker import MaxNLocator

from kinematics_figures import PANEL_SIZE, ROOT, VIEW_AZIMUTH, VIEW_ELEVATION

from docs.figures import probe_into_folder
from docs.plot_style import FAINT, INK, LABEL_SIZE, SLOTS, figure, save, style_axes
from docs.producer import Producer

SOURCES = (pathlib.Path(__file__).with_name("rig_figures_probe.c"),
           ROOT / "launcher/packages/kinematics/src/rig/rig.c")
PRODUCER = Producer(area="kinematics", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("rig-flight.png", "rig-handoff.png"))
PATHS = (("Target", INK, "--"), ("RIG_BIND_WORLD", SLOTS[0], "-"),
         ("RIG_BIND_TARGET", SLOTS[1], "-"), ("RIG_BIND_TARGET_YAW", SLOTS[2], "-"))


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    paths = np.asarray(data["paths"]).transpose(1, 2, 0)
    fig = figure((2.5 * PANEL_SIZE, 2.25 * PANEL_SIZE))
    grid = fig.add_gridspec(3, 2, height_ratios=(1.15, 0.20, 1))
    flight = fig.add_subplot(grid[0, :], projection="3d")
    flight.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH, vertical_axis="y")
    flight.set_box_aspect(np.ptp(paths, axis=(0, 2)))
    flight.set(xlabel="x", ylabel="y (up)", zlabel="z")
    flight.tick_params(labelsize=LABEL_SIZE, pad=1)
    for axis in (flight.xaxis, flight.yaxis, flight.zaxis):
        axis.set_major_locator(MaxNLocator(nbins=3))

    legend = fig.add_subplot(grid[1, :])
    legend.set_axis_off()
    top = fig.add_subplot(grid[2, 0])
    side = fig.add_subplot(grid[2, 1], sharex=top)
    for axis, title, label in ((top, "Top view", "z"), (side, "Side view", "y (up)")):
        style_axes(axis, title, square=True)
        axis.set(xlabel="x", ylabel=label)
        axis.set_anchor("N")
        axis.grid(color=FAINT, alpha=0.5)

    for (x, y, z), (label, colour, line) in zip(paths, PATHS):
        flight.plot(x, y, z, color=colour, linestyle=line, label=label)
        flight.scatter(x[0], y[0], z[0], facecolors="white", edgecolors=colour, s=18, depthshade=False)
        for axis, height in ((top, z), (side, y)):
            axis.plot(x, height, color=colour, linestyle=line)
            axis.scatter(x[0], height[0], facecolors="white", edgecolors=colour, s=18, zorder=3)
    handles, labels = flight.get_legend_handles_labels()
    legend.legend(handles, labels, loc="center", ncols=2, frameon=False, fontsize=LABEL_SIZE)
    save(fig, out / "rig-flight.png")

    fig = figure((2 * PANEL_SIZE, PANEL_SIZE))
    axis = fig.add_subplot()
    elapsed, weight = np.asarray(data["handoff"]).T
    style_axes(axis, "Hand-off weight")
    axis.plot(elapsed, weight, color=SLOTS[0])
    over_ms = data["over_ms"]
    axis.axvline(over_ms, linestyle="--", color=FAINT)
    axis.annotate("over_ms", (over_ms, 0.2), xytext=(5, 0), textcoords="offset points",
                  va="center", color=INK, fontsize=LABEL_SIZE)
    axis.set(xlabel="Time since hand-off started (ms)", ylabel="Incoming pose weight",
             xlim=(0, elapsed[-1]), ylim=(0, 1), yticks=(0, 0.5, 1))
    save(fig, out / "rig-handoff.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Plot the rig paths and hand-off for docs/kinematics/Rig.md; --cc CC --out DIR."""
import pathlib

from kinematics_figures import PANEL_SIZE, ROOT, VIEW_AZIMUTH, VIEW_ELEVATION
from docs.figures import probe_into_folder
from docs.plot_style import FAINT, INK, LABEL_SIZE, SLOTS, figure, save, style_axes
from docs.producer import Producer
import numpy as np

SOURCES = (pathlib.Path(__file__).with_name("rig_figures_probe.c"),
           ROOT / "launcher/packages/kinematics/src/rig/rig.c")
PRODUCER = Producer(area="kinematics", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("rig-flight.png", "rig-handoff.png"))
PATHS = (("Target", INK, "--"), ("World Space", SLOTS[0], "-"),
         ("Lock To Target", SLOTS[1], "-"), ("Lock To Target With World Up", SLOTS[2], "-"))


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    paths = np.asarray(data["paths"]).transpose(1, 2, 0)
    fig = figure((2.5 * PANEL_SIZE, 2 * PANEL_SIZE))
    grid = fig.add_gridspec(2, 2)
    flight = fig.add_subplot(grid[0, :], projection="3d")
    flight.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH, vertical_axis="y")
    flight.set_box_aspect(np.ptp(paths, axis=(0, 2)))
    flight.set(xlabel="x", ylabel="y (up)", zlabel="z")
    flight.tick_params(labelsize=LABEL_SIZE)

    top = fig.add_subplot(grid[1, 0])
    side = fig.add_subplot(grid[1, 1], sharex=top)
    for axis, title, label in ((top, "Top view", "z"), (side, "Side view", "y (up)")):
        style_axes(axis, title, square=True)
        axis.set(xlabel="x", ylabel=label)
        axis.grid(color=FAINT, alpha=0.5)

    for (x, y, z), (label, colour, line) in zip(paths, PATHS):
        flight.plot(x, y, z, color=colour, linestyle=line, label=label)
        for axis, height in ((top, z), (side, y)):
            axis.plot(x, height, color=colour, linestyle=line)
            axis.scatter(x[0], height[0], facecolors="white", edgecolors=colour, s=18, zorder=3)
    handles, labels = flight.get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside upper center", ncols=2, frameon=False, fontsize=LABEL_SIZE)
    save(fig, out / "rig-flight.png")

    fig = figure((2 * PANEL_SIZE, PANEL_SIZE))
    axis = fig.add_subplot()
    elapsed, weight = np.asarray(data["handoff"]).T
    style_axes(axis, "Hand-off weight")
    axis.plot(elapsed, weight, color=SLOTS[0])
    axis.axvline(data["blend_ms"], linestyle="--", color=FAINT)
    axis.set(xlabel="Time since hand-off started (ms)", ylabel="Incoming pose weight",
             xlim=(0, elapsed[-1]), ylim=(0, 1), yticks=(0, 0.5, 1))
    save(fig, out / "rig-handoff.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

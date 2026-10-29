#!/usr/bin/env python3
"""Plot one inverse-depth step against view depth for docs/math/Projection.md; --cc CC --out DIR."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, probe_into_folder
from docs.plot_style import LABEL_SIZE, SLOTS, save, subplots

PROBE = pathlib.Path(__file__).with_name("inverse_depth_figures_probe.c")
WIDTH = 2  # panels' worth of width: the depths span two decades


def step_figure(data, output):
    fig, axes = subplots(1, 1, (PANEL_SIZE * WIDTH, PANEL_SIZE + PANEL_TITLE_ROOM))
    axis = axes[0] if hasattr(axes, "__len__") else axes
    for colour, fit in zip(SLOTS, data["fits"]):
        if fit["scale"] == data["near_z"]:
            label = f"scale = near_z = {fit['scale']:g} cm"
        else:
            label = f"nearest at {fit['nearest']:g} cm: scale {fit['scale']:g} cm"
        axis.plot([z / 100 for z in fit["depth"]], fit["step"], color=colour, label=label)
    axis.set_xscale("log")
    axis.set(title="Depth one 16-bit step spans", xlabel="View depth (m)", ylabel="One step (cm)", ylim=(0, None))
    axis.legend(fontsize=LABEL_SIZE, loc="upper left")
    save(fig, output)


def main():
    data, out = probe_into_folder(__doc__, (PROBE,))
    step_figure(data, out / "inverse-depth-step.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

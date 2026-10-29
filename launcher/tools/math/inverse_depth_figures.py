#!/usr/bin/env python3
"""Plot one inverse-depth step against view depth for docs/math/Projection.md; --cc CC --out DIR."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, probe_into_folder
from docs.plot_style import FAINT, LABEL_SIZE, SLOTS, save, subplots
from matplotlib.ticker import FixedLocator, NullFormatter, ScalarFormatter

PROBE = pathlib.Path(__file__).with_name("inverse_depth_figures_probe.c")
PANELS = 2
CM_PER_M = 100
MM_PER_CM = 10
LABEL_GAP = 0.07  # labels at the guide, apart by this share of the panel's height
LABEL_TOP = 0.55  # the first label's height, as a share of the panel: above the curves at the guide
HEADROOM = 1.45  # the y range over the highest curve, so the legend clears them
DEPTH_TICKS = (1, 2, 5, 10, 20, 50)  # metres
LABEL_SHIFT = 0.85  # labels end left of the guide, clear of its dashes


def label(fit, near_z):
    if fit["scale"] == near_z:
        return f"scale = near_z = {fit['scale']:g} cm"
    return f"nearest at {fit['nearest']:g} cm: scale {fit['scale']:g} cm"


def at_guide(fit):
    step = fit["at_guide"]
    return f"{step:.0f} cm" if step >= 1 else f"{step * MM_PER_CM:.1f} mm"


def panel(axis, fits, data, title):
    guide = data["guide"] / CM_PER_M
    axis.axvline(guide, color=FAINT, linestyle="--", linewidth=1)
    for colour, fit in fits:
        axis.plot([z / CM_PER_M for z in fit["depth"]], fit["step"], color=colour, label=label(fit, data["near_z"]))
    axis.set_xscale("log")
    top = HEADROOM * max(max(fit["step"]) for _, fit in fits)
    axis.set(title=title, xlabel="View depth (m)", ylabel="One step (cm)", ylim=(0, top))
    axis.xaxis.set_major_locator(FixedLocator(DEPTH_TICKS))
    axis.xaxis.set_major_formatter(ScalarFormatter())
    axis.xaxis.set_minor_formatter(NullFormatter())
    for row, (colour, fit) in enumerate(sorted(fits, key=lambda f: -f[1]["at_guide"])):
        y = top * (LABEL_TOP - LABEL_GAP * row)
        axis.plot([guide], [fit["at_guide"]], "o", color=colour)
        axis.text(guide * LABEL_SHIFT, y, f"{at_guide(fit)} at {guide:g} m", color=colour, fontsize=LABEL_SIZE,
                  va="center", ha="right")
    axis.legend(fontsize=LABEL_SIZE, loc="upper left")


def step_figure(data, output):
    fig, axes = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    fits = list(zip(SLOTS, data["fits"]))
    panel(axes[0], fits, data, "Every scale")
    panel(axes[1], [f for f in fits if f[1]["scale"] != data["near_z"]], data, "Fitted scales alone")
    fig.suptitle("16-bit inverse-depth step against view depth")
    save(fig, output)


def main():
    data, out = probe_into_folder(__doc__, (PROBE,))
    step_figure(data, out / "inverse-depth-step.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

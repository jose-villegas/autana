"""The contact-disc figure in docs/render/frame/Contact-Shadows.md, drawn from contact_figures_probe.c, which calls
the runtime's contact_blob_draw().

    python launcher/tools/r3d/contact/contact_figures.py --cc CC --out DIR

contact-disc.png: the dithered disc at four strengths, seen straight down, and the share of pixels it halves
against distance from its centre, drawn and modelled.
"""
import math
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from docs.figures import ROOT, probe_into_folder  # noqa: E402
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, figure, save, style_axes  # noqa: E402
from docs.producer import Producer  # noqa: E402

PRODUCER = Producer(area="render", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("contact-disc.png",))

SOURCES = (HERE / "contact_figures_probe.c", ROOT / "launcher/main/render/contact/contact_blob.c")
DISC_SIZE = (9, 5)
DISC_HEIGHTS = (1.1, 1)
DISC_PAPER = 0.92  # the grey a disc darkens, so its edge shows on the page
COVERAGE_BINS = 24
COVERAGE_REACH = 1.2  # distance over the radius the coverage plot reaches, past the rim
MODEL_SAMPLES = 241
BAYER_LEVELS = 16  # a 4x4 Bayer matrix's thresholds


def radial_coverage(mask, side, radius):
    """(ring middles as distance from the centre over the radius, share of each ring's pixels halved)."""
    centre = side / 2
    y, x = np.mgrid[0:side, 0:side] + 0.5
    rho = np.hypot(x - centre, y - centre) / radius
    bins = np.linspace(0, COVERAGE_REACH, COVERAGE_BINS + 1)
    which = np.digitize(rho.ravel(), bins) - 1
    mask = np.asarray(mask).ravel()
    middles = (bins[:-1] + bins[1:]) / 2
    return middles, [mask[which == b].mean() if np.any(which == b) else 0 for b in range(COVERAGE_BINS)]


def disc_plot(data, out):
    disc = data["disc"]
    side, radius, strengths = disc["side"], disc["radius_px"], disc["strengths"]
    fig = figure(DISC_SIZE)
    grid = fig.add_gridspec(2, len(strengths), height_ratios=DISC_HEIGHTS)
    curve = fig.add_subplot(grid[1, :])
    rho = np.linspace(0, COVERAGE_REACH, MODEL_SAMPLES)
    for column, (strength, covered) in enumerate(zip(strengths, disc["covered"])):
        mask = np.array(covered, bool).reshape(side, side)
        axis = fig.add_subplot(grid[0, column])
        axis.imshow(np.where(mask, DISC_PAPER / 2, DISC_PAPER), cmap="gray", vmin=0, vmax=1, interpolation="nearest")
        axis.set_axis_off()
        default = "  (default)" if math.isclose(strength, disc["default"]) else ""
        axis.set_title(f"strength {strength:g}{default}", fontsize=LABEL_SIZE, color=INK)
        colour = SLOTS[column]
        middles, share = radial_coverage(covered, side, radius)
        curve.plot(middles, share, "o", color=colour, markersize=3)
        model = np.ceil(BAYER_LEVELS * strength * np.clip(1 - rho ** 2, 0, None) ** 2) / BAYER_LEVELS
        curve.plot(rho, np.minimum(model, 1), color=colour, linewidth=1, label=f"strength {strength:g}")
    curve.axvline(1, color=FAINT, linewidth=1)
    curve.set_xlabel("distance from the disc's centre over its radius", fontsize=LABEL_SIZE, color=MUTED)
    curve.set_ylabel("share of pixels halved", fontsize=LABEL_SIZE, color=MUTED)
    curve.legend(fontsize=LABEL_SIZE, frameon=False)
    style_axes(curve, "Coverage: drawn (dots) against ceil(16 s (1 - r^2)^2) / 16 (lines)")
    save(fig, out / "contact-disc.png")


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    disc_plot(data, out)


if __name__ == "__main__":
    main()

"""The figures in docs/render/frame/Ambient-Irradiance.md, drawn from ambient_figures_probe.c, which calls the
runtime's SH projection and convolution, its probe-grid sampler and its contact-disc fill.

    python launcher/tools/r3d/ambient/ambient_figures.py --cc CC --out DIR

ambient-probe-sphere.png: one probe projected from a sky with a sun, then a white diffuse sphere lit by the
exact irradiance, by the probe's L1 evaluation and by its constant band alone, with a meridian below.
ambient-probe-blend.png: an example grid's probes over a plane, the blended constant band between and past them,
and the corner weights one point takes.
ambient-contact-disc.png: the dithered disc at four strengths and its coverage against distance from the centre.
"""
import math
import pathlib
import sys

import numpy as np
from matplotlib.patches import Rectangle

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from docs.figures import ROOT, probe_into_folder  # noqa: E402
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, figure, save, style_axes, subplots  # noqa: E402
from docs.producer import Producer  # noqa: E402

PRODUCER = Producer(area="render", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("ambient-probe-sphere.png", "ambient-probe-blend.png", "ambient-contact-disc.png"))

SOURCES = (HERE / "ambient_figures_probe.c", ROOT / "launcher/packages/math/src/sh/sh1.c",
           ROOT / "launcher/main/render/probe/sh_probes.c", ROOT / "launcher/main/render/skin/r3d_skin_light.c",
           ROOT / "launcher/main/render/contact/contact_blob.c")
GAMMA = 1 / 2.2
BACKGROUND = 1.0  # white behind each sphere
EXACT, L1, FLAT = SLOTS[0], SLOTS[1], SLOTS[2]
SPHERE_SIZE = (9, 5.4)
SPHERE_HEIGHTS = (1.25, 1)
BLEND_SIZE = (7, 4.6)
PROBE_DOT = 120  # points squared
POINT_DOT = 40
DISC_SIZE = (9, 5)
DISC_HEIGHTS = (1.1, 1)
DISC_PAPER = 0.92  # the grey a disc darkens, so its edge shows on the page
COVERAGE_BINS = 24
BAYER_LEVELS = 16  # a 4x4 Bayer matrix's thresholds


def encode(linear, white):
    """Linear light over `white` to display values: clipped, then gamma encoded."""
    return np.clip(np.asarray(linear) / white, 0, 1) ** GAMMA


def sphere_picture(rows, white):
    """A probe picture's rows (RGB or None per pixel) as an image on a white page."""
    out = np.full((len(rows), len(rows[0]), 3), BACKGROUND)
    for y, row in enumerate(rows):
        for x, pixel in enumerate(row):
            if pixel is not None:
                out[y, x] = encode(pixel, white)
    return out


def luminance(rgb):
    """Rec. 709 luminance of linear RGB."""
    rgb = np.asarray(rgb)
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def sphere_plot(data, out):
    fig = figure(SPHERE_SIZE)
    grid = fig.add_gridspec(2, 4, height_ratios=SPHERE_HEIGHTS)
    white = max(luminance(row[1]) for row in data["meridian"])
    sky_white = float(np.max([luminance(p) for row in data["sky"] for p in row if p is not None
                              and luminance(p) < luminance(data["coefficients"][0]) * 4]))
    panels = (("sky", "what the probe sees\n(radiance; the sun clips)", sky_white),
              ("exact", "exact irradiance\n(every direction, cosine weighted)", white),
              ("l1", "the probe: L1\n(constant + gradient)", white),
              ("flat", "constant band only\n(flat ambient)", white))
    for column, (key, title, scale) in enumerate(panels):
        axis = fig.add_subplot(grid[0, column])
        axis.imshow(sphere_picture(data[key], scale), interpolation="nearest")
        axis.set_axis_off()
        axis.set_title(title, fontsize=LABEL_SIZE, color=INK)
    axis = fig.add_subplot(grid[1, :])
    degrees = [row[0] for row in data["meridian"]]
    for index, colour, label in ((1, EXACT, "exact"), (2, L1, "L1 probe"), (3, FLAT, "constant band only")):
        axis.plot(degrees, [luminance(row[index]) for row in data["meridian"]], color=colour, label=label)
    axis.set_xlim(0, 180)
    axis.set_xticks(range(0, 181, 30))
    axis.set_ylim(bottom=0)
    axis.set_xlabel("normal's angle from straight up, turning toward the sun's side, degrees", fontsize=LABEL_SIZE,
                    color=MUTED)
    axis.set_ylabel("irradiance / pi (luminance)", fontsize=LABEL_SIZE, color=MUTED)
    axis.legend(fontsize=LABEL_SIZE, frameon=False)
    style_axes(axis, "Along one meridian: L1 keeps the sky-to-ground gradient, not the sun's peak")
    save(fig, out / "ambient-probe-sphere.png")


def blend_plot(data, out):
    grid = data["grid"]
    columns, rows, margin = grid["x"], grid["z"], grid["margin"]
    extent = (-margin, columns - 1 + margin, rows - 1 + margin, -margin)
    plane = np.array(data["grid"]["plane"])
    fig, axis = subplots(1, 1, BLEND_SIZE)
    axis.imshow(encode(plane, 1.0), extent=extent, interpolation="bilinear")
    axis.add_patch(Rectangle(
        (0, 0), columns - 1, rows - 1, fill=False, edgecolor=INK, linewidth=0.8, linestyle="--"))
    xs = [p % columns for p in range(columns * rows)]
    zs = [p // columns for p in range(columns * rows)]
    axis.scatter(xs, zs, s=PROBE_DOT, c=[encode(c, 1.0) for c in grid["probes"]], edgecolors=INK, linewidths=1,
                 zorder=3)
    point = grid["point"]
    for p, weight in enumerate(grid["weights"]):
        if weight > 0:
            axis.plot([point[0], xs[p]], [point[2], zs[p]], color=INK, linewidth=0.8, zorder=2)
            axis.annotate(f"{weight:.2f}", ((point[0] + xs[p]) / 2, (point[2] + zs[p]) / 2), fontsize=LABEL_SIZE,
                          color=INK, ha="center", va="center", backgroundcolor="white", zorder=4)
    axis.scatter([point[0]], [point[2]], s=POINT_DOT, c="white", edgecolors=INK, zorder=5)
    axis.text(columns - 1 + margin * 0.9, -margin * 0.6, "past the last probe: clamped,\nthe edge probes' value",
              fontsize=LABEL_SIZE, color=INK, ha="right", va="center", backgroundcolor="white")
    axis.set_xlabel("x (probe spacing)", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel("z (probe spacing)", fontsize=LABEL_SIZE, color=MUTED)
    style_axes(axis, f"A {columns}x1x{rows} grid seen from above: each point blends its corners' coefficients",
               square=True)
    save(fig, out / "ambient-probe-blend.png")


def radial_coverage(mask, side, radius):
    """(distance from the centre over the radius, share of pixels darkened) in COVERAGE_BINS rings."""
    centre = side / 2
    y, x = np.mgrid[0:side, 0:side] + 0.5
    rho = np.hypot(x - centre, y - centre) / radius
    bins = np.linspace(0, 1.2, COVERAGE_BINS + 1)
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
    rho = np.linspace(0, 1.2, 241)
    for column, (strength, covered) in enumerate(zip(strengths, disc["covered"])):
        mask = np.array(covered, bool).reshape(side, side)
        picture = np.where(mask, DISC_PAPER / 2, DISC_PAPER)
        axis = fig.add_subplot(grid[0, column])
        axis.imshow(picture, cmap="gray", vmin=0, vmax=1, interpolation="nearest")
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
    save(fig, out / "ambient-contact-disc.png")


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    sphere_plot(data, out)
    blend_plot(data, out)
    disc_plot(data, out)


if __name__ == "__main__":
    main()

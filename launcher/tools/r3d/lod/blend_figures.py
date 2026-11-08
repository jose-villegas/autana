"""The blend figures in docs/render/frame/Levels-of-Detail.md, drawn from blend_figures_probe.c: one group walked in past its
switch and back out through the engine's pick, morph weight and dither bands at the default settings.

    python launcher/tools/r3d/lod/blend_figures.py --cc CC --out DIR

lod-blend-weight.png: the coarse surface's share of what is drawn against the pixels the group's error projects,
on the way in and on the way out, blended (solid) and switched (dashed), with where the blending bit is set.
lod-blend-dither.png: the 4x4 screen door at steps along the way in: which pixels draw the coarse level and
which the fine.
"""
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from docs.figures import ROOT, probe_into_folder  # noqa: E402
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, style_axes, subplots, save  # noqa: E402
from docs.producer import Producer  # noqa: E402

PRODUCER = Producer(area="render", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("lod-blend-weight.png", "lod-blend-dither.png"))

SOURCES = (HERE / "blend_figures_probe.c", ROOT / "launcher/main/render/lod/r3d_lod.c")
IN_COLOUR, OUT_COLOUR = SLOTS[0], SLOTS[1]
COARSE_COLOUR, FINE_COLOUR = SLOTS[1], SLOTS[0]
WEIGHT_SIZE = (8, 4)
SWITCH_ALPHA = 0.6  # the dashed plain switch, lod.blend = 0
BAR_ROWS = 0.06  # of the weight axis, per walk's blending bar below zero
DOOR_TILES = 2  # 4x4 tiles a side per screen-door panel
DOOR_SHARES = (0.0, 0.25, 0.5, 0.75, 1.0)  # of the ramp past leave, one panel each
DOOR_SIZE = (8, 2.4)


def band(value, shift, levels):
    """(lo, hi): the thresholds a dither band draws, as r3d_lod_dither_lo/hi read them."""
    return value & (levels - 1), levels - (value >> shift)


def weight_plot(data, out):
    one = data["one"]
    enter = data["tolerance_px"] - data["hysteresis_px"]
    leave = data["tolerance_px"] + data["hysteresis_px"]
    fig, axis = subplots(1, 1, WEIGHT_SIZE)
    for row, (walk, colour, label) in enumerate((("in", IN_COLOUR, "walking in: turns fine at leave"),
                                                 ("out", OUT_COLOUR, "walking out: turns coarse at enter"))):
        steps = data[walk]
        px = np.array([s["px"] for s in steps])
        coarse = np.array([s["coarse"] for s in steps], float)
        share = np.array([1.0 if s["coarse"] else s["weight"] / one for s in steps])
        axis.plot(px, coarse, color=colour, linestyle="--", linewidth=1, alpha=SWITCH_ALPHA)
        axis.plot(px, share, color=colour, label=label)
        blending = np.array([s["blending"] for s in steps], bool)
        bar = -BAR_ROWS * (row + 1)
        axis.fill_between(px, bar - BAR_ROWS / 3, bar + BAR_ROWS / 3, where=blending, color=colour, linewidth=0,
                          step="mid")
    marks = (enter, enter + data["blend_px"], leave, leave + data["blend_px"])
    for x in marks:
        axis.axvline(x, color=FAINT, linewidth=1, zorder=0)
    top = axis.secondary_xaxis("top")
    top.set_xticks(marks, ["enter", "+ blend_px", "leave", "+ blend_px"], fontsize=LABEL_SIZE, color=MUTED)
    top.tick_params(length=0)
    axis.set_ylim(-BAR_ROWS * 3, 1.25)
    axis.set_yticks([0, 0.5, 1])
    axis.set_yticklabels(["0 (fine)", "0.5", "1 (coarse)"])
    axis.set_xlabel("pixels the group's error projects")
    axis.set_ylabel("coarse share of the blend")
    axis.text(axis.get_xlim()[0], -BAR_ROWS * 1.5, " blending bit", va="center", fontsize=LABEL_SIZE, color=MUTED)
    axis.legend(fontsize=LABEL_SIZE, loc="upper center", ncol=2)
    style_axes(axis, "Coarse share across a switch, both ways: blended (solid), switched (dashed)")
    save(fig, out / "lod-blend-weight.png")


def door_plot(data, out):
    levels, shift = data["levels"], data["shift"]
    bayer = np.array(data["bayer"])
    leave = data["tolerance_px"] + data["hysteresis_px"]
    steps = [s for s in data["in"] if not s["coarse"]]
    side = len(bayer) * DOOR_TILES
    fig, axes = subplots(1, len(DOOR_SHARES), DOOR_SIZE)
    for axis, share in zip(axes, DOOR_SHARES):
        px = leave + share * data["blend_px"]
        step = min(steps, key=lambda s: abs(s["px"] - px))
        thresholds = np.tile(bayer, (DOOR_TILES, DOOR_TILES))
        fine_lo, fine_hi = band(step["fine_band"], shift, levels)
        coarse_lo, coarse_hi = band(step["coarse_band"], shift, levels)
        fine = (thresholds >= fine_lo) & (thresholds < fine_hi)
        coarse = (thresholds >= coarse_lo) & (thresholds < coarse_hi)
        if step["blending"] and np.any(fine == coarse):
            raise SystemExit(f"at {step['px']} px the bands share or miss a pixel")
        if not step["blending"]:
            fine, coarse = np.ones_like(fine), np.zeros_like(coarse)
        picture = np.where(coarse[..., None], np.array(rgb(COARSE_COLOUR)), np.array(rgb(FINE_COLOUR)))
        axis.imshow(picture.astype(np.uint8), interpolation="nearest")
        axis.set_xticks(np.arange(-0.5, side, 1), minor=True)
        axis.set_yticks(np.arange(-0.5, side, 1), minor=True)
        axis.grid(which="minor", color=INK, linewidth=0.3)
        axis.tick_params(which="both", length=0, labelbottom=False, labelleft=False)
        axis.set_title(f"{step['px']:.2f} px\n{100 * coarse.mean():.0f}% coarse", fontsize=LABEL_SIZE)
    fig.suptitle("Dither through the ramp past leave: coarse (orange) and fine (blue) never share a pixel",
                 fontsize=LABEL_SIZE + 1)
    save(fig, out / "lod-blend-dither.png")


def rgb(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    weight_plot(data, out)
    door_plot(data, out)


if __name__ == "__main__":
    main()

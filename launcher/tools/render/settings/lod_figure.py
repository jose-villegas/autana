#!/usr/bin/env python3
"""Draw what the LOD settings decide, from default.render.toml's values.

    python launcher/tools/render/settings/lod_figure.py OUT.png

Left: the error below which a fine group turns coarse and the error up to which a
coarse one stays coarse, around the tolerance, and how much a fine group's
vertices blend toward the coarse surface (render/lod/r3d_lod.h's blend:
whole up to the switch the group meets next, none blend_px past it). Right: the share of
the picture's height a coarse group may span, the coverage cap, against its
projected error.
"""
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from docs.plot_style import INK, MUTED, SLOTS, save, style_axes, subplots  # noqa: E402
import render_asset  # noqa: E402
from r3d.lod.pick import PERMILLE  # noqa: E402
# Steps the blend's ramp is shaded in.
RAMP_STEPS = 16

FIGURE_SIZE = (9, 3.2)
# The error axis runs to this many tolerances.
REACH = 2


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    lod = {key.split(".")[1]: value for key, value in render_asset.load(render_asset.DEFAULT).items()
           if key.startswith("lod.")}
    tolerance, hysteresis = lod["tolerance_px"], lod["hysteresis_px"]
    enter, leave, top = tolerance - hysteresis, tolerance + hysteresis, tolerance * REACH
    ramp = lod["blend_px"]
    fig, (band, cap) = subplots(1, 2, FIGURE_SIZE)
    for row, (label, start, end, colour) in enumerate((("fine group turns coarse", 0, enter, SLOTS[0]),
                                                        ("coarse group stays coarse", 0, leave, SLOTS[1]))):
        band.barh(row, end - start, left=start, color=colour, height=0.5)
        band.annotate(f"{end:g} px", (end, row), xytext=(4, 0), textcoords="offset points", va="center")
        band.annotate(label, (0, row + 0.4), fontsize="small", va="bottom")
    for blend_row, (label, whole) in enumerate((("arrived group blends toward coarse", leave),
                                                 ("other fine group blends toward coarse", enter)), start=2):
        band.barh(blend_row, whole, color=SLOTS[2], height=0.5)
        for step in range(RAMP_STEPS):
            band.barh(blend_row, ramp / RAMP_STEPS, left=whole + ramp * step / RAMP_STEPS, color=SLOTS[2],
                      alpha=1 - (step + 0.5) / RAMP_STEPS, height=0.5, linewidth=0)
        band.annotate(f"{whole + ramp:g} px", (whole + ramp, blend_row), xytext=(4, 0), textcoords="offset points",
                      va="center")
        band.annotate(label, (0, blend_row + 0.4), fontsize="small", va="bottom")
    band.axvline(tolerance, color=INK, linewidth=0.8)
    style_axes(band, "hysteresis_px either side of tolerance_px")
    band.set_xlabel(f"projected error, px (tolerance_px {tolerance:g})")
    band.set_xlim(0, top)
    band.set_ylim(-0.5, 3.9)
    band.set_yticks([])
    share = lod["coverage_permille"] / PERMILLE
    cap.fill_between([0, tolerance], 0, share, color=SLOTS[0], alpha=0.35)
    cap.axhline(share, color=INK, linewidth=0.8)
    cap.axvline(tolerance, color=INK, linewidth=0.8)
    cap.annotate("coarse", (tolerance / 2, share / 2), ha="center", va="center")
    cap.annotate("fine: too large or too much error", (top / 2, (1 + share) / 2), ha="center", va="center", color=MUTED)
    style_axes(cap, f"coverage_permille {lod['coverage_permille']}: share of the picture's height")
    cap.set_xlabel("projected error, px")
    cap.set_ylabel("group radius / picture height")
    cap.set_xlim(0, top)
    cap.set_ylim(0, 1)
    save(fig, argv[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The light-against-angle figure of docs/render/animation/Skinned-Lighting.md: one white
light on a normal swept from facing it to facing away, lit by the runtime's
direct sum at unit and blended length and by its irradiance table. The numbers
come from skin_light_figures_probe.c, which calls the runtime's kernels.

    python launcher/tools/r3d/skin_light/skin_light_figures.py --cc CC --out PNG

report_skin_light.sh runs it. Needs matplotlib.
"""

import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, save, subplots  # noqa: E402
# The math figures' folder: its name would shadow the standard math module as a package.
sys.path.insert(0, str(ROOT / "launcher/tools/math"))
from math_figures import run_probe  # noqa: E402

PROBE_SOURCES = (ROOT / "launcher/tools/r3d/skin_light/skin_light_figures_probe.c",
                 ROOT / "launcher/main/render/skin/r3d_skin_light.c")

UNIT, SHORT, TABLE = SLOTS[:3]
SIZE = (7.0, 4.6)  # inches, both panels
HEIGHTS = (3, 1.4)  # the light panel over the table's error
LINE_WIDTH = 1.6
QUARTER_TURN = 90  # degrees: past here the normal faces away and N.L clamps to zero
HALF_TURN = 180


def style(axis):
    axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
    for spine in axis.spines.values():
        spine.set_color(FAINT)
    axis.axvline(QUARTER_TURN, color=FAINT, linewidth=1)
    axis.set_xlim(0, HALF_TURN)
    axis.set_xticks(range(0, HALF_TURN + 1, QUARTER_TURN // 3))


def figure(numbers, output):
    angle, exact, short, table = zip(*numbers["samples"])
    side = numbers["table_side"]
    plot, (light, error) = subplots(2, 1, SIZE, sharex=True, gridspec_kw={"height_ratios": HEIGHTS})
    style(light)
    light.set_title("Light reaching a normal, by its angle to one white light", fontsize=TITLE_SIZE, color=INK)
    light.plot(angle, exact, color=UNIT, linewidth=LINE_WIDTH, label="direct at unit length = renormalised: N.L")
    light.plot(angle, short, color=SHORT, linewidth=LINE_WIDTH,
               label=f"direct, normal blended to {numbers['short_length']:g} long: darker")
    light.plot(angle, table, color=TABLE, linewidth=LINE_WIDTH, linestyle="--",
               label=f"table, {side}x{side} cells, bilinear")
    light.set_ylabel("light", fontsize=LABEL_SIZE, color=MUTED)
    light.text(QUARTER_TURN + 2, max(exact) * 0.9, "facing away: no light", fontsize=LABEL_SIZE, color=MUTED)
    light.legend(fontsize=LABEL_SIZE, frameon=False)
    style(error)
    error.plot(angle, [t - e for t, e in zip(table, exact)], color=TABLE, linewidth=LINE_WIDTH)
    error.axhline(0, color=FAINT, linewidth=1)
    error.set_ylabel("table - exact", fontsize=LABEL_SIZE, color=MUTED)
    error.set_xlabel("angle between normal and light, degrees", fontsize=LABEL_SIZE, color=MUTED)
    save(plot, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cc", required=True, help="the host C compiler")
    parser.add_argument("--out", required=True, help="the PNG to write")
    args = parser.parse_args()
    figure(run_probe(args.cc, PROBE_SOURCES), args.out)


if __name__ == "__main__":
    main()

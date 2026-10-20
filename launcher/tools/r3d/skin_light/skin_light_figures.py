#!/usr/bin/env python3
"""The light-against-angle figure of docs/render/Skinned-Lighting.md: one white
light on a normal swept from facing it to facing away, lit by the runtime's
direct sum at unit and blended length and by its irradiance table. The numbers
come from skin_light_figures_probe.c, which calls the runtime's kernels.

    python launcher/tools/r3d/skin_light/skin_light_figures.py --cc CC --out PNG

report_skin_light.sh runs it. Needs matplotlib.
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.packages import include_roots  # noqa: E402

import matplotlib  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

PROBE = ROOT / "launcher/tools/r3d/skin_light/skin_light_figures_probe.c"
PROBE_SOURCES = (PROBE, ROOT / "launcher/main/render/skin/r3d_skin_light.c")

INK, MUTED, FAINT = "#0b0b0b", "#52514e", "#c9c7c2"
UNIT, SHORT, TABLE = "#2a78d6", "#eb6834", "#1baf7a"
SIZE = (7.0, 4.6)  # inches, both panels
HEIGHTS = (3, 1.4)  # the light panel over the table's error
DPI = 110
TITLE_SIZE = 10
LABEL_SIZE = 8
LINE_WIDTH = 1.6
QUARTER_TURN = 90  # degrees: past here the normal faces away and N.L clamps to zero
HALF_TURN = 180


def probe(cc):
    """Build and run the probe; its JSON is every number the figure shows."""
    with tempfile.TemporaryDirectory() as work:
        binary = pathlib.Path(work) / "skin_light_figures_probe"
        includes = [flag for root in include_roots() for flag in ("-I", str(root))]
        subprocess.run([cc, "-std=gnu2x", "-O1", *includes, *map(str, PROBE_SOURCES), "-lm", "-o", str(binary)],
                       check=True)
        out = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


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
    plot, (light, error) = plt.subplots(2, 1, figsize=SIZE, sharex=True, layout="constrained",
                                        gridspec_kw={"height_ratios": HEIGHTS})
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
    plot.savefig(output, dpi=DPI, metadata={"Software": None})
    plt.close(plot)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cc", required=True, help="the host C compiler")
    parser.add_argument("--out", required=True, help="the PNG to write")
    args = parser.parse_args()
    figure(probe(args.cc), args.out)


if __name__ == "__main__":
    main()

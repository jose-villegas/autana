"""Plot the shake reader's response; --cc CC --out PNG."""
import argparse
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "math"))
from math_figures import ROOT, run_probe
from docs.plot_style import SLOTS, save, style_axes, subplots

PROBE = Path(__file__).with_name("shake_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/main/input/motion/shake.c")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    data = run_probe(args.cc, SOURCES)
    fig, axis = subplots(1, 1, (7, 3))
    axis.plot(*zip(*data["points"]), color=SLOTS[0])
    for name in ("start_mg", "full_mg"):
        mg = data[name]
        axis.axvline(mg, color=SLOTS[1], linestyle=":")
        axis.text(mg, 0.1, name.removesuffix("_mg").upper(), ha="right")
    axis.set(xlabel="Smoothed linear acceleration (mg)", ylabel="Shake level", ylim=(0, 1.1), xlim=(0, None))
    style_axes(axis, "Deliberate shake")
    save(fig, args.out)


if __name__ == "__main__":
    main()

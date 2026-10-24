#!/usr/bin/env python3
"""Plot the library's percentile ranks for docs/math/Stats.md; --cc CC --out DIR."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, run_probe
from docs.plot_style import FAINT, LABEL_SIZE, SLOTS, save, subplots
from matplotlib.patches import Patch

PROBE = pathlib.Path(__file__).with_name("stats_figures_probe.c")
MEDIAN, PERCENTILE = SLOTS[:2]
PANELS = 2
MEDIAN_PERCENT = 50


def ranks_panel(axis, sample, title):
    """Sorted sample as bars; the rank each percentile selects is coloured and named under its bar."""
    ordered = sorted(sample["values"])
    named = {}
    for percent, value in sample["percentiles"]:
        named.setdefault(ordered.index(value), []).append(f"p{percent}")
    median_rank = ordered.index(sample["median"])
    colours = [MEDIAN if rank == median_rank else PERCENTILE if rank in named else FAINT for rank in range(len(ordered))]
    axis.bar(range(len(ordered)), ordered, color=colours)
    axis.set_xticks(range(len(ordered)))
    axis.set_xticklabels([f"{rank}\n" + "\n".join(named.get(rank, [])) for rank in range(len(ordered))],
                         fontsize=LABEL_SIZE)
    axis.set(title=title.format(n=len(ordered), k=median_rank), xlabel="Rank k in ascending order",
             ylabel="Frame time (ms)", ylim=(0, None))


def ranks_figure(data, output):
    fig, axes = subplots(1, PANELS, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    ranks_panel(axes[0], data["odd"], "Odd count, n = {n}\nmedian: rank {k}, the middle")
    ranks_panel(axes[1], data["even"], "Even count, n = {n}\nmedian: rank {k}, upper middle")
    axes[0].legend(handles=[Patch(color=MEDIAN, label=f"median (p{MEDIAN_PERCENT})"),
                            Patch(color=PERCENTILE, label="other percentiles")], fontsize=LABEL_SIZE, loc="upper left")
    save(fig, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    ranks_figure(run_probe(args.cc, (PROBE,)), args.out / "stats-percentile-ranks.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

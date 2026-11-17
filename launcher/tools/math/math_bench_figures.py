#!/usr/bin/env python3
"""Draw benchmark families from committed measurements and annotated plain references."""
import argparse
import re
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf")]
import fast_paths
import math_bench_table as bench
from docs.plot_style import SLOTS, MUTED, LABEL_SIZE, TITLE_SIZE, subplots, style_axes, save
from matplotlib.patches import Patch

REFERENCE_HATCH = "///"
REFERENCE_FILL = "white"
REFERENCE_CUT_FACTOR = 1.25
FIGURE_WIDTH = 12
PANEL_HEIGHT = 2
BAR_WIDTH = 0.18
GROUP_WIDTH = 0.85
VALUE_SIZE = 6
VALUE_GAP = 2
REFERENCE_GAP = 11
VALUE_HEADROOM = 1.35
REFERENCE_HEADROOM = 1.5
CATEGORY_MARGIN = 0.6
BREAK_TOP_FRACTION = 0.94
BREAK_GAP_FRACTION = 0.04
BREAK_RISE_FRACTION = 0.025
BREAK_LINE_WIDTH = 2


def destination_type(kind):
    return kind.split("_to_")[-1]


def number_types(rows):
    kinds = {destination_type(row[2]) for row in rows}
    readme = (ROOT / "docs/math/README.md").read_text(encoding="utf-8")
    section = readme.split("## Number types\n", 1)[1].split("\n## ", 1)[0]
    documented = re.findall(r"^\| `(\w+)` \|", section, re.M)
    ordered = list(dict.fromkeys(kind for kind in documented if kind in kinds))
    ordered.extend(sorted(kinds - set(ordered)))
    if len(ordered) > len(SLOTS):
        raise ValueError(f"too many number types for plot palette: {ordered}")
    return ordered


def panel_groups(rows, references):
    """(family, {tick label: rows}) per non-empty family, ticks in row order."""
    panels = []
    for family in bench.ROW_FAMILIES:
        if family == "calibration":
            continue
        groups = {}
        for row in rows:
            key, label, kind, _, owner = row
            if owner != family or key in references:
                continue
            label = re.sub(r" \([^)]*\)$", "", label)
            if "_to_" in kind:
                label += " " + kind.replace("_to_", "→")
            groups.setdefault(label, []).append(row)
        if groups:
            panels.append((family, groups))
    return panels


def render(rows, measured, items):
    type_order = number_types(rows)
    type_indices = {kind: index for index, kind in enumerate(type_order)}
    metadata = {row[0]: row for row in rows}
    pairs = {item.fast: item.plain for item in items}
    references = set(pairs.values())
    panels = panel_groups(rows, references)
    if not panels:
        raise ValueError("no benchmark families to plot")
    fig, axes = subplots(len(panels), 1, (FIGURE_WIDTH, PANEL_HEIGHT * len(panels)), squeeze=False)
    for axis, (family, groups) in zip(axes.flat, panels):
        limit = max(measured[row[0]][0] for members in groups.values() for row in members) * VALUE_HEADROOM
        near_references = [measured[pairs[row[0]]][0] for members in groups.values() for row in members
                           if row[0] in pairs and limit < measured[pairs[row[0]]][0] <= limit * REFERENCE_CUT_FACTOR]
        limit = max(limit, max(near_references, default=0) * REFERENCE_HEADROOM)
        for tick, members in enumerate(groups.values()):
            bars = []
            for row in sorted(members, key=lambda row: type_indices[destination_type(row[2])]):
                bars.append((row, False))
                if row[0] in pairs:
                    bars.append((metadata[pairs[row[0]]], True))
            width = min(BAR_WIDTH, GROUP_WIDTH / len(bars))
            for offset, (row, reference) in enumerate(bars):
                key, label, kind, _, _ = row
                value = measured[key][0]
                height = min(value, limit) if reference else value
                position = tick + (offset - (len(bars) - 1) / 2) * width
                colour = SLOTS[type_indices[destination_type(kind)]]
                axis.bar(position, height, width, color=REFERENCE_FILL if reference else colour,
                         edgecolor=colour, hatch=REFERENCE_HATCH if reference else None)
                if reference and value > limit:
                    for top in (BREAK_TOP_FRACTION, BREAK_TOP_FRACTION - BREAK_GAP_FRACTION):
                        axis.plot([position - width / 2, position + width / 2],
                                  [limit * (top - BREAK_RISE_FRACTION), limit * top],
                                  color=REFERENCE_FILL, linewidth=BREAK_LINE_WIDTH, solid_capstyle="butt")
                axis.annotate(f"{value:.0f}", (position, height), xytext=(0, VALUE_GAP),
                              textcoords="offset points", ha="center", fontsize=VALUE_SIZE, color=MUTED)
                if reference:
                    axis.annotate(f"{label} ({kind})", (position, height), xytext=(0, REFERENCE_GAP),
                                  textcoords="offset points", ha="center", fontsize=VALUE_SIZE, color=MUTED)
        axis.set_xticks(range(len(groups)), list(groups))
        axis.set_xlim(-CATEGORY_MARGIN, len(groups) - 1 + CATEGORY_MARGIN)
        axis.set_ylim(0, limit)
        axis.set_ylabel("warm cycles/op", fontsize=LABEL_SIZE)
        style_axes(axis, "")
        title = "libm (no fast path)" if family == "libm" else family.capitalize()
        axis.set_title(title, loc="left", fontsize=TITLE_SIZE)
        axis.spines[["top", "right"]].set_visible(False)
    blank = Patch(visible=False)
    handles = [handle for colour in SLOTS[:len(type_order)] for handle in (Patch(color=colour), blank)]
    labels = [label for kind in type_order for label in (kind, "")]
    types = fig.legend(handles, labels, title="type", fontsize=LABEL_SIZE, ncols=len(type_order),
                       loc="outside upper right", frameon=False)
    fig.canvas.draw()
    box = types.get_window_extent().transformed(fig.transFigure.inverted())
    fig.legend([Patch(facecolor=REFERENCE_FILL, edgecolor=SLOTS[0], hatch=REFERENCE_HATCH)], ["plain reference"],
               fontsize=LABEL_SIZE, frameon=False, loc="lower right", bbox_to_anchor=(box.x1, box.y0),
               borderaxespad=0)
    fig.set_layout_engine("none")
    return fig


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=bench.DATA / "math_bench_capture.txt")
    parser.add_argument("--fpu", type=Path, default=bench.DATA / "math_bench_fpu_ops.txt")
    parser.add_argument("--out", type=Path, default=ROOT / "docs/images/math")
    args = parser.parse_args(argv)
    try:
        rows, _ = bench.read_fpu(args.fpu)
        _, measured = bench.parse_capture(args.capture.read_text(encoding="utf-8"), rows)
        fig = render(rows, measured, [item for item in fast_paths.annotations() if item.unit != "bit"])
        save(fig, args.out / "benchmark-families.png")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"math_bench_figures: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

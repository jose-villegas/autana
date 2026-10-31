"""The charts of docs/render/offline/Bake-Quality.md, read back from the fidelity tables doc_tables.py writes.

    python launcher/tools/render/bake_charts.py TABLES RENDER SCENE_ID

TABLES holds <scene>-fidelity.md, -indirect.md, -flat-sampling.md and -indirect-look.md; each makes the chart of
the same name in RENDER. doc_images_demo.sh runs it right after the tables, so a chart always shows its table's
numbers, and a table without the columns read here fails.
"""
import argparse
import pathlib
import re
import sys
import textwrap

import numpy as np
from matplotlib.lines import Line2D

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from doc_tables import LOOK_COLUMNS, SCORE_COLUMNS, parse_table  # noqa: E402
from docs.plot_style import LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, save, subplots  # noqa: E402

MEAN, P95, SSIM, EDGE, INTERIOR = SCORE_COLUMNS
DELTA_E = (MEAN, P95, EDGE, INTERIOR)
PANEL = (3.6, 2.8)  # inches per panel
BAR_SPAN = 0.8
# Category labels wrap between words past this many characters, so neighbours never overlap.
LABEL_WRAP = 8
# The space under a chart its legend takes.
LEGEND_DROP = -0.18


def rows(path, key, columns):
    """{key cell: {header: cell}} of the one table in `path`, which must have `columns`."""
    _, found = parse_table(path.read_text(encoding="utf-8"), key, (key, *columns))
    return {row[key]: row for row in found}


def number(cell):
    return float(cell) if cell else np.nan


def grouped_bars(axis, groups, series, values, colours, title, ylabel):
    """Bars per group, one per series; values[series][group]. Bars start at 0."""
    width = BAR_SPAN / len(series)
    x = np.arange(len(groups))
    for index, name in enumerate(series):
        offset = (index - (len(series) - 1) / 2) * width
        axis.bar(x + offset, values[name], width, label=name, color=colours[index])
    axis.set_xticks(x, [textwrap.fill(group.replace(",", ""), LABEL_WRAP, break_long_words=False) for group in groups])
    axis.set_ylim(bottom=0)
    axis.set_title(title, fontsize=TITLE_SIZE)
    axis.set_ylabel(ylabel, fontsize=LABEL_SIZE)
    axis.tick_params(labelsize=LABEL_SIZE)


def legend_below(axis, columns):
    """The legend under the axis, where it covers no bar."""
    axis.legend(fontsize=LABEL_SIZE, ncol=columns, loc="upper center", bbox_to_anchor=(0.5, LEGEND_DROP), frameon=False)


def fidelity_chart(table, path):
    variants = list(table)
    fig, (error_axis, ssim_axis) = subplots(1, 2, (PANEL[0] * 2.6, PANEL[1]), width_ratios=(3, 2))
    grouped_bars(error_axis, variants, DELTA_E, {name: [number(table[v][name]) for v in variants] for name in DELTA_E},
                 SLOTS, "Colour error against the reference (lower is better)", "dE76")
    legend_below(error_axis, len(DELTA_E))
    grouped_bars(ssim_axis, variants, (SSIM,), {SSIM: [number(table[v][SSIM]) for v in variants]}, SLOTS[4:],
                 "Structure (higher is better)", SSIM)
    ssim_axis.set_ylim(0, 1)
    save(fig, path)


def indirect_chart(table, path):
    """Direct beside indirect light for each mesh, on the mean and p95 error and the SSIM."""
    meshes = list(dict.fromkeys(label.split(",")[0] for label in table))
    lights = ("direct light", "indirect light")
    fig, axes = subplots(1, 3, (PANEL[0] * 3, PANEL[1]))
    for axis, metric, better in zip(axes, (MEAN, P95, SSIM), ("lower", "lower", "higher")):
        values = {light: [number(table[f"{mesh}, {light}"][metric]) for mesh in meshes] for light in lights}
        grouped_bars(axis, meshes, lights, values, (MUTED, SLOTS[0]), f"{metric} ({better} is better)", metric)
    legend_below(axes[0], len(lights))
    axes[2].set_ylim(0, 1)
    save(fig, path)


def sampling_chart(table, path):
    """Each sampling variant's mean and p95 error, best first, coloured by what it changes."""
    variants = sorted(table, key=lambda label: number(table[label][MEAN]))
    # A variant's kind is its label up to the first digit, in the order the sweep lists them.
    kind = lambda label: re.match(r"\D+", label)[0]  # noqa: E731
    kinds = list(dict.fromkeys(kind(label) for label in table))
    if len(kinds) > len(SLOTS):
        raise ValueError(f"{len(kinds)} sampling kinds, colours for {len(SLOTS)}")
    colours = [SLOTS[kinds.index(kind(label))] for label in variants]
    fig, axes = subplots(1, 2, (PANEL[0] * 2, PANEL[1] * 1.8), sharey=True)
    y = np.arange(len(variants))
    for axis, metric in zip(axes, (MEAN, P95)):
        values = [number(table[label][metric]) for label in variants]
        axis.hlines(y, 0, values, color=colours, alpha=0.35)
        axis.scatter(values, y, color=colours, zorder=3)
        axis.set_xlim(left=0)
        axis.set_title(f"{metric} (lower is better)", fontsize=TITLE_SIZE)
        axis.tick_params(labelsize=LABEL_SIZE)
        axis.grid(axis="x", color=MUTED, alpha=0.2)
    axes[0].set_yticks(y, variants)
    axes[0].invert_yaxis()
    handles = [Line2D([], [], marker="o", linestyle="none", color=SLOTS[index], label=name)
               for index, name in enumerate(kinds)]
    fig.legend(handles=handles, fontsize=LABEL_SIZE, ncol=len(kinds), loc="outside lower center", frameon=False)
    save(fig, path)


def look_chart(table, path):
    """Each look's mean error against the physical reference and against its own."""
    looks = list(table)
    columns = {"against the physical reference": LOOK_COLUMNS[0], "against its own reference": LOOK_COLUMNS[1]}
    fig, axis = subplots(1, 1, (PANEL[0] * 1.6, PANEL[1] * 1.25))
    grouped_bars(axis, looks, list(columns), {name: [number(table[look][column]) for look in looks]
                                              for name, column in columns.items()},
                 (MUTED, SLOTS[0]), "Mean dE76 per look (lower is better)", "dE76")
    legend_below(axis, len(columns))
    save(fig, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tables", type=pathlib.Path)
    parser.add_argument("render", type=pathlib.Path)
    parser.add_argument("scene_id")
    args = parser.parse_args()
    write_charts(args.tables, args.render, args.scene_id)


def write_charts(tables, render, scene_id):
    scores = lambda name: rows(tables / f"{scene_id}-{name}.md", "Variant", SCORE_COLUMNS)  # noqa: E731
    chart = lambda name: render / f"{scene_id}-{name}.png"  # noqa: E731
    fidelity_chart(scores("fidelity"), chart("fidelity"))
    indirect_chart(scores("indirect"), chart("indirect"))
    sampling_chart(scores("flat-sampling"), chart("flat-sampling"))
    look_chart(rows(tables / f"{scene_id}-indirect-look.md", "Look", LOOK_COLUMNS), chart("indirect-look"))


if __name__ == "__main__":
    main()

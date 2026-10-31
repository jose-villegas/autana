#!/usr/bin/env python3
"""The frame cost of each picture in each framebuffer layout, as a chart and
the tables in docs/gfx/Targets-and-Modes.md's `gfx-layout-costs` block: one row per
scene family and layout, then every picture in a dropdown. A picture's family is
its scene name up to the first hyphen (sponza-fitted-full, portrait: sponza).
launcher/tools/render/render_doc_images.sh runs it on the captures kept in
docs/render/data/.

    python launcher/tools/render/layout_costs.py --mesh LOG --boot LOG --tables DIR --chart PNG

The mesh capture holds the scene layout suite's `SCENE:LAYOUT:frame` rows, one
per scene, layout and quarter; the boot capture holds the boot animation
suite's checkpoints, one header per checkpoint and layout. Rows, layouts and
their order are read from the captures: a scene, checkpoint or layout added to
a suite shows up here once its capture is kept. Both captures must come from
one build, named by the `BUILD_ID=` line each starts with.
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
sys.path.insert(0, str(ROOT / "launcher/tools/boot_anim"))
sys.path.insert(0, str(ROOT / "launcher/tools"))
from capture import capture_board, quarter_name, require_same_board, require_same_build
from frame_cost import US_PER_MS
from report_boot_anim_perf import parse_capture
from docs.plot_style import FAINT, INK, MUTED, SLOTS, save, style_axes, subplots

BLOCK = "gfx-layout-costs"
FRAME_STAGE = "frame"
BOOT_FRAME_PHASE = "Total"
PERCENT_SCALE = 100
NOT_RUN = "not run"
# How each suite draws its pictures (launcher/main/apps/render_lab/tests/suite_layout_perf.c,
# launcher/test/suites/suite_boot_anim_perf.c): the conclusion table's scene type.
MESH_DRAWS = "`update()`, the frame before sent meanwhile"
BOOT_DRAWS = "a full frame in `frame()`"
CHART_WIDTH = 9
BAR_STEP = 0.35  # rows between one family's layout bars
FAMILY_GAP = 0.5  # rows between families
RANGE_WIDTH = 6
RANGE_ALPHA = 0.45
MEAN_LABEL_GAP = 6  # points above a bar's mean dot
CHART_BASE_HEIGHT = 1.2
MARKER_SIZE = 36

MESH_ROW = re.compile(r"\blayout_perf: (?P<scene>[^:\s]+):(?P<layout>\w+):(?P<stage>\w+) mean (?P<mean>\d+)us\b")


def mesh_costs(path):
    """Mean frame ms by (scene, quarter) row, then by layout, in capture order."""
    rows = {}
    for line in pathlib.Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        match = MESH_ROW.search(line)
        if not match or match["stage"] != FRAME_STAGE:
            continue
        quarter = quarter_name(line)
        row = f"{match['scene']}, {quarter}" if quarter else match["scene"]
        # A test whose first run leaves the heap lower runs again for the
        # heap check alone (launcher/test/timing.c); the first run is the measurement.
        rows.setdefault(row, {}).setdefault(match["layout"], int(match["mean"]) / US_PER_MS)
    if not rows:
        raise ValueError(f"{path}: no scene layout rows")
    return rows


def boot_costs(path):
    """Mean frame ms by checkpoint row, then by layout, in capture order."""
    runs, order, duplicates = parse_capture(str(path))
    if duplicates:
        raise ValueError(f"{path}: checkpoints reported twice: {', '.join(duplicates)}")
    rows = {}
    for key in order:
        run = runs[key]
        total = run["phases"].get(BOOT_FRAME_PHASE)
        if run["mode"] is None or total is None:
            raise ValueError(f"{path}: {key} has no layout or no {BOOT_FRAME_PHASE} line")
        rows.setdefault(f"boot animation, {run['label']}", {})[run["mode"]] = total["avg"] / US_PER_MS
    if not rows:
        raise ValueError(f"{path}: no boot animation checkpoints")
    return rows


def layouts_of(rows):
    """Every layout any row reports, in first-seen order."""
    return list(dict.fromkeys(layout for costs in rows.values() for layout in costs))


def change(costs, layout):
    """Percent change of `layout` against the row's first layout."""
    first = next(iter(costs))
    return (costs[layout] / costs[first] - 1) * PERCENT_SCALE


def ms_change(costs, layout):
    """Ms change of `layout` against the row's first layout."""
    return costs[layout] - costs[next(iter(costs))]


def family(name):
    """A picture's scene name up to its first hyphen: its variants and
    orientations share one family."""
    return name.split(", ")[0].split("-")[0]


def groups(rows):
    """(family, first layout, layout, rows) for each family, then each layout
    after the first its rows report, in capture order."""
    families = {}
    for name in rows:
        families.setdefault(family(name), []).append(name)
    found = []
    for name, members in families.items():
        for layout in layouts_of(rows):
            by_first = {}
            for member in members:
                first = next(iter(rows[member]))
                if layout in rows[member] and layout != first:
                    by_first.setdefault(first, []).append(member)
            found += [(name, first, layout, held) for first, held in by_first.items()]
    return found


def span(values, unit):
    """`low to high` with signs, or the one value when they agree at that precision."""
    low, high = (f"{value:+.1f}{unit}" if unit != "%" else f"{value:+.0f}%" for value in (min(values), max(values)))
    return low if low == high else f"{low} to {high}"


def chart(rows, path):
    """One band per family, a bar per layout from its least to its greatest
    ms change against the first layout and a dot at its mean."""
    layouts = layouts_of(rows)
    if len(layouts) > len(SLOTS):
        raise ValueError(f"{len(layouts)} layouts, {len(SLOTS)} colours")
    found = groups(rows)
    names = list(dict.fromkeys(name for name, _, _, _ in found))
    height = BAR_STEP * len(found) + FAMILY_GAP * len(names)
    figure, axis = subplots(1, 1, (CHART_WIDTH, CHART_BASE_HEIGHT + height))
    axis.axvline(0, color=MUTED, linewidth=1)
    labelled, ticks, y = set(), {}, 0.0
    for index, (name, first, layout, held) in enumerate(found):
        if index and found[index - 1][0] != name:
            y += FAMILY_GAP
        ticks.setdefault(name, []).append(y)
        changes = [ms_change(rows[member], layout) for member in held]
        colour = SLOTS[layouts.index(layout)]
        label = None if layout in labelled else f"{layout} against {first}"
        labelled.add(layout)
        axis.hlines(y, min(changes), max(changes), color=colour, linewidth=RANGE_WIDTH, alpha=RANGE_ALPHA)
        mean = sum(changes) / len(changes)
        axis.scatter([mean], [y], s=MARKER_SIZE, color=colour, label=label, zorder=2)
        axis.annotate(f"{mean:+.1f}", (mean, y), xytext=(0, MEAN_LABEL_GAP), textcoords="offset points",
                      ha="center", fontsize=8, color=INK)
        y += BAR_STEP
    axis.set_yticks([sum(at) / len(at) for at in ticks.values()],
                    [f"{name} ({sum(family(row) == name for row in rows)})" for name in ticks])
    axis.invert_yaxis()
    axis.set_xlabel("frame time change, ms (left is faster): bar from least to greatest, dot at the mean", color=INK)
    style_axes(axis, "Frame cost by framebuffer layout")
    figure.legend(loc="outside lower center", ncol=len(layouts), frameon=False, fontsize=8)
    save(figure, path)


def conclusions(rows, draws):
    """One row per family: how it draws, the layout with the lowest mean ms
    change against the first (the first itself at zero), that change, then
    the other layouts' changes."""
    found = groups(rows)
    firsts = list(dict.fromkeys(first for _, first, _, _ in found))
    against = f"`{firsts[0]}`" if len(firsts) == 1 else "its first layout"
    lines = [f"| Scene | Pictures | Draws | Best layout | Gain against {against} | Others |", "|---|---|---|---|---|---|"]
    for name in dict.fromkeys(name for name, _, _, _ in found):
        held = [entry for entry in found if entry[0] == name]
        first = held[0][1]
        members = [row for row in rows if family(row) == name]
        means = {first: (0.0, 0.0)}
        for _, _, layout, run in held:
            means[layout] = (sum(ms_change(rows[row], layout) for row in run) / len(run),
                             sum(change(rows[row], layout) for row in run) / len(run))
        best = min(means, key=lambda layout: means[layout][0])
        def cell(layout):
            return f"{means[layout][0]:+.1f} ms ({means[layout][1]:+.0f}%)"
        gain = "—" if best == first else cell(best)
        others = ", ".join(f"`{layout}` {cell(layout)}" if layout != first else f"`{layout}`"
                           for layout in means if layout != best)
        kinds = ", ".join(dict.fromkeys(draws[row] for row in members))
        lines.append(f"| {name} | {len(members)} | {kinds} | `{best}` | {gain} | {others} |")
    return lines


def table(rows, chart_name, draws):
    """The conclusions, a row per family; then, in a
    dropdown, one row per picture, one column per layout, each cell after a
    row's first layout giving its change against that first one."""
    layouts = layouts_of(rows)
    lines = [f"![Frame cost by framebuffer layout](../images/render/{chart_name})", "",
             *conclusions(rows, draws)]
    lines += ["", "<details><summary>Every picture</summary>", "",
              "| Picture | " + " | ".join(f"`{layout}`" for layout in layouts) + " |",
             "|---|" + "---|" * len(layouts)]
    for row, costs in rows.items():
        first = next(iter(costs))
        cells = []
        for layout in layouts:
            if layout not in costs:
                cells.append(NOT_RUN)
            elif layout == first:
                cells.append(f"{costs[layout]:.1f}")
            else:
                cells.append(f"{costs[layout]:.1f} ({change(costs, layout):+.0f}%)")
        lines.append(f"| {row} | " + " | ".join(cells) + " |")
    lines += ["", "</details>"]
    note = ("\nBoard ms per frame, mean of each suite's samples. A change is against the "
            "first layout its row reports; a scene row spans its variants and orientations, "
            "and its best layout has the lowest mean.")
    return "\n".join(lines) + "\n" + note


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mesh", required=True, type=pathlib.Path)
    parser.add_argument("--boot", required=True, type=pathlib.Path)
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--chart", required=True, type=pathlib.Path)
    args = parser.parse_args()
    require_same_board({str(path): capture_board(path) for path in (args.mesh, args.boot)})
    require_same_build([args.mesh, args.boot])
    mesh, boot = mesh_costs(args.mesh), boot_costs(args.boot)
    rows = {**mesh, **boot}
    draws = {**dict.fromkeys(mesh, MESH_DRAWS), **dict.fromkeys(boot, BOOT_DRAWS)}
    chart(rows, args.chart)
    args.tables.mkdir(parents=True, exist_ok=True)
    (args.tables / f"{BLOCK}.md").write_text(table(rows, args.chart.name, draws) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

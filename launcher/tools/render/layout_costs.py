#!/usr/bin/env python3
"""The frame cost of each picture in each framebuffer layout, as a chart and
the table in Gfx-and-Presentation.md's `gfx-layout-costs` block.
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
CHART_WIDTH = 9
ROW_HEIGHT = 0.28
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


def chart(rows, path):
    """One line per picture, a dot per layout at its change against the row's first."""
    layouts = layouts_of(rows)
    if len(layouts) > len(SLOTS):
        raise ValueError(f"{len(layouts)} layouts, {len(SLOTS)} colours")
    figure, axis = subplots(1, 1, (CHART_WIDTH, CHART_BASE_HEIGHT + ROW_HEIGHT * len(rows)))
    names = list(rows)
    axis.axvline(0, color=MUTED, linewidth=1)
    for y in range(len(names)):
        axis.axhline(y, color=FAINT, linewidth=0.5, zorder=0)
    for layout, colour in zip(layouts, SLOTS):
        points = [(change(rows[name], layout), y) for y, name in enumerate(names) if layout in rows[name]]
        axis.scatter([x for x, _ in points], [y for _, y in points], s=MARKER_SIZE, color=colour, label=layout,
                     zorder=2)
    axis.set_yticks(range(len(names)), names)
    axis.invert_yaxis()
    axis.set_xlabel("frame time change against the row's first layout, % (left is faster)", color=INK)
    style_axes(axis, "Frame cost by framebuffer layout")
    figure.legend(loc="outside lower center", ncol=len(layouts), frameon=False, fontsize=8)
    save(figure, path)


def table(rows, chart_name):
    """One row per picture, one column per layout; each cell after a row's
    first layout gives its change against that first one."""
    layouts = layouts_of(rows)
    lines = [f"![Frame cost by framebuffer layout](images/render/{chart_name})", "",
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
    note = ("\nBoard ms per frame, mean of each suite's samples. A change is against the "
            "first layout its row reports.")
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
    rows = {**mesh_costs(args.mesh), **boot_costs(args.boot)}
    chart(rows, args.chart)
    args.tables.mkdir(parents=True, exist_ok=True)
    (args.tables / f"{BLOCK}.md").write_text(table(rows, args.chart.name) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""The frame cost of each picture in each framebuffer layout, as the table in
Gfx-and-Presentation.md's `gfx-layout-costs` block.
launcher/tools/render/render_doc_images.sh runs it on the captures kept in
docs/render/data/.

    python launcher/tools/render/layout_costs.py --mesh LOG --boot LOG --tables DIR

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
from capture import build_id, capture_board, quarter_name, require_same_board
from report_boot_anim_perf import parse_capture

BLOCK = "gfx-layout-costs"
FRAME_STAGE = "frame"
BOOT_FRAME_PHASE = "Total"
US_PER_MS = 1000
PERCENT_SCALE = 100
NOT_RUN = "not run"

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
        layouts = rows.setdefault(row, {})
        if match["layout"] in layouts:
            raise ValueError(f"{path}: {row} reports {match['layout']} twice")
        layouts[match["layout"]] = int(match["mean"]) / US_PER_MS
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


def capture_build(paths):
    """The build id each capture's `BUILD_ID=` line names, one for all."""
    ids = {str(path): build_id(pathlib.Path(path).read_text(encoding="utf-8", errors="replace")) for path in paths}
    if len(set(ids.values())) != 1:
        raise ValueError("captures from different builds: " + "; ".join(f"{path}: {build}" for path, build in ids.items()))
    return next(iter(ids.values()))


def table(rows, build):
    """One row per picture, one column per layout; each cell after a row's
    first layout gives its change against that first one."""
    layouts = list(dict.fromkeys(layout for costs in rows.values() for layout in costs))
    lines = ["| Picture | " + " | ".join(f"`{layout}`" for layout in layouts) + " |",
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
                change = (costs[layout] / costs[first] - 1) * PERCENT_SCALE
                cells.append(f"{costs[layout]:.1f} ({change:+.0f}%)")
        lines.append(f"| {row} | " + " | ".join(cells) + " |")
    note = (f"\nBoard ms per frame, mean of each suite's samples, build `{build}`. A change is against the "
            "first layout its row reports.")
    return "\n".join(lines) + "\n" + note


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mesh", required=True, type=pathlib.Path)
    parser.add_argument("--boot", required=True, type=pathlib.Path)
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    args = parser.parse_args()
    require_same_board({str(path): capture_board(path) for path in (args.mesh, args.boot)})
    build = capture_build([args.mesh, args.boot])
    rows = {**mesh_costs(args.mesh), **boot_costs(args.boot)}
    args.tables.mkdir(parents=True, exist_ok=True)
    (args.tables / f"{BLOCK}.md").write_text(table(rows, build) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""What each kept cluster, transformed vertex, submitted triangle and set-up
triangle costs one core, apart, from the board's cull-cost suite
(render_lab tests/suite_cull_cost.c). launcher/tools/render/render_doc_images.sh
runs it on the capture kept in docs/render/data/.

    python launcher/tools/render/cull_cost.py LOG --tables DIR --chart PNG

Each `cull_cost:` line is one pose of one variant: the counts and the best
one-core time of each stage. Each stage is fitted by non-negative least
squares over every line to the counts that can drive it:

    cull      = a + b * nodes visited + c * clusters tested
    transform = a + b * clusters kept + c * vertices transformed
    set-up    = a + b * clusters kept + c * triangles submitted + d * triangles set up

where set-up is the draw stopped once each triangle is set up, so pixels
are not in it. The halved and mirrored variants move submitted and set-up
triangles apart from clusters, which a capture of baked poses alone cannot do.
Writes the `cull-cost` table for Mesh-Rendering.md and a chart of each
fit against what was measured.
"""

import argparse
import pathlib
import re
import sys

import numpy as np
from scipy.optimize import nnls

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.plot_style import INK, SLOTS, save, style_axes, subplots
from render.doc_tables import markdown

BLOCK = "cull-cost"
LINE = re.compile(r"cull_cost: (.*)")
FIELD = re.compile(r"(\w+)=(\S+)")
NUMBER = re.compile(r"-?\d+(\.\d+)?")
NANOSECONDS_PER_MICROSECOND = 1000
FIGURE_SIZE = (10, 3.4)
MARKER_SIZE = 9

# Each stage: its time field, then the count fields that drive it.
STAGES = (
    ("cull", "cull_us", ("nodes", "tested")),
    ("transform", "transform_us", ("clusters", "vertices")),
    ("set-up", "setup_us", ("clusters", "submitted", "set_up")),
)
TERMS = {
    "nodes": "node visited",
    "tested": "cluster tested",
    "clusters": "cluster kept",
    "vertices": "vertex transformed",
    "submitted": "triangle submitted",
    "set_up": "triangle set up",
}


def read(path):
    """Each line's fields, the numeric ones as numbers."""
    rows = []
    for line in pathlib.Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        found = LINE.search(line)
        if found:
            rows.append({key: float(value) if NUMBER.fullmatch(value) else value
                         for key, value in FIELD.findall(found[1])})
    if not rows:
        raise SystemExit(f"{path}: no cull_cost lines")
    return rows


def fit(rows, time_key, terms):
    """Constant then one weight per term, microseconds, and the fitted times."""
    design = np.array([[1.0] + [row[t] for t in terms] for row in rows])
    measured = np.array([row[time_key] for row in rows])
    weights, _ = nnls(design, measured)
    return weights, design @ weights, measured


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--chart", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    rows = read(args.log)

    table = []
    fig, axes = subplots(1, len(STAGES), FIGURE_SIZE)
    scenes = sorted({row["scene"] for row in rows})
    for axis, (stage, time_key, terms) in zip(axes, STAGES):
        weights, predicted, measured = fit(rows, time_key, terms)
        worst = float(np.max(np.abs(predicted - measured)))
        for term, weight in zip(terms, weights[1:]):
            table.append((stage, f"per {TERMS[term]}", f"{weight * NANOSECONDS_PER_MICROSECOND:.0f} ns"))
        table.append((stage, "constant", f"{weights[0]:.0f} µs"))
        table.append((stage, "worst error over the poses", f"{worst:.0f} µs"))
        for colour, scene in zip(SLOTS, scenes):
            mine = [i for i, row in enumerate(rows) if row["scene"] == scene]
            axis.scatter(measured[mine], predicted[mine], s=MARKER_SIZE, color=colour, label=scene)
        top = float(max(measured.max(), predicted.max()))
        axis.plot([0, top], [0, top], color=INK, linewidth=0.8)
        style_axes(axis, f"{stage}, one core")
        axis.set_xlabel("measured µs")
        axis.set_ylabel("fitted µs")
        axis.set_xlim(0, top)
        axis.set_ylim(0, top)
    axes[0].legend(frameon=False, fontsize="small")
    save(fig, args.chart)

    args.tables.mkdir(parents=True, exist_ok=True)
    body = markdown(["Stage", "Term", "One core"], table)
    (args.tables / f"{BLOCK}.md").write_text(body + "\n", encoding="utf-8")
    print(body)


if __name__ == "__main__":
    main()

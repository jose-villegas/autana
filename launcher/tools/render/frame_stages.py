#!/usr/bin/env python3
"""Each frame stage of the scene as shipped, and what each debug view adds on
top, as a chart and a table for Render-Pipeline.md's `pipeline-frame-stages`
block. launcher/tools/render/render_doc_images.sh runs it on the captures kept
in docs/render/data/.

    python launcher/tools/render/frame_stages.py --present LOG --views PATTERN --tables DIR --chart PNG

LOG holds the frame-cost report windows of the scene as shipped. PATTERN names
one capture per debug view, `{view}` standing for the view's name; the names
are read from the view table in render_context.c, so a view added there shows
up here, listed as not captured until its capture is kept.
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from r3d.dynres_report import frame_cost_means, frame_cost_windows

VIEW_TABLE = ROOT / "launcher/main/render/context/render_context.c"
FRAME_RATES = (30, 60)  # the budgets marked on the chart, frames per second
MS_PER_SECOND = 1000
BLOCK = "pipeline-frame-stages"
LABEL_SHARE = 0.07  # a segment this share of the axis or wider carries its value inside
INK, MUTED, GHOST = "#0b0b0b", "#52514e", "#b8b7b2"
# The reference categorical palette, one slot per bracket in report order.
SLOTS = ("#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948")


def view_names(source=VIEW_TABLE):
    table = re.search(r"\bviews\[\] = \{(.*?)\n\};", pathlib.Path(source).read_text(encoding="utf-8"), re.S)
    names = re.findall(r'\{"(\w+)",', table[1]) if table else []
    if not names:
        raise ValueError(f"{source}: no debug view table")
    return names


def stage_order(shipped, views):
    """The shipped brackets, each view's own bracket placed where its report has it."""
    order = [name for name, _ in shipped]
    for brackets in views.values():
        report = [name for name, _ in brackets]
        for position, name in enumerate(report):
            if name not in order:
                after = report[position + 1:]
                order.insert(next((i for i, known in enumerate(order) if known in after), len(order)), name)
    return order


def stage_costs(present, pattern, names):
    shipped = frame_cost_means(present)
    if shipped is None:
        raise ValueError(f"{present}: no capture of the scene as shipped")
    views = {}
    for name in names:
        brackets = frame_cost_means(pattern.format(view=name))
        if brackets is not None:
            views[name] = brackets
    order = stage_order(shipped, views)
    base = dict(shipped)
    added = {name: {stage: dict(brackets).get(stage, 0.0) - base.get(stage, 0.0) for stage in order}
             for name, brackets in views.items()}
    return order, base, added


def table(order, base, added, names, windows, chart_name):
    headers = ["Stage", "As shipped", *(f"{name} view adds" for name in added)]
    lines = [f"![Frame stages as shipped, and what each debug view adds](../images/render/{chart_name})", "",
             "| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    for stage in order:
        lines.append(f"| {stage} | " + " | ".join(
            [f"{base[stage]:.2f}" if stage in base else "not run",
             *(f"{delta[stage]:+.2f}" for delta in added.values())]) + " |")
    lines.append("| total | " + " | ".join(
        [f"{sum(base.values()):.2f}", *(f"{sum(delta.values()):+.2f}" for delta in added.values())]) + " |")
    missing = [name for name in names if name not in added]
    counts = f"{min(windows)}" if min(windows) == max(windows) else f"{min(windows)} to {max(windows)}"
    note = f"\nBoard ms per frame, mean of {counts} report windows per capture; a view adds the difference " \
           "between its capture and the scene as shipped."
    if missing:
        note += " Not captured: " + ", ".join(missing) + "."
    return "\n".join(lines) + "\n" + note


def chart(order, base, added, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch

    if len(order) > len(SLOTS):
        raise ValueError(f"{len(order)} stages, {len(SLOTS)} colours")
    colour = dict(zip(order, SLOTS))
    shipped = sum(base.values())
    rows = ["as shipped", *(f"with the {name} view" for name in added)]
    figure, axis = plt.subplots(figsize=(10, 1.8 + 0.6 * len(rows)), layout="constrained")
    widest = max([shipped + sum(d for d in delta.values() if d > 0) for delta in added.values()] + [shipped])

    def segment(row, stage, value, left, **style):
        axis.barh(row, value, left=left, color=colour[stage], height=0.6, **style)
        if value >= LABEL_SHARE * widest:
            axis.text(left + value / 2, row, f"{value:.1f}", ha="center", va="center", fontsize=8, color=INK)
        return left + value

    left = 0.0
    for stage in order:
        if stage in base:
            left = segment(0, stage, base[stage], left, edgecolor="white", linewidth=1.5)
    axis.annotate(f"{shipped:.1f} ms", (shipped, 0), xytext=(6, 0), textcoords="offset points", va="center")
    for row, delta in enumerate(added.values(), start=1):
        axis.barh(row, shipped, color="none", edgecolor=GHOST, linestyle="--", linewidth=1, height=0.6)
        left = shipped
        for stage in order:
            if delta[stage] > 0:
                left = segment(row, stage, delta[stage], left, alpha=0.45, hatch="///", edgecolor=colour[stage],
                               linewidth=1)
        axis.annotate(f"+{sum(delta.values()):.1f} ms, optional", (left, row), xytext=(6, 0),
                      textcoords="offset points", va="center")
    for rate in FRAME_RATES:
        budget = MS_PER_SECOND / rate
        axis.axvline(budget, color=MUTED, linestyle=":", linewidth=1)
        axis.annotate(f"{rate} fps, {budget:.1f} ms", (budget, -0.45), xytext=(3, 0),
                      textcoords="offset points", fontsize=8, color=MUTED, va="bottom")
    axis.set_yticks(range(len(rows)), rows)
    axis.set_ylim(len(rows) - 0.5, -0.9)
    axis.set_xlim(0, widest * 1.2)
    axis.set_xlabel("board ms per frame")
    for side in ("top", "right", "left"):
        axis.spines[side].set_visible(False)
    axis.tick_params(axis="y", length=0)
    figure.legend(handles=[Patch(color=colour[stage], label=stage) for stage in order], loc="outside lower center",
                  ncol=len(order), frameon=False, fontsize=8)
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=110)
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--present", required=True, type=pathlib.Path)
    parser.add_argument("--views", required=True, help="capture path with {view} for the view's name")
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--chart", required=True, type=pathlib.Path)
    args = parser.parse_args()
    names = view_names()
    order, base, added = stage_costs(args.present, args.views, names)
    chart(order, base, added, args.chart)
    args.tables.mkdir(parents=True, exist_ok=True)
    windows = [len(frame_cost_windows(path)) for path in
               [args.present, *(args.views.format(view=name) for name in added)]]
    body = table(order, base, added, names, windows, args.chart.name)
    (args.tables / f"{BLOCK}.md").write_text(body + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

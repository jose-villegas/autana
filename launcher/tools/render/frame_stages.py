#!/usr/bin/env python3
"""Each frame stage of the scene as shipped, what is inside transform and
draw, and what each debug view adds on top, as charts and tables for
docs/render/frame/README.md's `pipeline-frame-stages` block.
launcher/tools/render/render_doc_images.sh runs it on the captures kept in
docs/render/data/.

    python launcher/tools/render/frame_stages.py --present LOG --laps LOG --views PATTERN --tables DIR --chart PNG

--present holds the frame-cost report windows of the scene as shipped.
--laps holds a --frame-laps image's windows of the same scene: its lap lines
split a bracket into the laps named `<bracket>.<phase>`, each
with what the laps themselves cost taken out and shown apart. What the frame
core's laps leave of their parent is untimed; more than UNTIMED_SHARE of it
fails the run. PATTERN names one capture per debug view,
`{view}` standing for the view's name; the names are read from the view table
in render_context.c, so a view added there shows up here, listed as not
captured until its capture is kept.
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
sys.path.insert(0, str(ROOT / "launcher/tools"))
import frame_cost
from frame_cost import MS_PER_SECOND
from capture import capture_board, require_same_board
from docs.plot_style import INK, MUTED, SLOTS, save, subplots
from render.doc_tables import collapsed

VIEW_TABLE = ROOT / "launcher/main/render/context/render_context.c"
FRAME_RATES = (30, 60)  # the budgets marked on the chart, frames per second
BLOCK = "pipeline-frame-stages"
LABEL_SHARE = 0.07  # a segment this share of the axis or wider carries its value inside
PERCENT_SCALE = 100
LABEL_SEPARATOR = " · "
FIGURE_SIZE = (10, 2.4)
BAR_HEIGHT = 0.6
AXIS_HEADROOM = 0.2
AXIS_TICKS = 5
ADDED_BASE_LABEL = "base cost"
BUDGET_LABEL_SIZE = 8
FRAME_CORE = 0  # the core the frame task, and so every bracket, runs on
UNTIMED_SHARE = 0.02  # the most of a parent its laps may leave untimed
UNTIMED_LABEL = "untimed"
UNTIMED_COLOUR = "#c8c8c8"
LAPS_LABEL = "laps"  # what the laps themselves cost, taken out of the phases they time
LAPS_COLOUR = "#8c8c8c"
SUB_STAGE_FIGURE_SIZE = (10, 4)
SUB_STAGE_SUFFIX = "sub-stages"
TINT = 0.5  # the second round of sub-stage colours: each slot colour this far towards white
SUB_STAGE_LEGEND_ANCHOR = (0, 1)  # above the bar, left, under its title
SUB_STAGE_TITLE_PAD = 20  # points between a title and the bar, room for the legend


def view_names(source=VIEW_TABLE):
    table = re.search(r"\bviews\[\] = \{(.*?)\n\};", pathlib.Path(source).read_text(encoding="utf-8"), re.S)
    # A row names its view first: {"depth", ...} or {.name = "depth", ...}.
    names = re.findall(r'\{(?:\.name = )?"(\w+)"', table[1]) if table else []
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
    require_same_board({str(path): capture_board(path)
                        for path in [present, *(pattern.format(view=name) for name in names)]
                        if pathlib.Path(path).exists()})
    shipped = frame_cost.frame_cost_means(present)
    if shipped is None:
        raise ValueError(f"{present}: no capture of the scene as shipped")
    views = {}
    for name in names:
        brackets = frame_cost.frame_cost_means(pattern.format(view=name))
        if brackets is not None:
            views[name] = brackets
    order = stage_order(shipped, views)
    base = dict(shipped)
    added = {name: {stage: dict(brackets).get(stage, 0.0) - base.get(stage, 0.0) for stage in dict.fromkeys([*(stage for stage, _ in brackets), *base])}
             for name, brackets in views.items()}
    return order, base, added


def chart_path(path, view):
    path = pathlib.Path(path)
    return path.with_name(f"{path.stem}.{view}{path.suffix}")


def stage_shares(costs, shipped):
    return {stage: value * PERCENT_SCALE / shipped for stage, value in costs.items()}


def share_percent(value):
    return float(format(value, ".0f"))


def share_text(value, signed=False):
    return format(share_percent(value), "+.0f" if signed else ".0f") + "%"


def table(order, base, added, names, windows, chart_name, inside=""):
    """The shipped bar, then `inside`, what is inside its stages, then the
    debug views' bars and every stage's milliseconds, folded away."""
    shipped = sum(base.values())
    shares = stage_shares(base, shipped)
    headers = ["Stage", "As shipped", "Share", *(f"{name} view adds" for name in added)]
    lines = []
    for name in names:
        if name in added:
            lines.extend([f"![Frame stages with the {name} view](../images/render/{chart_path(chart_name, name).name})", ""])
    rows = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    for stage in order:
        rows.append(f"| {stage} | " + " | ".join(
            [f"{base[stage]:.2f}" if stage in base else "not run",
             share_text(shares[stage]) if stage in base else "not run",
             *(f"{delta.get(stage, 0.0):+.2f}" for delta in added.values())]) + " |")
    rows.append("| total | " + " | ".join(
        [f"{shipped:.2f}", share_text(sum(shares.values())), *(f"{sum(delta.values()):+.2f}" for delta in added.values())]) + " |")
    missing = [name for name in names if name not in added]
    counts = f"{min(windows)}" if min(windows) == max(windows) else f"{min(windows)} to {max(windows)}"
    note = f"\nBoard ms per frame, mean of {counts} report windows per capture; a view adds the difference " \
           "between its capture and the scene as shipped."
    if missing:
        note += " Not captured: " + ", ".join(missing) + "."
    head = f"![Frame stages as shipped](../images/render/{chart_name})\n\n" + (inside + "\n" if inside else "")
    return head + collapsed("Debug views and every stage in milliseconds",
                            "\n".join(lines) + "\n" + "\n".join(rows) + "\n" + note + "\n").rstrip("\n")


def chart(order, base, added, path):
    from matplotlib.ticker import MaxNLocator

    if len(order) > len(SLOTS):
        raise ValueError(f"{len(order)} stages, {len(SLOTS)} colours")
    colour_order = list(base)
    for delta in added.values():
        colour_order.extend(stage for stage in delta if stage not in colour_order)
    colour = dict(zip(colour_order, SLOTS))
    shipped = sum(base.values())
    slowest_budget = max(MS_PER_SECOND / rate for rate in FRAME_RATES)

    def segment(stage, value, left, **style):
        if share_percent(shares.get(stage, 0)) <= 0:
            return left
        bars = axis.barh(0, value, left=left, color=colour[stage], height=BAR_HEIGHT, **style)
        bars[0].set_label(f"{stage} {share_text(shares[stage], signed=delta is not None)}")
        drawn.append(bars[0])
        if value >= LABEL_SHARE * extent:
            label = f"{value:.1f}{LABEL_SEPARATOR}{share_text(shares[stage], signed=delta is not None)}"
            axis.text(left + value / 2, 0, label, ha="center", va="center", fontsize=8, color=INK)
        return left + value

    for name, delta in [(None, None), *added.items()]:
        shares = stage_shares(base if delta is None else delta, shipped)
        drawn = []
        figure, axis = subplots(1, 1, FIGURE_SIZE)
        if delta is None:
            extent = max(shipped, slowest_budget)
            start = 0.0
            left = start
            for stage in order:
                left = segment(stage, base.get(stage, 0), left, edgecolor="white", linewidth=1.5)
            label = f"{shipped:.1f} ms"
            title = "As shipped"
            output = pathlib.Path(path)
        else:
            net = sum(delta.values())
            extent = max(sum(value for value in delta.values() if value > 0), abs(net)) or slowest_budget
            start = shipped
            left = start
            for stage in order:
                left = segment(stage, delta.get(stage, 0), left, alpha=0.45, hatch="///", edgecolor=colour[stage],
                               linewidth=1)
            label = f"{net:+.1f} ms ({share_text(sum(shares.values()), signed=True)})"
            title = f"With the {name} view"
            output = chart_path(path, name)
        axis.annotate(label, (left, 0), xytext=(6, 0), textcoords="offset points", va="center")
        end = start + extent * (1 + AXIS_HEADROOM)
        axis.set_xlim(start, end)
        if delta is not None:
            offsets = MaxNLocator(nbins=AXIS_TICKS).tick_values(0, end - start)
            offsets = [offset for offset in offsets if 0 < offset <= end - start]
            axis.set_xticks([start + offset for offset in offsets], [f"+{offset:g}" for offset in offsets])
            axis.annotate(ADDED_BASE_LABEL, (start, -BAR_HEIGHT / 2),
                          xytext=(start + extent * AXIS_HEADROOM, -BAR_HEIGHT),
                          fontsize=BUDGET_LABEL_SIZE, color=MUTED, ha="left", va="center",
                          arrowprops=dict(arrowstyle="->", color=MUTED,
                                          connectionstyle="angle,angleA=180,angleB=90,rad=0"))
        for rate in FRAME_RATES:
            budget = MS_PER_SECOND / rate
            if not start <= budget <= end:
                continue
            axis.axvline(budget, color=MUTED, linestyle=":", linewidth=1)
            axis.annotate(f"{rate} fps, {budget:.1f} ms", (budget, -BAR_HEIGHT * 0.75), xytext=(3, 0),
                          textcoords="offset points", fontsize=BUDGET_LABEL_SIZE, color=MUTED, va="bottom")
        axis.set_title(title)
        axis.set_yticks([])
        axis.set_ylim(BAR_HEIGHT * 5 / 6, -BAR_HEIGHT * 1.5)
        axis.set_xlabel("board ms per frame" if delta is None else "added board ms per frame")
        for side in ("top", "right", "left"):
            axis.spines[side].set_visible(False)
        axis.tick_params(axis="y", length=0)
        figure.legend(handles=drawn, loc="outside lower center", ncol=max(1, len(drawn)), frameon=False, fontsize=8)
        save(figure, output)


def sub_stages(path):
    """{parent: (parent ms, {phase: {core: ms}}, untimed ms)}, in report
    order: each bracket of the perf-scope capture that its laps name, the laps
    on every core, LAPS_LABEL last for what
    they cost themselves, and what the frame core's laps leave of it."""
    brackets = dict(frame_cost.frame_cost_means(path) or [])
    laps = frame_cost.frame_laps_means(path)
    if FRAME_CORE not in laps:
        raise ValueError(f"{path}: no laps on cpu{FRAME_CORE}")
    named = {name.rpartition(".")[0] for core in laps.values() for name in core}
    strays = sorted(parent for parent in named if parent not in brackets)
    if strays:
        raise ValueError(f"{path}: laps inside no bracket of the capture: {', '.join(strays)}")
    found = {}
    for parent in (bracket for bracket in brackets if bracket in named):
        prefix = parent + "."
        phases = list(dict.fromkeys(name[len(prefix):] for core in laps.values() for name in core
                                    if name.startswith(prefix)))
        per_core = {phase: {core: laps[core].get(prefix + phase, (0.0, 0.0))[0] for core in laps} for phase in phases}
        per_core[LAPS_LABEL] = {core: sum(laps[core].get(prefix + phase, (0.0, 0.0))[1] for phase in phases)
                                for core in laps}
        untimed = brackets[parent] - sum(cores[FRAME_CORE] for cores in per_core.values())
        if abs(untimed) > UNTIMED_SHARE * brackets[parent]:
            raise ValueError(f"{path}: the laps leave {untimed:.2f} of {parent}'s {brackets[parent]:.2f} ms untimed, "
                             f"over {UNTIMED_SHARE:.0%}")
        found[parent] = (brackets[parent], per_core, untimed)
    return found


def sub_stage_colours():
    """The slot colours, then each lightened by TINT: twice as many sub-stages."""
    from matplotlib.colors import to_hex, to_rgb

    return [*SLOTS, *(to_hex([channel + (1 - channel) * TINT for channel in to_rgb(colour)]) for colour in SLOTS)]


def sub_stage_chart(stages, path):
    """One stacked bar per parent: its frame-core laps, then what they leave."""
    figure, axes = subplots(len(stages), 1, SUB_STAGE_FIGURE_SIZE)
    phases = list(dict.fromkeys(phase for total, per_core, _ in stages.values() for phase, cores in per_core.items()
                                if phase != LAPS_LABEL and share_percent(cores[FRAME_CORE] * PERCENT_SCALE / total) > 0))
    palette = sub_stage_colours()
    if len(phases) > len(palette):
        raise ValueError(f"{len(phases)} sub-stages drawn, {len(palette)} colours")
    colours = dict(zip(phases, palette), **{UNTIMED_LABEL: UNTIMED_COLOUR, LAPS_LABEL: LAPS_COLOUR})
    for axis, (parent, (total, per_core, untimed)) in zip(axes, stages.items()):
        segments = [(phase, cores[FRAME_CORE]) for phase, cores in per_core.items()]
        segments.append((UNTIMED_LABEL, max(untimed, 0.0)))
        left = 0.0
        drawn = []
        for phase, value in segments:
            share = value * PERCENT_SCALE / total
            if share_percent(share) > 0:
                bars = axis.barh(0, value, left=left, color=colours[phase], height=BAR_HEIGHT, edgecolor="white",
                                 linewidth=1.5)
                bars[0].set_label(f"{phase} {share_text(share)}")
                drawn.append(bars[0])
                if value >= LABEL_SHARE * total:
                    axis.text(left + value / 2, 0, f"{value:.1f}{LABEL_SEPARATOR}{share_text(share)}", ha="center",
                              va="center", fontsize=8, color=INK)
            left += value
        axis.annotate(f"{total:.1f} ms", (total, 0), xytext=(6, 0), textcoords="offset points", va="center")
        axis.set_xlim(0, total * (1 + AXIS_HEADROOM))
        axis.set_title(parent, loc="left", pad=SUB_STAGE_TITLE_PAD)
        axis.set_yticks([])
        axis.set_ylim(BAR_HEIGHT, -BAR_HEIGHT)
        for side in ("top", "right", "left"):
            axis.spines[side].set_visible(False)
        axis.legend(handles=drawn, loc="lower left", bbox_to_anchor=SUB_STAGE_LEGEND_ANCHOR, borderaxespad=0, ncol=len(drawn),
                    frameon=False, fontsize=8)
    axes[-1].set_xlabel(f"board ms per frame on cpu{FRAME_CORE}")
    save(figure, path)


def sub_stage_table(stages, windows, chart_name):
    cores = sorted({core for _, per_core, _ in stages.values() for values in per_core.values() for core in values})
    blank = ["" for _ in cores[1:]]
    headers = ["Sub-stage", *(f"cpu{core}" for core in cores), f"Share of the parent on cpu{FRAME_CORE}"]
    rows = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    for parent, (total, per_core, untimed) in stages.items():
        for phase, values in per_core.items():
            label = f"{parent} {LAPS_LABEL}" if phase == LAPS_LABEL else f"{parent}.{phase}"
            rows.append(f"| {label} | " + " | ".join(
                [*(f"{values.get(core, 0.0):.2f}" for core in cores),
                 share_text(values.get(FRAME_CORE, 0.0) * PERCENT_SCALE / total)]) + " |")
        rows.append(f"| {parent} {UNTIMED_LABEL} | " + " | ".join(
            [f"{untimed:.2f}", *blank, share_text(untimed * PERCENT_SCALE / total)]) + " |")
        rows.append(f"| {parent} | " + " | ".join([f"{total:.2f}", *blank, share_text(PERCENT_SCALE)]) + " |")
    note = (f"\nBoard ms per frame from a perf-scope image, mean of {windows} lap windows. A parent is its "
            f"frame-cost bracket on cpu{FRAME_CORE}; its sub-stages are cycle-counter laps on each core, and "
            f"cpu{FRAME_CORE}'s add up to the parent but for the untimed row, at most {UNTIMED_SHARE:.0%} of it. "
            f"The {LAPS_LABEL} row is what the laps cost themselves, one lap's measured cost times its starts, "
            "taken out of each sub-stage; the image runs them, so its parents are not the shipped bar's.")
    return (f"![Inside transform and draw](../images/render/{chart_name})\n\n"
            + collapsed("Sub-stages in milliseconds", "\n".join(rows) + "\n" + note + "\n"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--present", required=True, type=pathlib.Path)
    parser.add_argument("--laps", required=True, type=pathlib.Path, help="a --frame-laps capture with lap lines")
    parser.add_argument("--views", required=True, help="capture path with {view} for the view's name")
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--chart", required=True, type=pathlib.Path)
    args = parser.parse_args()
    names = view_names()
    order, base, added = stage_costs(args.present, args.views, names)
    chart(order, base, added, args.chart)
    args.tables.mkdir(parents=True, exist_ok=True)
    windows = [len(frame_cost.frame_cost_windows(path)) for path in
               [args.present, *(args.views.format(view=name) for name in added)]]
    stages = sub_stages(args.laps)
    inside_chart = chart_path(args.chart, SUB_STAGE_SUFFIX)
    sub_stage_chart(stages, inside_chart)
    lap_windows = sum(frame_cost.frame_laps_window(line) is not None
                      for line in args.laps.read_text(encoding="utf-8", errors="replace").splitlines())
    body = table(order, base, added, names, windows, args.chart.name,
                 sub_stage_table(stages, lap_windows, inside_chart.name))
    (args.tables / f"{BLOCK}.md").write_text(body + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

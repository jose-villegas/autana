#!/usr/bin/env python3
"""Dynamic-resolution results from a board capture: the stage split by render
size, what it says about the gaps between sizes, and how each policy flew the
path, as tables for the docs generator's block writer and a chart.
launcher/tools/render/render_doc_images.sh runs it on the capture and scores
kept in docs/render/data/.

    python launcher/tools/r3d/dynres_report.py CAPTURE [CAPTURE ...] --tables DIR --chart PNG
        [--quality CAMERA=CSV ...] [--upscale CSV] [--figures DIR]

CAPTURE is a device log holding the frame-cost suite's `scale_split`,
`scale_spans`, `dynres_step` and `dynres_frames` lines; a later line replaces
an earlier one of the same key, so several captures merge with the newest
last. CSV is the per-size reference score a scene's quality script writes
(width,height,frame,t_ms,mean_delta_e,p95_delta_e,ssim); each flown frame takes
the score of its size at the nearest scored time. DIR receives
dynres-stages.md, dynres-findings.md, dynres-policies.md and
dynres-prediction.md, one per block. --upscale takes the upscale filter
scores (upscale_quality.sh: those columns after scene and filter) and adds
dynres-upscale.md. --figures DIR receives the stage, one-core and
size-share figures the tables leave out.
"""

import argparse
import csv
import gzip
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "perf"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from capture import capture_board, require_same_board, QUARTERS, select_quarter


DESTINATION = (368, 448)

SPLIT = re.compile(r"scale_split: (\d+)x(\d+) poses=(\d+) tris=(\d+) frame mean/p50/max us (\d+)/(\d+)/(\d+) \| (.*?) \| total")
STAGE = re.compile(r"(\S+) ([\d.]+)/([\d.]+)")
SPANS = re.compile(r"scale_spans: (\d+)x(\d+) one core us/pose setup (-?\d+) edges (-?\d+) carry (-?\d+) fill (-?\d+) clear (-?\d+)")
SPAN_FIELDS = ("setup", "edges", "carry", "fill", "clear")
STEP = re.compile(r"dynres_step: (\w+) (\d+) (\d+)x(\d+) upscale")
FRAMES = re.compile(r"dynres_frames: (\w+) (\w+) (\w+) (\d+) (\d+)((?: -?\d+:\d+:\d+:\d+:\d+)+)(?![\d:])")

_REFIT_COST = re.compile(r"dynres_refit_cost: calls (\d+) mean_us ([\d.]+) max_us ([\d.]+)")
_REFIT = re.compile(r"dynres_refit: (\w+) (\w+) (\d+) scale ([\d.eE+-]+) offset_us ([\d.eE+-]+)")


def read_captures(paths, refit=None, quarter="portrait"):
    require_same_board({str(path): capture_board(path) for path in paths})
    splits, spans, ladders, frames = {}, {}, {}, {}
    for path in paths:
        raw = pathlib.Path(path).read_bytes()
        text = (gzip.decompress(raw) if path.endswith(".gz") else raw).decode("utf-8", errors="replace")
        for line in select_quarter(text, quarter).splitlines():
            if m := SPLIT.search(line):
                size = (int(m[1]), int(m[2]))
                stages = {name: float(avg) for name, avg, _ in STAGE.findall(m[8])}
                splits[size] = {"tris": int(m[4]), "mean": int(m[5]), "p50": int(m[6]), "max": int(m[7]), **stages}
            elif m := SPANS.search(line):
                spans[(int(m[1]), int(m[2]))] = dict(zip(SPAN_FIELDS,
                                                         (int(v) for v in m.groups()[2:])))
            elif m := STEP.search(line):
                ladders.setdefault(m[1], {})[int(m[2])] = (int(m[3]), int(m[4]))
            elif m := _REFIT_COST.search(line):
                if refit is not None:
                    refit["cost"] = {"calls": int(m[1]), "mean_us": float(m[2]), "max_us": float(m[3])}
            elif m := _REFIT.search(line):
                if refit is not None:
                    refit.setdefault("correction", {})[(m[1], m[2], int(m[3]))] = dict(zip(
                        ("scale", "offset_us"),
                        (float(value) for value in m.groups()[3:])))
            elif m := FRAMES.search(line):
                records = [tuple(int(v) for v in item.split(":")) for item in m[6].split()]
                frames.setdefault((m[1], m[2], m[3], int(m[4])), {})[int(m[5])] = records
    runs = {}
    for key, chunks in frames.items():
        runs[key] = [record for first in sorted(chunks) for record in chunks[first]]
    return splits, spans, ladders, runs


def read_quality(path):
    scores = {}
    if path is None:
        return scores
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            size = (int(row["width"]), int(row["height"]))
            scores.setdefault(size, []).append((int(row["t_ms"]), float(row["mean_delta_e"]), float(row["ssim"])))
    return scores


def divisor(size):
    across, down = DESTINATION[0] / size[0], DESTINATION[1] / size[1]
    return f"{across:.2f}" if abs(across - down) < 0.02 else f"{across:.2f} x {down:.2f}"


def ms(us):
    return f"{us / 1000:.1f}"


STAGES = (("cull", "r3d.cull"), ("transform", "r3d.transform"), ("draw", "r3d.draw"),
          ("resolve", "r3d.resolve"), ("upscale", "r3d.upscale"))
SPAN_STAGES = ("setup", "edges", "carry", "fill")
STAGE_FIGURE = "dynamic-resolution-stages.png"
ONE_CORE_FIGURE = "dynamic-resolution-one-core.png"
SIZES_FIGURE = "dynamic-resolution-sizes.png"
ROW_HEIGHT = 0.3  # inches per bar row of a figure
FIGURE_WIDTH = 7.0


def by_pixels(sizes):
    """Largest picture first, the wider of two equal pictures first."""
    return sorted(sizes, key=lambda size: (-size[0] * size[1], -size[0]))


def stages_table(splits):
    headers = ["Render size", "Divisor", "Pixels", "Frame mean", "p50", "max"]
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    full = DESTINATION[0] * DESTINATION[1]
    for size in by_pixels(splits):
        s = splits[size]
        lines.append(f"| {size[0]}x{size[1]} | {divisor(size)} | {100 * size[0] * size[1] / full:.0f}% "
                     f"| {ms(s['mean'])} | {ms(s['p50'])} | {ms(s['max'])} |")
    return "\n".join(lines) + "\n\nMilliseconds, both cores."


def stage_figures(splits, spans, directory):
    """The stage split as stacked bars per render size: both cores, then one core."""
    from docs.plot_style import SLOTS, figure, save_themes, stacked_barh, style_axes

    sizes = by_pixels(splits)
    height = ROW_HEIGHT * len(sizes) + 1.2
    labels = [f"{w}x{h}" for w, h in sizes]
    both = [(name, key) for name, key in STAGES if any(key in splits[size] for size in sizes)]
    one = [(name, name) for name in SPAN_STAGES]
    for path, title, parts, value in (
            (STAGE_FIGURE, "frame time by stage, both cores (ms)", both,
             lambda size, key: splits[size].get(key, 0.0)),
            (ONE_CORE_FIGURE, "one core, per pose (ms)", one,
             lambda size, key: spans.get(size, {}).get(key, 0) / 1000)):
        colours = {name: SLOTS[i] for i, (name, _) in enumerate(parts)}
        rows = [(label, [(name, value(size, key)) for name, key in parts]) for label, size in zip(labels, sizes)]
        ends = [ms(splits[size]["mean"]) if path == STAGE_FIGURE else f"{sum(v for _, v in segments):.1f}"
                for size, (_, segments) in zip(sizes, rows)]
        fig = figure((FIGURE_WIDTH, height))
        axis = fig.add_subplot()
        stacked_barh(axis, rows, colours, ends)
        style_axes(axis, title)
        axis.legend(fontsize=8, loc="lower right", ncol=len(parts), frameon=False)
        save_themes(fig, pathlib.Path(directory) / path)


def findings_table(splits, spans):
    rows = ["| Question | Compared | Frame | draw | upscale | 1 core: edges | carry | fill |",
            "|---|---|---|---|---|---|---|---|"]

    def row(question, a, b):
        if a not in splits or b not in splits:
            return
        sa, sb, oa, ob = splits[a], splits[b], spans.get(a, {}), spans.get(b, {})
        delta = lambda key, table_a, table_b: table_a.get(key, 0) - table_b.get(key, 0)  # noqa: E731
        rows.append(f"| {question} | {a[0]}x{a[1]} minus {b[0]}x{b[1]} | {ms(sa['mean'] - sb['mean'])} "
                    f"| {delta('r3d.draw', sa, sb):.1f} | {delta('r3d.upscale', sa, sb):.1f} "
                    + " ".join(f"| {ms(delta(k, oa, ob))}" for k in ("edges", "carry", "fill")) + " |")

    for question, a, b in (
        ("half width, more height over half", (184, 298), (184, 224)),
        ("half over a fifth less height", (184, 224), (184, 179)),
        ("half width over 2.5x, same height", (184, 179), (147, 179)),
        ("half width over 3x, same height", (184, 149), (122, 149)),
        ("full height over half", (368, 448), (368, 224)),
        ("full width over half", (368, 448), (184, 448)),
    ):
        row(question, a, b)
    return "\n".join(rows)


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))]


def quality_at(scores, size, t_ms):
    if size not in scores:
        return None
    return min(scores[size], key=lambda item: abs(item[0] - t_ms))


def policy_rows(runs, ladders, scores, splits):
    """One row per path, policy, ladder and budget; fixed runs repeat per budget."""
    rows = []
    for camera in sorted({key[0] for key in runs}):
        budgets = sorted({budget for (path, _, _, budget) in runs if path == camera and budget > 0})
        for budget in budgets:
            for (path, policy, ladder, run_budget), records in runs.items():
                if path != camera or run_budget not in (0, budget):
                    continue
                sizes = [ladders.get(ladder, {}).get(step, (DESTINATION[0] // 2, DESTINATION[1] // 2)) for step, *_ in records]
                costs = [draw + upscale for _, draw, upscale, _, _ in records]
                steps = {}
                for size in sizes:
                    steps[size] = steps.get(size, 0) + 1
                switches = sum(1 for a, b in zip(sizes, sizes[1:]) if a != b)
                quality = [quality_at(scores.get(camera, {}), size, (i + 1) * 50) for i, size in enumerate(sizes)]
                known = [q for q in quality if q is not None]
                rows.append({
                    "camera": camera, "policy": policy, "ladder": ladder, "budget": budget, "costs": costs, "sizes": sizes,
                    "p50": percentile(costs, 0.5), "p95": percentile(costs, 0.95), "max": max(costs),
                    "over": sum(1 for c in costs if c > budget) / len(costs), "switches": switches, "steps": steps,
                    "delta_e": sum(q[1] for q in known) / len(known) if known else None,
                    "ssim": sum(q[2] for q in known) / len(known) if known else None,
                })
    return rows


def size_spread(row):
    total = sum(row["steps"].values())
    return {size: n / total for size, n in row["steps"].items()}


def policies_table(rows):
    """One table per path; the fixed run is the same at every budget, so it is shown once."""
    out = []
    for camera in sorted({r["camera"] for r in rows}):
        lines = [f"**{camera}**", "",
                 "| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Mean dE | SSIM |",
                 "|---|---|---|---|---|---|---|---|---|---|"]
        seen = set()
        for r in (r for r in rows if r["camera"] == camera):
            if r["policy"] == "fixed":
                if (r["ladder"], r["policy"]) in seen:
                    continue
                seen.add((r["ladder"], r["policy"]))
            budget = "any" if r["policy"] == "fixed" else ms(r["budget"])
            quality = "- | -" if r["delta_e"] is None else f"{r['delta_e']:.2f} | {r['ssim']:.4f}"
            lines.append(f"| {budget} | {r['policy']} | {r['ladder']} | {ms(r['p50'])} | {ms(r['p95'])} "
                         f"| {ms(r['max'])} | {100 * r['over']:.1f}% | {r['switches']} | {quality} |")
        out.append("\n".join(lines))
    return "\n\n".join(out) + ("\n\nFrame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM "
                               "are against the reference at full size, lower dE and higher SSIM being closer.")


SHARE_COLOURMAP = "turbo"  # one distinct colour per step, like a height map
SHARE_RANGE = (0.1, 0.9)
MIN_LABELLED = 5  # a bar segment narrower than this share of frames carries no number
OUTLINE_WIDTH = 3.5  # points of dark outline that keep a number readable on any segment colour
LEGEND_BESIDE = (1.01, 0.5)  # the steps listed down the bars' right, first on top


def share_figure(panels, path):
    """Time at each render size as 100% bars, one panel per ladder with its steps numbered in ladder order,
    finest first: panels is [(title, [steps], [(label, {size: share})])]."""
    import numpy as np
    from matplotlib import colormaps
    from matplotlib.patheffects import withStroke
    from docs.plot_style import figure, save_themes, stacked_barh, style_axes

    count = sum(len(rows) for _, _, rows in panels)
    fig = figure((FIGURE_WIDTH, ROW_HEIGHT * count + 0.8 * len(panels) + 0.3))
    grid = fig.subplots(len(panels), 1, gridspec_kw={"height_ratios": [len(rows) + 2 for _, _, rows in panels]})
    grid = [grid] if len(panels) == 1 else list(grid)
    for axis, (title, steps, rows) in zip(grid, panels):
        used = [i for i, size in enumerate(steps) if any(spread.get(size) for _, spread in rows)]
        steps = steps[:used[-1] + 1]
        shades = colormaps[SHARE_COLOURMAP](np.linspace(*SHARE_RANGE, len(steps)))
        names = [f"{i + 1}: {w}x{h}" for i, (w, h) in enumerate(steps)]
        colours = dict(zip(names, shades))
        bars = [(label, [(name, 100 * spread.get(size, 0)) for name, size in zip(names, steps)])
                for label, spread in rows]
        stacked_barh(axis, bars, colours, extent=100)
        for row, (_, segments) in enumerate(bars):
            left = 0.0
            for name, value in segments:
                if value >= MIN_LABELLED:
                    label = axis.text(left + value / 2, row, f"{value:.0f}", ha="center", va="center", fontsize=9,
                                      fontweight="bold", color="white")
                    label.set_path_effects([withStroke(linewidth=OUTLINE_WIDTH, foreground="black")])
                left += value
        style_axes(axis, f"{title}, share of frames (%)")
        axis.legend(fontsize=8, loc="center left", bbox_to_anchor=LEGEND_BESIDE, frameon=False)
    save_themes(fig, path)


def policy_share_figure(rows, ladders, path):
    """Each path's stepped and predicted runs, one panel per ladder; the fixed run is the table's."""
    panels = []
    for camera in sorted({r["camera"] for r in rows}):
        for ladder in sorted({r["ladder"] for r in rows if r["camera"] == camera and r["policy"] != "fixed"}):
            bars = [(f"{r['policy']}, {ms(r['budget'])} ms", size_spread(r)) for r in rows
                    if r["camera"] == camera and r["ladder"] == ladder and r["policy"] != "fixed"]
            steps = [size for _, size in sorted(ladders[ladder].items())]
            panels.append((f"{camera}, {ladder} ladder", steps, bars))
    share_figure(panels, path)


def prediction_table(runs, refit=None):
    lines = ["| Path | Ladder | Budget | Frames | Median error | p95 error | Over budget |",
             "|---|---|---|---|---|---|---|"]
    for (camera, policy, ladder, budget), records in sorted(runs.items(), key=lambda item: (item[0][0], item[0][3], item[0][2])):
        if policy != "predicted":
            continue
        if any(predicted <= 0 for *_, predicted in records):
            raise ValueError("predicted frames must have a positive predicted price")
        errors = [100 * abs((draw + upscale) / predicted - 1) for _, draw, upscale, _, predicted in records]
        over = 100 * sum(draw + upscale > budget for _, draw, upscale, _, _ in records) / len(records)
        lines.append(f"| {camera} | {ladder} | {ms(budget)} | {len(records)} "
                     f"| {statistics.median(errors):.1f}% | {percentile(errors, 0.95):.1f}% | {over:.1f}% |")
    if refit and "cost" in refit:
        cost = refit["cost"]
        lines.extend(["", f"Online refit: {cost['calls']} calls, mean {cost['mean_us']:.3f} us, "
                          f"max {cost['max_us']:g} us per call."])

    return "\n".join(lines)


UPSCALE_COLUMNS = (("mean_delta_e", "mean dE", "%.2f"), ("p95_delta_e", "p95 dE", "%.1f"), ("ssim", "SSIM", "%.3f"),
                   ("edge_delta_e", "edge dE", "%.2f"), ("interior_delta_e", "interior dE", "%.2f"))
UPSCALE_MAIN = ("mean_delta_e", "p95_delta_e", "ssim")
MAIN_SIZE = (DESTINATION[0] // 2, DESTINATION[1] // 2)


def upscale_table(path):
    """The main size inline, one row per scene, each metric as first filter in the file → second
    (change); every size and column in a details block."""
    groups = {}
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            key = (row["scene"], (int(row["width"]), int(row["height"])))
            groups.setdefault(key, {}).setdefault(row["filter"], []).append(row)
    filters = list(next(iter(groups.values())))
    if len(filters) != 2:
        raise SystemExit(f"{path}: needs two filters, has {filters}")

    def means(by_filter, field):
        return [statistics.mean(float(row[field]) for row in by_filter[name]) for name in filters]

    columns = {field: (label, form) for field, label, form in UPSCALE_COLUMNS}
    lines = ["| Scene | " + " | ".join(columns[f][0] for f in UPSCALE_MAIN) + " |", "|---|" + "---|" * len(UPSCALE_MAIN)]
    for (scene, size), by_filter in groups.items():
        if size != MAIN_SIZE:
            continue
        row = []
        for field in UPSCALE_MAIN:
            a, b = means(by_filter, field)
            form = columns[field][1]
            row.append(f"{form % a} → {form % b} ({form.replace('%', '%+') % (b - a)})")
        lines.append(f"| {scene} | " + " | ".join(row) + " |")
    lines += ["", f"At {MAIN_SIZE[0]}x{MAIN_SIZE[1]}, each cell is {filters[0]} → {filters[1]} (change), the mean over the "
                  "path's frames against the reference at full size; lower dE and higher SSIM are closer.", "",
              "<details>", "<summary>Every size, with edge and interior dE</summary>", "",
              "| Scene | Size | " + " | ".join(label for _f, label, _m in UPSCALE_COLUMNS) + " |",
              "|---|---|" + "---|" * len(UPSCALE_COLUMNS)]
    for (scene, (width, height)), by_filter in groups.items():
        cells = []
        for field, _label, form in UPSCALE_COLUMNS:
            a, b = means(by_filter, field)
            cells.append(f"{form % a} → {form % b}")
        lines.append(f"| {scene} | {width}x{height} | " + " | ".join(cells) + " |")
    lines += ["", f"Each cell is {filters[0]} → {filters[1]}.", "", "</details>"]
    return "\n".join(lines)


def chart(rows, path):
    from docs.plot_style import save, subplots

    budgets = sorted({r["budget"] for r in rows})
    cameras = sorted({r["camera"] for r in rows})
    figure, axes = subplots(2 * len(cameras), len(budgets), (6 * len(budgets), 6 * len(cameras)), squeeze=False)
    for row, camera in enumerate(cameras):
        for column, budget in enumerate(budgets):
            cost_axis, size_axis = axes[2 * row][column], axes[2 * row + 1][column]
            for r in (r for r in rows if r["camera"] == camera and r["budget"] == budget):
                label = r["policy"] if r["ladder"] == "half" else f"{r['policy']}, {r['ladder']}"
                seconds = [(i + 1) * 0.05 for i in range(len(r["costs"]))]
                cost_axis.plot(seconds, [c / 1000 for c in r["costs"]], linewidth=0.8, label=label)
                share = [100 * w * h / (DESTINATION[0] * DESTINATION[1]) for w, h in r["sizes"]]
                size_axis.step(seconds, share, linewidth=0.8, where="post", label=label)
            cost_axis.axhline(budget / 1000, color="black", linestyle="--", linewidth=0.8)
            cost_axis.set_title(f"{camera}, budget {budget / 1000:.0f} ms")
            cost_axis.set_ylabel("draw + upscale, ms")
            size_axis.set_ylabel("pixels drawn, % of panel")
            size_axis.set_xlabel("path time, s")
            cost_axis.legend(fontsize=7, loc="upper right")
    save(figure, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--chart", required=True)
    parser.add_argument("--quarter", choices=QUARTERS, default="portrait")
    parser.add_argument("--quality", action="append", default=[], metavar="CAMERA=CSV")
    parser.add_argument("--upscale", metavar="CSV")
    parser.add_argument("--figures", type=pathlib.Path, help="directory for the stage and size-share figures")
    args = parser.parse_args()
    refit = {}
    splits, spans, ladders, runs = read_captures(args.captures, refit, args.quarter)
    if not splits or not runs:
        parser.error("the captures hold no scale_split or no dynres_frames lines")
    scores = {}
    for quality in args.quality:
        camera, separator, csv_path = quality.partition("=")
        if not separator or not camera or not csv_path or camera in scores:
            parser.error("--quality needs a unique CAMERA=CSV")
        scores[camera] = read_quality(csv_path)
    rows = policy_rows(runs, ladders, scores, splits)
    args.tables.mkdir(parents=True, exist_ok=True)
    tables = {"dynres-stages": stages_table(splits), "dynres-findings": findings_table(splits, spans),
              "dynres-policies": policies_table(rows), "dynres-prediction": prediction_table(runs, refit)}
    if args.upscale:
        tables["dynres-upscale"] = upscale_table(args.upscale)
    for name, body in tables.items():
        label = f"Render quarter={args.quarter}.\n\n" if args.quarter != "portrait" else ""
        (args.tables / f"{name}.md").write_text(label + body + "\n", encoding="utf-8")
    chart(rows, args.chart)
    if args.figures:
        stage_figures(splits, spans, args.figures)
        policy_share_figure(rows, ladders, args.figures / SIZES_FIGURE)


if __name__ == "__main__":
    main()

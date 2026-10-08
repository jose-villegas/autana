#!/usr/bin/env python3
"""Dynamic-resolution results from a board capture: the stage split by render
size, what it says about the gaps between sizes, and how each policy flew the
path, as tables for the docs generator's block writer and a chart.
launcher/tools/render/render_doc_images.sh runs it on the capture and scores
kept in docs/render/data/.

    python launcher/tools/r3d/dynres_report.py CAPTURE [CAPTURE ...] --tables DIR --chart PNG
        [--quality CAMERA=CSV ...]

CAPTURE is a device log holding the frame-cost suite's `scale_split`,
`scale_spans`, `dynres_step` and `dynres_frames` lines; a later line replaces
an earlier one of the same key, so several captures merge with the newest
last. CSV is the per-size reference score a scene's quality script writes
(width,height,frame,t_ms,mean_delta_e,p95_delta_e,ssim); each flown frame takes
the score of its size at the nearest scored time. DIR receives
dynres-stages.md, dynres-findings.md, dynres-policies.md and
dynres-prediction.md, one per block.
"""

import argparse
import csv
import gzip
import pathlib
import re
import statistics


DESTINATION = (368, 448)

SPLIT = re.compile(r"scale_split: (\d+)x(\d+) poses=(\d+) tris=(\d+) frame mean/p50/max us (\d+)/(\d+)/(\d+) \| (.*?) \| total")
STAGE = re.compile(r"(r3d\.\w+) ([\d.]+)/([\d.]+)")
SPANS = re.compile(r"scale_spans: (\d+)x(\d+) one core us/pose setup (-?\d+) rows (-?\d+) span_setup (-?\d+) fill (-?\d+) clear (-?\d+)")
STEP = re.compile(r"dynres_step: (\w+) (\d+) (\d+)x(\d+) upscale")
FRAMES = re.compile(r"dynres_frames: (\w+) (\w+) (\w+) (\d+) (\d+)((?: -?\d+:\d+:\d+:\d+:\d+)+)(?![\d:])")

_REFIT_COST = re.compile(r"dynres_refit_cost: calls (\d+) mean_us ([\d.]+) max_us ([\d.]+)")
_REFIT = re.compile(r"dynres_refit: (\w+) (\w+) (\d+) scale ([\d.eE+-]+) offset_us ([\d.eE+-]+)")


def read_captures(paths, refit=None):
    splits, spans, ladders, frames = {}, {}, {}, {}
    for path in paths:
        raw = pathlib.Path(path).read_bytes()
        text = (gzip.decompress(raw) if path.endswith(".gz") else raw).decode("utf-8", errors="replace")
        for line in text.splitlines():
            if m := SPLIT.search(line):
                size = (int(m[1]), int(m[2]))
                stages = {name: float(avg) for name, avg, _ in STAGE.findall(m[8])}
                splits[size] = {"tris": int(m[4]), "mean": int(m[5]), "p50": int(m[6]), "max": int(m[7]), **stages}
            elif m := SPANS.search(line):
                spans[(int(m[1]), int(m[2]))] = dict(zip(("setup", "rows", "span_setup", "fill", "clear"),
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


def frame_cost_windows(path):
    if path is None or not pathlib.Path(path).exists():
        return None
    windows = [re.findall(r"(\S+) ([\d.]+)/[\d.]+", line.split("ms/frame avg/worst:", 1)[1])
               for line in pathlib.Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
               if "ms/frame avg/worst:" in line]
    if not windows or not all(windows):
        raise ValueError(f"{path}: no frame-cost windows")
    return windows


def frame_cost_means(path):
    """Every bracket in report order, averaged over its report windows."""
    windows = frame_cost_windows(path)
    if windows is None:
        return None
    values = {}
    for window in windows:
        for name, value in window:
            values.setdefault(name, []).append(float(value))
    return [(name, statistics.mean(samples)) for name, samples in values.items()]


def pipeline_table(present, resolve):
    shipped, attached = frame_cost_means(present), frame_cost_means(resolve)
    rows = list(shipped or [])
    resolve_ms = dict(attached or []).get("r3d.resolve")
    names = [name for name, _ in rows]
    if "r3d.resolve" in names:
        index = names.index("r3d.resolve")
        rows.pop(index)
    else:
        report = [name for name, _ in attached or []]
        index = len(rows)
        if "r3d.resolve" in report:
            after = report[report.index("r3d.resolve") + 1:]
            index = next((i for i, (name, _) in enumerate(rows) if name in after), len(rows))
    rows.insert(index, ("r3d.resolve, with motion vectors attached", resolve_ms))
    lines = ["| Stage | Board ms/frame |", "|---|---|"]
    if shipped is None:
        lines.append("| scene as shipped | not in capture |")
    lines.extend(f"| {name} | {value:.2f} |" if value is not None else f"| {name} | not in capture |"
                 for name, value in rows)
    count = len(frame_cost_windows(present) or [])
    return "\n".join(lines) + f"\n\nSource: the scene as shipped, mean of {count} windows."


def stages_table(splits, spans):
    stages = [("cull", "r3d.cull"), ("transform", "r3d.transform"), ("draw", "r3d.draw")]
    if any("r3d.resolve" in row for row in splits.values()):
        stages.append(("resolve", "r3d.resolve"))
    stages.append(("upscale", "r3d.upscale"))
    headers = ["Render size", "Divisor", "Pixels", "Frame mean", "p50", "max",
               *(label for label, _ in stages), "1 core: setup", "rows", "span setup", "fill"]
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    full = DESTINATION[0] * DESTINATION[1]
    for size, s in splits.items():
        o = spans.get(size, {})
        cells = [f"{size[0]}x{size[1]}", divisor(size), f"{100 * size[0] * size[1] / full:.0f}%",
                 ms(s['mean']), ms(s['p50']), ms(s['max']),
                 *(f"{s[name]:.1f}" if name in s else "not in capture" for _, name in stages),
                 *(ms(o[key]) for key in ("setup", "rows", "span_setup", "fill"))]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n\nMilliseconds; both cores unless marked one core."


def findings_table(splits, spans):
    rows = ["| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |",
            "|---|---|---|---|---|---|---|---|"]

    def row(question, a, b):
        if a not in splits or b not in splits:
            return
        sa, sb, oa, ob = splits[a], splits[b], spans.get(a, {}), spans.get(b, {})
        delta = lambda key, table_a, table_b: table_a.get(key, 0) - table_b.get(key, 0)  # noqa: E731
        rows.append(f"| {question} | {a[0]}x{a[1]} minus {b[0]}x{b[1]} | {ms(sa['mean'] - sb['mean'])} "
                    f"| {delta('r3d.draw', sa, sb):.1f} | {delta('r3d.upscale', sa, sb):.1f} "
                    + " ".join(f"| {ms(delta(k, oa, ob))}" for k in ("rows", "span_setup", "fill")) + " |")

    for question, a, b in (
        ("half width, more height over half", (184, 298), (184, 224)),
        ("half over floor", (184, 224), (184, 179)),
        ("floor over 2.5x cost reference", (184, 179), (147, 179)),
        ("recovery over 3x cost reference", (184, 149), (122, 149)),
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


def policies_table(rows):
    lines = ["| Path | Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        total = sum(r["steps"].values())
        spread = ", ".join(f"{w}x{h} {100 * n / total:.0f}%" for (w, h), n in
                           sorted(r["steps"].items(), key=lambda item: -item[0][0] * item[0][1]))
        quality = "" if r["delta_e"] is None else f"{r['delta_e']:.2f} | {r['ssim']:.4f}"
        lines.append(f"| {r['camera']} | {ms(r['budget'])} | {r['policy']} | {r['ladder']} | {ms(r['p50'])} | {ms(r['p95'])} "
                     f"| {ms(r['max'])} | {100 * r['over']:.1f}% | {r['switches']} | {spread} | {quality or '- | -'} |")
    return "\n".join(lines) + ("\n\nFrame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM "
                               "are against the reference at full size, lower dE and higher SSIM being closer.")


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


def chart(rows, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    budgets = sorted({r["budget"] for r in rows})
    cameras = sorted({r["camera"] for r in rows})
    figure, axes = plt.subplots(2 * len(cameras), len(budgets), figsize=(6 * len(budgets), 6 * len(cameras)),
                                layout="constrained", squeeze=False)
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
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=110)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--chart", required=True)
    parser.add_argument("--present", type=pathlib.Path, help="frame-cost reports of the scene as shipped")
    parser.add_argument("--resolve", type=pathlib.Path, help="frame-cost reports with an attachment that resolves")
    parser.add_argument("--quality", action="append", default=[], metavar="CAMERA=CSV")
    args = parser.parse_args()
    refit = {}
    splits, spans, ladders, runs = read_captures(args.captures, refit)
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
    tables = {"dynres-stages": stages_table(splits, spans), "dynres-findings": findings_table(splits, spans),
              "dynres-policies": policies_table(rows), "dynres-prediction": prediction_table(runs, refit)}
    tables["pipeline-frame-stages"] = pipeline_table(args.present, args.resolve)
    for name, body in tables.items():
        (args.tables / f"{name}.md").write_text(body + "\n", encoding="utf-8")
    chart(rows, args.chart)


if __name__ == "__main__":
    main()

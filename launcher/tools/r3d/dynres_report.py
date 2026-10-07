#!/usr/bin/env python3
"""Dynamic-resolution results from a board capture: the stage split by render
size, what it says about the gaps between sizes, and how each policy flew the
path, as tables for the docs generator's block writer and a chart.
launcher/tools/render/render_doc_images.sh runs it on the capture and scores
kept in docs/render/data/.

    python launcher/tools/r3d/dynres_report.py CAPTURE [CAPTURE ...] --tables DIR --chart PNG
        [--quality CSV]

CAPTURE is a device log holding the frame-cost suite's `scale_split`,
`scale_spans`, `dynres_step` and `dynres_frames` lines; a later line replaces
an earlier one of the same key, so several captures merge with the newest
last. CSV is the per-size reference score a scene's quality script writes
(width,height,frame,t_ms,mean_delta_e,p95_delta_e,ssim); each flown frame takes
the score of its size at the nearest scored time. DIR receives
dynres-stages.md, dynres-findings.md and dynres-policies.md, one per block.
"""

import argparse
import csv
import gzip
import pathlib
import re


DESTINATION = (368, 448)

SPLIT = re.compile(r"scale_split: (\d+)x(\d+) poses=(\d+) tris=(\d+) frame mean/p50/max us (\d+)/(\d+)/(\d+) \| (.*?) \| total")
STAGE = re.compile(r"(r3d\.\w+) ([\d.]+)/([\d.]+)")
SPANS = re.compile(r"scale_spans: (\d+)x(\d+) one core us/pose setup (-?\d+) rows (-?\d+) span_setup (-?\d+) fill (-?\d+) clear (-?\d+)")
STEP = re.compile(r"dynres_step: (\w+) (\d+) (\d+)x(\d+) upscale")
FRAMES = re.compile(r"dynres_frames: (\w+) (\w+) (\d+) (\d+)((?: -?\d+:\d+:\d+:\d+)*)")


def read_captures(paths):
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
            elif m := FRAMES.search(line):
                records = [tuple(int(v) for v in item.split(":")) for item in m[5].split()]
                frames.setdefault((m[1], m[2], int(m[3])), {})[int(m[4])] = records
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


def stages_table(splits, spans):
    lines = ["| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale "
             "| 1 core: setup | rows | span setup | fill |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    full = DESTINATION[0] * DESTINATION[1]
    for size, s in splits.items():
        o = spans.get(size, {})
        lines.append(f"| {size[0]}x{size[1]} | {divisor(size)} | {100 * size[0] * size[1] / full:.0f}% "
                     f"| {ms(s['mean'])} | {ms(s['p50'])} | {ms(s['max'])} | {s.get('r3d.cull', 0):.1f} "
                     f"| {s.get('r3d.transform', 0):.1f} | {s.get('r3d.draw', 0):.1f} | {s.get('r3d.upscale', 0):.1f} "
                     + " ".join(f"| {ms(o[k])}" for k in ("setup", "rows", "span_setup", "fill")) + " |")
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

    row("1.5x over 2x", (245, 298), (184, 224))
    row("2x over 2.5x", (184, 224), (147, 179))
    row("full height over half", (368, 448), (368, 224))
    row("full width over half", (368, 448), (184, 448))
    return "\n".join(rows)


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))]


def quality_at(scores, size, t_ms):
    if size not in scores:
        return None
    return min(scores[size], key=lambda item: abs(item[0] - t_ms))


def policy_rows(runs, ladders, scores, splits):
    """One row per (policy, ladder, budget) and, for the fixed run, one per budget."""
    budgets = sorted({budget for (_, _, budget) in runs if budget > 0})
    rows = []
    for budget in budgets:
        for (policy, ladder, run_budget), records in runs.items():
            if run_budget not in (0, budget):
                continue
            sizes = [ladders.get(ladder, {}).get(step, (DESTINATION[0] // 2, DESTINATION[1] // 2)) for step, *_ in records]
            costs = [draw + upscale for _, draw, upscale, _ in records]
            steps = {}
            for size in sizes:
                steps[size] = steps.get(size, 0) + 1
            switches = sum(1 for a, b in zip(sizes, sizes[1:]) if a != b)
            quality = [quality_at(scores, size, (i + 1) * 50) for i, size in enumerate(sizes)]
            known = [q for q in quality if q is not None]
            rows.append({
                "policy": policy, "ladder": ladder, "budget": budget, "costs": costs, "sizes": sizes,
                "p50": percentile(costs, 0.5), "p95": percentile(costs, 0.95), "max": max(costs),
                "over": sum(1 for c in costs if c > budget) / len(costs), "switches": switches, "steps": steps,
                "delta_e": sum(q[1] for q in known) / len(known) if known else None,
                "ssim": sum(q[2] for q in known) / len(known) if known else None,
            })
    return rows


def policies_table(rows):
    lines = ["| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        total = sum(r["steps"].values())
        spread = ", ".join(f"{w}x{h} {100 * n / total:.0f}%" for (w, h), n in
                           sorted(r["steps"].items(), key=lambda item: -item[0][0] * item[0][1]))
        quality = "" if r["delta_e"] is None else f"{r['delta_e']:.2f} | {r['ssim']:.4f}"
        lines.append(f"| {ms(r['budget'])} | {r['policy']} | {r['ladder']} | {ms(r['p50'])} | {ms(r['p95'])} "
                     f"| {ms(r['max'])} | {100 * r['over']:.1f}% | {r['switches']} | {spread} | {quality or '- | -'} |")
    return "\n".join(lines) + ("\n\nFrame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM "
                               "are against the reference at full size, lower dE and higher SSIM being closer.")


def chart(rows, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    budgets = sorted({r["budget"] for r in rows})
    figure, axes = plt.subplots(2, len(budgets), figsize=(6 * len(budgets), 6), sharex=True,
                                layout="constrained", squeeze=False)
    for column, budget in enumerate(budgets):
        cost_axis, size_axis = axes[0][column], axes[1][column]
        for r in (r for r in rows if r["budget"] == budget):
            label = r["policy"] if r["ladder"] in ("half", "isotropic") else f"{r['policy']}, {r['ladder']}"
            seconds = [(i + 1) * 0.05 for i in range(len(r["costs"]))]
            cost_axis.plot(seconds, [c / 1000 for c in r["costs"]], linewidth=0.8, label=label)
            share = [100 * w * h / (DESTINATION[0] * DESTINATION[1]) for w, h in r["sizes"]]
            size_axis.step(seconds, share, linewidth=0.8, where="post", label=label)
        cost_axis.axhline(budget / 1000, color="black", linestyle="--", linewidth=0.8)
        cost_axis.set_title(f"budget {budget / 1000:.0f} ms")
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
    parser.add_argument("--quality")
    args = parser.parse_args()
    splits, spans, ladders, runs = read_captures(args.captures)
    if not splits or not runs:
        parser.error("the captures hold no scale_split or no dynres_frames lines")
    rows = policy_rows(runs, ladders, read_quality(args.quality), splits)
    args.tables.mkdir(parents=True, exist_ok=True)
    tables = {"dynres-stages": stages_table(splits, spans), "dynres-findings": findings_table(splits, spans),
              "dynres-policies": policies_table(rows)}
    for name, body in tables.items():
        (args.tables / f"{name}.md").write_text(body + "\n", encoding="utf-8")
    chart(rows, args.chart)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Replays a board's per-step frame costs through the engine's dynamic-resolution
predictor, so its target, margin and step-up delay can be tuned without
reflashing, and refits the cost model on every scenario.

    python launcher/tools/r3d/dynres_replay.py CAPTURE --cc CC [--json OUT]
        [--targets MS,...] [--margins PERCENT,...] [--holds MS,...] [--top STEP] [--first STEP]
        [--tables DIR]

CAPTURE is a device log holding the `dynres_steps` lines of
run_render_lab_dynres_steps_suite: every scenario's path drawn with each
ladder step held, at one fixed frame step, so frame i is the same pose at
every step. A replay draws frame i at the step the predictor picks and pays
that step's measured cost; the whole frame is the measured whole frame at
that step. dynres_replay_probe.c runs the engine's own fit and predictor.

It prints the refit model and its miss per scenario and step (the up margin
is set at the 90th percentile of |miss|), then for each
target, margin and step-up delay each scenario's whole-frame mean, p95 and
spread against the fixed scale at the top step. --tables writes the docs
generator's blocks: dynres-steps.md, each scenario's mean whole frame and
compose at every step, and dynres-flight.md, the first replay of each
scenario against the fixed scale.
"""

import argparse
import gzip
import itertools
import json
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
sys.path.insert(0, str(ROOT / "launcher/tools/math"))
from build.c_header import read_define  # noqa: E402
from capture import QUARTER_RE, QUARTERS, select_quarter  # noqa: E402
from math_figures import build_probe  # noqa: E402
from dynres_report import percentile  # noqa: E402

DYNRES = ROOT / "launcher/main/apps/render_lab/render_lab_dynres.h"
RESOLUTION = ROOT / "launcher/main/render/resolution/resolution.c"
PROBE_SOURCES = (ROOT / "launcher/tools/r3d/dynres_replay_probe.c", RESOLUTION)
US_PER_MS = 1000
PERCENT = 100
P95 = 0.95
MARGIN_SHARE = 0.90  # the up margin covers this share of the prediction's misses

LINE = re.compile(r"dynres_steps: (\S+) step=(\d+) (\d+)x(\d+) dt=(\d+)ms first=(\d+) n=(\d+)((?: -?\d+:-?\d+:-?\d+:-?\d+)*)")


def whole(line):
    """A steps line that arrived whole: every frame it counts and a known quarter."""
    m = LINE.search(line)
    tag = QUARTER_RE.search(line)
    return m is not None and len(m[8].split()) == int(m[7]) and (tag is None or tag[1] in QUARTERS)


def read_capture(path, chosen):
    """{scenario: {step: [(whole, draw, upscale, triangles), ...]}} over the
    captured sizes `chosen`, renumbered from 0, and their sizes; a scenario
    keeps the ones it was captured at."""
    raw = pathlib.Path(path).read_bytes()
    raw = gzip.decompress(raw) if path.endswith(".gz") else raw
    lines = [line for line in raw.decode("utf-8", errors="replace").splitlines() if whole(line)]
    scenarios, sizes = {}, {}
    for line in select_quarter("\n".join(lines)).splitlines():
        m = LINE.search(line)
        step, first = int(m[2]), int(m[6])
        sizes[step] = (int(m[3]), int(m[4]))
        frames = scenarios.setdefault(m[1], {}).setdefault(step, {})
        for i, field in enumerate(m[8].split()):
            frames[first + i] = tuple(int(v) for v in field.split(":"))
    ladder = [sizes[s] for s in chosen if s in sizes]
    out = {}
    for name, steps in scenarios.items():
        held = [s for s in chosen if s in steps]
        if held:
            count = min(next(i for i in range(len(steps[s]) + 1) if i not in steps[s]) for s in held)
            out[name] = {i: [steps[s][f] for f in range(count)] for i, s in enumerate(chosen) if s in steps}
    return out, ladder


def ladder_text(ladder, recovery):
    return f"steps {len(ladder)} {recovery}\n" + "".join(f"{w} {h}\n" for w, h in ladder)


def run(binary, arguments, stdin):
    out = subprocess.run([str(binary), *map(str, arguments)], input=stdin, check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def refit(binary, scenarios, ladder, recovery):
    """The model fitted on every frame of every scenario, and its signed miss
    (measured - predicted) / predicted per scenario and step."""
    samples = [(name, step, f) for name, steps in scenarios.items() for step, frames in steps.items() for f in frames]
    stdin = ladder_text(ladder, recovery) + f"{len(samples)}\n" + "".join(
        f"{step} {f[3]} {f[1]} {f[2]}\n" for _, step, f in samples)
    fitted = run(binary, ["fit"], stdin)
    errors = {}
    for (name, step, f), price in zip(samples, fitted["predicted"]):
        errors.setdefault(name, {}).setdefault(step, []).append((f[1] + f[2] - price) / price)
    return fitted, errors


def replay(binary, frames_by_step, ladder, recovery, fitted, target_ms, margin, hold_ms, top, first):
    count = len(frames_by_step[0])
    stdin = (ladder_text(ladder, recovery) + " ".join(f"{w}" for w in fitted["model"]) + " "
             + " ".join(f"{u}" for u in fitted["upscale"]) + f"\n{count}\n"
             + "".join(" ".join(f"{frames_by_step[s][i][0]} {frames_by_step[s][i][3]} {frames_by_step[s][i][1]} {frames_by_step[s][i][2]}"
                                for s in range(len(ladder))) + "\n" for i in range(count)))
    return run(binary, ["replay", target_ms * US_PER_MS, margin, hold_ms * US_PER_MS, top, first], stdin)


def whole_stats(values):
    return {"mean": statistics.fmean(values) / US_PER_MS, "p95": percentile(values, P95) / US_PER_MS,
            "sd": statistics.pstdev(values) / US_PER_MS, "max": max(values) / US_PER_MS}


def flight(scenario, replayed, target_ms):
    """Whole-frame figures of a replay, the share of frames whose scaled part
    ran over the target, and the live miss once the refit has corrected the
    price, the miss the up margin must cover."""
    steps = replayed["steps"]
    whole = [scenario[s][i][0] for i, s in enumerate(steps)]
    scaled = [scenario[s][i][1] + scenario[s][i][2] for i, s in enumerate(steps)]
    live = [abs(m - p) / p for m, p in zip(scaled, replayed["priced"]) if p > 0]
    finer = [abs(scenario[s - 1][i][1] + scenario[s - 1][i][2] - p) / p
             for i, (s, p) in enumerate(zip(steps, replayed["finer"])) if p > 0]
    share = {s: steps.count(s) / len(steps) for s in sorted(set(steps))}
    over = sum(v > target_ms * US_PER_MS for v in scaled) / len(scaled)
    return {**whole_stats(whole), "share": share, "over": over, "live_miss": percentile(live, MARGIN_SHARE),
            "finer_miss": percentile(finer, MARGIN_SHARE) if finer else 0.0}


def steps_table(scenarios, ladder):
    head = "| Scenario | " + " | ".join(f"{w}x{h}" for w, h in ladder) + " |\n|---|" + "---|" * len(ladder) + "\n"
    rows = [f"| {name} | " + " | ".join(f"{statistics.fmean(f[0] for f in steps[s]) / US_PER_MS:.1f}"
                                         for s in range(len(ladder))) + " |"
            for name, steps in scenarios.items() if len(steps) == len(ladder)]
    compose = [statistics.fmean(f[2] for steps in scenarios.values() if s in steps for f in steps[s]) / US_PER_MS
               for s in range(len(ladder))]
    rows.append("| compose, every scenario | " + " | ".join(f"{v:.1f}" for v in compose) + " |")
    return head + "\n".join(rows) + "\n\nMean whole frame in milliseconds; compose is the upscale into the panel's picture.\n"


def flight_table(results, ladder):
    head = ("| Scenario | Fixed mean | p95 | sd | Dynamic mean | p95 | sd | max | Switches | Time at each size |\n"
            "|---|---|---|---|---|---|---|---|---|---|\n")
    seen, rows = set(), []
    for r in results:
        if r["scenario"] in seen:
            continue
        seen.add(r["scenario"])
        at = ", ".join(f"{ladder[s][0]}x{ladder[s][1]} {v:.0%}" for s, v in r["share"].items())
        rows.append(f"| {r['scenario']} | {r['fixed']['mean']:.1f} | {r['fixed']['p95']:.1f} | {r['fixed']['sd']:.1f} | "
                    f"{r['mean']:.1f} | {r['p95']:.1f} | {r['sd']:.1f} | {r['max']:.1f} | {r['switches']} | {at} |")
    return head + "\n".join(rows) + "\n\nWhole frame in milliseconds.\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture")
    parser.add_argument("--cc", required=True)
    target = read_define(DYNRES, "RENDER_LAB_FRAME_TARGET_MS") - read_define(DYNRES, "RENDER_LAB_FRAME_OVERHEAD_MS")
    parser.add_argument("--targets", default=str(target), help="budgets for the scaled part in ms")
    parser.add_argument("--margins", default=str(read_define(RESOLUTION, "DEFAULT_UP_MARGIN_PERCENT")))
    parser.add_argument("--holds", default=str(read_define(RESOLUTION, "DEFAULT_UP_HOLD_US") // US_PER_MS),
                        help="step-up holds in ms")
    parser.add_argument("--top", type=int, default=read_define(DYNRES, "RENDER_LAB_LADDER_HALF"))
    parser.add_argument("--first", type=int, help="the step the flight starts at; the top when omitted")
    parser.add_argument("--steps", help="captured sizes forming the ladder, by capture index; the ladder's when omitted")
    parser.add_argument("--recovery", type=int, help="the first recovery-only step of --steps; none when omitted")
    parser.add_argument("--lead", help="the scenario reported first")
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--tables", type=pathlib.Path, help="directory for the docs generator's blocks")
    args = parser.parse_args()
    chosen = ([int(v) for v in args.steps.split(",")] if args.steps
              else list(range(read_define(DYNRES, "RENDER_LAB_LADDER_STEPS"))))
    recovery = args.recovery or len(chosen)
    scenarios, ladder = read_capture(args.capture, chosen)
    scenarios = dict(sorted(scenarios.items(), key=lambda item: item[0] != args.lead))
    if not scenarios:
        sys.exit(f"{args.capture}: no dynres_steps lines")
    with tempfile.TemporaryDirectory() as work:
        binary = build_probe(args.cc, PROBE_SOURCES, work)
        fitted, errors = refit(binary, scenarios, ladder, recovery)
        print("model", fitted["model"], "upscale", fitted["upscale"])
        print(f"cells: median miss / p{MARGIN_SHARE * PERCENT:.0f} |miss|, miss = (measured - predicted) / predicted")
        print("| Scenario | " + " | ".join(f"{w}x{h}" for w, h in ladder) + " |")
        print("|---|" + "---|" * len(ladder))
        for name, steps in errors.items():
            cells = [f"{statistics.median(e) * PERCENT:+.1f}% / {percentile([abs(v) for v in e], MARGIN_SHARE) * PERCENT:.1f}%"
                     for _, e in sorted(steps.items())]
            print(f"| {name} | " + " | ".join(cells) + " |")
        every = [abs(v) for steps in errors.values() for e in steps.values() for v in e]
        print(f"p{MARGIN_SHARE * PERCENT:.0f} |miss| over every frame: {percentile(every, MARGIN_SHARE) * PERCENT:.1f}%")
        results = []
        for name, steps in scenarios.items():
            if len(steps) < len(ladder):
                print(f"\n{name}: not captured at every step, fitted only")
                continue
            fixed = whole_stats([f[0] for f in steps[args.top]])
            print(f"\n{name}: fixed at {ladder[args.top][0]}x{ladder[args.top][1]} "
                  f"mean {fixed['mean']:.1f} p95 {fixed['p95']:.1f} sd {fixed['sd']:.1f} max {fixed['max']:.1f}")
            for target, margin, up in itertools.product(*(map(int, v.split(","))
                                                          for v in (args.targets, args.margins, args.holds))):
                r = replay(binary, steps, ladder, recovery, fitted, target, margin, up, args.top,
                           args.top if args.first is None else args.first)
                f = flight(steps, r, target)
                results.append({"scenario": name, "target": target, "margin": margin, "hold_ms": up,
                                "switches": r["switches"], "fixed": fixed, **f})
                at = ", ".join(f"{ladder[s][0]}x{ladder[s][1]} {v:.0%}" for s, v in f["share"].items())
                print(f"  target {target} margin {margin} hold {up}: mean {f['mean']:.1f} p95 {f['p95']:.1f} "
                      f"sd {f['sd']:.1f} max {f['max']:.1f} switches {r['switches']} over {f['over']:.0%} "
                      f"p{MARGIN_SHARE * PERCENT:.0f} miss drawn {f['live_miss']:.1%} finer {f['finer_miss']:.1%} | {at}")
    if args.tables:
        args.tables.mkdir(parents=True, exist_ok=True)
        (args.tables / "dynres-steps.md").write_text(steps_table(scenarios, ladder), encoding="utf-8")
        (args.tables / "dynres-flight.md").write_text(flight_table(results, ladder), encoding="utf-8")
    if args.json:
        args.json.write_text(json.dumps({"fit": fitted, "results": results}, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()

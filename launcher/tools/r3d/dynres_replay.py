#!/usr/bin/env python3
"""Replays a board's per-step frame costs through the engine's dynamic-resolution
predictor, so its target, margin and step-up delay can be tuned without
reflashing, and refits the cost model on every scenario.

    python launcher/tools/r3d/dynres_replay.py CAPTURE --cc CC [--json OUT]
        [--targets MS,...] [--margins PERCENT,...] [--holds MS,...] [--top STEP] [--first STEP]

CAPTURE is a device log holding the `dynres_steps` lines of
run_render_lab_dynres_steps_suite: every scenario's path drawn with each
ladder step held, at one fixed frame step, so frame i is the same pose at
every step. A replay draws frame i at the step the predictor picks and pays
that step's measured cost; the whole frame is the measured whole frame at
that step. dynres_replay_probe.c runs the engine's own fit and predictor.

It prints the refit model and its error per scenario and step, then for each
target, margin and step-up delay each scenario's whole-frame mean, p95 and
spread against the fixed scale at the top step.
"""

import argparse
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
from capture import select_quarter  # noqa: E402
from math_figures import build_probe  # noqa: E402

DYNRES = ROOT / "launcher/main/apps/render_lab/render_lab_dynres.h"
PROBE_SOURCES = (ROOT / "launcher/tools/r3d/dynres_replay_probe.c",
                 ROOT / "launcher/main/render/resolution/resolution.c")
US_PER_MS = 1000
PERCENT = 100
P95 = 0.95

LINE = re.compile(r"dynres_steps: (\S+) step=(\d+) (\d+)x(\d+) dt=(\d+)ms first=(\d+) n=(\d+)((?: -?\d+:-?\d+:-?\d+:-?\d+)*)")


def read_capture(path):
    """{scenario: {step: [(whole, draw, upscale, triangles), ...]}} and the ladder's sizes."""
    text = select_quarter(pathlib.Path(path).read_text(encoding="utf-8", errors="replace"))
    scenarios, sizes = {}, {}
    for m in LINE.finditer(text):
        step, first = int(m[2]), int(m[6])
        sizes[step] = (int(m[3]), int(m[4]))
        frames = scenarios.setdefault(m[1], {}).setdefault(step, {})
        for i, field in enumerate(m[8].split()):
            frames[first + i] = tuple(int(v) for v in field.split(":"))
    ladder = [sizes[s] for s in sorted(sizes)]
    out = {}
    for name, steps in scenarios.items():
        count = min(len(f) for f in steps.values())
        out[name] = {s: [steps[s][i] for i in range(count)] for s in sorted(steps)}
    return out, ladder


def ladder_text(ladder, recovery):
    return f"steps {len(ladder)} {recovery}\n" + "".join(f"{w} {h}\n" for w, h in ladder)


def run(binary, arguments, stdin):
    out = subprocess.run([str(binary), *map(str, arguments)], input=stdin, check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))]


def refit(binary, scenarios, ladder, recovery):
    """The model fitted on every frame of every scenario, and its error per scenario and step."""
    samples = [(name, step, f) for name, steps in scenarios.items() for step, frames in steps.items() for f in frames]
    stdin = ladder_text(ladder, recovery) + f"{len(samples)}\n" + "".join(
        f"{step} {f[3]} {f[1]} {f[2]}\n" for _, step, f in samples)
    fitted = run(binary, ["fit"], stdin)
    errors = {}
    for (name, step, f), price in zip(samples, fitted["predicted"]):
        errors.setdefault(name, {}).setdefault(step, []).append((price - (f[1] + f[2])) / (f[1] + f[2]))
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


def flight(scenario, steps):
    whole = [scenario[s][i][0] for i, s in enumerate(steps)]
    share = {s: steps.count(s) / len(steps) for s in sorted(set(steps))}
    return {**whole_stats(whole), "share": share}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture")
    parser.add_argument("--cc", required=True)
    parser.add_argument("--targets", default=str(read_define(DYNRES, "RENDER_LAB_TARGET_MS")))
    parser.add_argument("--margins", default="8")
    parser.add_argument("--holds", default="500", help="step-up holds in ms")
    parser.add_argument("--top", type=int, default=read_define(DYNRES, "RENDER_LAB_LADDER_HALF"))
    parser.add_argument("--first", type=int, help="the step the flight starts at; the top when omitted")
    parser.add_argument("--json", type=pathlib.Path)
    args = parser.parse_args()
    recovery = read_define(DYNRES, "RENDER_LAB_LADDER_RECOVERY")
    scenarios, ladder = read_capture(args.capture)
    if not scenarios:
        sys.exit(f"{args.capture}: no dynres_steps lines")
    with tempfile.TemporaryDirectory() as work:
        binary = build_probe(args.cc, PROBE_SOURCES, work)
        fitted, errors = refit(binary, scenarios, ladder, recovery)
        print("model", fitted["model"], "upscale", fitted["upscale"])
        print("| Scenario | " + " | ".join(f"{w}x{h}" for w, h in ladder) + " |")
        print("|---|" + "---|" * len(ladder))
        for name, steps in errors.items():
            cells = [f"{statistics.median(e) * PERCENT:+.1f}% / {percentile([abs(v) for v in e], P95) * PERCENT:.1f}%"
                     for _, e in sorted(steps.items())]
            print(f"| {name} | " + " | ".join(cells) + " |")
        results = []
        for name, steps in scenarios.items():
            fixed = whole_stats([f[0] for f in steps[args.top]])
            print(f"\n{name}: fixed at {ladder[args.top][0]}x{ladder[args.top][1]} "
                  f"mean {fixed['mean']:.1f} p95 {fixed['p95']:.1f} sd {fixed['sd']:.1f} max {fixed['max']:.1f}")
            for target, margin, up in itertools.product(*(map(int, v.split(","))
                                                          for v in (args.targets, args.margins, args.holds))):
                r = replay(binary, steps, ladder, recovery, fitted, target, margin, up, args.top,
                           args.top if args.first is None else args.first)
                f = flight(steps, r["steps"])
                results.append({"scenario": name, "target": target, "margin": margin, "hold_ms": up,
                                "switches": r["switches"], "fixed": fixed, **f})
                at = ", ".join(f"{ladder[s][0]}x{ladder[s][1]} {v:.0%}" for s, v in f["share"].items())
                print(f"  target {target} margin {margin} hold {up}: mean {f['mean']:.1f} p95 {f['p95']:.1f} "
                      f"sd {f['sd']:.1f} max {f['max']:.1f} switches {r['switches']} | {at}")
    if args.json:
        args.json.write_text(json.dumps({"fit": fitted, "results": results}, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()

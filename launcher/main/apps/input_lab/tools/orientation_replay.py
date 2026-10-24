"""Replay a labdump with --model accel|fused|both at fine and coarse cadence."""
import argparse
from dataclasses import dataclass
import gzip
import json
import math
from statistics import median
from functools import cache
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define

DISPLAY = ROOT / "launcher/main/display/display.h"
JUDGE = ROOT / "launcher/main/display/display_judge.h"
COARSE_SAMPLE_MS = 100
US_PER_MS = 1000
NO_QUARTER = -1
RESULT_CAPTURE = ROOT / "launcher/main/apps/input_lab/tools/data/orientation.log.gz"
RESULT_DOC = Path(__file__).resolve().parents[1] / "README.md"
RESULT_META = RESULT_CAPTURE.with_suffix("").with_suffix(".json")
SOURCES = (Path(__file__).with_name("orientation_replay_probe.c"),
           ROOT / "launcher/main/apps/input_lab/orientation_probe.c",
           ROOT / "launcher/main/display/display.c", ROOT / "launcher/main/input/motion/gravity.c")


@cache
def display_define(name):
    return read_define(JUDGE if name in JUDGE.read_text() else DISPLAY, name)


def parse_capture(lines):
    segments, samples = [], []
    open_gate = None
    for line in lines:
        if "LABDUMP_TRUNCATED" in line:
            raise ValueError("truncated orientation round")
        if "LABDUMP " not in line:
            continue
        fields = line.split("LABDUMP ", 1)[1].split()
        if fields[:1] == ["orientation_step"]:
            if len(fields) != 4:
                raise ValueError("invalid orientation segment")
            step, time_us, target = map(int, fields[1:])
            if time_us < 0 or (segments and (time_us <= segments[-1][1] or step <= segments[-1][0])):
                raise ValueError("segments must increase in time and script order")
            segments.append((step, time_us, target))
        elif fields[:1] == ["orientation_gate"]:
            if len(fields) != 4 or not segments:
                raise ValueError("invalid orientation ready gate")
            step, time_us = map(int, fields[1:3])
            if step != segments[-1][0] or time_us < segments[-1][1]:
                raise ValueError("ready gate outside its step")
            if fields[3] == "begin" and open_gate is None:
                open_gate = (step, time_us)
            elif fields[3] == "end" and open_gate is not None and open_gate[0] == step and time_us > open_gate[1]:
                open_gate = None
            else:
                raise ValueError("invalid orientation ready gate order")
        elif fields[:1] == ["orientation"]:
            if len(fields) != 9:
                raise ValueError("invalid orientation sample")
            step, *sample = map(int, fields[1:])
            if not segments or step != segments[-1][0] or sample[0] < segments[-1][1]:
                raise ValueError("sample needs its segment annotation")
            if samples and sample[0] <= samples[-1][0]:
                raise ValueError("sample timestamps must increase")
            samples.append(tuple(sample))
    if open_gate is not None:
        raise ValueError("unfinished orientation ready gate")
    if not samples or len(segments) < 2:
        raise ValueError("complete orientation round required")
    return segments, samples


@dataclass
class Score:
    wanted: int
    unwanted: int
    latencies_us: list
    turns: list


@cache
def default_quarter():
    name = next(line.split()[2] for line in DISPLAY.read_text().splitlines()
                if line.startswith("#define DISPLAY_DEFAULT_QUARTER "))
    return display_define(name)


CANDIDATES = {}


def classify(binary, samples):
    vectors = list(dict.fromkeys(tuple(sample[1:4]) for sample in samples))
    vectors = [v for v in vectors if (binary, *v, 0, False) not in CANDIDATES]
    if not vectors:
        return
    payload = "".join(" ".join(map(str, vector)) + "\n" for vector in vectors)
    result = subprocess.run([str(binary), "--candidates"], input=payload,
                            text=True, capture_output=True, check=True)
    for vector, line in zip(vectors, result.stdout.splitlines(), strict=True):
        for index, quarter in enumerate(map(int, line.split())):
            CANDIDATES[binary, *vector, index // 2, bool(index % 2)] = quarter


def candidate(binary, x, y, z, old, tilt_limits):
    classify(binary, [(0, x, y, z)])
    return CANDIDATES[binary, x, y, z, old, tilt_limits]


def motion_quarter(binary, sample, old):
    _, x, y, z, *_ = sample
    quarter = candidate(binary, x, y, z, old, False)
    if quarter in (old, NO_QUARTER):
        return None, False
    return quarter, candidate(binary, x, y, z, old, True) == quarter


def measure(binary, samples, events, initial, segments=()):
    scale = read_define(ROOT / "launcher/main/input/input.h", "INPUT_ACCEL_ONE_G")
    samples = [(sample[0], *(value / scale for value in sample[1:4])) for sample in samples]
    classify(binary, samples)
    pending, turns, unwanted, latencies = None, [], 0, []
    position = 0
    current = initial
    quiet_start = next((time for _, time, target in segments if target == NO_QUARTER), math.inf)
    settle_us = display_define("DISPLAY_SETTLE_MS") * US_PER_MS
    for sample in samples:
        time = sample[0]
        candidate, valid = motion_quarter(binary, sample, current)
        if candidate is None:
            pending = None
        elif pending is None or pending[0] != candidate:
            pending = (candidate, time, time if valid else None, time if valid else None)
        elif valid:
            pending = (candidate, pending[1], pending[2] if pending[2] is not None else time, time)
        else:
            pending = (candidate, pending[1], None, None)
        while position < len(events) and events[position][0] <= time:
            commit, quarter = events[position]
            wanted = (commit < quiet_start and pending is not None and pending[0] == quarter
                      and pending[2] is not None and commit - pending[2] >= settle_us
                      and commit - pending[3] <= display_define("DISPLAY_SAMPLE_MS") * 2 * US_PER_MS)
            if wanted:
                latencies.append(commit - pending[1])
                turns.append((pending[1], commit, quarter))
            else:
                unwanted += 1
            current = quarter
            pending = None
            position += 1
    unwanted += len(events) - position
    return Score(len(turns), unwanted, latencies, turns)


def run(binary, model, period_ms, initial, samples):
    text = "".join(" ".join(map(str, sample)) + "\n" for sample in samples)
    result = subprocess.run([str(binary), model, str(period_ms), str(initial)], input=text,
                            text=True, capture_output=True, check=True)
    return [tuple(map(int, line.split())) for line in result.stdout.splitlines()]


def gate_trace(binary, model, period_ms, initial, samples):
    payload = "".join(" ".join(map(str, sample)) + "\n" for sample in samples)
    result = subprocess.run([str(binary), model, str(period_ms), str(initial), "--gates"],
                            input=payload, text=True, capture_output=True, check=True)
    return [(fields[0], tuple(map(int, fields[1:9])) + tuple(map(float, fields[9:12]))
             + tuple(map(int, fields[12:]))) if fields[0] == "G" else
            (fields[0], tuple(map(int, fields[1:])))
            for fields in (line.split() for line in result.stdout.splitlines())]


def gate_report(binary, rows, turns):
    classify(binary, [(row[0], *row[8:11]) for kind, row in rows if kind == "G"])
    limits = {"settle": "DISPLAY_SETTLE_MS", "after-flat": "DISPLAY_AFTER_FLAT_MS",
              "after-accel": "DISPLAY_AFTER_ACCEL_MS", "after-swing": "DISPLAY_AFTER_SWING_MS",
              "touch": "DISPLAY_AFTER_TOUCH_MS"}
    for start, commit, quarter in turns:
        spans = {}
        active = {}
        last = {}
        for kind, row in rows:
            if kind != "G" or not start <= row[0] <= commit:
                continue
            time, current, predicted, predicted_us, flat_us, accel_us, swing_us, touch_us = row[:8]
            blocked = set()
            if predicted != quarter:
                filtered_quarter, tilt_valid = motion_quarter(binary, (time, *row[8:11]), current)
                blocked.add("filtered tilt" if filtered_quarter == quarter and not tilt_valid
                            else "filtered hysteresis / angle")
            for name, since in zip(limits, (predicted_us, flat_us, accel_us, swing_us, touch_us)):
                if name == "settle" and predicted != quarter:
                    continue
                if since != -(1 << 63) and time - since < display_define(limits[name]) * US_PER_MS:
                    blocked.add(name)
            for name in active.keys() - blocked:
                spans[name] = max(spans.get(name, 0), time - active[name])
                del active[name]
            for name in blocked:
                active.setdefault(name, time)
                last[name] = time
        for name, began in active.items():
            spans[name] = max(spans.get(name, 0), commit - began)
        if not last:
            yield start, commit, quarter, "none", 0, "none", 0
            continue
        last_gate = max(last, key=last.get)
        dominant = max(spans, key=spans.get)
        yield start, commit, quarter, last_gate, spans[last_gate] / US_PER_MS, dominant, spans[dominant] / US_PER_MS


def validate_targets(binary, segments):
    result = subprocess.run([str(binary), "--script"], text=True, capture_output=True, check=True)
    targets = {int(step): int(target) for step, target, _ in
               (line.split(" ", 2) for line in result.stdout.splitlines())}
    if segments[-1][0] != max(targets):
        raise ValueError("complete orientation round required")
    if any(targets.get(step) != target for step, _, target in segments):
        raise ValueError("segment target does not match orientation script")


def plot_gates(rows, output):
    from docs.plot_style import subplots, save, stacked_barh, style_axes, SLOTS
    figure, axes = subplots(1, 2, (12, 7))
    last_chart, longest_chart = [], []
    for model, period, quarter, gate, duration, dominant, dominant_ms in rows:
        label = f"{model} {period} ms q{quarter}"
        last_chart.append((label, [(gate, duration)]))
        longest_chart.append((label, [(dominant, dominant_ms)]))
    for axis, title, chart in zip(axes, ("Last blocking gate", "Longest holding gate"), (last_chart, longest_chart)):
        gates = sorted({gate for _, segments in chart for gate, _ in segments})
        stacked_barh(axis, chart, dict(zip(gates, SLOTS)))
        style_axes(axis, title)
        axis.set_xlabel("Hold (ms)")
        axis.legend(loc="lower right")
    save(figure, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, nargs="?")
    parser.add_argument("--model", default="both", choices=("accel", "fused", "both"))
    parser.add_argument("--results", action="store_true", help="generate the orientation results block")
    parser.add_argument("--check", action="store_true", help="check the generated block")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--gates", action="store_true", help="show the last holding gate for each commit")
    parser.add_argument("--initial-quarter", type=int,
                        help="quarter actually committed when recording started, not the first requested pose")
    args = parser.parse_args()
    if args.capture is None:
        if not args.results:
            parser.error("capture required unless --results is set")
        args.capture = RESULT_CAPTURE
    if args.initial_quarter is None:
        if not args.results:
            parser.error("--initial-quarter required for a capture")
        args.initial_quarter = json.loads(RESULT_META.read_text(encoding="utf-8"))["initial_quarter"]
    if args.check and not args.results:
        parser.error("--check requires --results")
    try:
        opener = gzip.open if args.capture.suffix == ".gz" else open
        with opener(args.capture, "rt", encoding="utf-8") as capture:
            segments, samples = parse_capture(capture)
    except ValueError as error:
        parser.error(str(error))
    initial = args.initial_quarter
    if not 0 <= initial <= display_define("DISPLAY_LANDSCAPE_UPSIDE_DOWN"):
        parser.error("invalid initial quarter")
    sys.path.insert(0, str(ROOT / "launcher/tools/math"))
    from math_figures import build_probe
    models = ("accel", "fused") if args.model == "both" else (args.model,)
    with tempfile.TemporaryDirectory() as directory:
        binary = build_probe(args.cc, SOURCES, directory)
        try:
            validate_targets(binary, segments)
        except ValueError as error:
            parser.error(str(error))
        result_rows = []
        for model in models:
            for period in (display_define("DISPLAY_SAMPLE_MS"), COARSE_SAMPLE_MS):
                events = run(binary, model, period, initial, samples)
                score = measure(binary, samples, events, initial, segments)
                latency = [value / US_PER_MS for value in score.latencies_us]
                result_rows.append((model, period, score, latency))
                if args.results:
                    continue
                print(f"{model} {period} ms: wanted={score.wanted} unwanted={score.unwanted} "
                      f"median_ms={median(latency):.1f} max_ms={max(latency):.1f}" if latency
                      else f"{model} {period} ms: wanted=0 unwanted={score.unwanted} median_ms=- max_ms=-")
                for start, commit, quarter in score.turns:
                    print(f"  {start / 1e6:.2f} -> {commit / 1e6:.2f} q{quarter}: "
                          f"{(commit - start) / US_PER_MS:.1f} ms")
                if args.gates:
                    for start, commit, quarter, gate, duration, dominant, dominant_ms in gate_report(binary,
                            gate_trace(binary, model, period, initial, samples), score.turns):
                        print(f"  q{quarter} at {commit / 1e6:.2f}: last gate {gate} for {duration:.1f} ms; "
                              f"longest hold {dominant} for {dominant_ms:.1f} ms")

        if args.results:
            if args.model != "both":
                parser.error("--results needs both models")
            sys.path.insert(0, str(ROOT / "launcher/tools"))
            from render.generated_blocks import replace_block
            body = "| Model | Cadence | Wanted turns | Unwanted switches | Median latency | Max latency |\n"
            body += "|---|---:|---:|---:|---:|---:|\n"
            for model, period, score, latency in result_rows:
                body += (f"| {model} | {period} ms | {score.wanted} | {score.unwanted} | "
                         f"{median(latency):.0f} ms | {max(latency):.0f} ms |\n")
            body += "\n![Gate hold durations](images/orientation-gates.png)\n\n"
            body += "<details>\n<summary>Per-commit gate durations</summary>\n\n"
            body += "| Model | Cadence | Quarter | Last gate | Last hold | Longest gate | Longest hold |\n"
            body += "|---|---:|---:|---|---:|---|---:|\n"
            gate_rows = []
            for model, period, score, _ in result_rows:
                rows = gate_trace(binary, model, period, initial, samples)
                for _, _, quarter, gate, duration, dominant, dominant_ms in gate_report(binary, rows, score.turns):
                    gate_rows.append((model, period, quarter, gate, duration, dominant, dominant_ms))
                    body += (f"| {model} | {period} ms | {quarter} | {gate} | {duration:.0f} ms | "
                             f"{dominant} | {dominant_ms:.0f} ms |\n")
            body += "\n</details>\n"
            changed = replace_block(RESULT_DOC, "orientation-results", body, check=args.check,
                                    command="python launcher/main/apps/input_lab/tools/orientation_replay.py --results --check --cc gcc")
            if args.check and changed:
                parser.exit(1, "orientation results differ from capture\n")
            if not args.check:
                plot_gates(gate_rows, RESULT_DOC.parent / "images/orientation-gates.png")


if __name__ == "__main__":
    main()

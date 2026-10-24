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

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define

DISPLAY = ROOT / "launcher/main/display/display.h"
COARSE_SAMPLE_MS = 100
US_PER_MS = 1000
NO_QUARTER = -1
RESULT_CAPTURE = ROOT / "launcher/main/apps/input_lab/tools/data/orientation.log.gz"
RESULT_DOC = ROOT / "docs/input/Orientation.md"
RESULT_META = RESULT_CAPTURE.with_suffix("").with_suffix(".json")
SOURCES = (Path(__file__).with_name("orientation_replay_probe.c"),
           ROOT / "launcher/main/apps/input_lab/orientation_probe.c",
           ROOT / "launcher/main/display/display.c", ROOT / "launcher/main/input/motion/gravity.c")


@cache
def display_define(name):
    return read_define(DISPLAY, name)


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


def motion_quarter(sample, old):
    _, x, y, z, *_ = sample
    plane = math.hypot(x, y)
    magnitude = math.hypot(plane, z)
    if not plane or not magnitude:
        return None, False
    quarter = ((display_define("DISPLAY_LANDSCAPE_UPSIDE_DOWN") if x > 0 else display_define("DISPLAY_LANDSCAPE"))
               if abs(x) > abs(y) else
               (display_define("DISPLAY_PORTRAIT") if y > 0 else display_define("DISPLAY_PORTRAIT_UPSIDE_DOWN")))
    if quarter == old:
        return None, False
    old_axes = {display_define("DISPLAY_PORTRAIT"): (0, 1),
                display_define("DISPLAY_LANDSCAPE"): (-1, 0),
                display_define("DISPLAY_PORTRAIT_UPSIDE_DOWN"): (0, -1),
                display_define("DISPLAY_LANDSCAPE_UPSIDE_DOWN"): (1, 0)}[old]
    aligned = x * old_axes[0] + y * old_axes[1]
    edge = (display_define("DISPLAY_QUARTER_DEG") +
            display_define("DISPLAY_HYSTERESIS_GAP_DEG")) / 2
    if aligned >= plane * math.cos(math.radians(edge)):
        return None, False
    count = display_define("DISPLAY_LANDSCAPE_UPSIDE_DOWN") + 1
    relative = (quarter - default_quarter()) % count
    max_tilt = ("DISPLAY_TILT_DEFAULT_MAX_DEG" if relative == 0 else
                "DISPLAY_TILT_OPPOSITE_MAX_DEG" if relative == count // 2 else
                "DISPLAY_TILT_ADJACENT_MAX_DEG")
    tilt = math.degrees(math.asin(-z / magnitude))
    valid = display_define("DISPLAY_TILT_MIN_DEG") <= tilt <= display_define(max_tilt)
    return quarter, valid


def measure(samples, events, initial, segments=()):
    pending, turns, unwanted, latencies = None, [], 0, []
    position = 0
    current = initial
    quiet_start = next((time for _, time, target in segments if target == NO_QUARTER), math.inf)
    settle_us = display_define("DISPLAY_SETTLE_MS") * US_PER_MS
    for sample in samples:
        time = sample[0]
        candidate, valid = motion_quarter(sample, current)
        if candidate is not None and (pending is None or pending[0] != candidate):
            pending = (candidate, time, time if valid else None, time if valid else None)
        elif candidate is not None and valid:
            pending = (candidate, pending[1], pending[2] if pending[2] is not None else time, time)
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


def gate_report(rows, turns):
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
                filtered_quarter, tilt_valid = motion_quarter((time, *row[8:11]), current)
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
                score = measure(samples, events, initial, segments)
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
                    for start, commit, quarter, gate, duration, dominant, dominant_ms in gate_report(
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
            changed = replace_block(RESULT_DOC, "orientation-results", body, check=args.check,
                                    command="python launcher/tools/input/orientation_replay.py --results --check --cc gcc")
            if args.check and changed:
                parser.exit(1, "orientation results differ from capture\n")


if __name__ == "__main__":
    main()

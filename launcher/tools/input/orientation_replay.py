"""Replay a labdump with --model accel|fused|both at fine and coarse cadence."""
import argparse
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
SOURCES = (Path(__file__).with_name("orientation_replay_probe.c"),
           ROOT / "launcher/main/apps/input_lab/orientation_probe.c",
           ROOT / "launcher/main/display/display.c", ROOT / "launcher/main/input/motion/gravity.c")


def parse_capture(lines):
    segments, samples = [], []
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
        elif fields[:1] == ["orientation"]:
            if len(fields) != 9:
                raise ValueError("invalid orientation sample")
            step, *sample = map(int, fields[1:])
            if not segments or step != segments[-1][0] or sample[0] < segments[-1][1]:
                raise ValueError("sample needs its segment annotation")
            if samples and sample[0] <= samples[-1][0]:
                raise ValueError("sample timestamps must increase")
            samples.append(tuple(sample))
    if not samples or len(segments) < 2:
        raise ValueError("complete orientation round required")
    return segments, samples


def measure(segments, events, initial):
    rows = []
    current = initial
    for (step, start, target), (_, end, _) in zip(segments, segments[1:]):
        changes = [(time, q) for time, q in events if start <= time < end]
        latency = 0 if current == target else None
        for time, q in changes:
            if q == target and latency is None:
                latency = time - start
            current = q
        false = sum(target == NO_QUARTER or q != target for _, q in changes)
        rows.append((step, len(changes), false, latency))
    return rows


def run(binary, model, period_ms, initial, samples):
    text = "".join(" ".join(map(str, sample)) + "\n" for sample in samples)
    result = subprocess.run([str(binary), model, str(period_ms), str(initial)], input=text,
                            text=True, capture_output=True, check=True)
    return [tuple(map(int, line.split())) for line in result.stdout.splitlines()]


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
    parser.add_argument("capture", type=Path)
    parser.add_argument("--model", required=True, choices=("accel", "fused", "both"))
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--initial-quarter", type=int, required=True,
                        help="quarter actually committed when recording started, not the first requested pose")
    args = parser.parse_args()
    try:
        segments, samples = parse_capture(args.capture.read_text(encoding="utf-8").splitlines())
    except ValueError as error:
        parser.error(str(error))
    initial = args.initial_quarter
    if not 0 <= initial <= read_define(DISPLAY, "DISPLAY_LANDSCAPE_UPSIDE_DOWN"):
        parser.error("invalid initial quarter")
    sys.path.insert(0, str(ROOT / "launcher/tools/math"))
    from math_figures import build_probe
    models = ("accel", "fused") if args.model == "both" else (args.model,)
    targets = {step: target for step, _, target in segments}
    with tempfile.TemporaryDirectory() as directory:
        binary = build_probe(args.cc, SOURCES, directory)
        try:
            validate_targets(binary, segments)
        except ValueError as error:
            parser.error(str(error))
        for model in models:
            for period in (read_define(DISPLAY, "DISPLAY_SAMPLE_MS"), COARSE_SAMPLE_MS):
                events = run(binary, model, period, initial, samples)
                print(f"{model}, {period} ms (unit fused input, filtered magnitude gate)" if model == "fused"
                      else f"{model}, {period} ms")
                print("segment switches false latency_ms")
                for step, switches, false, latency in measure(segments, events, initial):
                    wanted = "hold" if targets[step] == NO_QUARTER else "missed"
                    print(step, switches, false, wanted if latency is None else f"{latency / US_PER_MS:.1f}")


if __name__ == "__main__":
    main()

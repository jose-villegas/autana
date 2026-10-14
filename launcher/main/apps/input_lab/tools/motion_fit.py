"""Summarize calibration rounds and stationary stretches and flat orientation; no correction is applied."""

import argparse
import re
import sys
from statistics import median, pstdev
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools"))
from build.c_header import read_define

COUNTS_PER_DPS = read_define(Path(__file__).resolve().parents[3] / "input" / "input.h", "INPUT_ROTATION_ONE_DPS")
MILLI = 1000
DEFAULT_WINDOW_SECONDS = 60
STILL_TILT_BAND_DEG = 2
STILL_TURN_LIMIT_DPS = 15
FLAT_TILT_LIMIT_DEG = 10
MAX_SAMPLE_GAP_MS = 250
TRANSITION_PREVIEW_COUNT = 5
QUARTERS = range(4)
AXES = ("x", "y", "z")
STAMP = re.compile(r"\((\d+)\)")
CALIB = re.compile(r"calib x off (-?\d+) scale (-?\d+) y off (-?\d+) scale (-?\d+) z off (-?\d+) scale (-?\d+) gyro (-?\d+),(-?\d+),(-?\d+)")
POSE = re.compile(r"pose (\w+) ((?:-?\d+ ){6}\d+)")
MOTION = re.compile(r"motion raw (-?\d+),(-?\d+),(-?\d+).*?tilt (-?\d+).*?bearing (-?\d+).*?turn (\d+).*?rot (-?\d+),(-?\d+),(-?\d+) quarter (\d+)")


def parse(lines):
    rounds, motion = [], []
    current = None
    before_round = True
    for line in lines:
        if "round imu" in line:
            before_round = False
            current = {"poses": {}, "fit": None}
            rounds.append(current)
        pose = POSE.search(line)
        calib = CALIB.search(line)
        if pose or calib:
            if current is None or current["fit"] is not None:
                current = {"poses": {}, "fit": None}
                rounds.append(current)
            if pose:
                current["poses"][pose[1]] = tuple(map(int, pose[2].split()))
            if calib:
                current["fit"] = tuple(map(int, calib.groups()))
        sample, stamp = MOTION.search(line), STAMP.search(line)
        if sample and stamp:
            values = tuple(map(int, sample.groups()))
            motion.append({"ms": int(stamp[1]), "raw": values[:3], "tilt": values[3],
                           "bearing": values[4], "turn": values[5], "rotation": values[6:9],
                           "quarter": values[9], "before_round": before_round})
    return rounds, motion


def endpoints(motion, seconds):
    if not motion:
        return [], []
    width = seconds * MILLI
    return ([s for s in motion if s["ms"] <= motion[0]["ms"] + width],
            [s for s in motion if s["ms"] >= motion[-1]["ms"] - width])


def gyro_mean(samples):
    if not samples:
        return None
    return tuple(sum(s["rotation"][axis] for s in samples) * MILLI / (len(samples) * COUNTS_PER_DPS)
                 for axis in range(len(AXES)))


def rest_stretch(motion):
    longest, run = [], []
    for sample in motion:
        if not sample["before_round"]:
            break
        if sample["turn"] > STILL_TURN_LIMIT_DPS:
            run = []
            continue
        if run and (sample["ms"] - run[-1]["ms"] > MAX_SAMPLE_GAP_MS
                    or abs(sample["tilt"] - run[0]["tilt"]) > STILL_TILT_BAND_DEG):
            run = []
        run.append(sample)
        if len(run) > len(longest):
            longest = run.copy()
    return longest


def flat_samples(motion):
    return [s for s in motion if s["tilt"] < FLAT_TILT_LIMIT_DEG
            and s["raw"][2] < 0 and s["turn"] <= STILL_TURN_LIMIT_DPS]


def transitions(motion):
    flat_times = {s["ms"] for s in flat_samples(motion)}
    return [(a["quarter"], b) for a, b in zip(motion, motion[1:])
            if a["ms"] in flat_times and b["ms"] in flat_times
            and b["ms"] - a["ms"] <= MAX_SAMPLE_GAP_MS
            and a["quarter"] != b["quarter"]]


def gyro_noise(samples):
    if not samples:
        return None
    return tuple(pstdev(s["rotation"][axis] for s in samples) * MILLI / COUNTS_PER_DPS
                 for axis in range(len(AXES)))


def vector(values):
    return ",".join(f"{v:.0f}" for v in values) if values is not None else "unavailable"


def report(captures, seconds):
    out = ["Offsets: mg; scales: ppm (unity 1000000); gyro: mdps. Delta compares consecutive completed rounds.",
           f"Still: turn <= {STILL_TURN_LIMIT_DPS} dps; rest tilt within {STILL_TILT_BAND_DEG} deg of stretch start.",
           f"Rest: longest pre-round stretch; gaps <= {MAX_SAMPLE_GAP_MS} ms; endpoint windows may overlap.",
           f"Flat face-up: tilt < {FLAT_TILT_LIMIT_DEG} deg, z < 0, still turn limit.",
           "", "| Capture / round | Poses | Offset xyz | Scale xyz | Gyro xyz | Delta off / scale / gyro |",
           "|---|---|---|---|---|---|"]
    previous = None
    for name, rounds, motion in captures:
        for index, round_ in enumerate(rounds, 1):
            fit = round_["fit"]
            if fit is None:
                out.append(f"| {name} / {index} | {len(round_['poses'])} | incomplete | | | |")
                continue
            off, scale, gyro = fit[0:6:2], fit[1:6:2], fit[6:]
            delta = "unavailable"
            if previous is not None:
                d = tuple(a - b for a, b in zip(fit, previous))
                delta = " / ".join(map(vector, (d[0:6:2], d[1:6:2], d[6:])))
            out.append(f"| {name} / {index} | {len(round_['poses'])} | {vector(off)} | {vector(scale)} | {vector(gyro)} | {delta} |")
            previous = fit
    out.extend(["", f"| Capture | Rest uptime s | First {seconds:g}s gyro xyz / n | Last {seconds:g}s gyro xyz / n | Rest sigma xyz |",
                "|---|---|---|---|---|"])
    for name, rounds, motion in captures:
        rest = rest_stretch(motion)
        if not rest:
            out.append(f"| {name} | no pre-round still stretch | | | |")
            continue
        first, last = endpoints(rest, seconds)
        uptime = f"{rest[0]['ms'] / MILLI:.3f}-{rest[-1]['ms'] / MILLI:.3f}"
        out.append(f"| {name} | {uptime} | {vector(gyro_mean(first))} / {len(first)} | {vector(gyro_mean(last))} / {len(last)} | {vector(gyro_noise(rest))} |")
    out.extend(["", "| Capture | Flat still n | Quarter shares q0,q1,q2,q3 % | Median bearing deg | Flat-still transitions / first few (uptime s: old -> new) |",
                "|---|---|---|---|---|"])
    for name, rounds, motion in captures:
        flat = flat_samples(motion)
        if not flat:
            out.append(f"| {name} | 0 | unavailable | unavailable | 0 |")
            continue
        shares = ",".join(f"{sum(s['quarter'] == q for s in flat) * 100 / len(flat):.1f}" for q in QUARTERS)
        changes = transitions(motion)
        preview = "; ".join(f"{s['ms'] / MILLI:.3f}: {old} -> {s['quarter']}"
                            for old, s in changes[:TRANSITION_PREVIEW_COUNT])
        out.append(f"| {name} | {len(flat)} | {shares} | {median(s['bearing'] for s in flat):g} | {len(changes)} / {preview or 'none'} |")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--window-seconds", type=float, default=DEFAULT_WINDOW_SECONDS)
    args = parser.parse_args()
    if args.window_seconds <= 0:
        parser.error("--window-seconds must be positive")
    captures = []
    for path in args.captures:
        rounds, motion = parse(path.read_text(encoding="utf-8", errors="replace").splitlines())
        if any(b["ms"] < a["ms"] for a, b in zip(motion, motion[1:])):
            parser.error(f"{path}: uptime resets; supply one boot per capture")
        captures.append((path.name, rounds, motion))
    print(report(captures, args.window_seconds))


if __name__ == "__main__":
    main()

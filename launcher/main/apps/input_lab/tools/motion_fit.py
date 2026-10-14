"""Summarize calibration rounds and stationary capture endpoints; no correction is applied."""

import argparse
import re
from pathlib import Path

COUNTS_PER_DPS = 64
MILLI = 1000
DEFAULT_WINDOW_SECONDS = 10
AXES = ("x", "y", "z")
STAMP = re.compile(r"\((\d+)\)")
CALIB = re.compile(r"calib x off (-?\d+) scale (-?\d+) y off (-?\d+) scale (-?\d+) z off (-?\d+) scale (-?\d+) gyro (-?\d+),(-?\d+),(-?\d+)")
POSE = re.compile(r"pose (\w+) ((?:-?\d+ ){6}\d+)")
MOTION = re.compile(r"motion raw (-?\d+),(-?\d+),(-?\d+).*?tilt (-?\d+).*?rot (-?\d+),(-?\d+),(-?\d+) quarter (\d+)")


def parse(lines):
    rounds, motion = [], []
    current = None
    for line in lines:
        if "round imu quarter " in line:
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
                           "rotation": values[4:7], "quarter": values[7]})
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


def transitions(motion):
    return [(a["quarter"], b) for a, b in zip(motion, motion[1:]) if a["quarter"] != b["quarter"]]


def vector(values):
    return ",".join(f"{v:.0f}" for v in values) if values is not None else "unavailable"


def report(captures, seconds):
    out = ["Offsets: mg; scales: ppm (unity 1000000); gyro: mdps. Delta compares consecutive completed rounds.",
           "Endpoint gyro means require the board to be held still; overlapping endpoint windows are allowed.",
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
    out.extend(["", f"| Capture | First {seconds:g}s gyro xyz / n | Last {seconds:g}s gyro xyz / n |",
                "|---|---|---|"])
    for name, rounds, motion in captures:
        first, last = endpoints(motion, seconds)
        out.append(f"| {name} | {vector(gyro_mean(first))} / {len(first)} | {vector(gyro_mean(last))} / {len(last)} |")
    out.extend(["", "| Capture | Time ms | Quarter transition | Tilt from flat deg | Raw xyz Q12 |", "|---|---|---|---|---|"])
    for name, rounds, motion in captures:
        changes = transitions(motion)
        for old, sample in changes:
            out.append(f"| {name} | {sample['ms']} | {old} -> {sample['quarter']} | {sample['tilt']} | {vector(sample['raw'])} |")
        if not changes:
            out.append(f"| {name} | | none recorded | | |")
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

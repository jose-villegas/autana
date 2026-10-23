"""Decimate retained motion segments into screen-axis acceleration counts."""
import argparse
from bisect import bisect_right
from pathlib import Path
import shlex
import sys

from shake_fit import read_capture, STEP_MS, MS_PER_S
from build.c_header import read_define

ROOT = Path(__file__).resolve().parents[4]
MG_PER_G = read_define(Path(__file__).resolve().parents[1] / "imu_probe.h", "IMU_PROBE_MILLI")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--hz-header", type=Path, required=True)
    parser.add_argument("--hz-name", required=True)
    parser.add_argument("--segment", action="append", required=True,
                        help="STEP:offset_ms:duration_ms; concatenate in argument order")
    args = parser.parse_args()
    frame_ms = MS_PER_S // read_define(args.hz_header, args.hz_name)
    one_g = read_define(ROOT / "main/input/input.h", "INPUT_ACCEL_ONE_G")
    samples = read_capture(args.capture)
    rows = []
    for segment in args.segment:
        step, offset, duration = segment.split(":")
        offset, duration = int(offset), int(duration)
        if offset < 0 or duration <= 0 or offset + duration > STEP_MS or duration % frame_ms:
            parser.error("segments must fit a capture step and contain whole frames")
        selected = [s for s in samples if s["step"] == step]
        if not selected:
            parser.error(f"no samples for {step}")
        start_ms = selected[0]["ms"] // STEP_MS * STEP_MS
        times = [s["ms"] - start_ms for s in selected]
        for ms in range(offset, offset + duration, frame_ms):
            index = bisect_right(times, ms) - 1
            if index < 0 or ms - times[index] >= frame_ms:
                parser.error(f"missing sample at {step}:{ms}")
            rows.append(tuple(round(value * one_g / MG_PER_G) for value in selected[index]["acc"]))
    command = shlex.join(["python", *sys.argv])
    text = ["/* GENERATED FILE - do not edit.", f" * {command}", " */", "#pragma once", "",
            '#include "math/linear/vec3i.h"', "",
            f"#define MOTION_REPLAY_FRAME_MS {frame_ms}", "",
            "static const vec3i_t motion_replay_acceleration[] = {"]
    text.extend(f"    {{{x}, {y}, {z}}}," for x, y, z in rows)
    text.extend(["};", ""])
    args.output.write_text("\n".join(text), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()

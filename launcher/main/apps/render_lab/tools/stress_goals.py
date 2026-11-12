"""Generate stress performance goals from complete board replay captures."""
import argparse
from fractions import Fraction
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path[:0] = [str(ROOT / "launcher/tools/perf"), str(ROOT / "launcher/tools")]
from build.c_header import read_define
from frame_profile import replays

SUITE = ROOT / "launcher/main/apps/render_lab/tests/suite_stress_perf.c"
STAGE_HEADER = ROOT / "launcher/main/render/r3d_stages.h"
STAGES = tuple(read_define(STAGE_HEADER, f"R3D_STAGE_{name}_NAME")
               for name in ("CULL", "TRANSFORM", "DRAW", "UPSCALE"))
US_PER_MS = read_define(ROOT / "launcher/packages/math/include/math/scalar/units.h", "MATH_US_PER_MS")
GOAL_MARGIN = Fraction("1.01")
CEILING_MARGIN = Fraction("1.15")
GOAL_QUANTUM_US = 10
UNPEGGED_PRESET = "stress-uniform-full"
CASE_SEPARATOR = read_define(ROOT / "launcher/test/suites.h", "SUITE_TEST_SEPARATOR")
CASE = re.compile(r"^TEST_TIME name=(\S+)", re.MULTILINE)  # timing.c, after each test's lines
TABLE_ROW = re.compile(r'\{"(stress-[^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*\{[^}]+\},\s*\{[^}]+\}\}')


def table_rows():
    """The suite owns the row order and the exact preset, quarter and size keys."""
    table = SUITE.read_text(encoding="utf-8").split("static const stress_goal_t stress_goals[] = {", 1)[1].split("};", 1)[0]
    rows = TABLE_ROW.findall(table)
    if not rows or len(set(rows)) != len(rows):
        raise ValueError(f"{SUITE}: empty or duplicate goal rows")
    return rows


def capture_means(path, rows):
    """Mean each row's stages over its replayed poses, before comparing captures."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if not replays(text):
        raise ValueError(f"{path}: no FRAME_PROFILE_REPLAY lines")
    grouped = {}
    start = 0
    for marker in CASE.finditer(text):
        segment, start = text[start:marker.start()], marker.end()
        case = marker[1].split(CASE_SEPARATOR)
        if case[0] != "test_stress_stages":
            continue
        if len(case) != 4 or case[1] not in {key for key, _, _ in rows}:
            raise ValueError(f"{path}: unknown case {marker[1]}")
        for pose in replays(segment):
            if (pose["row"], pose["quarter"]) != tuple(case[1:3]):
                raise ValueError(f"{path}: replay {pose['row']} {pose['quarter']} inside case {marker[1]}")
            key = tuple(case[1:])
            if key not in rows:
                raise ValueError(f"{path}: unknown row {' '.join(key)}")
            missing = set(STAGES) - pose["stages"].keys()
            if missing:
                raise ValueError(f"{path}: {' '.join(key)}: missing stages {', '.join(sorted(missing))}")
            grouped.setdefault(key, []).append([Fraction(str(pose["stages"][stage])) * US_PER_MS for stage in STAGES])
    for row in rows:
        if row not in grouped:
            raise ValueError(f"{path}: missing row {' '.join(row)}")
    return {key: [sum(values) / len(poses) for values in zip(*poses)] for key, poses in grouped.items()}


def ceil_goal(value):
    """Round a pegged microsecond value up, reserving zero for unpegged goals."""
    return max(GOAL_QUANTUM_US, math.ceil(value / GOAL_QUANTUM_US) * GOAL_QUANTUM_US)


def generate(paths):
    rows = table_rows()
    captures = [capture_means(path, rows) for path in paths]
    if not captures:
        raise ValueError("no captures")
    lines = []
    for row in rows:
        if row[0] == UNPEGGED_PRESET:
            goals = [0 for _ in STAGES]
            goal = ceiling = 0
        else:
            goals = [ceil_goal(max(capture[row][stage] for capture in captures) * GOAL_MARGIN)
                     for stage in range(len(STAGES))]
            goal = ceil_goal(max(sum(capture[row]) for capture in captures) * GOAL_MARGIN)
            ceiling = ceil_goal(goal * CEILING_MARGIN)
        key, quarter, size = row
        stages = ", ".join(map(str, goals))
        lines.append(f'    {{"{key}", "{quarter}", "{size}", {{{stages}}}, {{{goal}, {ceiling}}}}},')
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=pathlib.Path, nargs="+")
    args = parser.parse_args()
    try:
        print(generate(args.captures))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

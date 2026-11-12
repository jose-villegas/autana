"""Print the stress suite's goal table from board captures of run_stress_perf_suite.

Each capture's "PERF TARGET" lines carry every row's stage means and r3d sum
as the suite measured them; a goal is the worst of them over all captures.
"""
import argparse
from fractions import Fraction
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path[:0] = [str(ROOT / "launcher/tools"), str(ROOT / "scripts/device")]
from build.c_header import read_define
from device_report import parse_perf_targets, parse_unpegged_targets

SUITE = ROOT / "launcher/main/apps/render_lab/tests/suite_stress_perf.c"
STAGE_HEADER = ROOT / "launcher/main/render/r3d_stages.h"
GOAL_MARGIN = Fraction("1.01")
CEILING_MARGIN = Fraction("1.15")
GOAL_QUANTUM_US = 10
DEFAULT_SIZE = read_define(SUITE, "STRESS_PERF_DEFAULT_SIZE")
TABLE_ROW = re.compile(r'\{"(stress-[^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*\{([^}]+)\},\s*\{[^}]+\}\}')


def stage_names():
    """The bracket names in the stage enum's order, which indexes a row's goals."""
    enum = re.search(r"enum\s*\{([^}]+)\}", STAGE_HEADER.read_text(encoding="utf-8"))[1]
    ids = [name.strip() for name in enum.split(",") if name.strip() != "R3D_STAGE_COUNT"]
    return tuple(read_define(STAGE_HEADER, f"{name}_NAME") for name in ids)


STAGES = stage_names()


def table_rows():
    """The suite owns the row order and the exact preset, quarter and size keys."""
    table = SUITE.read_text(encoding="utf-8").split("static const stress_goal_t stress_goals[] = {", 1)[1].split("};", 1)[0]
    rows = []
    for key, quarter, size, stages in TABLE_ROW.findall(table):
        if len(stages.split(",")) != len(STAGES):
            raise ValueError(f"{SUITE}: {key} {quarter} {size} holds {len(stages.split(','))} stage goals, "
                             f"the stages are {len(STAGES)}")
        rows.append((key, quarter, size))
    if not rows or len(set(rows)) != len(rows):
        raise ValueError(f"{SUITE}: empty or duplicate goal rows")
    return rows


def row_name(row):
    """The PERF TARGET name of a row's r3d sum, as report_goals() prints it."""
    key, quarter, size = row
    return " ".join([key, quarter] + ([] if size == DEFAULT_SIZE else [size]))


def capture_targets(path, expected):
    """Every target's measured us; a rerun case keeps its worst."""
    text = path.read_text(encoding="utf-8", errors="replace")
    found = {}
    for target in parse_perf_targets(text)[0] + parse_unpegged_targets(text):
        name, measured = target["name"], int(target["measured"])
        if name.startswith("stress-"):
            if name not in expected:
                raise ValueError(f"{path}: target {name} has no goal row")
            found[name] = max(found.get(name, 0), measured)
    missing = [name for name in expected if name not in found]
    if missing:
        raise ValueError(f"{path}: missing {missing[0]}" + (f" and {len(missing) - 1} more" if len(missing) > 1 else ""))
    return found


def ceil_goal(value):
    """Round a pegged microsecond value up, reserving zero for unpegged goals."""
    return max(GOAL_QUANTUM_US, math.ceil(value / GOAL_QUANTUM_US) * GOAL_QUANTUM_US)


def generate(paths, unpegged=()):
    rows = table_rows()
    unknown = set(unpegged) - {key for key, _, _ in rows}
    if unknown:
        raise ValueError(f"--unpegged {', '.join(sorted(unknown))}: no such preset in {SUITE.name}")
    if not paths:
        raise ValueError("no captures")
    names = {row: [row_name(row)] + [f"{row_name(row)} {stage}" for stage in STAGES] for row in rows}
    expected = {name for row_names in names.values() for name in row_names}
    captures = [capture_targets(path, expected) for path in paths]
    lines = []
    for row in rows:
        key, quarter, size = row
        if key in unpegged:
            goal, ceiling, goals = 0, 0, [0] * len(STAGES)
        else:
            worst = [max(capture[name] for capture in captures) for name in names[row]]
            goal = ceil_goal(worst[0] * GOAL_MARGIN)
            ceiling = ceil_goal(goal * CEILING_MARGIN)
            goals = [ceil_goal(value * GOAL_MARGIN) for value in worst[1:]]
        stages = ", ".join(map(str, goals))
        lines.append(f'    {{"{key}", "{quarter}", "{size}", {{{stages}}}, {{{goal}, {ceiling}}}}},')
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("captures", type=pathlib.Path, nargs="+")
    parser.add_argument("--unpegged", action="append", default=[], metavar="PRESET",
                        help="a preset whose rows stay at zero; repeatable")
    args = parser.parse_args()
    try:
        print(generate(args.captures, args.unpegged))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

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
MAIN = ROOT / "launcher/main"
GOAL_MARGIN = Fraction("1.01")
CEILING_MARGIN = Fraction("1.15")
GOAL_QUANTUM_US = 10
DEFAULT_SIZE = read_define(SUITE, "STRESS_PERF_DEFAULT_SIZE")
FLAP_STAGE = read_define(SUITE.parent.parent / "stress/scene_stress.h", "STRESS_FLAP_STAGE_NAME")
TABLE_ROW = re.compile(r'\{"(stress-[^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*\{([^}]+)\},\s*\{([^}]+)\}\}')


def stage_names():
    """The bracket names in the suite's stress_perf_stages order, which indexes a row's goals:
    each stage's define, which one engine header owns."""
    listed = re.search(r"stress_perf_stages\[\w+\]\s*=\s*\{([^}]+)\}", SUITE.read_text(encoding="utf-8"))[1]
    headers = [header.read_text(encoding="utf-8", errors="replace") for header in MAIN.rglob("*.h")]
    names = []
    for define in (name.strip() for name in listed.split(",") if name.strip()):
        found = [value for text in headers for value in re.findall(rf'^#define\s+{define}\s+"([^"]+)"', text, re.M)]
        if len(found) != 1:
            raise ValueError(f"{SUITE}: stage {define} is defined {len(found)} times in {MAIN}'s headers")
        names.append(found[0])
    return tuple(names)


STAGES = stage_names()
ANIMATION_HEADER = MAIN / "scene/scene_anim_stages.h"
STATIC_STAGES = STAGES.index(read_define(ANIMATION_HEADER, "SCENE_ANIM_STAGE_POSE_NAME"))


def goal_table():
    """The text of the suite's stress_goals[] initialiser."""
    return SUITE.read_text(encoding="utf-8").split("static const stress_goal_t stress_goals[] = {", 1)[1].split("};", 1)[0]


def stages_for(row):
    """The stage prefix a row stores: raster only or raster plus animation."""
    for key, quarter, size, stages, _ in TABLE_ROW.findall(goal_table()):
        if (key, quarter, size) == row:
            return STAGES[:len(stages.split(","))]
    raise ValueError(f"{SUITE}: no goal row {row}")


def table_rows():
    """The suite owns the row order and the exact preset, quarter and size keys."""
    rows = []
    for key, quarter, size, stages, _ in TABLE_ROW.findall(goal_table()):
        if len(stages.split(",")) not in (STATIC_STAGES, len(STAGES)):
            raise ValueError(f"{SUITE}: {key} {quarter} {size} holds {len(stages.split(','))} stage goals, "
                             f"the stages are {len(STAGES)}")
        rows.append((key, quarter, size))
    if not rows or len(set(rows)) != len(rows):
        raise ValueError(f"{SUITE}: empty or duplicate goal rows")
    return rows


def goal_values():
    """{(preset, quarter, size): (stage goals by stage name, sum goal, sum ceiling)}, as the suite holds them."""
    values = {}
    for key, quarter, size, stages, budget in TABLE_ROW.findall(goal_table()):
        goals = [int(value) for value in stages.split(",")]
        goal, ceiling = (int(value) for value in budget.split(","))
        values[(key, quarter, size)] = (dict(zip(STAGES, goals)), goal, ceiling)
    return values


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
            if name.endswith(" " + FLAP_STAGE) and name.removesuffix(" " + FLAP_STAGE) in expected:
                continue
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


def generate(paths, unpegged=(), presets=()):
    """The goal rows from `paths`; only `presets`' rows when given, from captures of those alone."""
    rows = table_rows()
    for flag, keys in (("--unpegged", unpegged), ("--preset", presets)):
        unknown = set(keys) - {key for key, _, _ in rows}
        if unknown:
            raise ValueError(f"{flag} {', '.join(sorted(unknown))}: no such preset in {SUITE.name}")
    if presets:
        rows = [row for row in rows if row[0] in presets]
    if not paths:
        raise ValueError("no captures")
    names = {row: [row_name(row)] + [f"{row_name(row)} {stage}" for stage in stages_for(row)] for row in rows}
    expected = {name for row_names in names.values() for name in row_names}
    captures = [capture_targets(path, expected) for path in paths]
    lines = []
    for row in rows:
        key, quarter, size = row
        if key in unpegged:
            goal, ceiling, goals = 0, 0, [0] * len(stages_for(row))
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
    parser.add_argument("--preset", action="append", default=[], metavar="PRESET",
                        help="print only this preset's rows, from captures of it alone (--test); repeatable")
    args = parser.parse_args()
    try:
        print(generate(args.captures, args.unpegged, args.preset))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

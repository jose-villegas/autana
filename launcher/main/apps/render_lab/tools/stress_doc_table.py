"""Write the stress suite's goals for some presets into a generated block of a doc page.

    python launcher/main/apps/render_lab/tools/stress_doc_table.py PAGE BLOCK PRESET ... [--check]

One row per goal row of each preset, in the suite's order, with every stage the row holds and its sum's goal
and ceiling, in microseconds per frame. The goals come from suite_stress_perf.c, which stress_goals.py prints
from board captures; --check fails when the block differs.
"""
import argparse
import pathlib
import shlex
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from stress_goals import ROOT, SUITE, goal_values  # noqa: E402

sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from generated_blocks import replace_block  # noqa: E402

COMMAND = "python launcher/main/apps/render_lab/tools/stress_doc_table.py"
NOTHING = "—"


def table(presets):
    """The Markdown table of `presets`' goal rows."""
    values = {row: value for row, value in goal_values().items() if row[0] in presets}
    missing = set(presets) - {key for key, _, _ in values}
    if missing:
        raise ValueError(f"{SUITE.name} has no goal row for {', '.join(sorted(missing))}")
    stages = max((list(stage) for stage, _, _ in values.values()), key=len)
    head = ["Preset", "Quarter", *(f"`{stage}`" for stage in stages), "Sum goal", "Ceiling"]
    lines = ["| " + " | ".join(head) + " |", "|---|---|" + "---:|" * (len(head) - 2)]
    for (key, quarter, size), (goals, goal, ceiling) in values.items():
        name = key if size == "default" else f"{key} {size}"
        cells = [str(goals[stage]) if stage in goals else NOTHING for stage in stages]
        lines.append("| " + " | ".join([f"`{name}`", quarter, *cells, str(goal), str(ceiling)]) + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("page", type=pathlib.Path)
    parser.add_argument("block")
    parser.add_argument("presets", nargs="+")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    command = shlex.join([*COMMAND.split(), args.page.as_posix(), args.block, *args.presets, "--check"])
    try:
        stale = replace_block(ROOT / args.page, args.block, table(args.presets), check=args.check, command=command)
    except (OSError, ValueError) as error:
        parser.exit(2, f"{error}\n")
    if stale and args.check:
        parser.exit(1, f"{args.page}#{args.block} is stale: run {command.removesuffix(' --check')}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

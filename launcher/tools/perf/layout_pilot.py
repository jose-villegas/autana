#!/usr/bin/env python3
"""Measure how much a firmware row moves with the code and rodata layout.

`run` builds one image per layout seed (autana's --layout-seed), flashes it
once, and captures each requested suite R times on it, writing one JSON file
per flash into OUT. A seed may repeat: the same image flashed again is the
boot-to-boot control. `report` reads those files and prints, for every timing
row, the run-to-run noise inside a flash, the spread between flashes, the
Kalibera-Jones split of runs per flash, and the flashes needed for a given
confidence interval on B/A (docs/tools/Layout-Noise.md).

  layout_pilot.py run --out DIR --seeds 1 2 3 --runs 3 \\
      --suite run_sponza_perf_suite flythrough - \\
      --suite run_sand_perf_suite frame_budget 'python3 table.py @CAPTURE@ @TABLE@'
  layout_pilot.py report DIR [--markdown]

Each --suite is: the suite name, its --test patterns ('-' for all) and a
command that turns a capture into a table ('-' to read the capture itself;
@CAPTURE@ and @TABLE@ become capture and table files, @PROJECT@ the project tree).
"""

import argparse
import json
import sys
from pathlib import Path
from statistics import mean

sys.path.insert(0, str(Path(__file__).resolve().parent))

from layout_measure import (analyse, autana_command, captured_runs, make_table,
                            parse_report, required_seeds, run_flash)

def run(args):
    for seed in args.seeds:
        run_flash(args, seed)
    return 0


def load(directory):
    """{suite: {flash file: {"seed": n, "flash": s, "run": s, "rows": {name: [values]}}}}
    from `run`'s files."""
    suites = {}
    for path in sorted(Path(directory).glob("*.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        for name, entry in record["suites"].items():
            rows = {}
            for report in entry["runs"]:
                for row, value in parse_report(report["table"]).items():
                    rows.setdefault(row, []).append(value)
            suites.setdefault(name, {})[path.stem] = {
                "seed": record["seed"], "flash": record["flash_seconds"], "run": mean(entry["run_seconds"]), "rows": rows}
    return suites


def hundredths(value):
    return "n/a" if value is None else f"{value * 100:.3f}%"


def report(args):
    suites = load(args.directory)
    lines = []
    for name, seeds in suites.items():
        flash = mean(entry["flash"] for entry in seeds.values())
        run_seconds = mean(entry["run"] for entry in seeds.values())
        ratio = flash / run_seconds
        lines += [f"### {name}", "",
                  f"{len(seeds)} flashes, seeds {', '.join(str(e['seed']) for e in seeds.values())}; one flash "
                  f"{flash:.0f} s (build, flash, boot), one run {run_seconds:.0f} s, "
                  f"c_flash/c_run = {ratio:.1f}; K is at the measured runs per flash; "
                  f"Shapiro-Wilk on fewer than 15 flashes cannot judge", "",
                  "| row | mean (us) | sigma_run | sigma_flash | sd of flash means | "
                  "R* | K for +-0.1% | K for +-0.5% | Shapiro-Wilk p |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        names = sorted(set().union(*(entry["rows"] for entry in seeds.values())))
        for row in names:
            result = analyse([seeds[s]["rows"].get(row, []) for s in seeds], ratio)
            lines.append(
                f"| `{row}` | {result['mean']:.0f} | {hundredths(result['sigma_run'])} | "
                f"{hundredths(result['sigma_flash'])} | {hundredths(result['seed_spread'])} | "
                f"{'inf' if result['optimum_runs'] is None else result['optimum_runs']} | "
                f"{result['seeds_01'] or 'inf'} | {result['seeds_05'] or 'inf'} | "
                f"{'n/a' if result['shapiro'] is None else format(result['shapiro'], '.2f')} |")
        lines.append("")
    print("\n".join(lines))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    runner = commands.add_parser("run")
    runner.add_argument("--out", required=True)
    runner.add_argument("--seeds", type=int, nargs="+", required=True)
    runner.add_argument("--runs", type=int, default=3)
    runner.add_argument("--suite", nargs=3, action="append", required=True,
                        metavar=("NAME", "TESTS", "TABLE"))
    runner.set_defaults(handler=run)
    reporter = commands.add_parser("report")
    reporter.add_argument("directory")
    reporter.set_defaults(handler=report)
    args = parser.parse_args(argv)
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

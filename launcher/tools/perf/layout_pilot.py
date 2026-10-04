#!/usr/bin/env python3
"""Measure how much a firmware row moves with the code and rodata layout.

`run` builds one image per layout seed (autana's --layout-seed), flashes it
once, and captures each requested suite R times on it, writing one JSON file
per seed into OUT. `report` reads those files and prints, for every timing
row, the run-to-run noise inside a flash, the spread between layouts, the
Kalibera-Jones split of runs per flash, and the seeds needed for a given
confidence interval on B/A (docs/tools/Layout-Noise.md).

  layout_pilot.py run --out DIR --seeds 1 2 3 --runs 3 \\
      --suite run_sponza_perf_suite flythrough - \\
      --suite run_sand_perf_suite frame_budget 'python3 table.py @CAPTURE@ @TABLE@'
  layout_pilot.py report DIR [--markdown]

Each --suite is: the suite name, its --test patterns ('-' for all) and a
command that turns a capture into a table ('-' to read the capture itself;
@CAPTURE@ and @TABLE@ are replaced).
"""

import argparse
import json
import math
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean, stdev

sys.path.insert(0, str(Path(__file__).resolve().parent))

from perf_compare import parse_report  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
REPORT_LINE = re.compile(r"^report: (.+\.md)\s*$")
RUN_LINE = re.compile(r"^batch: (\S+) run (\d+)/(\d+)")
BUILD_LINE = re.compile(r"BUILD_ID=(\S+)")

# Two-sided 95% Student t by degrees of freedom; 1.96 past the table.
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306,
       9: 2.262, 10: 2.228, 12: 2.179, 14: 2.145, 16: 2.120, 18: 2.101, 20: 2.086,
       25: 2.060, 30: 2.042, 40: 2.021, 60: 2.000, 120: 1.980}


def t95(degrees):
    """The two-sided 95% t quantile for `degrees` of freedom, rounding the
    degrees down to the nearest table entry, which errs wide."""
    if degrees < 1:
        return T95[1]
    known = [d for d in T95 if d <= degrees]
    return T95[max(known)] if degrees < 120 else 1.96


def autana_command(*words):
    return [sys.executable, str(REPO / "scripts" / "autana" / "autana.py"), *words]


def run_stamped(command, log):
    """Run `command`, copy its output to `log` and stdout, and return
    (exit code, [(time, line)], seconds)."""
    started = time.monotonic()
    lines = []
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1, cwd=REPO)
    for line in process.stdout:
        line = line.rstrip("\n")
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()
        lines.append((time.monotonic() - started, line))
    return process.wait(), lines, time.monotonic() - started


def captured_runs(lines, suite_name, runs):
    """The capture log of each run, in order, and each run's seconds: from its
    own `batch: ... run i/N` line to the next one, or to the end."""
    starts = [at for at, line in lines if (m := RUN_LINE.match(line)) and m.group(1) == suite_name]
    reports = [Path(m.group(1)) for _, line in lines if (m := REPORT_LINE.match(line))]
    end = lines[-1][0] if lines else 0.0
    if len(starts) != runs or len(reports) != runs:
        raise RuntimeError(f"{suite_name}: wanted {runs} runs, saw {len(starts)} starts and "
                           f"{len(reports)} reports")
    seconds = [(starts[i + 1] if i + 1 < runs else end) - starts[i] for i in range(runs)]
    return [report.with_suffix(".log") for report in reports], starts[0], seconds


def make_table(template, capture, table):
    if template == "-":
        return capture
    command = [part.replace("@CAPTURE@", str(capture)).replace("@TABLE@", str(table))
               for part in shlex.split(template)]
    command[0] = sys.executable if command[0] == "python3" else command[0]
    subprocess.run(command, check=True, cwd=REPO, capture_output=True)
    return table


def run_seed(args, seed):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    record = {"seed": seed, "suites": {}}
    with open(out / f"seed_{seed}.log", "w", encoding="utf-8") as log:
        for index, (name, tests, template) in enumerate(args.suite):
            command = autana_command("suite", name, "--runs", str(args.runs))
            if tests != "-":
                command += ["--test", tests]
            if index == 0:
                command += ["--flash", "--perf-scope", "--layout-seed", str(seed)]
            code, lines, wall = run_stamped(command, log)
            if code not in (0, 1):
                raise RuntimeError(f"{name} exited {code}; see {log.name}")
            captures, first_run, run_seconds = captured_runs(lines, name, args.runs)
            if index == 0:
                record["flash_seconds"] = first_run
                record["build_id"] = next(m.group(1) for _, line in lines
                                          if (m := BUILD_LINE.search(line)) and "booted" in line)
            tables = [make_table(template, capture, out / f"seed_{seed}_{name}_{n + 1}.md")
                      for n, capture in enumerate(captures)]
            record["suites"][name] = {
                "run_seconds": run_seconds,
                "runs": [{"capture": str(c), "table": str(t)} for c, t in zip(captures, tables)],
            }
        answer = subprocess.run(autana_command("buildid"), capture_output=True, text=True, cwd=REPO)
        if record["build_id"] not in answer.stdout:
            raise RuntimeError(f"the board runs {answer.stdout.strip()!r}, not {record['build_id']}")
    (out / f"seed_{seed}.json").write_text(json.dumps(record, indent=1), encoding="utf-8")


def run(args):
    for seed in args.seeds:
        run_seed(args, seed)
    return 0


def load(directory):
    """{suite: {seed: {"flash": s, "run": s, "rows": {name: [values]}}}} from `run`'s files."""
    suites = {}
    for path in sorted(Path(directory).glob("seed_*.json")):
        record = json.loads(path.read_text(encoding="utf-8"))
        for name, entry in record["suites"].items():
            rows = {}
            for report in entry["runs"]:
                for row, value in parse_report(report["table"]).items():
                    rows.setdefault(row, []).append(value)
            suites.setdefault(name, {})[record["seed"]] = {
                "flash": record["flash_seconds"], "run": mean(entry["run_seconds"]), "rows": rows}
    return suites


def hundredths(value):
    return "n/a" if value is None else f"{value * 100:.3f}%"


def required_seeds(layout, run, runs, half_width):
    """Seeds per side so a 95% interval on B/A is within +-half_width, for
    relative layout and run spreads. None when the spread is zero."""
    per_seed = layout ** 2 + run ** 2 / runs
    if per_seed <= 0:
        return None
    seeds = 2
    while seeds < 100000:
        width = t95(2 * (seeds - 1)) * math.sqrt(2 * per_seed / seeds)
        if width <= half_width:
            return seeds
        seeds += 1
    return seeds


def analyse(rows_by_seed, flash_over_run):
    """The pilot numbers for one row: values per seed, each a list of runs."""
    per_seed = [values for values in rows_by_seed if len(values) >= 2]
    runs = round(mean(len(values) for values in per_seed))
    grand = mean(mean(values) for values in per_seed)
    within = math.sqrt(mean(stdev(values) ** 2 for values in per_seed))
    means = [mean(values) for values in per_seed]
    seed_spread = stdev(means)
    layout_variance = max(0.0, seed_spread ** 2 - within ** 2 / runs)
    layout = math.sqrt(layout_variance)
    optimum = (None if layout == 0 else
               max(1, math.ceil(math.sqrt(flash_over_run * within ** 2 / layout_variance))))
    relative = (layout / grand, within / grand)
    chosen = optimum or runs
    return {
        "mean": grand, "seeds": len(per_seed), "runs": runs,
        "sigma_run": relative[1], "sigma_layout": relative[0], "seed_spread": seed_spread / grand,
        "optimum_runs": optimum, "shapiro": shapiro(means),
        "seeds_01": required_seeds(*relative, chosen, 0.001),
        "seeds_05": required_seeds(*relative, chosen, 0.005),
    }


def shapiro(values):
    """Shapiro-Wilk p-value of the seed means, or None without scipy or with
    fewer than three distinct values."""
    try:
        from scipy.stats import shapiro as test
    except ImportError:
        return None
    if len(set(values)) < 3:
        return None
    return float(test(values).pvalue)


def report(args):
    suites = load(args.directory)
    lines = []
    for name, seeds in suites.items():
        flash = mean(entry["flash"] for entry in seeds.values())
        run_seconds = mean(entry["run"] for entry in seeds.values())
        ratio = flash / run_seconds
        lines += [f"### {name}", "",
                  f"{len(seeds)} seeds ({', '.join(str(s) for s in sorted(seeds))}); one flash "
                  f"{flash:.0f} s (build, flash, boot), one run {run_seconds:.0f} s, "
                  f"c_flash/c_run = {ratio:.1f}", "",
                  "| row | mean (us) | sigma_run | sigma_layout | sd of seed means | "
                  "R* | K for +-0.1% | K for +-0.5% | Shapiro-Wilk p |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        names = sorted(set().union(*(entry["rows"] for entry in seeds.values())))
        for row in names:
            result = analyse([seeds[s]["rows"].get(row, []) for s in sorted(seeds)], ratio)
            lines.append(
                f"| `{row}` | {result['mean']:.0f} | {hundredths(result['sigma_run'])} | "
                f"{hundredths(result['sigma_layout'])} | {hundredths(result['seed_spread'])} | "
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

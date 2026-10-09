#!/usr/bin/env python3
"""Sweep console tunables on seeded images; run acquires, report rebuilds the table."""
import argparse
import itertools
import json
from pathlib import Path
import random
import re
from statistics import mean
import sys

from layout_measure import (REPO, MEAN_RE, autana_command, booted_build_id,
                            capture_metadata, captured_runs, make_table, parse_report,
                            run_stamped, split_filters, filter_limits, status_snapshot)
from perf_compare import DEFAULT_ALPHA, DEFAULT_THRESHOLD
from seed_statistics import compare
from device_capture import PERF_SEGMENT

sys.path.insert(0, str(REPO / "launcher/tools/r3d"))
from dynres_report import SPANS, SPAN_FIELDS, STAGE

DEFAULT_RUNS = 5
US_PER_MS = 1000
MAX_CONSECUTIVE_FAILURES = 2
LAYOUT_SEED_MAX = 2**32 - 1
TUNE_VALUE_MIN = -(2**31)
TUNE_VALUE_MAX = 2**31 - 1
TUNE_NAME_MAX = int(re.search(r"^#define TUNE_NAME_MAX\s+(\d+)",
                            (REPO / "launcher/main/util/runtime/tune.h").read_text(), re.MULTILINE)[1])


def assignment(text):
    name, separator, value = text.partition("=")
    if not separator or not name or not value:
        raise argparse.ArgumentTypeError("expected NAME=VALUE")
    return name, value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    reader = commands.add_parser("report", help="rebuild sweep.md and sweep.json")
    reader.add_argument("directory", type=Path)
    run_parser = commands.add_parser("run", help="flash once per build/seed, randomise points each round")
    run_parser.add_argument("--out", "-o", type=Path, required=True)
    run_parser.add_argument("--suite", nargs=3, action="append", required=True,
                            metavar=("NAME", "TESTS", "TABLE"))
    run_parser.add_argument("--knob", type=assignment, action="append", required=True)
    run_parser.add_argument("--build", type=assignment, action="append")
    run_parser.add_argument("--seeds", nargs="+", type=int, default=[1])
    run_parser.add_argument("--runs", type=int, default=DEFAULT_RUNS)
    run_parser.add_argument("--timeout", type=int, default=1800)
    run_parser.add_argument("--wait", type=int, default=3600)
    run_parser.add_argument("--rng-seed", type=int)
    run_parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    run_parser.add_argument("--alpha", type=float, default=DEFAULT_ALPHA)
    run_parser.add_argument("--autana")
    run_parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if args.command == "run":
        if not 0 < args.threshold < 100 or not 0 < args.alpha < 1 or min(args.runs, args.timeout, args.wait) <= 0:
            parser.error("threshold, alpha, runs and time limits must be positive and in range")
        if len(set(args.seeds)) != len(args.seeds) or any(seed < 0 or seed > LAYOUT_SEED_MAX for seed in args.seeds):
            parser.error("layout seeds must be distinct uint32 values")
        if len({name for name, _ in args.knob}) != len(args.knob):
            parser.error("knob names must be unique")
        try:
            args.knob = [(name, list(dict.fromkeys(int(value, 0) for value in values.split(","))))
                         for name, values in args.knob]
        except ValueError:
            parser.error("knob values must be integers")
        if any(not re.fullmatch(r"[a-zA-Z0-9_.]+", name) or len(name) > TUNE_NAME_MAX or
               any(not TUNE_VALUE_MIN <= value <= TUNE_VALUE_MAX for value in values) for name, values in args.knob):
            parser.error("knobs need console names and int32 values")
        args.build = args.build or [("here", str(REPO))]
        if len({label for label, _ in args.build}) != len(args.build):
            parser.error("build labels must be unique")
    return args


def plan(args):
    """Record a replayable point order before any console command runs."""
    rng_seed = args.rng_seed if args.rng_seed is not None else random.SystemRandom().randrange(2**63)
    rng = random.Random(rng_seed)
    points = [dict(zip((name for name, _ in args.knob), values))
              for values in itertools.product(*(values for _, values in args.knob))]
    builds = [dict(label=label, project=str(Path(project).resolve()), build_ids={})
              for label, project in args.build]
    flashes = []
    for build, entry in enumerate(builds):
        suites = split_filters(args.suite, filter_limits(entry["project"]))
        for seed in args.seeds:
            rounds = []
            for _ in range(args.runs):
                order = list(range(len(points)))
                rng.shuffle(order)
                rounds.append(order)
            command = ["--project", entry["project"], "flash", "diag",
                       "--perf-scope", "--layout-seed", str(seed)]
            if args.knob:
                command.append("--hot-tunables")
            flashes.append(dict(build=build, seed=seed, rounds=rounds, suites=suites, command=command))
    return dict(builds=builds, points=points, seeds=args.seeds, runs=args.runs, hot_tunables=bool(args.knob),
                rng_seed=rng_seed, flashes=flashes, captures=[], threshold=args.threshold,
                alpha=args.alpha, timeout=args.timeout, wait=args.wait)


def metrics(capture, table):
    """Pool capture metrics and generated table rows, retaining their units."""
    text = Path(capture).read_text(encoding="utf-8", errors="replace")
    values = {}
    units = {}

    def add(name, value, unit="us"):
        values.setdefault(name, []).append(value)
        units[name] = unit

    rows = {match["name"]: int(match["value"]) for match in MEAN_RE.finditer(text)}
    rows.update(parse_report(table))
    for name, value in rows.items():
        add(name, value)
    for line in text.splitlines():
        if "ms/frame avg/worst:" in line:
            segment = line.split("ms/frame avg/worst:", 1)[1]
        elif "scale_split:" in line and " | " in line:
            segment = line.split(" | ", 1)[1]
        else:
            continue
        for name, average, worst in STAGE.findall(segment.split("| total", 1)[0]):
            add(name + ".avg", float(average) * US_PER_MS)
            add(name + ".worst", float(worst) * US_PER_MS)
    for match in SPANS.finditer(text):
        for field, value in zip(SPAN_FIELDS, match.groups()[2:]):
            add(f"spans.{match[1]}x{match[2]}.{field}", int(value))
    for match in PERF_SEGMENT.finditer(text):
        if int(match[7]) > 0:
            add(match[1] + ".cycles", int(match[2]), "cycles/call")
            add(match[1] + "." + match[5], int(match[6]), "events/call")
    return {name: mean(samples) for name, samples in values.items()}, units


def write_json(path, payload):
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def run(args, runner=None):
    """Acquire in the foreground; stopped sweeps retain the plan and report."""
    runner = runner or run_stamped
    saved = plan(args)
    if args.dry_run:
        print(json.dumps(saved, indent=2))
        print(f"{len(saved['flashes'])} flashes; "
              f"{sum(len(f['suites']) for f in saved['flashes']) * args.runs * len(saved['points'])} captures")
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    if (args.out / "plan.json").exists():
        raise ValueError("output already contains plan.json; choose a fresh directory")
    failures = 0

    def save():
        write_json(args.out / "plan.json", saved)

    def command(words, stem, timeout=None):
        with open(args.out / (stem + ".log"), "w", encoding="utf-8") as log:
            code, lines, wall = runner(autana_command("--wait", str(args.wait), *words,
                                                    override=args.autana), log,
                                       timeout or args.wait + args.timeout)
            if not log.tell():
                log.write("\n".join(line for _, line in lines) + "\n")
        return code, lines, wall

    save()
    try:
        for flash_number, flash in enumerate(saved["flashes"]):
            build = saved["builds"][flash["build"]]
            stem = f"flash_{flash_number + 1}"
            flashed = False
            try:
                status_snapshot(args.out, stem, "before", runner, args.autana, identity=True, wait=args.wait)
                code, lines, wall = command(flash["command"], stem)
                if code:
                    raise RuntimeError(f"flash exited {code}; see {stem}.log")
                flash["build_id"] = booted_build_id(build["project"], lines)
                flashed = True
                flash["flash_seconds"] = wall
                build["build_ids"][str(flash["seed"])] = flash["build_id"]
                save()
                for round_index, order in enumerate(flash["rounds"]):
                    for point in order:
                        prefix = f"{stem}_round_{round_index + 1}_point_{point}"
                        for knob, value in saved["points"][point].items():
                            code, lines, _ = command(["tune", knob, str(value)], prefix + "_" + knob)
                            reply = "\n".join(line for _, line in lines)
                            if code or f"{knob}={value}" not in reply.splitlines():
                                raise RuntimeError(f"tune {knob}={value} refused or mismatched: {reply!r}")
                        for suite_index, (suite, tests, template) in enumerate(flash["suites"]):
                            capture_stem = prefix + f"_suite_{suite_index}"
                            item = dict(build=flash["build"], seed=flash["seed"], point=point,
                                        round=round_index, suite=suite, tests=tests, template=template,
                                        record=str(args.out / (capture_stem + ".json")))
                            saved["captures"].append(item)
                            save()
                            try:
                                words = ["--project", build["project"], "suite", suite, str(args.timeout),
                                         "--runs", "1", "--expect-build-id", flash["build_id"]]
                                if tests != "-":
                                    words += ["--test", tests]
                                code, lines, wall = command(words, capture_stem)
                                captures, _, seconds = captured_runs(lines, suite, 1)
                                capture = captures[0]
                                item.update(capture=str(capture), seconds=seconds[0], wall_seconds=wall)
                                if code or "- Ended: complete" not in capture.with_suffix(".md").read_text(encoding="utf-8", errors="replace"):
                                    raise RuntimeError(f"{suite}: failed or incomplete capture; exit {code}")
                                table = make_table(template, capture, args.out / (capture_stem + ".md"),
                                                   build["project"], args.timeout)
                                item["table"] = str(table)
                                rows, units = metrics(capture, table)
                                if not rows:
                                    raise RuntimeError(f"{suite}: no metrics in capture or table {table}")
                                owners, instructions = capture_metadata(capture, rows)
                                item.update(metrics={suite + "/" + key: value for key, value in rows.items()},
                                            units={suite + "/" + key: value for key, value in units.items()},
                                            owners=owners, instructions=instructions)
                                failures = 0
                            except (RuntimeError, OSError, ValueError) as error:
                                item["error"] = str(error)
                                failures += 1
                                if failures >= MAX_CONSECUTIVE_FAILURES:
                                    raise RuntimeError(f"two consecutive capture failures: {error}") from error
                            finally:
                                write_json(args.out / (capture_stem + ".json"), item)
                                save()
            finally:
                active_error = sys.exc_info()[1]
                cleanup_errors = []
                if flashed:
                    for knob in saved["points"][0]:
                        try:
                            code, lines, _ = command(["tune", "reset", knob], stem + "_reset_" + knob)
                            if code:
                                raise RuntimeError(repr("\n".join(line for _, line in lines)))
                        except (RuntimeError, OSError) as error:
                            cleanup_errors.append(f"reset {knob}: {error}")
                identity_valid = False
                try:
                    after = status_snapshot(args.out, stem, "after", runner, args.autana, identity=True, wait=args.wait)
                    if flashed and after != flash.get("build_id"):
                        raise RuntimeError("build identity changed during measurement; captures discarded")
                    identity_valid = True
                except (RuntimeError, OSError) as error:
                    cleanup_errors.append(str(error))
                if flashed and not identity_valid:
                    for item in saved["captures"]:
                        if item["build"] == flash["build"] and item["seed"] == flash["seed"]:
                            item.pop("metrics", None)
                            item["error"] = "build identity after measurement could not be verified"
                            write_json(Path(item["record"]), item)
                if cleanup_errors:
                    detail = "; ".join(cleanup_errors)
                    raise RuntimeError(f"{active_error}; cleanup: {detail}" if active_error else detail)
    except (RuntimeError, OSError, ValueError) as error:
        saved["error"] = str(error)
        print(f"ERROR: {error}", file=sys.stderr)
    finally:
        save()
        report(args.out)
    print(args.out / "sweep.md")
    return int("error" in saved)


def report(directory):
    """Use complete seed means, or runs on one seed, in one Holm family."""
    directory = Path(directory)
    saved = json.loads((directory / "plan.json").read_text(encoding="utf-8"))
    multi_seed = len(saved["seeds"]) >= 2
    gathered, units = {}, {}
    for capture in saved["captures"]:
        if capture.get("error"):
            continue
        units.update(capture.get("units", {}))
        for metric, value in capture.get("metrics", {}).items():
            key = (capture["build"], capture["point"], metric)
            gathered.setdefault(key, {}).setdefault(capture["seed"], {})[capture["round"]] = value
    observations = {}
    for key, seeds in gathered.items():
        observations[key] = ([mean(runs.values()) for runs in seeds.values() if len(runs) == saved["runs"]]
                             if multi_seed else [value for runs in seeds.values() for value in runs.values()])
    metrics_list = sorted({key[2] for key in observations})
    left, right, keys = {}, {}, {}
    for build in range(len(saved["builds"])):
        for point in range(len(saved["points"])):
            if (build, point) == (0, 0):
                continue
            for metric in metrics_list:
                cell = str(len(keys))
                keys[cell] = (build, point, metric)
                left[cell] = observations.get((0, 0, metric), [])
                right[cell] = observations.get((build, point, metric), [])
    decisions = compare(left, right, saved["threshold"], saved["alpha"], saved["rng_seed"])
    scored = {}
    for cell, decision in decisions.items():
        key = keys[cell]
        if decision["verdict"] not in ("improved", "regressed", "no change", "inconclusive"):
            decision.update(verdict="inconclusive", reason="missing or nonpositive observations")
        if not multi_seed and key[0] != 0:
            decision.update(verdict="inconclusive", reason="one flash per build cannot separate a build from its layout")
        elif not multi_seed and decision["verdict"] in ("improved", "regressed") and abs(decision["ratio"] - 1) * 100 <= saved["threshold"]:
            decision.update(verdict="inconclusive", reason="difference does not exceed threshold")
        scored[key] = decision
    rows = []
    for build, entry in enumerate(saved["builds"]):
        for point, knobs in enumerate(saved["points"]):
            cells = {}
            for metric in metrics_list:
                values = observations.get((build, point, metric), [])
                cells[metric] = dict(scored.get((build, point, metric), {}),
                                     mean=mean(values) if values else None, observations=values)
            rows.append(dict(build=entry["label"], point=point, knobs=knobs, metrics=cells))
    result = dict(unit="seed mean" if multi_seed else "run", rows=rows, units=units)
    write_json(directory / "sweep.json", result)
    lines = ["# Parameter sweep", ""]
    if saved.get("hot_tunables"):
        lines += ["knobs live: built with --hot-tunables", ""]
    for build in saved["builds"]:
        lines.append(f"Build {build['label']}: {build['project']}; build ids: {json.dumps(build['build_ids'], sort_keys=True)}")
    lines += ["", f"Seeds: {saved['seeds']}; runs per flash: {saved['runs']}; RNG seed: {saved['rng_seed']}.",
              f"{len(saved['flashes'])} flashes: one per build per seed; knobs changed over the console.",
              f"Observation unit: {result['unit']}. " +
              ("Layout noise is in the variance of per-seed means." if multi_seed else
               "The layout floor was not measured; only a significant difference beyond the threshold counts. "
               "Across builds, one flash per build cannot separate a build from its layout."),
              f"Threshold: +/-{saved['threshold']:g}%; alpha: {saved['alpha']:g}; Holm across all point/metric cells.", ""]
    if saved.get("error"):
        lines += ["Stopped: " + saved["error"], ""]
    lines += ["| Build / point | " + " | ".join(f"{metric} ({units.get(metric, 'us')})" for metric in metrics_list) + " |",
              "| --- | " + " | ".join("---" for _ in metrics_list) + " |"]
    for row in rows:
        label = row["build"] + " / " + ", ".join(f"{knob}={value}" for knob, value in row["knobs"].items())
        cells = []
        for metric in metrics_list:
            cell = row["metrics"][metric]
            text = f"{cell['mean']:.3f}" if cell["mean"] is not None else "not measured"
            if "verdict" in cell:
                if cell.get("ratio") is not None:
                    text += f" ({(cell['ratio'] - 1) * 100:+.2f}%)"
                text += " " + cell["verdict"]
                if cell.get("reason"):
                    text += " — " + cell["reason"]
            cells.append(text)
        lines.append("| " + label + " | " + " | ".join(cells) + " |")
    (directory / "sweep.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return result


def main(argv=None):
    args = parse_args(argv)
    try:
        if args.command == "report":
            report(args.directory)
            print(args.directory / "sweep.md")
            return 0
        return run(args)
    except (RuntimeError, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

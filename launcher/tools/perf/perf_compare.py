#!/usr/bin/env python3
"""Compare revisions using independent layout seeds as the measurement unit."""
import argparse
import json
import math
import random
import sys
import subprocess
from pathlib import Path
from statistics import mean
from types import SimpleNamespace

from layout_measure import analyse, autana_command, required_seeds, run_flash, filter_limits, split_filters
from seed_statistics import compare, minimum_seeds, permutation_samples

MAX_RUNS = 16


def schedule(cap, alpha):
    """Plan doubling looks from nominal permutation resolution to the cap."""
    permutation_samples(alpha)
    first = minimum_seeds(alpha)
    if cap < first:
        raise ValueError(f"--max-seeds must be at least {first} for permutation resolution")
    sizes = [first]
    while sizes[-1] < cap:
        sizes.append(min(cap, sizes[-1] * 2))
    return sizes


def observations(records):
    """Complete per-seed rows, counters, and capture test ownership."""
    rows, instructions, owners = {}, {}, {}
    for record in records:
        for suite, entry in record["suites"].items():
            gathered, counters = {}, {}
            for run in entry["runs"]:
                for name, value in run["rows"].items():
                    key = suite + "/" + name
                    rows.setdefault(key, [])
                    gathered.setdefault(key, []).append(value)
                    owners[key] = (suite, run["owners"].get(name))
                for name, value in run["instructions"].items():
                    counters.setdefault(suite + "/" + name, []).append(value)
            for key, values in gathered.items():
                if len(values) == len(entry["runs"]):
                    rows.setdefault(key, []).append(values)
            for key, values in counters.items():
                if len(values) == len(entry["runs"]):
                    instructions.setdefault(key, []).append(mean(values))
    return rows, instructions, owners


def seed_means(rows):
    """One independent timing observation per complete seed."""
    return {name: [mean(values) for values in seeds] for name, seeds in rows.items()}


def selected_suites(suites, active, owners, limits=None, listed=None):
    """Keep user scope; unfiltered suites use all tests their captures ran."""
    selected = []
    for suite, tests, template in suites:
        names = [owners.get(name, (suite, None))[1] for name in active if name.startswith(suite + "/")]
        if not names:
            continue
        known = set((listed or {}).get(suite, []))
        if not all(names):
            selected.append((suite, tests, template))
            continue
        if tests != "-":
            patterns = [pattern for pattern in tests.split(",") if any(pattern in name for name in names)]
            selected.append((suite, ",".join(patterns) if patterns else tests, template))
            continue
        if not known:
            selected.append((suite, tests, template))
            continue
        width = (limits or filter_limits(Path(__file__).resolve().parents[3]))[0]
        patterns = []
        for name in sorted(set(names)):
            pattern = next((name[start:start+length]
                            for length in range(1, min(width, len(name)) + 1)
                            for start in range(len(name)-length+1)
                            if name in known and sum(name[start:start+length] in test for test in known) == 1), None)
            if pattern is None:
                patterns = []
                break
            patterns.append(pattern)
        if not patterns:
            selected.append((suite, tests, template))
        else:
            selected.append((suite, ",".join(patterns), template))
    return selected


def absent_by_measurement(name, owner, plan, records, side):
    """Every other-side attempt succeeded and ran the owner without its row."""
    suite, row = name.split("/", 1)
    attempts = [item for item in plan if item["side"] == side and
                any(entry[0] == suite for entry in item["suites"])]
    captures = [record["suites"][suite] for record in records if suite in record["suites"]]
    return bool(owner and attempts and len(captures) == len(attempts) and
                all(not item.get("error") for item in attempts) and
                all(entry["runs"] and all(owner in run.get("listed_tests", []) and
                    row not in run["rows"] for run in entry["runs"]) for entry in captures))


def recommendations(records, rows, delta, alpha=0.025):
    """Recommend bounded runs and seeds using measured costs and spreads."""
    costs = [(record["flash_seconds"], mean(entry["run_seconds"]))
             for record in records for entry in record["suites"].values() if mean(entry["run_seconds"]) > 0]
    ratio = mean(flash / run for flash, run in costs) if costs else 1
    result = {}
    for name, values in rows.items():
        if sum(len(seed) >= 2 for seed in values) < 2 or mean(map(mean, values)) <= 0:
            continue
        estimate = analyse(values, ratio, diagnostics=False)
        if estimate["optimum_runs"] is None:
            runs = min(MAX_RUNS, max(1, math.ceil(ratio))) if estimate["sigma_run"] else 1
        else:
            runs = min(MAX_RUNS, max(1, estimate["optimum_runs"]))
        count = required_seeds(estimate["sigma_flash"], estimate["sigma_run"], runs, delta, alpha=alpha) or 2
        result[name] = dict(estimate, recommended_runs=runs, recommended_seeds=count)
    return result


def measure(args, runner=None):
    """Acquire seeded observations and save decisions even after capture failure."""
    sizes = schedule(args.max_seeds, args.alpha)
    limits = {side: filter_limits(getattr(args, "project_" + side)) for side in ("a", "b")}
    for side in limits:
        split_filters(args.suite, limits[side])
    look_alpha = args.alpha / len(sizes) / 2
    if args.rng_seed is None:
        args.rng_seed = random.SystemRandom().randrange(2**63)
    rng = random.Random(args.rng_seed)
    used, plan = set(), []
    records = {"a": [], "b": []}
    decisions, active = {}, None
    failures, skip_until, previous_target = 0, 0, 0
    stop_error = None
    destination = Path(args.out)
    destination.mkdir(parents=True, exist_ok=True)

    def save_plan():
        payload = dict(rng_seed=args.rng_seed, plan=plan)
        (destination / "plan.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")

    for look, target in enumerate(sizes):
        if look < skip_until:
            continue
        extracted = {side: observations(records[side]) for side in records}
        family_size = max(1, len(set(extracted["a"][0]) | set(extracted["b"][0])))
        decision_alpha = look_alpha / family_size
        hints = {side: recommendations(records[side], extracted[side][0], args.threshold / 100,
                                       decision_alpha) for side in records}
        runs = max([1] + [hint["recommended_runs"] for side in hints
                          for name, hint in hints[side].items() if name in (active or set())])
        for pair in range(previous_target, target):
            sides = ["a", "b"]
            rng.shuffle(sides)
            for side in sides:
                suites = args.suite if active is None else selected_suites(
                    args.suite, active, extracted[side][2], limits[side], device_inventory(records[side]))
                seed = rng.randint(1, 2147483647)
                while seed in used:
                    seed = rng.randint(1, 2147483647)
                used.add(seed)
                repeated = sum(any(len(entry["runs"]) >= 2 for entry in record["suites"].values())
                               for record in records[side])
                actual_runs = 2 if look == 0 and repeated < 2 else runs
                item = dict(side=side, seed=seed, runs=actual_runs, suites=suites, look=look + 1)
                plan.append(item)
                save_plan()
                flash_args = SimpleNamespace(out=destination / side,
                    project=getattr(args, "project_" + side), runs=actual_runs, suite=suites,
                    timeout=args.timeout, wait=args.wait, autana=getattr(args, "autana", None))
                try:
                    record = run_flash(flash_args, seed, runner)
                except (RuntimeError, OSError, ValueError) as error:
                    failures += 1
                    item["error"] = str(error)
                    save_plan()
                    if failures >= 2:
                        stop_error = RuntimeError("stopping after two consecutive capture failures")
                        break
                    continue
                failures = 0
                records[side].append(record)
                silent = [suite for suite, entry in record["suites"].items()
                          if not any(run["rows"] for run in entry["runs"])]
                if len(records[side]) == 1 and silent:
                    tables = {suite: template for suite, _, template in args.suite}
                    stop_error = RuntimeError("; ".join(
                        f"{suite}: the first {side.upper()} flash's captures gave no timing rows with table "
                        f"'{tables[suite]}'; pass a table command that turns this suite's capture into a timing table"
                        for suite in silent))
                    break
            if stop_error:
                break
        previous_target = target
        extracted = {side: observations(records[side]) for side in records}
        current = compare(seed_means(extracted["a"][0]), seed_means(extracted["b"][0]),
                          args.threshold, look_alpha, args.rng_seed, permutation_alpha=args.alpha)
        for name, result in current.items():
            if result["verdict"] in ("added", "removed"):
                missing_side = "a" if result["verdict"] == "added" else "b"
                owner = extracted["b" if missing_side == "a" else "a"][2].get(name, (None, None))[1]
                if not absent_by_measurement(name, owner, plan, records[missing_side], missing_side):
                    result["verdict"] = "not measured"
            if name not in decisions or decisions[name]["verdict"] == "inconclusive":
                counter_means = {side: mean(extracted[side][1][name]) if extracted[side][1].get(name) else None
                                 for side in records}
                delta_insn = (100 * (counter_means['b'] / counter_means['a'] - 1)
                              if counter_means['a'] and counter_means['b'] is not None else None)
                decisions[name] = dict(result, look=look + 1, counter_means=counter_means, delta_insn=delta_insn)
        active = {name for name, result in decisions.items() if result["verdict"] == "inconclusive"}
        if stop_error or not active:
            break
        needed = []
        family_size = max(1, len(current))
        for side in records:
            estimates_now = recommendations(records[side], extracted[side][0], args.threshold / 100,
                                             look_alpha / family_size)
            needed.extend(hint["recommended_seeds"] for name, hint in estimates_now.items() if name in active)
        desired = max([target + 1] + needed)
        skip_until = next((index for index in range(look + 1, len(sizes)) if sizes[index] >= desired),
                          len(sizes) - 1)
    incomplete = stop_error is not None or not decisions or any(
        row["verdict"] == "not measured" for row in decisions.values())
    estimates = {side: recommendations(records[side], observations(records[side])[0],
                                      args.threshold / 100, look_alpha / max(1, len(decisions)))
                 for side in records}
    write_summary(destination / "summary.md", args, records, decisions, estimates, sizes, plan,
                  incomplete=incomplete)
    payload = dict(threshold=args.threshold, alpha=args.alpha, look_alpha=look_alpha,
                   rng_seed=args.rng_seed, max_seeds=args.max_seeds, first_pass=sizes[0],
                   max_runs=MAX_RUNS, incomplete=incomplete,
                   rows=decisions, estimates=estimates, plan=plan)
    (destination / "comparison.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print((destination / "summary.md").read_text(encoding="utf-8"))
    if stop_error:
        raise stop_error
    return payload


def write_summary(path, args, records, rows, estimates, sizes, plan, incomplete=False):
    """Write observed costs, seed identities, decisions, and acquisition completeness."""
    lines = ["# Performance comparison", "",
             f"A: `{args.label_a}`; B: `{args.label_b}`.", "",
             f"Threshold: +/-{args.threshold:g}%; alpha: {args.alpha:g}; "
             f"per-family per-look alpha: {args.alpha / len(sizes) / 2:g}. "
             f"First pass: {sizes[0]} seeds per side. Cap: {args.max_seeds} seeds per side.", "",
             f"RNG seed: {args.rng_seed}; R clamp: [1, {MAX_RUNS}].", "",
             "Incomplete: stopped after capture failures, no rows, or rows not measured; see plan errors and the not-measured rows." if incomplete else "Complete.", "",
             "Seed means decide; medians describe the seed means. "
             f"Intervals are {100 * (1 - args.alpha / len(sizes) / 2):g}% two-sided Welch intervals on log seed means. "
             "Holm corrects difference and equivalence tests separately over all rows. "
             "Inconclusive rows alone receive more seeds at the planned looks.", ""]
    lines += ["R by look: " + "; ".join(
        f"{look}: " + ", ".join(str(value) for value in sorted({item["runs"] for item in plan if item["look"] == look}))
        for look in sorted({item["look"] for item in plan})), ""]
    for side in records:
        lines += [f"{side.upper()} seeds: " + ", ".join(str(record["seed"]) for record in records[side]),
                  f"{side.upper()} build ids: " + ", ".join(record["build_id"] for record in records[side]), ""]
        known, _, owners = observations(records[side])
        for name in sorted(known):
            suite, row_name = name.split("/", 1)
            owner = owners.get(name, (suite, None))[1]
            missing = [record["seed"] for record in records[side] if suite in record["suites"]
                       and any(row_name not in run["rows"] for run in record["suites"][suite]["runs"])
                       and (record["suites"][suite].get("tests", "-") == "-"
                            or (owner and any(pattern in owner for pattern in
                                record["suites"][suite]["tests"].split(","))))]
            if missing:
                lines += [f"{side.upper()} incomplete timing seeds for `{name}`: " +
                          ", ".join(map(str, missing)), ""]
    lines += ["| Row | A mean us | A median us | B mean us | B median us | B/A | Delta time % | Delta insn % | Interval B/A | Welch Holm p | TOST Holm p | Permutation p | Verdict |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---|"]
    def number(value):
        return "n/a" if value is None else f"{value:.6g}"
    for name, row in sorted(rows.items()):
        ratio = row["ratio"]
        instruction_delta = row.get("delta_insn")
        interval = "n/a" if row["interval"] is None else ", ".join(number(x) for x in row["interval"])
        lines.append(f"| `{name}` | {number(row['a'])} | {number(row.get('a_median'))} | "
                     f"{number(row['b'])} | {number(row.get('b_median'))} | {number(ratio)} | "
                     f"{number(100 * (ratio - 1) if ratio else None)} | {number(instruction_delta)} | "
                     f"{interval} | {number(row['p_adjusted'])} | {number(row['equivalence_adjusted'])} | "
                     f"{number(row['permutation'])} | {row['verdict']} |")
    lines += ["", "Permutation n/a means there is too little positive timing data to compute it. Missing or zero timings are not measured.", "",
              "No change requires Holm-adjusted TOST equivalence within the threshold. "
              "Regressed or improved requires a Holm-adjusted Welch difference and permutation agreement. "
              "All other results are inconclusive, including undecided rows at the cap.", "",
              "| Side / row | sigma run | sigma flash | R* | K for threshold |",
              "|---|---:|---:|---:|---:|"]
    for side, estimates_by_row in estimates.items():
        for name, estimate in sorted(estimates_by_row.items()):
            lines.append(f"| {side}/{name} | {estimate['sigma_run']:.6g} | {estimate['sigma_flash']:.6g} | "
                         f"{estimate['recommended_runs']} | {estimate['recommended_seeds']} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv=None):
    """Parse a comparison, preflight validation, or restore request."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-plan", action="store_true")
    parser.add_argument("--restore-project", type=Path)
    parser.add_argument("--out", "-o", type=Path)
    parser.add_argument("--project-a", type=Path)
    parser.add_argument("--project-b", type=Path)
    parser.add_argument("--label-a", default="A")
    parser.add_argument("--label-b", default="B")
    parser.add_argument("--suite", nargs=3, action="append", metavar=("NAME", "TESTS", "TABLE"))
    parser.add_argument("--threshold", type=float, default=1.0)
    parser.add_argument("--alpha", type=float, default=0.05)
    parser.add_argument("--max-seeds", type=int, default=32)
    parser.add_argument("--rng-seed", type=int, help="replay a recorded random plan; default: fresh draw")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--wait", type=int, default=3600)
    parser.add_argument("--autana", help="inject an autana-compatible command for host replay")
    args = parser.parse_args(argv)
    if args.restore_project:
        return subprocess.call(autana_command("--wait", str(args.wait), "flash", "rel",
                                             "--project", str(args.restore_project), override=args.autana))
    if not 0 < args.threshold < 100 or not 0 < args.alpha < 1 or min(args.timeout, args.wait) <= 0:
        parser.error("threshold, alpha and time limits must be positive and in range")
    if args.validate_plan:
        if not args.suite:
            parser.error("--suite is required")
        try:
            schedule(args.max_seeds, args.alpha)
            for project in (args.project_a, args.project_b):
                if project:
                    split_filters(args.suite, filter_limits(project))
        except ValueError as error:
            parser.error(str(error))
        return 0
    if not all((args.out, args.project_a, args.project_b, args.suite)):
        parser.error("--out, --project-a, --project-b and --suite are required")
    try:
        measure(args)
    except (RuntimeError, ValueError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


def device_inventory(records):
    """Pool results from unfiltered invocations as the complete suite inventory."""
    listed = {}
    for record in records:
        for suite, entry in record["suites"].items():
            for run in entry["runs"]:
                listed.setdefault(suite, set()).update(run.get("inventory", []))
    return listed


if __name__ == "__main__":
    sys.exit(main())

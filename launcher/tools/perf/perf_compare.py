#!/usr/bin/env python3
"""Compare revisions using independent layout seeds as the measurement unit."""
import argparse
import json
import math
import random
import sys
from pathlib import Path
from statistics import mean, median
from types import SimpleNamespace

from layout_measure import analyse, parse_report, required_seeds, run_flash
from seed_statistics import compare, minimum_seeds


def schedule(cap, alpha):
    first = minimum_seeds(alpha)
    while True:
        looks = 1 + math.ceil(math.log2(max(1, cap/first)))
        needed = minimum_seeds(alpha/looks)
        if needed <= first:
            break
        first = needed
    if cap < first:
        raise ValueError(f"--max-seeds must be at least {first} for permutation resolution")
    sizes = [first]
    while sizes[-1] < cap:
        sizes.append(min(cap, sizes[-1]*2))
    return sizes


def observations(records):
    rows, instructions, owners = {}, {}, {}
    for record in records:
        for suite, entry in record['suites'].items():
            gathered, counters = {}, {}
            for run in entry['runs']:
                for name, value in run['rows'].items():
                    key = suite + '/' + name
                    gathered.setdefault(key, []).append(value)
                    owners[key] = (suite, run['owners'].get(name))
                for name, value in run['instructions'].items():
                    counters.setdefault(suite + '/' + name, []).append(value)
            for key, values in gathered.items():
                if len(values) == len(entry['runs']):
                    rows.setdefault(key, []).append(values)
            for key, values in counters.items():
                if len(values) == len(entry['runs']):
                    instructions.setdefault(key, []).append(mean(values))
    return rows, instructions, owners


def seed_means(rows):
    return {name: [mean(values) for values in seeds] for name, seeds in rows.items()}


def selected_suites(suites, active, owners):
    selected = []
    for suite, tests, template in suites:
        names = [owners.get(name, (suite, None))[1] for name in active if name.startswith(suite + '/')]
        if names:
            patterns = ','.join(sorted(set(names))) if all(names) else tests
            selected.append((suite, patterns, template))
    return selected


def recommendations(records, rows, delta):
    costs = [(record['flash_seconds'], mean(entry['run_seconds']))
             for record in records for entry in record['suites'].values() if mean(entry['run_seconds']) > 0]
    ratio = mean(flash/run for flash, run in costs) if costs else 1
    result = {}
    for name, values in rows.items():
        if len(values) < 2 or any(len(seed) < 2 for seed in values) or mean(map(mean, values)) <= 0:
            continue
        estimate = analyse(values, ratio, diagnostics=False)
        if estimate['optimum_runs'] is None:
            runs = max(2, math.ceil(ratio)) if estimate['sigma_run'] else 2
        else:
            runs = max(2, estimate['optimum_runs'])
        count = required_seeds(estimate['sigma_flash'], estimate['sigma_run'], runs, delta) or 2
        result[name] = dict(estimate, recommended_runs=runs, recommended_seeds=count)
    return result


def measure(args, runner=None):
    sizes = schedule(args.max_seeds, args.alpha)
    look_alpha = args.alpha / len(sizes)
    rng = random.Random(args.rng_seed)
    used, plan = set(), []
    records = {'a': [], 'b': []}
    decisions, family, active = {}, set(), None
    failures = 0
    skip_until = 0
    for look, target in enumerate(sizes):
        if look < skip_until:
            continue
        extracted = {side: observations(records[side]) for side in records}
        hints = {side: recommendations(records[side], extracted[side][0], args.threshold/100)
                 for side in records} if look else {'a': {}, 'b': {}}
        runs = max([2] + [hint['recommended_runs'] for side in hints
                          for name, hint in hints[side].items() if name in (active or set())])
        suites = args.suite if active is None else selected_suites(
            args.suite, active, {**extracted['a'][2], **extracted['b'][2]})
        count = target - previous_target if look else target
        previous_target = target
        for _ in range(count):
            sides = ['a', 'b']
            rng.shuffle(sides)
            for side in sides:
                seed = rng.randint(1, 2147483647)
                while seed in used:
                    seed = rng.randint(1, 2147483647)
                used.add(seed)
                item = dict(side=side, seed=seed, runs=runs, suites=suites, look=look+1)
                plan.append(item)
                destination = Path(args.out)
                destination.mkdir(parents=True, exist_ok=True)
                (destination/'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
                flash_args = SimpleNamespace(out=destination/side, project=getattr(args, 'project_'+side),
                                             runs=runs, suite=suites, timeout=args.timeout, wait=args.wait, autana=getattr(args, 'autana', None))
                try:
                    record = run_flash(flash_args, seed, runner)
                except (RuntimeError, OSError, ValueError) as error:
                    failures += 1
                    item['error'] = str(error)
                    (destination/'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
                    if failures >= 2:
                        raise RuntimeError('stopping after two consecutive capture failures') from error
                    continue
                failures = 0
                records[side].append(record)
        extracted = {side: observations(records[side]) for side in records}
        family.update(set(extracted['a'][0]) | set(extracted['b'][0]))
        current = compare(seed_means(extracted['a'][0]), seed_means(extracted['b'][0]),
                          args.threshold, look_alpha, args.rng_seed, family)
        for name, result in current.items():
            if name not in decisions or decisions[name]['verdict'] == 'inconclusive':
                decisions[name] = dict(result, look=look+1)
        active = {name for name, result in decisions.items() if result['verdict'] == 'inconclusive'}
        if not active:
            break
        needed = []
        for side in records:
            estimates_now = recommendations(records[side], extracted[side][0], args.threshold/100)
            needed.extend(hint['recommended_seeds'] for name, hint in estimates_now.items() if name in active)
        desired = max([target+1] + needed)
        skip_until = next((index for index in range(look+1, len(sizes)) if sizes[index] >= desired), len(sizes)-1)
    estimates = {side: recommendations(records[side], observations(records[side])[0], args.threshold/100)
                 for side in records}
    write_summary(Path(args.out)/'summary.md', args, records, decisions, estimates, sizes)
    payload = dict(threshold=args.threshold, alpha=args.alpha, look_alpha=look_alpha,
                   max_seeds=args.max_seeds, first_pass=sizes[0], rows=decisions, estimates=estimates, plan=plan)
    (Path(args.out)/'comparison.json').write_text(json.dumps(payload, indent=2), encoding='utf-8')
    print((Path(args.out)/'summary.md').read_text(encoding='utf-8'))
    return payload


def write_summary(path, args, records, rows, estimates, sizes):
    lines = ['# Performance comparison', '',
             f'A: `{args.label_a}`; B: `{args.label_b}`.', '',
             f'Threshold: +/-{args.threshold:g}%; alpha: {args.alpha:g}; '
             f'per-look alpha: {args.alpha/len(sizes):g}. First pass: {sizes[0]} seeds per side, '
             f'2 runs per seed. Cap: {args.max_seeds} seeds per side.', '',
             'Seed means decide; medians describe the seed means. Intervals are Welch intervals on log seed means. '
             'Holm corrects difference and equivalence tests separately over all rows. '
             'Inconclusive rows alone receive more seeds at the planned looks.', '']
    for side in records:
        lines += [f"{side.upper()} seeds: " + ', '.join(str(record['seed']) for record in records[side]),
                  f"{side.upper()} build ids: " + ', '.join(record['build_id'] for record in records[side]), '']
    lines += ['| Row | A mean us | A median us | B mean us | B median us | B/A | Delta time % | Delta insn % | Interval B/A | Welch Holm p | TOST Holm p | Permutation p | Verdict |',
              '|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---|']
    counters = {side: observations(records[side])[1] for side in records}
    def number(value):
        return 'n/a' if value is None else f'{value:.6g}'
    for name, row in sorted(rows.items()):
        ratio = row['ratio']
        instruction_delta = None
        if counters['a'].get(name) and counters['b'].get(name) and mean(counters['a'][name]):
            instruction_delta = 100*(mean(counters['b'][name])/mean(counters['a'][name])-1)
        interval = 'n/a' if row['interval'] is None else ', '.join(number(x) for x in row['interval'])
        lines.append(f"| `{name}` | {number(row['a'])} | {number(row.get('a_median'))} | "
                     f"{number(row['b'])} | {number(row.get('b_median'))} | {number(ratio)} | "
                     f"{number(100*(ratio-1) if ratio else None)} | {number(instruction_delta)} | "
                     f"{interval} | {number(row['p_adjusted'])} | {number(row['equivalence_adjusted'])} | "
                     f"{number(row['permutation'])} | {row['verdict']} |")
    lines += ['', 'Permutation n/a means the seed count cannot reach the per-look alpha, '
              'or positive timing data is insufficient. Zero timings remain inconclusive.', '',
              'No change requires Holm-adjusted TOST equivalence within the threshold. '
              'Regressed or improved requires a Holm-adjusted Welch difference and permutation agreement. '
              'All other results are inconclusive, including undecided rows at the cap.', '',
              '| Side / row | sigma run | sigma flash | R* | K for threshold |',
              '|---|---:|---:|---:|---:|']
    for side, estimates_by_row in estimates.items():
        for name, estimate in sorted(estimates_by_row.items()):
            lines.append(f"| {side}/{name} | {estimate['sigma_run']:.6g} | {estimate['sigma_flash']:.6g} | "
                         f"{estimate['recommended_runs']} | {estimate['recommended_seeds']} |")
    path.write_text('\n'.join(lines)+'\n', encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-report', type=Path)
    parser.add_argument('--out', '-o', type=Path)
    parser.add_argument('--project-a', type=Path)
    parser.add_argument('--project-b', type=Path)
    parser.add_argument('--label-a', default='A')
    parser.add_argument('--label-b', default='B')
    parser.add_argument('--suite', nargs=3, action='append', metavar=('NAME', 'TESTS', 'TABLE'))
    parser.add_argument('--threshold', type=float, default=1.0)
    parser.add_argument('--alpha', type=float, default=0.05)
    parser.add_argument('--max-seeds', type=int, default=32)
    parser.add_argument('--rng-seed', type=int, default=0)
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--wait', type=int, default=3600)
    parser.add_argument('--autana', help='inject an autana-compatible command for host replay')
    args = parser.parse_args(argv)
    if args.check_report:
        return 0 if parse_report(args.check_report) else 1
    if not all((args.out, args.project_a, args.project_b, args.suite)):
        parser.error('--out, --project-a, --project-b and --suite are required')
    if not 0 < args.threshold < 100 or not 0 < args.alpha < 1 or min(args.timeout, args.wait) <= 0:
        parser.error('threshold, alpha and time limits must be positive and in range')
    try:
        measure(args)
    except (RuntimeError, ValueError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

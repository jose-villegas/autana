"""Seed inference and foreground acquisition exercised without hardware."""
import contextlib
import io
import inspect
import math
import pathlib
import random
import sys
import tempfile
import subprocess
import os
import unittest
from unittest.mock import patch
from types import SimpleNamespace

PERF = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = pathlib.Path(__file__).resolve().parent / 'fixtures'
sys.path.insert(0, str(PERF))
import perf_compare as tool
import seed_statistics as stats
from layout_measure import capture_metadata, parse_report, run_flash, run_stamped


class FakeAutana:
    def __init__(self, root, shifted=False, mode='ok', noisy=False):
        self.root, self.shifted, self.mode, self.noisy = pathlib.Path(root), shifted, mode, noisy
        self.calls = []

    def __call__(self, command, log, timeout):
        if "status" in command:
            return 0, [(0, "free")], 0
        self.calls.append(command)
        def arg(name):
            return command[command.index(name)+1]
        project = pathlib.Path(arg('--project'))
        seed, runs = int(arg('--layout-seed')), int(arg('--runs'))
        suite = command[command.index('suite')+1]
        build = f'{seed}-diag'
        directory = project/'launcher/build.diag'
        directory.mkdir(parents=True, exist_ok=True)
        (directory/'build_id.txt').write_text(build)
        lines = [(1, 'booted BUILD_ID=' + (build if self.mode != 'mismatch' else 'wrong-diag'))]
        tests = arg('--test').split(',') if '--test' in command else ['test_quiet', 'test_heavy']
        for run in range(runs):
            base = self.root/f'capture_{len(self.calls)}_{run}'
            lines.append((10+run*10, f'batch: {suite} run {run+1}/{runs}'))
            text = ''.join(f'SUITE_TEST name={name} selected={int(any(p in name for p in tests))}\n'
                           for name in ['test_quiet', 'test_heavy']) if '--test' in command else ''
            for name in ['test_quiet', 'test_heavy']:
                if not any(pattern in name for pattern in tests):
                    continue
                row = name.removeprefix('test_')
                value = 10000
                if self.shifted and project.name == 'b':
                    value += 1000
                if row == 'heavy' and self.noisy:
                    value += 3000 if seed % 2 else -3000
                text += f'I (1) perf: {row} both cores: mean {value}us\n'
                text += f'I (2) xtperf: scene={row} event=insn cycles_per_step=99 value_per_step={value*2} steps=1\n'
                text += f':1:{name}:PASS\nTEST_TIME name={name} elapsed_ms=1\n'
            base.with_suffix('.log').write_text(text)
            base.with_suffix('.md').write_text('- Ended: complete\n' if self.mode != 'incomplete' else '- Ended: timeout\n')
            lines.append((19+run*10, 'report: ' + str(base.with_suffix('.md'))))
        return (1 if self.mode == 'budget' else 0), lines, runs*10+10


def arguments(root, cap=16):
    root = pathlib.Path(root)
    for side in ("a", "b"):
        header = root/side/"launcher/test/suites.h"
        if not header.exists():
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_bytes((PERF.parents[2]/"launcher/test/suites.h").read_bytes())
    return SimpleNamespace(out=root/'out', project_a=root/'a', project_b=root/'b',
                           suite=[('suite', '-', '-')], max_seeds=cap, alpha=.05,
                           threshold=1., rng_seed=9, timeout=1800, wait=3600,
                           label_a='A', label_b='B')


def record_for_rows(args, seed, values):
    """Records for decision tests; capture parsing has its own acquisition tests."""
    suites = {}
    inventory = ['test_' + row for row in values]
    for suite, tests, _ in args.suite:
        rows = {row: value for row, value in values.items()
                if tests == '-' or any(pattern in 'test_' + row for pattern in tests.split(','))}
        run = dict(rows=rows, owners={row: 'test_' + row for row in rows},
                   instructions={row: value * 2 for row, value in rows.items()},
                   listed_tests=['test_' + row for row in rows], inventory=inventory)
        suites[suite] = dict(tests=tests, runs=[run.copy() for _ in range(args.runs)],
                             run_seconds=[10] * (args.runs - 1) + [9])
    return dict(seed=seed, build_id=f'{seed}-diag', flash_seconds=10, suites=suites)


class SeedInferenceTest(unittest.TestCase):
    def test_parsers_pool_tables_and_ignore_capture_worst(self):
        self.assertEqual(parse_report(FIXTURES/'sponza_capture.txt')['sponza'], 51000)
        self.assertIn('hot', parse_report(FIXTURES/'sand_a.md'))

    def test_student_distribution_and_holm(self):
        self.assertAlmostEqual(stats.t_cdf(1, 1), .75)
        self.assertAlmostEqual(stats.t_quantile(.975, 10), 2.228139, places=5)
        self.assertEqual(stats.holm({'a': .01, 'b': .04, 'c': .1}), {'a': .03, 'b': .08, 'c': .1})

    def test_three_verdicts_and_missing_or_zero_rows(self):
        a = {'quiet': [100]*8, 'up': [100]*8, 'down': [100]*8, 'zero': [0]*8, 'missing': [1]*8}
        b = {'quiet': [100]*8, 'up': [110]*8, 'down': [90]*8, 'zero': [0]*8}
        result = stats.compare(a, b)
        self.assertEqual([result[name]['verdict'] for name in ['quiet', 'up', 'down', 'zero', 'missing']],
                         ['no change', 'regressed', 'improved', 'not measured', 'removed'])
        self.assertAlmostEqual(result['up']['ratio'], 1.1)

    def test_permutation_resolution_and_monte_carlo(self):
        self.assertAlmostEqual(stats.permutation([1]*3, [2]*3, .05, random.Random(1)), .1)
        self.assertAlmostEqual(stats.permutation([1]*4, [2]*4, .05, random.Random(1)), 2/70)
        self.assertLess(stats.permutation([1]*12, [2]*12, .05, random.Random(1), samples=999), .05)

    @patch.object(stats, "permutation_samples", new=lambda alpha: 199)
    def test_aa_smoke_and_planted_shift_on_synthetic_and_recorded_data(self):
        rng = random.Random(412)
        fixtures = [parse_report(path) for path in sorted((FIXTURES/'sponza_runs').glob('*.txt'))]
        false, detected, draws = 0, 0, 4
        for _ in range(draws):
            a, b = {}, {}
            for side in (a, b):
                side['quiet'] = [math.exp(rng.gauss(0, .002)) for _ in range(6)]
                side['heavy'] = [math.exp(rng.gauss(0, .3)) for _ in range(6)]
                side['near_zero'] = [1e-6*math.exp(rng.gauss(0, .1)) for _ in range(6)]
                side['bimodal'] = [math.exp(rng.choice([-.15, .15])+rng.gauss(0, .003)) for _ in range(6)]
                for name in fixtures[0]:
                    side[name] = [rng.choice(fixtures)[name] for _ in range(6)]
            result = stats.compare(a, b, rng_seed=rng.randrange(100000))
            false += any(row['verdict'] in ('improved', 'regressed') for row in result.values())
            b['quiet'] = [value*1.05 for value in b['quiet']]
            detected += stats.compare(a, b)['quiet']['verdict'] == 'regressed'
        tolerance = draws*.05 + 3*math.sqrt(draws*.05*.95)
        self.assertLessEqual(false, tolerance)
        self.assertGreaterEqual(detected, draws*.95)

    def test_seed_unit_does_not_treat_repeated_runs_as_independent(self):
        rows = tool.seed_means({'row': [[100]*100, [200]*100]})
        self.assertEqual(rows['row'], [100, 200])
        self.assertEqual(stats.compare(rows, rows)['row']['verdict'], 'inconclusive')


class AcquisitionTest(unittest.TestCase):
    def measure(self, root, fake, cap=16):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            payload = tool.measure(arguments(root, cap), fake)
        return payload, output.getvalue()

    def test_ab_and_aa_replay_interleaved_distinct_seed_sets(self):
        for shift in (False, True):
            with tempfile.TemporaryDirectory() as root:
                fake = FakeAutana(root, shifted=shift, mode='budget')
                payload, summary = self.measure(root, fake)
                self.assertTrue(all(row['verdict'] == ('regressed' if shift else 'no change')
                                    for row in payload['rows'].values()))
                seeds = [item['seed'] for item in payload['plan']]
                self.assertEqual(len(seeds), len(set(seeds)))
                self.assertTrue(all(set(item['side'] for item in payload['plan'][i:i+2]) == {'a', 'b'}
                                    for i in range(0, len(seeds), 2)))
                self.assertIn('Delta insn %', summary)
                self.assertIn('Cap: 16', summary)
                self.assertGreaterEqual(payload['first_pass'], 4)
                self.assertTrue(all('--perf-scope' in call and '--flash' in call for call in fake.calls))
                args = arguments(root)
                if not shift:
                    args.project_b = args.project_a
                    with contextlib.redirect_stdout(io.StringIO()):
                        aa = tool.measure(args, FakeAutana(root))
                    self.assertEqual([item['seed'] for item in aa['plan']], seeds)

    def test_shell_wrapper_runs_ab_and_aa_with_fake_autana(self):
        with tempfile.TemporaryDirectory() as root:
            root = pathlib.Path(root)
            fake = root/'fake.py'
            fake.write_text("import pathlib,sys\n" + inspect.getsource(FakeAutana) + "\n" +
                            f"fake = FakeAutana({str(root)!r}, shifted=True)\n"
                            "code, lines, wall = fake(sys.argv[1:], None, 1800)\n"
                            "for at, line in lines: print(line, flush=True)\n"
                            "sys.exit(code)\n")
            for name in ('a', 'b'):
                (root/name/'launcher/test').mkdir(parents=True)
                (root/name/'launcher/test/suites.h').write_bytes((PERF.parents[2]/'launcher/test/suites.h').read_bytes())
            def shell_path(path):
                return pathlib.Path(path).as_posix()
            for side in ('a', 'b'):
                destination = root/f'out_{side}'
                command = ['sh', shell_path(PERF/'perf_compare.sh'), shell_path(root/'a'), shell_path(root/side),
                           '--no-restore', '-o', shell_path(destination), '--max-seeds', '8', '--suite', 'suite', '-', '-',
                           '--autana', f'"{shell_path(sys.executable)}" "{shell_path(fake)}"']
                done = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertIn('no change' if side == 'a' else 'regressed', done.stdout)

    def test_extra_seeds_only_remeasure_inconclusive_rows_and_stop_at_cap(self):
        with tempfile.TemporaryDirectory() as root:
            fake = FakeAutana(root, noisy=True)
            payload, summary = self.measure(root, fake, cap=10)
            self.assertEqual(payload['rows']['suite/quiet']['verdict'], 'no change')
            self.assertEqual(payload['rows']['suite/heavy']['verdict'], 'inconclusive')
            extra = [item for item in payload['plan'] if item['look'] > 1]
            self.assertTrue(extra)
            self.assertTrue(all(item['suites'][0][1] == 'h' for item in extra))
            self.assertEqual(len(payload['plan']), 20)
            self.assertIn('inconclusive', summary)

    def test_unmapped_row_falls_back_to_original_suite_filter(self):
        self.assertEqual(tool.selected_suites([('suite', '-', '-')], {'suite/row'}, {}), [('suite', '-', '-')])

    def test_foreground_runner_enforces_capture_deadline(self):
        with self.assertRaisesRegex(RuntimeError, 'timed out'):
            run_stamped([sys.executable, '-c', 'import time; time.sleep(2)'], io.StringIO(), timeout=.05)

    def test_wrong_build_and_incomplete_captures_stop_after_two_failures(self):
        for mode in ('mismatch', 'incomplete'):
            with tempfile.TemporaryDirectory() as root:
                fake = FakeAutana(root, mode=mode)
                with self.assertRaisesRegex(RuntimeError, 'two consecutive'):
                    self.measure(root, fake)
                self.assertEqual(len(fake.calls), 2)

    def test_capture_rows_map_to_test_and_counter(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.out = pathlib.Path(root)/'flash'
            args.project = args.project_a
            args.runs = 2
            record = run_flash(args, 3, FakeAutana(root))
            run = record['suites']['suite']['runs'][0]
            self.assertEqual(run['owners']['quiet'], 'test_quiet')
            self.assertEqual(run['instructions']['quiet'], 20000)

    @patch.object(stats, "permutation_samples", new=lambda alpha: 199)
    def test_aa_smoke_over_generated_captures_and_planted_shift(self):
        class GeneratedAutana(FakeAutana):
            def __init__(self, root, draw, planted=False):
                super().__init__(root)
                self.draw, self.planted = draw, planted
            def __call__(self, command, log, timeout):
                code, lines, wall = super().__call__(command, log, timeout)
                if "status" in command:
                    return code, lines, wall
                seed = int(command[command.index('--layout-seed')+1])
                project = pathlib.Path(command[command.index('--project')+1])
                rng = random.Random(seed + self.draw*2147483647)
                layout = {'quiet': rng.gauss(0, .0005), 'heavy': rng.gauss(0, .3),
                          'near_zero': rng.gauss(0, .2), 'bimodal': rng.choice([-.15, .15])}
                for _, line in lines:
                    if not line.startswith('report: '):
                        continue
                    text = ''.join(f'SUITE_TEST name=test_{name} selected=1\n'
                                   for name in layout)
                    for name, base in [('quiet', 100000), ('heavy', 100000), ('near_zero', 2), ('bimodal', 100000)]:
                        value = max(1, round(base*math.exp(layout[name]+rng.gauss(0, .0005))))
                        if self.planted and name == 'quiet' and project.name == 'b':
                            value = round(value*1.05)
                        text += f'I (1) perf: {name} both cores: mean {value}us\n'
                        text += f':1:test_{name}:PASS\n'
                    pathlib.Path(line[8:]).with_suffix('.log').write_text(text)
                return code, lines, wall
        false, detected, equivalent, draws = 0, 0, 0, 4
        for draw in range(draws):
            with tempfile.TemporaryDirectory() as root:
                aa, _ = self.measure(root, GeneratedAutana(root, draw), cap=4)
                false += any(row['verdict'] in ('improved', 'regressed') for row in aa['rows'].values())
                equivalent += aa['rows']['suite/quiet']['verdict'] == 'no change'
                ab, _ = self.measure(root, GeneratedAutana(root, draw, planted=True), cap=4)
                detected += ab['rows']['suite/quiet']['verdict'] == 'regressed'
        tolerance = 3*math.sqrt(draws*.05*.95)
        self.assertLessEqual(false, draws*.05+tolerance)
        self.assertGreaterEqual(equivalent, draws*.95-tolerance)
        self.assertGreaterEqual(detected, draws*.95-tolerance)

    def test_real_table_command_reads_revision_source_and_pools_tables(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            project = args.project_a
            project.mkdir(parents=True, exist_ok=True)
            source = project/'suite.c'
            source.write_text('test_quiet 200\ntest_heavy 300\n')
            reporter = FIXTURES / 'table_report.py'
            args.suite = [('suite', '-', f'python3 "{reporter}" @CAPTURE@ @TABLE@ --source @PROJECT@/suite.c')]
            class TableRunner(FakeAutana):
                def __call__(self, command, log, timeout):
                    code, lines, wall = super().__call__(command, log, timeout)
                    for _, line in lines:
                        if line.startswith('report: '):
                            path = pathlib.Path(line[8:]).with_suffix('.log')
                            path.write_text('I (1) device_tests: row 100 us\n'
                                            ':1:test_quiet:PASS\n'
                                            'I (2) device_tests: row 200 us\n'
                                            ':2:test_heavy:PASS\n')
                    return code, lines, wall
            args.project = project
            args.out = pathlib.Path(root)/'flash'
            args.runs = 2
            record = run_flash(args, 2, TableRunner(root))
            self.assertEqual(record['suites']['suite']['runs'][0]['rows'], {'test_quiet': 100, 'test_heavy': 200})

    def test_wall_cost_and_variance_drive_recommendations(self):
        records = [{'flash_seconds': 100, 'suites': {'suite': {'run_seconds': [10, 10]}}}]
        rows = {'row': [[990, 1010], [991, 1011], [989, 1009], [992, 1012]]}
        result = tool.recommendations(records, rows, .01)['row']
        self.assertGreater(result['recommended_runs'], 2)
        self.assertGreaterEqual(result['recommended_seeds'], 2)


if __name__ == '__main__':
    unittest.main()

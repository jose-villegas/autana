"""Seed inference and foreground acquisition exercised without hardware."""
import contextlib
import io
import math
import pathlib
import random
import sys
import tempfile
import unittest
from shell_job import ShellJobTests
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
                           threshold=1., rng_seed=9, timeout=1800, wait=3600, max_minutes=tool.DEFAULT_MAX_MINUTES,
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
    def test_raw_rows_accept_single_dual_and_unspecified_cores(self):
        with tempfile.TemporaryDirectory() as root:
            capture = pathlib.Path(root) / 'capture.log'
            capture.write_text('dual both cores: mean 10us\n'
                               'single one core: mean 20us\n'
                               'neutral mean 30us\n:1:test_timing:PASS\n')
            rows = parse_report(capture)
            self.assertEqual(rows, {'dual': 10, 'single': 20, 'neutral': 30})
            owners, _ = capture_metadata(capture, rows)
            self.assertEqual(owners, {row: 'test_timing' for row in rows})

    def test_quarters_keep_separate_values_and_test_ownership(self):
        with tempfile.TemporaryDirectory() as root:
            capture = pathlib.Path(root) / 'capture.log'
            capture.write_text('mesh both cores: mean 10us quarter=portrait\n'
                               ':1:test_portrait:PASS\n'
                               'mesh both cores: mean 20us quarter=landscape\n'
                               ':1:test_landscape:PASS\n')
            expected = {'mesh quarter=portrait': 10, 'mesh quarter=landscape': 20}
            self.assertEqual(parse_report(capture), expected)
            owners, _ = capture_metadata(capture, expected)
            self.assertEqual(owners, {'mesh quarter=portrait': 'test_portrait',
                                      'mesh quarter=landscape': 'test_landscape'})
            from capture import select_quarter
            with self.assertRaisesRegex(ValueError, 'landscape'):
                select_quarter('mesh mean 10us', 'landscape')
            self.assertNotIn('20us', select_quarter(capture.read_text(), 'portrait'))

    def test_dense_pose_summary_and_frame_profile(self):
        from layout_measure import parse_timings
        text = ('atrium both cores: mean 800us (1250.0 fps before present), worst 1000us, '
                'med 750us, p95 990us quarter=portrait\n'
                'frame_profile: sponza frames=20 mean 900us med 880us p95 1000us worst 1100us '
                'best 700us | fps mean 1111.11 low 1000.00 worst 909.09 quarter=landscape\n')
        self.assertEqual(parse_timings(text), {'atrium quarter=portrait': 800,
                                              'sponza quarter=landscape': 900})

    def test_parsers_pool_tables_and_ignore_capture_worst(self):
        self.assertEqual(parse_report(FIXTURES/'sponza_capture.txt')['sponza quarter=portrait'], 51000)
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


class AcquisitionTest(ShellJobTests, unittest.TestCase):
    job_kind = "compare"

    def measure(self, root, fake, cap=16):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            payload = tool.measure(arguments(root, cap), fake)
        return payload, output.getvalue()

    def test_quarter_identity_through_acquisition_and_comparison(self):
        for baseline in ("legacy", "portrait", "both"):
            with self.subTest(baseline=baseline), tempfile.TemporaryDirectory() as root:
                fake = FakeAutana(root)
                def runner(command, log, timeout):
                    result = fake(command, log, timeout)
                    if "suite" not in command:
                        return result
                    candidate = pathlib.Path(command[command.index('--project') + 1]).name == 'b'
                    for _, line in result[1]:
                        if not line.startswith('report: '):
                            continue
                        path = pathlib.Path(line.removeprefix('report: ')).with_suffix('.log')
                        suffix = ' quarter=portrait' if candidate or baseline != 'legacy' else ''
                        text = f'mesh both cores: mean 10000us{suffix}\n'
                        if candidate or baseline == 'both':
                            text += 'mesh both cores: mean 20000us quarter=landscape\n'
                        path.write_text(text + ':1:test_quiet:PASS\n')
                    return result
                payload, _ = self.measure(root, runner, cap=4)
                rows = payload['rows']
                self.assertEqual(rows['suite/mesh quarter=landscape']['b'], 20000)
                self.assertEqual(rows['suite/mesh quarter=landscape']['verdict'],
                                 'no change' if baseline == 'both' else 'added')
                if baseline != 'legacy':
                    self.assertEqual(rows['suite/mesh quarter=portrait']['verdict'], 'no change')
                    self.assertEqual(rows['suite/mesh quarter=portrait']['ratio'], 1)
                else:
                    self.assertEqual(rows['suite/mesh']['verdict'], 'removed')
                    self.assertEqual(rows['suite/mesh quarter=portrait']['verdict'], 'added')

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

    def test_compare_estimate_covers_first_pass(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, cap=8)
            with patch("perf_job.job_estimate", return_value=1) as estimate, \
                    contextlib.redirect_stdout(io.StringIO()):
                tool.main(["--estimate-plan", "--max-seeds", str(args.max_seeds),
                           "--project-a", str(args.project_a), "--project-b", str(args.project_b),
                           "--suite", "suite", "-", "-",
                           "--send-a", "sound stop all", "--send-b", "sound play tap loop"])
            # Two sides: 2 * 4 = 8 flashes; 2 * (4 + 2 repeats) = 12 captures.
            estimate.assert_called_once_with(8, 12, 1800, True)

    def test_job_estimate_uses_history_and_fallbacks(self):
        from perf_job import DEFAULT_FLASH_SECONDS, job_estimate
        with patch("perf_job.duration_history", return_value={"flash": 10, "run-suite": 20}):
            self.assertEqual(job_estimate(2, 3, 900), 90)
        with patch("perf_job.duration_history", return_value={}):
            self.assertEqual(job_estimate(2, 3, 900, restore=False),
                             2 * DEFAULT_FLASH_SECONDS + 3 * 900)

    def test_shell_validation_takes_no_hold(self):
        from shell_job import replay
        for options in (("--validate-plan",),
                        ("--validate-plan", "--send-a", "sound stop all",
                         "--send-b", "sound stop all", "--send-b", "sound play tap loop"),
                        ("--bad-option",), ("--help",)):
            result, calls = replay(self, "compare", extra=options)
            self.assertEqual(calls, [])
            self.assertEqual(result.returncode == 0, options != ("--bad-option",), result.stderr)

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

    def budget_run(self, minutes, clock):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.max_minutes = minutes
            with contextlib.redirect_stdout(io.StringIO()) as output:
                payload = tool.measure(args, FakeAutana(root, noisy=True), clock=clock)
            return payload, output.getvalue()

    def test_a_look_past_the_time_budget_is_not_started_and_names_unresolved_rows(self):
        payload, summary = self.budget_run(1 / 60, lambda: 0)
        stop = payload['budget_stop']
        self.assertGreater(stop['look'], 1)
        self.assertEqual({item['look'] for item in payload['plan']}, {1})
        self.assertEqual(payload['rows']['suite/quiet']['verdict'], 'no change')
        self.assertIn('Unresolved: `suite/heavy`.', summary)
        self.assertIn(f"Complete; stopped at the time budget before look {stop['look']}", summary)
        self.assertIn(f"--max-minutes {stop['budget_seconds'] / 60:g}", summary)

    def test_an_incomplete_comparison_still_says_it_stopped_at_the_budget(self):
        stop = dict(look=2, elapsed_seconds=0, estimate_seconds=60, budget_seconds=1)
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            path = pathlib.Path(root) / 'summary.md'
            tool.write_summary(path, args, {'a': [], 'b': []}, {}, {}, tool.schedule(16, args.alpha), [],
                               incomplete=True, budget_stop=stop)
            text = path.read_text(encoding='utf-8')
            self.assertIn('Incomplete', text)
            self.assertIn(f"stopped at the time budget before look {stop['look']}", text)
            self.assertIn('Unresolved: none.', text)

    def test_time_already_used_counts_against_the_budget(self):
        estimate = self.budget_run(1 / 60, lambda: 0)[0]['budget_stop']['estimate_seconds']
        spare = 1
        minutes = (estimate + spare) / 60
        payload, summary = self.budget_run(minutes, lambda: 0)
        self.assertIsNone(payload['budget_stop'])
        self.assertNotIn('Stopped at the time budget', summary)
        ticks = iter([0, 0, 0, 0, 0])
        payload, _ = self.budget_run(minutes, lambda: next(ticks, 2 * spare))
        self.assertEqual(payload['budget_stop']['elapsed_seconds'], 2 * spare)
        self.assertEqual({item['look'] for item in payload['plan']}, {1})

    def test_look_seconds_scales_runs_by_suite_passes_and_seeds_by_both_sides(self):
        def record(wall, *suite_runs):
            return dict(wall_seconds=wall, suites={str(index): dict(run_seconds=runs, runs=[{}] * len(runs))
                                                   for index, runs in enumerate(suite_runs)})
        a_pass, b_pass = (4 + 6) / 2 + (2 + 2) / 2, 1 + 2
        a_wall, b_wall = 30 + 2 * a_pass, 50 + b_pass
        records = dict(a=[record(a_wall, [4, 6], [2, 2])], b=[record(b_wall, [1], [2])])
        overhead = ((a_wall - 2 * a_pass) + (b_wall - b_pass)) / 2
        suite_pass = (a_pass + b_pass) / 2
        for seeds, runs in ((1, 1), (3, 2)):
            self.assertEqual(tool.look_seconds(records, seeds, runs), 2 * seeds * (overhead + runs * suite_pass))
        coarse = dict(a=[record(0, [4, 6], [2, 2])], b=[record(0, [1], [2])])
        self.assertEqual(tool.look_seconds(coarse, 1, 1), 2 * suite_pass)

    def test_a_budget_that_is_not_positive_is_refused(self):
        for minutes in ("0", "-1", "nan"):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                tool.main(["--validate-plan", "--suite", "suite", "-", "-", "--max-minutes", minutes])

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

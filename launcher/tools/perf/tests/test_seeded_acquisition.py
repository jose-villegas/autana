"""Device filter limits, acquisition completeness, and bounded process lifetime."""
import contextlib
import io
import itertools
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from test_perf_compare import FakeAutana, arguments
import layout_measure as capture
import perf_compare as tool
import seed_statistics as stats


class AcquisitionTests(unittest.TestCase):
    def test_shortest_unique_filters_and_batches(self):
        names = ['prefix_long_test_alpha', 'prefix_long_test_beta', 'prefix_long_test_gamma']
        owners = {'s/' + name: ('s', name) for name in names}
        selected = tool.selected_suites([('s', '-', '-')], set(owners), owners,
                                       (3, 2), {'s': names})
        self.assertEqual(len(selected), 2)
        patterns = [p for _, filters, _ in selected for p in filters.split(',')]
        self.assertEqual(len(patterns), 3)
        for name in names:
            pattern = next(p for p in patterns if p in name)
            self.assertEqual(sum(pattern in other for other in names), 1)
            self.assertLessEqual(len(pattern), 3)
            self.assertFalse(any(sum(name[i:i+n] in other for other in names) == 1
                                 for n in range(1, len(pattern)) for i in range(len(name)-n+1)))
        self.assertEqual(tool.selected_suites([('s', 'prefix', '-')], set(owners), owners,
                                             (1, 2), {'s': ['a', 'aa', 'aaa']}), [('s', 'prefix', '-')])
        self.assertEqual(tool.selected_suites([('s', '-', '-')], {'s/row'},
                                             {'s/row': ('s', 'alpha')}, (3, 2),
                                             {'s': ['alpha', 'ab']}), [('s', 'l', '-')])

    def test_filter_limits_preflight_both_projects(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            for project in (args.project_a, args.project_b):
                header = project / 'launcher/test/suites.h'
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#define SUITE_FILTER_LEN 4\n#define SUITE_FILTER_MAX 2\n')
            for filters in ('abcd', 'a,b,c'):
                args.suite = [('suite', filters, '-')]
                fake = FakeAutana(root)
                with self.assertRaisesRegex(ValueError, 'filter'):
                    tool.measure(args, fake)
                self.assertEqual(fake.calls, [])

    def test_status_after_observes_before_gates(self):
        for phase in ('before', 'after'):
            with tempfile.TemporaryDirectory() as root:
                args = arguments(root)
                args.project, args.runs = args.project_a, 1
                fake = FakeAutana(root)
                def runner(command, log, timeout):
                    if 'status' in command and phase in str(log.name):
                        return 7, [(0, 'status unavailable')], 0
                    return fake(command, log, timeout)
                if phase == 'before':
                    with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                        tool.measure(args, runner)
                    plan = json.loads((args.out / 'plan.json').read_text())
                    self.assertIn('status', plan['plan'][0]['error'])
                    self.assertFalse(fake.calls)
                else:
                    record = capture.run_flash(args, 3, runner)
                    self.assertTrue(record['suites'])
                    self.assertIn('exit code: 7', next(args.out.glob('*status_after.log')).read_text())

    def test_device_refusal_in_plan(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            def runner(command, log, timeout):
                if 'status' in command:
                    return 0, [], 0
                return 1, [(0, 'SUITE_FILTER_REFUSED: too long')], 0
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                tool.measure(args, runner)
            plan = json.loads((args.out / 'plan.json').read_text())
            self.assertIn('SUITE_FILTER_REFUSED: too long', plan['plan'][0]['error'])

    def test_device_refusal_from_raw_capture(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            report = Path(root) / 'refused.md'
            report.write_text('- Ended: filter error\n')
            report.with_suffix('.log').write_text('SUITE_FILTER_REFUSED pattern=long_name\n')
            def runner(command, log, timeout):
                if 'status' in command:
                    return 0, [], 0
                return 1, [(0, 'batch: suite run 1/2'), (1, 'report: ' + str(report))], 1
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                tool.measure(args, runner)
            plan = json.loads((args.out / 'plan.json').read_text())
            self.assertIn('SUITE_FILTER_REFUSED pattern=long_name', plan['plan'][0]['error'])

    def test_failed_side_is_not_removed_and_empty_is_incomplete(self):
        for mode in ('failed', 'empty'):
            with tempfile.TemporaryDirectory() as root:
                args = arguments(root, 4)
                fake = FakeAutana(root)
                def runner(command, log, timeout):
                    if 'status' not in command and mode == 'failed' and command[command.index('--project')+1] == str(args.project_b):
                        raise RuntimeError('failed side')
                    code, lines, wall = fake(command, log, timeout)
                    if mode == 'empty':
                        for _, line in lines:
                            if line.startswith('report: '):
                                Path(line[8:]).with_suffix('.log').write_text('')
                    return code, lines, wall
                with contextlib.redirect_stdout(io.StringIO()):
                    try:
                        tool.measure(args, runner)
                    except RuntimeError:
                        pass
                payload = json.loads((args.out / 'comparison.json').read_text())
                self.assertTrue(payload['incomplete'])
                self.assertTrue(all(row['verdict'] == 'not measured' for row in payload['rows'].values()))

    def test_table_deadline(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                capture.make_table('python3 -c "import time; time.sleep(30)"', Path(root)/'c', Path(root)/'t', timeout=.1)

    def test_exited_parent_grandchild_is_stopped(self):
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / 'alive'
            child = f'import time; from pathlib import Path; time.sleep(3); Path({str(marker)!r}).touch(); time.sleep(20)'
            parent = f'import subprocess,sys; subprocess.Popen([sys.executable,"-c",{child!r}])'
            started = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                capture.run_stamped([sys.executable, '-c', parent], io.StringIO(), timeout=.3)
            self.assertLess(time.monotonic() - started, 3)
            time.sleep(4)
            self.assertFalse(marker.exists())

    def test_final_wait_does_not_hide_original_exception(self):
        import process_tree
        process = unittest.mock.Mock(pid=123)
        process.wait.side_effect = subprocess.TimeoutExpired('capture', 10)
        with patch.object(process_tree, 'terminate_tree'), patch.object(process_tree, 'close_process_tree'):
            process_tree.stop_process_tree(process)

    def test_unequal_permutation_and_boundaries(self):
        self.assertAlmostEqual(stats.permutation([1]*3, [2]*4, .05, random.Random(1)), 1/35)
        for k in range(2, 8):
            self.assertEqual(stats.minimum_seeds(2/math.comb(2*k, k)), k)
        first = stats.minimum_seeds(.05)
        self.assertEqual(tool.schedule(first, .05), [first])
        with self.assertRaises(ValueError):
            tool.schedule(first-1, .05)
        self.assertEqual(stats.compare({'r': [0]}, {})['r']['verdict'], 'not measured')

    def test_difference_family_boundary_after_holm(self):
        alpha = .00625
        for adjusted, expected in ((alpha, 'regressed'), (1.5*alpha, 'inconclusive')):
            row = dict(a=100, b=110, ratio=1.1, log_difference=.1, interval=None,
                       permutation=0, p=adjusted/2, equivalence=1)
            with patch.object(stats, 'row_test', side_effect=lambda *a: row.copy()):
                result = stats.compare({'r': [100]*4, 's': [100]*4}, {'r': [110]*4, 's': [110]*4}, alpha=alpha)
            self.assertEqual(result['r']['verdict'], expected)

    def test_runs_clamp_and_zero_noise(self):
        records = [{'flash_seconds': 1, 'suites': {'s': {'run_seconds': [1]}}}]
        for optimum, expected in ((100, tool.MAX_RUNS), (0, 1), (None, 1)):
            estimate = dict(optimum_runs=optimum, sigma_run=0, sigma_flash=.01)
            with patch.object(tool, 'analyse', return_value=estimate):
                result = tool.recommendations(records, {'r': [[1, 1], [1, 1]]}, .01)
            self.assertEqual(result['r']['recommended_runs'], expected)

    def test_two_sided_interval_confidence(self):
        with tempfile.TemporaryDirectory() as root:
            with contextlib.redirect_stdout(io.StringIO()) as output:
                payload = tool.measure(arguments(root), FakeAutana(root))
            self.assertIn(f"{100*(1-payload['look_alpha']):g}% two-sided", output.getvalue())
        with patch.object(stats, 't_quantile', return_value=1) as quantile:
            stats.row_test([100, 101, 102], [110, 111, 112], .01, .02, random.Random(1))
        self.assertEqual(quantile.call_args.args[0], .99)

    def test_conditional_null_false_positive_rate(self):
        values = [100, 101, 102, 103, 104, 105, 106, 107]
        false = 0
        for indices in itertools.combinations(range(8), 4):
            a = [values[i] for i in indices]
            b = [value for i, value in enumerate(values) if i not in indices]
            row = stats.compare({'r': a}, {'r': b}, alpha=.025, permutation_alpha=.05)['r']
            false += row['verdict'] in ('regressed', 'improved')
        self.assertEqual(false, 2)
        self.assertLessEqual(false / math.comb(8, 4), .05)

    def test_second_failure_stops_inside_pairs_and_keeps_look_one(self):
        for fail_at in (9, 10):
            with tempfile.TemporaryDirectory() as root:
                args = arguments(root, 16)
                fake = FakeAutana(root, noisy=True)
                attempts = 0
                def runner(command, log, timeout):
                    nonlocal attempts
                    if 'status' not in command:
                        attempts += 1
                        if attempts >= fail_at:
                            raise RuntimeError('failed extra seed')
                    return fake(command, log, timeout)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, 'two consecutive'):
                    tool.measure(args, runner)
                self.assertEqual(attempts, fail_at + 1)
                result = json.loads((args.out / 'comparison.json').read_text())
                self.assertTrue(result['incomplete'])
                self.assertEqual(result['rows']['suite/quiet']['look'], 1)
                self.assertEqual(result['rows']['suite/quiet']['verdict'], 'no change')
                self.assertIn('Incomplete', (args.out / 'summary.md').read_text())

    def test_seed_recommendation_alpha_shrinks_with_family_for_both_choices(self):
        for family in (1, 2):
            with tempfile.TemporaryDirectory() as root:
                args = arguments(root, 16)
                fake = FakeAutana(root, noisy=True)
                calls = []
                def runner(command, log, timeout):
                    code, lines, wall = fake(command, log, timeout)
                    if family == 1:
                        for _, line in lines:
                            if line.startswith('report: '):
                                path = Path(line[8:]).with_suffix('.log')
                                path.write_text('\n'.join(line for line in path.read_text().splitlines() if 'quiet' not in line))
                    return code, lines, wall
                def required(*a, alpha):
                    calls.append((len(fake.calls), alpha))
                    return 2
                estimate = dict(optimum_runs=3, sigma_run=.01, sigma_flash=.02)
                with patch.object(tool, 'required_seeds', side_effect=required), \
                        patch.object(tool, 'analyse', return_value=estimate), \
                        contextlib.redirect_stdout(io.StringIO()):
                    tool.measure(args, runner)
                share = .05 / 3 / 2 / family
                self.assertTrue(all(alpha == share for _, alpha in calls))
                self.assertEqual(sum(count == 8 for count, _ in calls), 4 * family)

    def test_split_filters_reuse_build_and_merge_seed_runs(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.project, args.runs = args.project_a, 2
            args.suite = [('suite', 'q', '-'), ('suite', 'h', '-')]
            fake = FakeAutana(root)
            def runner(command, log, timeout):
                if 'suite' in command and '--layout-seed' not in command:
                    self.assertIn('--expect-build-id', command)
                    self.assertNotIn('--flash', command)
                    command = command + ['--layout-seed', '3']
                return fake(command, log, timeout)
            record = capture.run_flash(args, 3, runner)
            self.assertEqual(sum('--flash' in call for call in fake.calls), 1)
            rows, _, _ = tool.observations([record])
            self.assertEqual(rows, {'suite/quiet': [[10000, 10000]], 'suite/heavy': [[10000, 10000]]})

    def test_interruption_kills_descendants(self):
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / 'alive'
            child = f'import time; from pathlib import Path; time.sleep(3); Path({str(marker)!r}).touch(); time.sleep(20)'
            parent = f'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",{child!r}]); print("ready",flush=True); time.sleep(20)'
            class Interrupted(io.StringIO):
                def write(self, value):
                    raise KeyboardInterrupt('capture interrupted')
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(KeyboardInterrupt, 'interrupted'):
                capture.run_stamped([sys.executable, '-c', parent], Interrupted(), timeout=3)
            time.sleep(4)
            self.assertFalse(marker.exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX process groups')
    def test_sigterm_ignoring_child_is_killed(self):
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / 'alive'
            child = f'import signal,time; from pathlib import Path; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(3); Path({str(marker)!r}).touch(); time.sleep(20)'
            parent = f'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",{child!r}]); time.sleep(20)'
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                capture.run_stamped([sys.executable, '-c', parent], io.StringIO(), timeout=.3)
            self.assertFalse(marker.exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX escaped session')
    def test_escaped_pipe_holder_has_bounded_wall_time(self):
        import signal
        with tempfile.TemporaryDirectory() as root:
            pid_file = Path(root) / 'holder.pid'
            parent = ('import subprocess,sys; from pathlib import Path; '
                      'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(6)"],'
                      f'start_new_session=True); Path({str(pid_file)!r}).write_text(str(child.pid))')
            started = time.monotonic()
            try:
                with self.assertRaisesRegex(RuntimeError, 'timed out'):
                    capture.run_stamped([sys.executable, '-c', parent], io.StringIO(), timeout=.1)
                self.assertLess(time.monotonic() - started, 3)
            finally:
                if pid_file.exists():
                    try:
                        os.kill(int(pid_file.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass

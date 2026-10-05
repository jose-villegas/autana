"""Capture ownership, filter scope, and measured absence without hardware."""
import contextlib
import copy
import io
import math
import json
import subprocess
import os
from pathlib import Path
import random
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from test_perf_compare import FakeAutana, arguments, FIXTURES
import layout_measure as capture
import perf_compare as tool
import seed_statistics as stats


class CaptureRulesTests(unittest.TestCase):
    def test_real_device_results(self):
        path = FIXTURES / 'device_capture.log'
        name = 'test_a_full_size_step_fits_in_the_frame_budget'
        self.assertIn(name, capture.capture_tests(path))
        self.assertEqual(capture.capture_metadata(path, {name: 1})[0][name], name)

    def test_filtered_scope_and_unknown_owner(self):
        suites = [('s', 'quiet,heavy', '-')]
        owners = {'s/r': ('s', 'test_heavy')}
        listed = {'s': ['test_quiet', 'test_heavy', 'test_change_clocks']}
        self.assertEqual(tool.selected_suites(suites, {'s/r'}, owners, (23, 4), listed),
                         [('s', 'heavy', '-')])
        self.assertEqual(tool.selected_suites(suites, {'s/new'}, {}, (23, 4), listed), suites)
        self.assertEqual(tool.selected_suites(suites, {'s/r'}, {'s/r': ('s', 'test_outside')},
                                             (23, 4), listed), suites)
        self.assertEqual(tool.selected_suites([('s', '-', '-')], {'s/r'}, owners, (23, 4), {}),
                         [('s', '-', '-')])

    def test_unfiltered_inventory_uses_all_result_statuses(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'capture.log'
            path.write_text('SUITE_TEST name=test_heavy selected=1\n'
                            ':1:test_failed:FAIL: failed\n:1:test_ignored:IGNORE: ignored\n:1:test_heavy:PASS\n')
            self.assertEqual(capture.capture_tests(path), ['test_failed', 'test_ignored', 'test_heavy'])

    def test_instruction_pair_uses_empty_file_result(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'capture.log'
            path.write_text('I perf: row both cores: mean 10us\n'
                            'I xtperf: scene=row event=insn cycles_per_step=8 value_per_step=20 steps=1\n'
                            ':4670:test_owner:PASS\n')
            self.assertEqual(capture.capture_metadata(path, {'row': 10}),
                             ({'row': 'test_owner'}, {'row': 20}))

    def test_absence_conditions_independently(self):
        attempts = [dict(side='a', suites=[('s', '-', '-')])]
        records = [dict(suites={'s': dict(runs=[dict(rows={}, listed_tests=['test_owner'])])})]
        self.assertTrue(tool.absent_by_measurement('s/row', 'test_owner', attempts, records, 'a'))
        cases = [(None, attempts, records), ('test_owner', [], records),
                 ('test_owner', [dict(attempts[0], error='failed')], records),
                 ('test_owner', attempts, []), ('test_owner', attempts, [dict(suites={})])]
        for key, value in [('listed_tests', []), ('listed_tests', ['other']), ('rows', {'row': 1})]:
            changed = copy.deepcopy(records)
            changed[0]['suites']['s']['runs'][0][key] = value
            cases.append(('test_owner', attempts, changed))
        changed = copy.deepcopy(records)
        changed[0]['suites']['s']['runs'] = []
        cases.append(('test_owner', attempts, changed))
        for owner, plan, data in cases:
            with self.subTest(owner=owner, plan=plan, data=data):
                self.assertFalse(tool.absent_by_measurement('s/row', owner, plan, data, 'a'))

    def test_added_row_and_positive_removal_in_existing_test(self):
        for missing in ('a', 'b'):
            with tempfile.TemporaryDirectory() as root:
                args = arguments(root, 4)
                fake = FakeAutana(root)
                def runner(command, log, timeout):
                    code, lines, wall = fake(command, log, timeout)
                    if 'suite' in command and Path(command[command.index('--project')+1]).name == missing:
                        for _, line in lines:
                            if line.startswith('report: '):
                                path = Path(line[8:]).with_suffix('.log')
                                path.write_text('\n'.join(part for part in path.read_text().splitlines()
                                                          if not ('quiet' in part and ('both cores' in part or 'xtperf' in part))))
                    return code, lines, wall
                with contextlib.redirect_stdout(io.StringIO()):
                    result = tool.measure(args, runner)
                self.assertEqual(result['rows']['suite/quiet']['verdict'], 'added' if missing == 'a' else 'removed')
                self.assertFalse(result['incomplete'])

    @patch.object(stats, 'permutation_samples', new=lambda alpha: 199)
    def test_unfiltered_extra_seeds_use_unique_bounded_patterns(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, 8)
            fake = FakeAutana(root, noisy=True)
            with patch.object(tool, 'required_seeds', return_value=2), contextlib.redirect_stdout(io.StringIO()):
                result = tool.measure(args, fake)
            extra = [item for item in result['plan'] if item['look'] > 1]
            self.assertTrue(extra)
            for item in extra:
                for _, filters, _ in item['suites']:
                    self.assertNotEqual(filters, '-')
                    patterns = filters.split(',')
                    self.assertEqual(len(patterns), 1)
                    self.assertIn(patterns[0], 'test_heavy')
                    self.assertNotIn(patterns[0], 'test_quiet')
                    self.assertLessEqual(len(patterns[0]), capture.filter_limits(args.project_a)[0])

    @patch.object(stats, 'permutation_samples', new=lambda alpha: 199)
    def test_no_unique_substring_still_remeasures_row(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, 8)
            fake = FakeAutana(root, noisy=True)
            def runner(command, log, timeout):
                code, lines, wall = fake(command, log, timeout)
                for _, line in lines:
                    if line.startswith('report: '):
                        path = Path(line[8:]).with_suffix('.log')
                        with path.open('a') as output:
                            output.write(':1:test_heavy_extra:IGNORE: unavailable\n')
                return code, lines, wall
            with patch.object(tool, 'required_seeds', return_value=2), contextlib.redirect_stdout(io.StringIO()):
                result = tool.measure(args, runner)
            extra = [item for item in result['plan'] if item['look'] > 1]
            self.assertTrue(extra)
            self.assertTrue(all(item['suites'] == args.suite for item in extra))
            self.assertGreater(sum(item['side'] == 'a' and not item.get('error') for item in result['plan']), result['first_pass'])

    def test_no_unique_bounded_pattern_keeps_unfiltered_scope(self):
        self.assertEqual(tool.selected_suites([('s', '-', '-')], {'s/r'},
            {'s/r': ('s', 'test_a')}, (1, 1), {'s': ['test_a', 'test_aa']}), [('s', '-', '-')])

    def test_timeout_cleanup_cannot_replace_capture_error(self):
        stop = capture.stop_process_tree
        def timed_out_stop(process):
            stop(process)
            raise subprocess.TimeoutExpired('taskkill', 10)
        with patch.object(capture, 'stop_process_tree', side_effect=timed_out_stop), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, 'capture timed out'):
            capture.run_stamped([sys.executable, '-c', 'import time; time.sleep(.3)'], io.StringIO(), timeout=.05)

    def test_chatty_command_stops_at_deadline(self):
        command = 'for i in range(1000): print(i,flush=True)'
        class SlowLog(io.StringIO):
            def write(self, text):
                time.sleep(.01)
                return super().write(text)
        started = time.monotonic()
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, 'timed out'):
            capture.run_stamped([sys.executable, '-c', command], SlowLog(), timeout=.2)
        self.assertLess(time.monotonic() - started, 1)

    def test_filter_boundary_and_different_project_limits(self):
        capture.validate_filters([('s', 'abc,def', '-')], (3, 2))
        for filters in ('abcd', 'a,b,c', ''):
            with self.assertRaises(ValueError):
                capture.validate_filters([('s', filters, '-')], (3, 2))
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            (args.project_a / 'launcher/test/suites.h').write_text('#define SUITE_FILTER_LEN 5\n#define SUITE_FILTER_MAX 3\n')
            (args.project_b / 'launcher/test/suites.h').write_text('#define SUITE_FILTER_LEN 4\n#define SUITE_FILTER_MAX 2\n')
            self.assertEqual(capture.filter_limits(args.project_a), (4, 3))
            self.assertEqual(capture.filter_limits(args.project_b), (3, 2))
            args.suite = [('suite', 'abcd', '-')]
            fake = FakeAutana(root)
            with self.assertRaises(ValueError):
                tool.measure(args, fake)
            self.assertFalse(fake.calls)

    @patch.object(stats, 'permutation_samples', new=lambda alpha: 199)
    def test_extra_seed_limits_and_unmatchable_owner_are_remeasured(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, 8)
            args.suite = [('suite', 'heavy', '-')]
            for project, width, count in ((args.project_a, 6, 1), (args.project_b, 8, 2)):
                (project / 'launcher/test/suites.h').write_text(
                    f'#define SUITE_FILTER_LEN {width}\n#define SUITE_FILTER_MAX {count}\n')
            metadata = capture.capture_metadata
            def unknown_owner(path, rows):
                owners, instructions = metadata(path, rows)
                return {row: 'test_outside' for row in owners}, instructions
            with patch.object(capture, 'capture_metadata', side_effect=unknown_owner), \
                    patch.object(tool, 'selected_suites', wraps=tool.selected_suites) as selected, \
                    patch.object(tool, 'required_seeds', return_value=2), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = tool.measure(args, FakeAutana(root, noisy=True))
            extra = [item for item in result['plan'] if item['look'] > 1]
            self.assertTrue(extra)
            self.assertTrue(all(item['suites'] == args.suite for item in extra))
            self.assertEqual({call.args[3] for call in selected.call_args_list}, {(5, 1), (7, 2)})

    def test_status_after_exception_and_table_timeout(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.project, args.runs, args.timeout = args.project_a, 1, 7
            args.suite = [('suite', '-', 'table')]
            fake = FakeAutana(root)
            def runner(command, log, timeout):
                if 'status' in command and 'after' in str(log.name):
                    raise RuntimeError('status timed out')
                return fake(command, log, timeout)
            with patch.object(capture, 'make_table', side_effect=lambda template, path, *rest: path) as table:
                capture.run_flash(args, 3, runner)
            self.assertEqual(table.call_args.args[-1], 7)
            self.assertIn('status timed out', next(args.out.glob('*status_after.log')).read_text())

    def test_split_invocations_keep_tables_seconds_and_metadata(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root)
            args.project, args.runs = args.project_a, 2
            args.suite = [('suite', 'quiet', 'table'), ('suite', 'heavy', 'table')]
            fake = FakeAutana(root)
            def runner(command, log, timeout):
                if 'suite' in command and '--layout-seed' not in command:
                    command = command + ['--layout-seed', '3']
                return fake(command, log, timeout)
            def table(template, path, output, project, timeout):
                output.write_bytes(path.read_bytes())
                return output
            with patch.object(capture, 'make_table', side_effect=table):
                record = capture.run_flash(args, 3, runner)
            entry = record['suites']['suite']
            self.assertEqual(entry['run_seconds'], [20, 18])
            tables = [table for run in entry['runs'] for table in run['tables']]
            self.assertEqual(len(set(tables)), 4)
            for run in entry['runs']:
                self.assertEqual(set(run['listed_tests']), {'test_quiet', 'test_heavy'})
                self.assertEqual(run['owners'], {'quiet': 'test_quiet', 'heavy': 'test_heavy'})

    def test_stop_does_not_flash_if_runner_would_recover(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, 32)
            fake = FakeAutana(root, noisy=True)
            attempts = []
            def runner(command, log, timeout):
                if 'suite' in command:
                    attempts.append(command)
                    if 9 <= len(attempts) <= 10:
                        raise RuntimeError('two fail after inconclusive first pass')
                return fake(command, log, timeout)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                tool.measure(args, runner)
            self.assertEqual(len(attempts), 10)
            payload = json.loads((args.out / 'comparison.json').read_text())
            self.assertEqual(payload['rows']['suite/heavy']['verdict'], 'inconclusive')
            self.assertTrue(payload['incomplete'])

    def test_exact_aa_family_calibration_uses_production_alpha(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, 32)
            with patch.object(tool, 'compare', wraps=stats.compare) as decision, contextlib.redirect_stdout(io.StringIO()):
                tool.measure(args, FakeAutana(root))
            alpha = decision.call_args.args[3]
        # Uniform null p-values isolate the decision budget from statistic estimation.
        atoms = 1600
        false = []
        for share in (alpha, 2 * alpha):
            count = 0
            for index in range(atoms):
                row = dict(a=100, b=110, ratio=1.1, interval=None, log_difference=.1,
                           p=(index + .5) / atoms, equivalence=1, permutation=0)
                with patch.object(stats, 'row_test', return_value=row):
                    verdict = stats.compare({'r': [100]*4}, {'r': [110]*4}, alpha=share,
                                            permutation_alpha=.05)['r']['verdict']
                count += verdict in ('regressed', 'improved')
            false.append(count / atoms)
        budget = args.alpha / len(tool.schedule(args.max_seeds, args.alpha)) / 2
        self.assertAlmostEqual(false[0], budget)
        self.assertAlmostEqual(false[1], 2 * budget)
        self.assertLess(false[0], false[1])

    def test_absence_requires_every_run_and_ignores_other_suites(self):
        plan = [dict(side='a', suites=[('s', '-', '-')]),
                dict(side='a', suites=[('other', '-', '-')], error='failed')]
        absent = dict(rows={}, listed_tests=['owner'])
        records = [dict(suites={'s': dict(runs=[absent, dict(absent)])}), dict(suites={})]
        self.assertTrue(tool.absent_by_measurement('s/r', 'owner', plan, records, 'a'))
        for changed in (dict(absent, listed_tests=[]), dict(absent, rows={'r': 10})):
            for index in (0, 1):
                runs = [dict(absent), dict(absent)]
                runs[index] = changed
                self.assertFalse(tool.absent_by_measurement('s/r', 'owner', plan,
                    [dict(suites={'s': dict(runs=runs)}), dict(suites={})], 'a'))

    def test_not_measured_alone_marks_incomplete(self):
        for missing in (False, True):
            with tempfile.TemporaryDirectory() as root:
                args = arguments(root, 4)
                fake = FakeAutana(root)
                metadata = capture.capture_metadata
                def without_owner(path, rows):
                    owners, instructions = metadata(path, rows)
                    return {row: None for row in owners}, instructions
                def runner(command, log, timeout):
                    code, lines, wall = fake(command, log, timeout)
                    if missing and 'suite' in command and Path(command[command.index('--project')+1]).name == 'a':
                        for _, line in lines:
                            if line.startswith('report: '):
                                path = Path(line[8:]).with_suffix('.log')
                                path.write_text('\n'.join(part for part in path.read_text().splitlines()
                                                          if not ('quiet' in part and 'both cores' in part)))
                    return code, lines, wall
                with patch.object(capture, 'capture_metadata', side_effect=without_owner), contextlib.redirect_stdout(io.StringIO()):
                    result = tool.measure(args, runner)
                self.assertEqual(result['incomplete'], missing)
                self.assertEqual(result['rows']['suite/heavy']['verdict'], 'no change')
                self.assertEqual('Incomplete:' in (args.out / 'summary.md').read_text(), missing)

    @unittest.skipUnless(os.name == 'nt', 'Windows taskkill fallback')
    def test_real_job_failure_kills_pipe_holding_child(self):
        import process_tree
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / 'alive'
            child = f"import time; from pathlib import Path; time.sleep(1); Path({str(marker)!r}).touch(); time.sleep(10)"
            parent = f"import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',{child!r}]); time.sleep(10)"
            with patch.object(process_tree, 'create_kill_on_close_job', return_value=None), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaisesRegex(RuntimeError, 'capture timed out'):
                capture.run_stamped([sys.executable, '-c', parent], io.StringIO(), timeout=.3)
            time.sleep(1.1)
            self.assertFalse(marker.exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows job closes on normal completion')
    def test_normal_completion_kills_lingering_grandchild(self):
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / 'alive'
            grandchild = f"import time; from pathlib import Path; time.sleep(1); Path({str(marker)!r}).touch(); time.sleep(10)"
            child = f"import subprocess,sys; subprocess.Popen([sys.executable,'-c',{grandchild!r}], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)"
            parent = f"import subprocess,sys; subprocess.run([sys.executable,'-c',{child!r}])"
            with contextlib.redirect_stdout(io.StringIO()):
                code, _, _ = capture.run_stamped([sys.executable, '-c', parent], io.StringIO(), timeout=2)
            self.assertEqual(code, 0)
            time.sleep(1.1)
            self.assertFalse(marker.exists())

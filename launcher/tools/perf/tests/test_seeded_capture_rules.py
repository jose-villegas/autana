"""Capture ownership, filter scope, and measured absence without hardware."""
import contextlib
import copy
import io
import math
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

    def test_full_inventory_contains_unselected_tests(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'capture.log'
            path.write_text('SUITE_TEST name=test_heavy selected=1\n'
                            'SUITE_TEST name=test_change_clocks selected=0\n:1:test_heavy:PASS\n')
            self.assertEqual(capture.capture_inventory(path), ['test_heavy', 'test_change_clocks'])
            self.assertEqual(capture.capture_tests(path), ['test_heavy'])

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

    def test_chatty_command_stops_at_deadline(self):
        command = 'import time\nfor i in range(200):\n print(i,flush=True); time.sleep(.0001)'
        class SlowLog(io.StringIO):
            def write(self, text):
                time.sleep(.002)
                return super().write(text)
        started = time.monotonic()
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, 'timed out'):
            capture.run_stamped([sys.executable, '-c', command], SlowLog(), timeout=.05)
        self.assertLess(time.monotonic() - started, 3)

    def test_filter_boundary_and_different_project_limits(self):
        capture.validate_filters([('s', 'abc,def', '-')], (3, 2))
        for filters in ('abcd', 'a,b,c', 'a,,b'):
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

    @unittest.skipUnless(os.name == 'nt', 'Windows job assignment')
    def test_job_failure_degrades_and_assignment_precedes_resume(self):
        import ctypes
        from unittest.mock import Mock
        import process_tree
        for created, assigned in ((None, False), (123, False), (123, True)):
            kernel, process, ntdll = Mock(), Mock(), Mock()
            process._handle = 456
            kernel.AssignProcessToJobObject.return_value = assigned
            ntdll.NtResumeProcess.return_value = 0
            order = []
            kernel.AssignProcessToJobObject.side_effect = lambda *args: order.append('assign') or assigned
            ntdll.NtResumeProcess.side_effect = lambda *args: order.append('resume') or 0
            with patch.object(process_tree, 'windows_job_binding', return_value=kernel), \
                    patch.object(process_tree, 'create_kill_on_close_job', return_value=created), \
                    patch.object(process_tree.subprocess, 'Popen', return_value=process) as popen, \
                    patch.object(ctypes, 'WinDLL', return_value=ntdll), contextlib.redirect_stderr(io.StringIO()):
                self.assertIs(process_tree.launch_process_tree(['command']), process)
                if created:
                    self.assertEqual(order, ['assign', 'resume'])
                    self.assertTrue(popen.call_args.kwargs['creationflags'] & process_tree.CREATE_SUSPENDED)
                    if assigned:
                        self.assertEqual(process._tree_job, 123)
                    else:
                        kernel.CloseHandle.assert_called_once_with(123)
                else:
                    self.assertNotIn('creationflags', popen.call_args.kwargs)

    @unittest.skipUnless(os.name == 'nt', 'Windows job creation')
    def test_shared_job_creation_failure_degrades(self):
        from unittest.mock import Mock
        import process_tree
        for created, configured in ((None, False), (123, False)):
            kernel = Mock()
            kernel.CreateJobObjectW.return_value = created
            kernel.SetInformationJobObject.return_value = configured
            with contextlib.redirect_stderr(io.StringIO()) as log:
                self.assertIsNone(process_tree.create_kill_on_close_job(kernel=kernel))
            self.assertIn('using tree kill', log.getvalue())
            if created:
                kernel.CloseHandle.assert_called_once_with(created)

    def test_stop_does_not_flash_if_runner_would_recover(self):
        with tempfile.TemporaryDirectory() as root:
            args = arguments(root, 32)
            fake = FakeAutana(root)
            attempts = []
            def runner(command, log, timeout):
                if 'suite' in command:
                    attempts.append(command)
                    if len(attempts) <= 2:
                        raise RuntimeError('first two fail')
                return fake(command, log, timeout)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                tool.measure(args, runner)
            self.assertEqual(len(attempts), 2)

    @patch.object(stats, 'permutation_samples', new=lambda alpha: 19)
    def test_production_aa_calibration(self):
        rng = random.Random(51493)
        draws, false = 2000, 0
        # A 3-sigma binomial bound at the allocated one-look difference share.
        alpha = .05 / len(tool.schedule(32, .05)) / 2
        bound = alpha + 3 * (alpha * (1-alpha) / draws) ** .5
        for draw in range(draws):
            a = {'r': [100 * math.exp(rng.gauss(0, .15)) for _ in range(16)]}
            b = {'r': [100 * math.exp(rng.gauss(0, .15)) for _ in range(16)]}
            result = stats.compare(a, b, 1, alpha, draw, permutation_alpha=.05)
            false += result['r']['verdict'] in ('regressed', 'improved')
        print(f'Production A/A false decisions: {false}/{draws} ({false/draws:.4%}); bound {bound:.4%}')
        self.assertLessEqual(false / draws, bound)

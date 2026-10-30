"""board_alarm raises one outage issue once, clears it, and reads recovery evidence from night manifests and
board CI runs, and stuck-job evidence from runs."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
from datetime import datetime, timedelta, timezone
from unittest import mock
from urllib.parse import parse_qs, urlsplit

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import board_alarm as alarm
from device_capture import CAPTURE_COMPLETE, CONSOLE_COMPLETE_LINE, capture_ended_line

NOW = datetime(2026, 10, 24, 8, tzinfo=timezone.utc)


class Repository:
    def __init__(self):
        self.calls = []
        self.issues = []
        self.runs = {}
        self.jobs = {}
        self.active = True

    def __call__(self, method, path, body=None, **kwargs):
        self.calls.append((method, path, body))
        route = path.split('?')[0]
        if route == '/issues':
            if method == 'GET':
                return self.issues
            self.issues = [{'number': 7}]
            return self.issues[0]
        if route == '/actions/workflows/' + alarm.NIGHTLY_WORKFLOW:
            return {'state': 'active' if self.active else 'disabled_manually'}
        if route.endswith('/runs'):
            return {'workflow_runs': self.runs.get(route, [])}
        if route.endswith('/jobs'):
            return {'jobs': self.jobs.get(route, [])}
        return {}


def night(directory, board, selftest='passed', stop_reason=None, ended=NOW, steps=()):
    """A lane manifest on perf-data, as scoped_chain.py writes it, with steps of these outcomes."""
    path = Path(directory) / 'scoped' / board / '2026-10-23-1900' / 'night.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(dict(board=board, stop_reason=stop_reason, selftest=dict(outcome=selftest),
                                    ended=ended and ended.isoformat(),
                                    steps=[dict(outcome=outcome) for outcome in steps])))


def board_job(status='completed', age=timedelta(), conclusion='success'):
    return {'name': 'board', 'labels': [alarm.BOARD_LABEL], 'status': status,
            'conclusion': conclusion, 'created_at': (NOW - age).isoformat(),
            'completed_at': NOW.isoformat(), 'html_url': 'https://logs/board'}


class BoardAlarmTests(unittest.TestCase):
    def test_raise_creates_once_then_comments(self):
        request = Repository()
        with mock.patch.dict(os.environ, GITHUB_WORKFLOW='Board CI', GITHUB_JOB='board'):
            alarm.raise_alarm('broken', 'https://run', request)
            alarm.raise_alarm('still broken', 'https://run2', request)
        writes = [item for item in request.calls if item[0] == 'POST']
        issues = [item for item in writes if item[1] == '/issues']
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0][2]['title'], 'Board CI is down')
        self.assertEqual(issues[0][2]['labels'], [alarm.LABEL])
        self.assertIn('Board CI / board', issues[0][2]['body'])
        self.assertEqual(writes[-1][1], '/issues/7/comments')
        self.assertIn('still broken', writes[-1][2]['body'])
        self.assertIn('https://run2', writes[-1][2]['body'])

    def test_clear_comments_and_closes_or_noops(self):
        for opened in (False, True):
            request = Repository()
            request.issues = [{'number': 7}] if opened else []
            alarm.clear_alarm('https://run', request)
            self.assertEqual(request.calls[1:], [
                ('POST', '/issues/7/comments', {'body': 'Recovered: https://run'}),
                ('PATCH', '/issues/7', {'state': 'closed'})] if opened else [])

    def test_completed_runs_query_has_utc_success_window(self):
        request = Repository()
        alarm.last_board_success(request, NOW.astimezone(timezone(timedelta(hours=2))))
        self.assertEqual(len(request.calls), 1)
        for _, path, _ in request.calls:
            query = parse_qs(urlsplit(path).query)
            self.assertEqual(query['status'], ['completed'])
            self.assertEqual(query['created'], ['>=' + (NOW - alarm.SUCCESS_WINDOW).isoformat().replace('+00:00', 'Z')])

    def test_success_requires_board_success_and_successful_clear_step(self):
        for conclusion, cleared in [('success', 'success'), ('success', 'skipped'),
                                    ('failure', 'success'), ('success', 'failure')]:
            request = Repository()
            request.runs[f'/actions/workflows/{alarm.BOARD_WORKFLOW}/runs'] = [{'id': 1}]
            request.jobs['/actions/runs/1/jobs'] = [board_job(conclusion=conclusion),
                {'name': 'alarm', 'steps': [{'name': alarm.CLEAR_STEP, 'conclusion': cleared}]}]
            expected = NOW if conclusion == cleared == 'success' else None
            self.assertEqual(alarm.last_board_success(request, NOW), expected)

    def test_busy_and_too_late_runs_do_not_recover(self):
        for jobs in ([board_job(), {'name': 'alarm', 'steps': [
                {'name': alarm.CLEAR_STEP, 'conclusion': 'skipped'}]}],
                [{'name': 'nightly', 'labels': [alarm.BOARD_LABEL], 'conclusion': 'success'},
                 {'name': 'alarm', 'steps': []}]):
            request = Repository()
            request.runs[f'/actions/workflows/{alarm.BOARD_WORKFLOW}/runs'] = [{'id': 1}]
            request.jobs['/actions/runs/1/jobs'] = jobs
            self.assertIsNone(alarm.last_board_success(request, NOW))

    def test_nights_that_ran_count_whatever_their_tests_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            night(directory, '2124', selftest='failed', steps=('capture_failed', 'measured', 'build_failed'))
            night(directory, '20E8', ended=NOW - timedelta(hours=1))
            later = NOW + timedelta(hours=1)
            night(directory, 'eeee', steps=('skipped_same_image', 'capture_failed'), ended=later)
            night(directory, 'aaaa', selftest='incomplete', ended=later)
            night(directory, 'bbbb', stop_reason='unplugged', ended=later)
            night(directory, 'cccc', stop_reason='capture_failures', ended=later)
            night(directory, 'dddd', ended=None)
            self.assertEqual(sorted(alarm.night_ends(directory)), [NOW - timedelta(hours=1), NOW])
            self.assertEqual(alarm.last_board_success(Repository(), NOW, alarm.night_ends(directory)), NOW)

    def test_night_red_only_on_ceilings_clears_but_a_missing_board_still_raises(self):
        with tempfile.TemporaryDirectory() as directory:
            # The self-test completed with scenes over their frame-time ceilings: the lane failed, the board ran.
            night(directory, '2124', selftest='failed')
            nights = list(alarm.night_ends(directory))
            self.assertEqual(alarm.check(Repository(), NOW, ['2124'], ['2124'], nights=nights), [])
            self.assertEqual(alarm.check(Repository(), NOW, ['2124', '20E8'], ['2124'], nights=nights),
                             ['Boards missing from USB: 20E8'])
            night(directory, '2124', selftest='incomplete')
            self.assertEqual(alarm.check(Repository(), NOW, ['2124'], ['2124'], nights=alarm.night_ends(directory)),
                             [f'No board success within {alarm.SUCCESS_WINDOW}'])

    def test_stuck_jobs_need_old_queue_and_no_board_in_progress(self):
        for age, running, expected in [(alarm.QUEUED_LIMIT, False, False),
                (alarm.QUEUED_LIMIT + timedelta(seconds=1), False, True),
                (alarm.QUEUED_LIMIT + timedelta(seconds=1), True, False)]:
            request = Repository()
            request.runs['/actions/runs'] = [{'id': 1}]
            request.jobs['/actions/runs/1/jobs'] = [board_job('queued', age)]
            if running:
                request.jobs['/actions/runs/1/jobs'].append(board_job('in_progress'))
            self.assertEqual(bool(alarm.stuck_board_jobs(request, NOW)), expected)

    def test_running_night_suppresses_health_and_probe_wait(self):
        for status in ('completed', 'in_progress'):
            request = Repository()
            request.runs[f'/actions/workflows/{alarm.NIGHTLY_WORKFLOW}/runs'] = [dict(id=1, status='in_progress')]
            job = board_job('in_progress')
            job['name'] = 'nightly'
            request.jobs['/actions/runs/1/jobs'] = [
                {'name': 'marker', 'labels': ['ubuntu-24.04'], 'status': 'completed'},
                {'name': 'wait (1)', 'labels': ['ubuntu-24.04'], 'status': status}, job]
            with mock.patch('sys.stdout', new_callable=io.StringIO) as output:
                self.assertEqual(alarm.check(request, NOW, ['missing'], []), [])
                with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ,
                        GITHUB_OUTPUT=str(Path(directory) / 'output')):
                    self.assertEqual(alarm.probe_devices('/absent', '/absent', request), 0)
                    self.assertEqual((Path(directory) / 'output').read_text(), 'enabled=false\n')
            self.assertIn('::notice::', output.getvalue())
            self.assertIn('night is still running', output.getvalue())

    def test_queued_night_with_offline_runner_reports_stuck_job(self):
        request = Repository()
        request.runs[f'/actions/workflows/{alarm.NIGHTLY_WORKFLOW}/runs'] = [dict(id=1, status='queued')]
        request.runs['/actions/runs'] = [dict(id=1, status='queued')]
        request.jobs['/actions/runs/1/jobs'] = [board_job('queued', alarm.SUCCESS_WINDOW)]
        with mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            problems = alarm.check(request, NOW, [], [])
        self.assertTrue(any('Board job queued beyond' in problem for problem in problems))
        self.assertNotIn('night is still running', output.getvalue())

    def test_night_requires_in_progress_board_job(self):
        for status, labels in [('queued', [alarm.BOARD_LABEL]),
                               ('completed', [alarm.BOARD_LABEL]), ('in_progress', ['ubuntu-latest'])]:
            with self.subTest(status=status, labels=labels):
                request = Repository()
                request.runs[f'/actions/workflows/{alarm.NIGHTLY_WORKFLOW}/runs'] = [dict(id=1, status='in_progress')]
                job = board_job(status)
                job['labels'] = labels
                request.jobs['/actions/runs/1/jobs'] = [job]
                self.assertFalse(alarm.night_running(request))

    def test_hosted_wait_does_not_suppress_board_health(self):
        request = Repository()
        request.runs[f'/actions/workflows/{alarm.NIGHTLY_WORKFLOW}/runs'] = [dict(id=1, status='in_progress')]
        request.jobs['/actions/runs/1/jobs'] = [
            {'name': 'marker', 'labels': ['ubuntu-24.04'], 'status': 'completed'},
            {'name': 'wait (1)', 'labels': ['ubuntu-24.04'], 'status': 'in_progress'},
            {'name': 'nightly', 'labels': [alarm.BOARD_LABEL], 'status': 'queued'}]
        self.assertFalse(alarm.night_running(request))

    def test_missing_boards(self):
        self.assertEqual(alarm.missing_boards(['2124', 'abcd'], ['abcd', 'eeee']), ['2124'])

    def test_unreachable_host_leaves_its_boards_unchecked(self):
        with mock.patch.object(alarm, 'last_board_success', return_value=NOW), \
                mock.patch.object(alarm, 'stuck_board_jobs', return_value=[]), \
                mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            self.assertEqual(alarm.check(Repository(), NOW, ['20E8', '2124'], ['2124'], ['book2']), [])
            self.assertEqual(alarm.check(Repository(), NOW, ['20E8', '2124'], ['2124']),
                             ['Boards missing from USB: 20E8'])
        self.assertIn('not checked: 20E8', output.getvalue())

    def test_check_success_window(self):
        for age, expected in [(alarm.SUCCESS_WINDOW, False),
                              (alarm.SUCCESS_WINDOW + timedelta(seconds=1), True)]:
            with mock.patch.object(alarm, 'last_board_success', return_value=NOW - age), \
                    mock.patch.object(alarm, 'stuck_board_jobs', return_value=[]):
                self.assertEqual(bool(alarm.check(Repository(), NOW, [], [])), expected)

    def test_latest_lanes_and_usb_use_shared_short_id(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for board, night in [('2124', '2026-10-23-1900'), ('abcd', '2026-10-23-1900'),
                                 ('eeee', '2026-10-22-1900')]:
                manifest = root / 'scoped' / board / night / 'night.json'
                manifest.parent.mkdir(parents=True)
                manifest.write_text('{}')
            self.assertEqual(alarm.latest_lanes(root), ['2124', 'abcd'])
            devices = root / 'devices.json'
            devices.write_text(json.dumps({'boards': [
                {'board': 'AA:BB:CC:DD:21:24', 'port': 'COM3'},
                {'board': 'AA:BB:CC:DD:AB:CD', 'port': None}]}))
            self.assertEqual(alarm.read_plugged(devices), (['2124'], []))
            devices.write_text(json.dumps({'boards': [], 'unreachable': ['book2']}))
            self.assertEqual(alarm.read_plugged(devices), ([], ['book2']))

    def test_cli_disabled_needs_no_devices_file(self):
        request = Repository()
        request.active = False
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(alarm, 'call', request):
            self.assertEqual(alarm.main(['check', '--lanes-from', '/absent', '--devices', '/absent',
                                         '--reason-file', str(Path(directory) / 'reason')]), 0)
        self.assertEqual(len(request.calls), 1)

    def test_probe_disabled_marks_workflow_output(self):
        request = Repository()
        request.active = False
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'output'
            with mock.patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
                self.assertEqual(alarm.probe_devices(Path(directory) / 'devices', Path(directory) / 'reason', request), 0)
            self.assertEqual(output.read_text(), 'enabled=false\n')

    def test_probe_wait_is_bounded_and_records_offline_without_cancel(self):
        request = Repository()
        request.jobs['/actions/runs/42/attempts/2/jobs'] = [board_job('queued')]
        clock = mock.Mock(side_effect=[0, 0, alarm.QUEUED_LIMIT.total_seconds()])
        sleep = mock.Mock()
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ,
                GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='2'):
            reason = Path(directory) / 'reason'
            self.assertEqual(alarm.probe_devices('/unused', reason, request, clock, sleep), 1)
            self.assertEqual(reason.read_text(), 'board runner offline or idle')
        sleep.assert_called_once_with(alarm.POLL_SECONDS)
        self.assertFalse(any(method != 'GET' for method, _, _ in request.calls))
        self.assertFalse(any('cancel' in path for _, path, _ in request.calls))

    def test_probe_downloads_only_current_attempt_artifact(self):
        request = Repository()
        job = board_job()
        job['name'] = alarm.PROBE_JOB
        request.jobs['/actions/runs/42/attempts/2/jobs'] = [job]
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr(alarm.DEVICES_FILE, '{"boards": []}')
        def download(method, path, body=None, **kwargs):
            if path.startswith('/actions/runs/42/artifacts'):
                return {'artifacts': [dict(id=1, name=f'{alarm.ARTIFACT_PREFIX}-1', expired=False),
                                      dict(id=2, name=f'{alarm.ARTIFACT_PREFIX}-2', expired=False)]}
            if path == '/actions/artifacts/2/zip':
                self.assertTrue(kwargs['raw'])
                return archive.getvalue()
            return request(method, path, body)
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ,
                GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='2'):
            destination = Path(directory) / 'devices'
            self.assertEqual(alarm.probe_devices(destination, Path(directory) / 'reason', download), 0)
            self.assertEqual(json.loads(destination.read_text()), {'boards': []})

    def test_probe_failed_job_and_missing_artifact_record_reason(self):
        for conclusion in ('failure', 'success'):
            request = Repository()
            job = board_job(conclusion=conclusion)
            job['name'] = alarm.PROBE_JOB
            request.jobs['/actions/runs/42/attempts/2/jobs'] = [job]
            def without_artifact(method, path, body=None):
                if '/artifacts?' in path:
                    return {'artifacts': []}
                return request(method, path, body)
            with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ,
                    GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='2'):
                reason = Path(directory) / 'reason'
                self.assertEqual(alarm.probe_devices('/unused', reason, without_artifact), 1)
                self.assertIn('failure' if conclusion == 'failure' else 'no devices artifact', reason.read_text())
            self.assertFalse(any(method != 'GET' for method, _, _ in request.calls))

    def test_cli_check_exit_codes_and_raise_clear(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ,
                GITHUB_SERVER_URL='https://github.com', GITHUB_REPOSITORY='owner/repo', GITHUB_RUN_ID='42'):
            devices = Path(directory) / 'devices.json'
            devices.write_text('{"boards": []}')
            reason = Path(directory) / 'reason'
            for problems in ([], ['offline', 'missing']):
                with mock.patch.object(alarm, 'nights_enabled', return_value=True) as enabled, \
                        mock.patch.object(alarm, 'check', return_value=problems), \
                        mock.patch.object(alarm, 'raise_alarm') as raised:
                    result = alarm.main(['check', '--lanes-from', directory, '--devices', str(devices),
                                         '--reason-file', str(reason)])
                    self.assertEqual(result, int(bool(problems)))
                    enabled.assert_called_once()
                    raised.assert_not_called()
                    if problems:
                        self.assertEqual(reason.read_text(), 'offline\nmissing')
                    else:
                        self.assertFalse(reason.exists())
            with mock.patch.object(alarm, 'raise_alarm') as raised, \
                    mock.patch.object(alarm, 'clear_alarm') as cleared:
                self.assertEqual(alarm.main(['raise', '--reason', 'failed']), 0)
                self.assertEqual(alarm.main(['clear']), 0)
                raised.assert_called_once()
                cleared.assert_called_once()

    def test_cli_enabled_guard_is_called_once(self):
        request = Repository()
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(alarm, 'call', request):
            devices = Path(directory) / 'devices'
            devices.write_text('{"boards": []}')
            self.assertEqual(alarm.main(['check', '--lanes-from', directory, '--devices', str(devices),
                                         '--reason-file', str(Path(directory) / 'reason')]), 1)
        self.assertEqual(sum(path == '/actions/workflows/' + alarm.NIGHTLY_WORKFLOW
                             for _, path, _ in request.calls), 1)
        self.assertFalse(any(method != 'GET' for method, _, _ in request.calls))

    def test_cli_raise_reads_reason_file_or_falls_back_after_crash(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(alarm, 'run_url', return_value='logs'):
            reason = Path(directory) / 'reason'
            for text in (None, 'offline\nmissing'):
                if text is not None:
                    reason.write_text(text)
                with mock.patch.object(alarm, 'raise_alarm') as raised:
                    self.assertEqual(alarm.main(['raise', '--reason-file', str(reason)]), 0)
                    raised.assert_called_once_with(text or alarm.FAILURE_REASON, 'logs')
            with mock.patch('sys.stderr', new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
                alarm.main(['raise', '--reason', 'failed', '--reason-file', str(reason)])
            self.assertEqual(error.exception.code, 2)

    def test_workflow_has_one_reporting_path_without_shared_check_concurrency(self):
        root = Path(__file__).resolve().parents[3] / '.github/workflows'
        workflow = yaml.safe_load((root / 'board-health.yml').read_text())
        check = workflow['jobs']['check']
        self.assertNotIn('concurrency', check)
        commands = [step for step in check['steps'] if 'board_alarm.py' in step.get('run', '')]
        self.assertEqual(len(commands), 3)
        for step in commands:
            self.assertIn('--reason-file "$RUNNER_TEMP/board-alarm-reason"', step['run'])
        reports = [step for step in commands if 'board_alarm.py raise ' in step['run']]
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]['if'], 'failure()')
        for name in ('nightly-perf.yml', 'board-ci.yml'):
            job = yaml.safe_load((root / name).read_text())['jobs']['alarm']
            self.assertEqual(job['concurrency']['group'], 'board-ci-alarm')
            self.assertEqual(job['timeout-minutes'], 5)

    def test_nightly_alarm_follows_whether_the_boards_ran_not_the_job_result(self):
        workflow = Path(__file__).resolve().parents[3] / '.github/workflows/nightly-perf.yml'
        jobs = yaml.safe_load(workflow.read_text())['jobs']
        self.assertEqual(jobs['nightly']['outputs']['boards_ran'], '${{ steps.lanes.outputs.boards_ran }}')
        conditions = {step['name']: step['if'] for step in jobs['alarm']['steps'] if 'if' in step}
        self.assertEqual(conditions['Raise the board-ci-down alarm'], "needs.nightly.result == 'cancelled' || "
                         "(needs.nightly.result != 'skipped' && needs.nightly.outputs.boards_ran != 'true')")
        self.assertEqual(conditions[alarm.CLEAR_STEP], "(needs.nightly.result == 'success' || "
                         "needs.nightly.result == 'failure') && needs.nightly.outputs.boards_ran == 'true'")

    def test_board_ci_alarm_follows_whether_the_board_ran_not_the_job_result(self):
        workflow = Path(__file__).resolve().parents[3] / '.github/workflows/board-ci.yml'
        jobs = yaml.safe_load(workflow.read_text())['jobs']
        self.assertEqual(jobs['board']['outputs']['boards_ran'], '${{ steps.device.outputs.boards_ran }}')
        self.assertEqual(jobs['board']['outputs']['prepared'], '${{ steps.prepare.outcome }}')
        self.assertIn('board_alarm.py ran --log', next(step['run'] for step in jobs['board']['steps']
                                                       if step.get('id') == 'device'))
        conditions = {step['name']: step['if'] for step in jobs['alarm']['steps'] if 'if' in step}
        # A failed build never reached the board; a board that ran its tests is up whatever they reported.
        self.assertEqual(conditions['Raise the board-ci-down alarm'], "needs.board.result == 'cancelled' || "
                         "(needs.board.result == 'failure' && needs.board.outputs.prepared != 'failure' && "
                         "needs.board.outputs.boards_ran != 'true')")
        self.assertEqual(conditions[alarm.CLEAR_STEP], "(needs.board.result == 'success' || "
                         "needs.board.result == 'failure') && needs.board.outputs.boards_ran == 'true'")

    def test_cli_ran_needs_both_board_steps_stages_whatever_their_tests_reported(self):
        selftest = capture_ended_line('selftest', CAPTURE_COMPLETE)
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'steps.log'
            for lines, ran in [([selftest + '\r', 'selftest results: 2998 PASS, 6 FAIL', CONSOLE_COMPLETE_LINE], 'true'),
                               ([selftest], 'false'), ([CONSOLE_COMPLETE_LINE], 'false'), (None, 'false')]:
                with self.subTest(lines=lines):
                    log.unlink(missing_ok=True)
                    if lines is not None:
                        log.write_text('\n'.join(lines))
                    with mock.patch('sys.stdout', new_callable=io.StringIO) as output:
                        self.assertEqual(alarm.main(['ran', '--log', str(log)]), 0)
                    self.assertEqual(output.getvalue(), f'boards_ran={ran}\n')


if __name__ == '__main__':
    unittest.main()

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

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import board_alarm as alarm

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

    def test_disabled_nights_bypass_every_problem(self):
        request = Repository()
        request.active = False
        with mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            self.assertEqual(alarm.check(request, NOW, ['2124'], []), [])
        self.assertIn('disabled', output.getvalue())
        self.assertEqual(len(request.calls), 1)

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
            request.runs[f'/actions/workflows/{alarm.NIGHTLY_WORKFLOW}/runs'] = [{'id': 1}]
            request.jobs['/actions/runs/1/jobs'] = jobs
            self.assertIsNone(alarm.last_board_success(request, NOW))

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

    def test_missing_boards(self):
        self.assertEqual(alarm.missing_boards(['2124', 'abcd'], ['abcd', 'eeee']), ['2124'])

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
            self.assertEqual(alarm.read_plugged(devices), ['2124'])

    def test_cli_disabled_needs_no_devices_file(self):
        request = Repository()
        request.active = False
        with mock.patch.object(alarm, 'call', request):
            self.assertEqual(alarm.main(['check', '--lanes-from', '/absent', '--devices', '/absent']), 0)

    def test_probe_disabled_marks_workflow_output(self):
        request = Repository()
        request.active = False
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'output'
            with mock.patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
                self.assertEqual(alarm.probe_devices(Path(directory) / 'devices', request), 0)
            self.assertEqual(output.read_text(), 'enabled=false\n')

    def test_probe_wait_is_bounded_and_raises_offline_without_cancel(self):
        request = Repository()
        request.jobs['/actions/runs/42/attempts/2/jobs'] = [board_job('queued')]
        clock = mock.Mock(side_effect=[0, 0, alarm.QUEUED_LIMIT.total_seconds()])
        sleep = mock.Mock()
        with mock.patch.dict(os.environ, GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='2',
                GITHUB_SERVER_URL='https://github.com', GITHUB_REPOSITORY='owner/repo'):
            self.assertEqual(alarm.probe_devices('/unused', request, clock, sleep), 1)
        sleep.assert_called_once_with(alarm.POLL_SECONDS)
        issue = next(body for method, path, body in request.calls if method == 'POST' and path == '/issues')
        self.assertIn('board runner offline or idle', issue['body'])
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
            self.assertEqual(alarm.probe_devices(destination, download), 0)
            self.assertEqual(json.loads(destination.read_text()), {'boards': []})

    def test_probe_failed_job_and_missing_artifact_raise(self):
        for conclusion in ('failure', 'success'):
            request = Repository()
            job = board_job(conclusion=conclusion)
            job['name'] = alarm.PROBE_JOB
            request.jobs['/actions/runs/42/attempts/2/jobs'] = [job]
            def without_artifact(method, path, body=None):
                if '/artifacts?' in path:
                    return {'artifacts': []}
                return request(method, path, body)
            with mock.patch.dict(os.environ, GITHUB_RUN_ID='42', GITHUB_RUN_ATTEMPT='2',
                    GITHUB_SERVER_URL='https://github.com', GITHUB_REPOSITORY='owner/repo'):
                self.assertEqual(alarm.probe_devices('/unused', without_artifact), 1)
            self.assertTrue(request.issues)

    def test_cli_check_exit_codes_and_raise_clear(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ,
                GITHUB_SERVER_URL='https://github.com', GITHUB_REPOSITORY='owner/repo', GITHUB_RUN_ID='42'):
            devices = Path(directory) / 'devices.json'
            devices.write_text('{"boards": []}')
            for problems in ([], ['offline', 'missing']):
                with mock.patch.object(alarm, 'nights_enabled', return_value=True), \
                        mock.patch.object(alarm, 'check', return_value=problems), \
                        mock.patch.object(alarm, 'raise_alarm') as raised:
                    result = alarm.main(['check', '--lanes-from', directory, '--devices', str(devices)])
                    self.assertEqual(result, int(bool(problems)))
                    self.assertEqual(raised.call_count, int(bool(problems)))
                    if problems:
                        self.assertIn('offline', raised.call_args.args[0])
                        self.assertEqual(raised.call_args.args[1], 'https://github.com/owner/repo/actions/runs/42')
            with mock.patch.object(alarm, 'raise_alarm') as raised, \
                    mock.patch.object(alarm, 'clear_alarm') as cleared:
                self.assertEqual(alarm.main(['raise', '--reason', 'failed']), 0)
                self.assertEqual(alarm.main(['clear']), 0)
                raised.assert_called_once()
                cleared.assert_called_once()


if __name__ == '__main__':
    unittest.main()

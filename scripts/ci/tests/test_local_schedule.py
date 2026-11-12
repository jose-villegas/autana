"""Night markers use a local calendar and a UTC cron trigger."""
from datetime import datetime, timedelta, timezone
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError
import unittest
from unittest.mock import Mock, patch

from scripts.lib import local_schedule as schedule


class Clock:
    def __init__(self, hour, minute=0, *, day=21, tzinfo=None):
        self.value = datetime(2026, 10, day, hour, minute, tzinfo=tzinfo)
        self.sleeps = []

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.value += timedelta(seconds=seconds)


class LocalScheduleTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ, NIGHT_ZONE="Europe/Paris")
        environment.start()
        self.addCleanup(environment.stop)

    @property
    def zone(self):
        try:
            return ZoneInfo(os.environ["NIGHT_ZONE"])
        except ZoneInfoNotFoundError:
            self.skipTest("requires test zone timezone data")

    def test_marker_same_date_and_next_date(self):
        for hour, day in ((0, 21), (1, 21), (2, 22)):
            trigger = Clock(hour, tzinfo=self.zone).now()
            self.assertEqual(schedule.next_marker(trigger, '01:00'),
                             datetime(2026, 10, day, 1, tzinfo=self.zone))

    def test_marker_uses_target_date_dst_rules(self):
        trigger = datetime(2026, 10, 24, 23, tzinfo=self.zone).astimezone(timezone.utc)
        marker = schedule.next_marker(trigger, '04:00')
        self.assertEqual(marker.isoformat(), '2026-10-25T04:00:00+01:00')
        same_day = datetime(2026, 10, 25, 1, tzinfo=self.zone)
        self.assertEqual(schedule.next_marker(same_day, '04:00'), marker)

    def test_marker_date_is_independent_of_host_zone(self):
        trigger = datetime(2026, 10, 22, 0, 30, tzinfo=self.zone).astimezone(timezone.utc)
        self.assertEqual(schedule.next_marker(trigger, '01:00').isoformat(),
                         '2026-10-22T01:00:00+02:00')

    def test_marker_validation(self):
        trigger = Clock(0, tzinfo=self.zone).now()
        for value in ('00:00', '05:59', ''):
            with patch.dict(os.environ, NIGHT_START=value):
                self.assertIsNotNone(schedule.next_marker(trigger))
        for value in ('06:00', '23:00', '1:00', '00:60', 'bad'):
            with patch.dict(os.environ, NIGHT_START=value):
                with self.assertRaisesRegex(ValueError, '00:00–05:59'):
                    schedule.next_marker(trigger)

    def test_cutoff_and_end_are_their_setting_on_the_markers_date_and_offset(self):
        marker = datetime(2026, 10, 25, 3, tzinfo=self.zone)
        for name, at in (('BOARD_LAST_START', schedule.night_cutoff), ('BOARD_END', schedule.night_end)):
            default, latest = schedule.LOCAL_TIMES[name]
            for value, hour in (('', int(default[:2])), ('04:30', 4), (f'{latest:02d}:59', latest)):
                with self.subTest(name=name, value=value), patch.dict(os.environ, {name: value}):
                    time = at(marker)
                    self.assertEqual(time.utcoffset(), marker.utcoffset())
                    self.assertEqual((time.date(), time.hour), (marker.date(), hour))
            for value in (f'{latest + 1:02d}:00', '23:00', '8:00', '08:60'):
                with self.subTest(name=name, value=value), patch.dict(os.environ, {name: value}), \
                        self.assertRaisesRegex(ValueError, f'{name} must be HH:MM in 00:00–{latest:02d}:59'):
                    at(marker)
        # A daytime run's cut-off has passed: it starts no lane after its plan.
        afternoon = datetime(2026, 10, 25, 14, tzinfo=self.zone)
        self.assertLess(schedule.night_cutoff(afternoon), afternoon)

    def test_invalid_night_zone_raises(self):
        trigger = Clock(0, tzinfo=timezone.utc).now()
        for value in ('Invalid/Zone', '/invalid/zone'):
            with self.subTest(value=value), patch.dict(os.environ, NIGHT_ZONE=value):
                with self.assertRaises(ValueError) as raised:
                    schedule.next_marker(trigger)
                self.assertIn('NIGHT_ZONE', str(raised.exception))
                self.assertIn(value, str(raised.exception))

    def test_empty_or_unset_night_zone_uses_utc(self):
        try:
            ZoneInfo('UTC')
        except ZoneInfoNotFoundError:
            self.skipTest("requires UTC timezone data")
        trigger = Clock(0, tzinfo=timezone.utc).now()
        for value in ('', None):
            with self.subTest(value=value), patch.dict(os.environ, NIGHT_ZONE=''):
                if value is None:
                    del os.environ['NIGHT_ZONE']
                marker = schedule.next_marker(trigger, '01:00')
                self.assertEqual(marker.isoformat(), '2026-10-21T01:00:00+00:00')

    def test_late_daily_cron_keeps_its_trigger(self):
        now = datetime(2026, 10, 25, 7, tzinfo=timezone.utc)
        self.assertEqual(schedule.trigger_time('schedule', '17 20 * * *', now),
                         datetime(2026, 10, 24, 20, 17, tzinfo=timezone.utc))
        self.assertEqual(schedule.trigger_time('schedule', '0 7 * * *', now), now)
        for event in ('workflow_dispatch', 'workflow_run'):
            self.assertEqual(schedule.trigger_time(event, '', now), now)

    def test_marker_outputs_without_sleep_and_override(self):
        for override in ('true', 'false'):
            with tempfile.TemporaryDirectory() as root:
                output = Path(root) / 'output'
                clock = Clock(0, 58, tzinfo=self.zone)
                with patch.dict(os.environ, OVERRIDE=override, NIGHT_START='01:00',
                                GITHUB_EVENT_NAME='workflow_dispatch', GITHUB_OUTPUT=str(output)), \
                        patch.object(schedule, 'Clock', return_value=clock), \
                        patch.object(sys, 'argv', ['schedule', 'marker']), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(schedule.main(), 0)
                self.assertEqual(clock.sleeps, [])
                marker = clock.now() if override == 'true' else schedule.next_marker(clock.now())
                self.assertEqual(output.read_text(), 'marker=' + marker.isoformat() + '\n')

    def test_override_marker_is_in_night_zone(self):
        zone = self.zone
        clock = Clock(1, tzinfo=timezone.utc)
        with patch.dict(os.environ, OVERRIDE='true', GITHUB_OUTPUT=''), \
                patch.object(schedule, 'Clock', return_value=clock), \
                patch.object(sys, 'argv', ['schedule', 'marker']), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(schedule.main(), 0)
        marker = datetime.fromisoformat(output.getvalue().strip())
        self.assertEqual(marker, clock.now())
        self.assertEqual(marker.utcoffset(), clock.now().astimezone(zone).utcoffset())

    def test_late_cron_after_marker_starts_without_sleep(self):
        now = datetime(2026, 10, 25, 9, tzinfo=self.zone)
        clock = Mock()
        clock.now.return_value = now
        with patch.dict(os.environ, OVERRIDE='false', NIGHT_START='04:00',
                        GITHUB_EVENT_NAME='schedule', SCHEDULE='17 20 * * *', GITHUB_OUTPUT=''), \
                patch.object(schedule, 'Clock', return_value=clock), \
                patch.object(sys, 'argv', ['schedule', 'marker']), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(schedule.main(), 0)
        clock.sleep.assert_not_called()
        self.assertLess(datetime.fromisoformat(output.getvalue().strip()), now)

    def test_hosted_wait_is_bounded_and_passed_marker_returns_at_once(self):
        for remaining in (-1, 120, 24 * 60 * 60):
            clock = Clock(1, tzinfo=timezone.utc)
            marker = clock.now() + timedelta(seconds=remaining)
            with patch.object(schedule, 'Clock', return_value=clock), \
                    patch.object(sys, 'argv', ['schedule', 'wait', '--until', marker.isoformat()]):
                self.assertEqual(schedule.main(), 0)
            self.assertEqual(sum(clock.sleeps), max(0, min(remaining, schedule.HOSTED_WAIT_SECONDS)))

    def test_bakes_without_marker_records_stage_start(self):
        with tempfile.TemporaryDirectory() as root:
            clock = Clock(3, tzinfo=timezone.utc)
            process = Mock()
            process.wait.return_value = 0
            with patch.dict(os.environ, GPU_WORKFLOW=schedule.BAKES_WORKFLOW, MARKER='',
                            GITHUB_OUTPUT='', _AUTANA_HEAVY_LOCK_ROOT=root), \
                    patch.object(schedule, 'Clock', return_value=clock), \
                    patch.object(sys, 'argv', ['schedule', 'start', '--', 'bake']), \
                    patch.object(schedule, 'launch_process_tree', return_value=process), \
                    patch.object(schedule, 'close_process_tree'):
                self.assertEqual(schedule.main(), 0)
            key = clock.now().strftime(schedule.MARKER_KEY_FORMAT)
            record = json.loads((Path(root) / 'night-steps' / key / (schedule.BAKES_WORKFLOW + '.json')).read_text())
            self.assertEqual(record['marker'], clock.now().isoformat())

    def test_gpu_shell_only_deferral_ends_green(self):
        root = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            tools = fixture / 'tools'
            tools.mkdir()
            autana = tools / 'autana'
            autana.write_text('#!/bin/sh\nprintf "%s\\n" "$*"\nexit 75\n')
            autana.chmod(0o755)
            python = fixture / 'python3'
            python.write_text('#!/bin/sh\nif [ "$2" = ready ]; then exit "$READY_CODE"; fi\n'
                              + f'exec "{sys.executable}" "$@"\n')
            python.chmod(0o755)
            for code in (0, schedule.EXIT_DEFERRED):
                env = dict(os.environ, PYTHONPATH=str(root), READY_CODE=str(code),
                           GPU_WORKFLOW=schedule.DOC_IMAGES_WORKFLOW,
                           PATH=str(fixture) + os.pathsep + os.environ['PATH'])
                result = subprocess.run(['sh', str(root / 'scripts/ci/gpu_stage.sh'), 'render'],
                                        cwd=fixture, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0 if code == schedule.EXIT_DEFERRED else 75, result.stderr)
                if not code:
                    self.assertIn('--wait ' + str(schedule.NIGHT_STEP_HANG_SECONDS), result.stdout)
                    self.assertIn('start -- render', result.stdout)
                else:
                    self.assertEqual(result.stdout, '')
            # A fit takes no heavy: autana (exit 75 here) is never asked; the stage starts beside board work.
            python.write_text('#!/bin/sh\nif [ "$2" = ready ]; then exit 0; fi\n'
                              'if [ "$2" = start ]; then printf "%s\\n" "$*"; exit 0; fi\n'
                              + f'exec "{sys.executable}" "$@"\n')
            result = subprocess.run(['sh', str(root / 'scripts/ci/gpu_stage.sh'), 'fit'], cwd=fixture,
                                    env=dict(env, GPU_WORKFLOW=schedule.BAKES_WORKFLOW), capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout), (0, 'scripts/lib/local_schedule.py start -- fit\n'),
                             result.stderr)

    def start(self, root, waits, clock=None, progressing=None):
        """Run `start` with a process whose wait() yields `waits` in turn; `progressing` touches the progress file."""
        marker = Clock(1, tzinfo=timezone.utc).now().isoformat()
        clock = clock or Clock(1, tzinfo=timezone.utc)
        process = Mock()

        def wait(timeout):
            clock.sleep(timeout)
            if progressing:
                stamp = progressing()
                os.utime(os.environ[schedule.STAGE_PROGRESS_ENV], (stamp, stamp))
            outcome = waits.pop(0)
            if isinstance(outcome, BaseException):
                raise outcome
            return outcome
        process.wait.side_effect = wait
        with patch.dict(os.environ, GPU_WORKFLOW=schedule.DOC_IMAGES_WORKFLOW,
                        GITHUB_RUN_ID='42', MARKER=marker, _AUTANA_HEAVY_LOCK_ROOT=root), \
                 patch.object(sys, 'argv', ['schedule', 'start', '--', 'render']), \
                 patch.object(schedule, 'Clock', return_value=clock), \
                 patch.object(schedule, 'launch_process_tree', return_value=process), \
                 patch.object(schedule, 'stop_process_tree') as stop, \
                 patch.object(schedule, 'close_process_tree'), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
            code = schedule.main()
            progress = os.environ[schedule.STAGE_PROGRESS_ENV]
        record = json.loads(next((Path(root) / 'night-steps').glob('*/*.json')).read_text())
        self.assertEqual((record['marker'], record['run'], record['workflow'], record['exit']),
                         (marker, '42', schedule.DOC_IMAGES_WORKFLOW, code))
        self.assertTrue(record['started'] <= record['ended'])
        self.assertFalse(Path(progress).exists())
        return code, stop.called, output.getvalue()

    def test_start_returns_the_stage_exit_and_names_a_progress_file(self):
        with tempfile.TemporaryDirectory() as root:
            timeout = subprocess.TimeoutExpired('render', 1)
            self.assertEqual(self.start(root, [timeout, timeout, 7])[:2], (7, False))

    def test_start_stops_a_stage_that_stops_progressing(self):
        polls = schedule.GPU_STAGE_PROGRESS_SECONDS // schedule.QUEUE_POLL_SECONDS
        with tempfile.TemporaryDirectory() as root:
            clock = Clock(1, tzinfo=timezone.utc)
            touched = clock.now().timestamp()
            code, stopped, said = self.start(root, [subprocess.TimeoutExpired('render', 1)] * (polls + 1), clock,
                                             progressing=lambda: touched)
        self.assertEqual((code, stopped), (schedule.EXIT_HUNG, True))
        self.assertIn('GPU_STAGE_PROGRESS_SECONDS', said)

    def test_start_keeps_a_slow_stage_that_progresses_until_the_backstop(self):
        polls = schedule.GPU_STAGE_HANG_SECONDS[schedule.DOC_IMAGES_WORKFLOW] // schedule.QUEUE_POLL_SECONDS
        with tempfile.TemporaryDirectory() as root:
            clock = Clock(1, tzinfo=timezone.utc)
            code, stopped, said = self.start(root, [subprocess.TimeoutExpired('render', 1)] * (polls + 1), clock,
                                             progressing=lambda: clock.now().timestamp())
        self.assertEqual((code, stopped), (schedule.EXIT_HUNG, True))
        self.assertIn('GPU_STAGE_HANG_SECONDS', said)
        self.assertNotIn('GPU_STAGE_PROGRESS_SECONDS', said)

    def test_wait_idle_gives_up_at_its_deadline(self):
        clock = Clock(1)
        until = clock.now() + timedelta(seconds=3 * schedule.QUEUE_POLL_SECONDS)
        self.assertFalse(schedule.wait_idle(lambda: True, clock, until=until))
        self.assertEqual(clock.now(), until)
        self.assertTrue(schedule.wait_idle(lambda: False, Clock(1), until=until))

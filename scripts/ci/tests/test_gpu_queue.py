"""GPU and board work share a bounded night without touching hardware."""
from datetime import datetime, timedelta
from functools import partial
from pathlib import Path
import os
import sys
import unittest
from unittest.mock import patch

from scripts.lib import local_schedule as schedule
from scripts.ci.tests.test_local_schedule import Clock
import yaml


class GpuQueueTests(unittest.TestCase):
    def test_invalid_night_start_fails_loudly(self):
        for value in ('7:00', '24:00', '17:59', '07:00', '22:60', 'evening'):
            with self.subTest(value=value), patch.dict(os.environ, NIGHT_START=value):
                with self.assertRaisesRegex(ValueError, 'NIGHT_START'):
                    schedule.night_window(clock=Clock(23))

    def test_waiting_runner_yields_to_new_bakes(self):
        clock = Clock(19)
        def request(*args):
            return {'workflow_runs': [{'status': 'queued'}] if clock.sleeps else []}
        self.assertEqual(schedule.gpu_queue(60, scheduled=True, clock=clock, request=request), schedule.DEFERRED)
        self.assertEqual(clock.sleeps, [schedule.QUEUE_POLL_SECONDS])

    def test_daytime_trigger_leaves_pending_until_evening(self):
        for hour, expected in ((7, schedule.PENDING), (14, schedule.PENDING), (19, None)):
            clock = Clock(hour)
            self.assertEqual(schedule.gpu_queue(60, scheduled=True, clock=clock,
                             request=lambda *args: {'workflow_runs': []}), expected)
            self.assertEqual(bool(clock.sleeps), hour == 19)

    def test_idle_poll_resets_when_trigger_arrives_in_gap(self):
        clock = Clock(23)
        pending = iter((False, True, False, False))
        self.assertTrue(schedule.wait_idle(lambda: next(pending), clock.now() + timedelta(minutes=10),
                        0, clock, idle_polls=2))
        self.assertEqual(clock.sleeps, [schedule.QUEUE_POLL_SECONDS] * 3)

    def test_override_board_timeout_message(self):
        import contextlib
        import io
        clock = Clock(12)
        with patch.dict(os.environ, OVERRIDE='true', GITHUB_EVENT_NAME='workflow_dispatch'), \
                patch.object(sys, 'argv', ['local_schedule', 'ready']), \
                patch.object(schedule, 'gpu_queue', return_value=None), \
                patch.object(schedule, 'gpu_ready', partial(schedule.gpu_ready, board_held=lambda: True)), \
                patch.object(schedule, 'Clock', return_value=clock), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(schedule.main(), schedule.EXIT_REFUSED)
        self.assertEqual((clock.now() - datetime(2026, 10, 21, 12)).total_seconds(), schedule.DEFAULT_LOCK_WAIT_SECONDS)
        self.assertIn('boards busy', output.getvalue())
        self.assertNotIn(schedule.REFUSED, output.getvalue())

    def test_start_default_empty_and_configured(self):
        for value, hour, minute in (('', 22, 30), ('23:15', 23, 15)):
            with patch.dict(os.environ, NIGHT_START=value):
                clock = Clock(18)
                window = schedule.night_window(scheduled=True, clock=clock)
                self.assertEqual(window[0], datetime(2026, 10, 21, hour, minute))
                self.assertEqual(window[1], datetime(2026, 10, 22, 7))
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(schedule.night_window(scheduled=True, clock=Clock(18))[0].hour, 22)

    def test_gpu_waits_for_board_release_and_yields_to_bakes(self):
        clock = Clock(23)
        released = clock.now() + timedelta(minutes=2)
        api_calls = []
        def request(method, path):
            api_calls.append(path)
            return {'workflow_runs': [{'status': 'in_progress'}] if clock.now() < released else []}
        self.assertEqual(schedule.gpu_queue(60, clock=clock, request=request), schedule.DEFERRED)
        self.assertEqual(clock.sleeps, [])
        self.assertTrue(schedule.gpu_ready(estimate=60, clock=clock,
                        board_held=lambda: clock.now() < released))
        self.assertEqual(clock.now(), released)
        self.assertIsNone(schedule.gpu_queue(60, clock=clock, request=request))
        self.assertTrue(any('/bakes-gpu.yml/runs' in path for path in api_calls))

    def test_runner_waits_for_start_and_caps_workflow_run(self):
        clock = Clock(18)
        self.assertIsNone(schedule.gpu_queue(60, scheduled=True, clock=clock,
                          request=lambda *a: {'workflow_runs': []}))
        self.assertEqual(clock.now(), datetime(2026, 10, 21, 22, 30))
        self.assertEqual(schedule.gpu_queue(20 * 60, scheduled=True, clock=Clock(6, 45), request=lambda *a: {'workflow_runs': []}), schedule.SKIPPED)
        workflow = yaml.safe_load((Path(__file__).resolve().parents[3] / '.github/workflows/doc-images-gpu.yml').read_text())
        hosted = workflow['jobs']['fingerprint']
        self.assertNotIn('gpu', hosted['outputs'])
        self.assertFalse(any(step.get('id') == 'queue' for step in hosted['steps']))
        refresh = workflow['jobs']['refresh']
        self.assertEqual(refresh['timeout-minutes'], 1260)
        self.assertEqual(refresh['runs-on'], ['self-hosted', 'linux', 'gpu'])
        self.assertEqual(workflow[True]["workflow_run"], {"workflows": ["Bakes GPU"], "types": ["completed"]})

    def test_board_lock_past_estimate_cutoff_skips_or_refuses(self):
        for scheduled in (True, False):
            clock = Clock(6, 30)
            self.assertFalse(schedule.gpu_ready(estimate=20 * 60,
                             scheduled=scheduled, clock=clock, board_held=lambda: True))
            self.assertEqual(clock.now(), datetime(2026, 10, 21, 6, 40))

    def test_ready_cutoff_and_post_heavy_check_emit_skip_or_refusal(self):
        import contextlib
        import io
        for event, expected in (('schedule', schedule.EXIT_SKIPPED),
                                ('workflow_dispatch', schedule.EXIT_REFUSED)):
            with self.subTest(event=event), patch.dict(os.environ, GITHUB_EVENT_NAME=event,
                    OVERRIDE='false'), \
                    patch.object(schedule, 'Clock', return_value=Clock(6, 41)), \
                    patch.object(schedule, 'gpu_pending', return_value=False), \
                    patch.object(sys, 'argv', ['local_schedule', 'ready']), \
                    patch.object(schedule, 'gpu_ready', partial(schedule.gpu_ready, board_held=lambda: True)), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(schedule.main(), expected)
            with patch.dict(os.environ, GITHUB_EVENT_NAME=event,
                    OVERRIDE='false'), patch.object(schedule, 'Clock', return_value=Clock(6, 41)), \
                    patch.object(schedule, 'gpu_pending', return_value=False), \
                    patch.object(sys, 'argv', ['local_schedule', 'start', '--', 'render']), \
                    patch.object(schedule.subprocess, 'call') as run, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(schedule.main(), 0 if event == 'schedule' else schedule.EXIT_REFUSED)
                run.assert_not_called()

    def test_pending_includes_queued_waiting_and_running_on_any_branch(self):
        for status in ('queued', 'waiting', 'in_progress', 'pending', 'requested', 'completed'):
            paths = []
            def request(method, path):
                paths.append(path)
                return {'workflow_runs': [dict(status=status)]}
            self.assertEqual(schedule.gpu_pending(request=request), status != 'completed')
            self.assertTrue(all('branch=' not in path and 'status=' not in path for path in paths))
            self.assertEqual(len(paths), 1 if status != 'completed' else len(schedule.GPU_ESTIMATES))


if __name__ == '__main__':
    unittest.main()

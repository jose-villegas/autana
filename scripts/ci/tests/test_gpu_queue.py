"""GPU and board work share a bounded night without touching hardware."""
from datetime import datetime, timedelta
from functools import partial
from pathlib import Path
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'lib'))
import local_schedule as schedule
from test_local_schedule import Clock


class GpuQueueTests(unittest.TestCase):
    def test_start_default_empty_and_configured(self):
        for value, hour, minute in (('', 22, 30), ('23:15', 23, 15)):
            with patch.dict(os.environ, NIGHT_START=value):
                clock = Clock(18)
                window = schedule.night_window(scheduled=True, clock=clock)
                self.assertEqual(window[0], datetime(2026, 10, 21, hour, minute))
                self.assertEqual(window[1], datetime(2026, 10, 22, 7))
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(schedule.night_window(scheduled=True, clock=Clock(18))[0].hour, 22)

    def test_gpu_waits_for_board_release_and_hosted_queue_defers_bakes(self):
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

    def test_hosted_queue_waits_for_start_and_caps_workflow_run(self):
        clock = Clock(18)
        self.assertIsNone(schedule.gpu_queue(60, scheduled=True, clock=clock,
                          request=lambda *a: {'workflow_runs': []}))
        self.assertEqual(clock.now(), datetime(2026, 10, 21, 22, 30))
        self.assertEqual(schedule.gpu_queue(20 * 60, scheduled=True, clock=Clock(6, 45)), schedule.SKIPPED)
        workflow = (Path(__file__).resolve().parents[3] / '.github/workflows/doc-images-gpu.yml').read_text()
        hosted, gpu = workflow.split('  refresh:', 1)
        self.assertIn('run: python3 scripts/lib/local_schedule.py queue', hosted)
        self.assertIn('timeout-minutes: 360', hosted)
        self.assertIn('workflows: [Bakes GPU]', hosted)
        self.assertIn("GPU_QUEUE_READY: 'true'", gpu)

    def test_board_lock_past_estimate_cutoff_skips_or_refuses(self):
        for scheduled in (True, False):
            clock = Clock(6, 30)
            self.assertFalse(schedule.gpu_ready(estimate=20 * 60,
                             scheduled=scheduled, clock=clock, board_held=lambda: True))
            self.assertEqual(clock.now(), datetime(2026, 10, 21, 6, 40))

    def test_override_still_waits_for_board(self):
        clock = Clock(12)
        released = clock.now() + timedelta(minutes=2)
        self.assertTrue(schedule.gpu_ready(override=True, estimate=60,
                        clock=clock, board_held=lambda: clock.now() < released))
        self.assertEqual(clock.now(), released)

    def test_override_wait_has_a_deadline(self):
        from autana_config import DEFAULT_LOCK_WAIT_SECONDS
        clock = Clock(12)
        self.assertFalse(schedule.gpu_ready(override=True, estimate=60,
                         clock=clock, board_held=lambda: True))
        self.assertEqual(clock.now(), datetime(2026, 10, 21, 12) +
                         timedelta(seconds=DEFAULT_LOCK_WAIT_SECONDS))

    def test_ready_cutoff_and_post_heavy_check_emit_skip_or_refusal(self):
        import contextlib
        import io
        for event, expected in (('schedule', schedule.EXIT_SKIPPED),
                                ('workflow_dispatch', schedule.EXIT_REFUSED)):
            with self.subTest(event=event), patch.dict(os.environ, GITHUB_EVENT_NAME=event,
                    GPU_ESTIMATE_SECONDS='1200', OVERRIDE='false'), \
                    patch.object(schedule, 'Clock', return_value=Clock(6, 41)), \
                    patch.object(sys, 'argv', ['local_schedule', 'ready']), \
                    patch.object(schedule, 'gpu_ready', partial(schedule.gpu_ready, board_held=lambda: True)), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(schedule.main(), expected)
            with patch.dict(os.environ, GITHUB_EVENT_NAME=event, GPU_ESTIMATE_SECONDS='1200',
                    OVERRIDE='false'), patch.object(schedule, 'Clock', return_value=Clock(6, 41)), \
                    patch.object(sys, 'argv', ['local_schedule', 'start', '--', 'render']), \
                    patch.object(schedule.subprocess, 'call') as run, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(schedule.main(), 0 if event == 'schedule' else schedule.EXIT_REFUSED)
                run.assert_not_called()

    def test_estimate_last_successful_main_workflow_or_named_fallback(self):
        paths = []
        def request(method, path):
            paths.append(path)
            return {'workflow_runs': [dict(run_started_at='2026-10-21T23:00:00Z',
                                           updated_at='2026-10-21T23:12:00Z')]}
        self.assertEqual(schedule.gpu_estimate('bakes-gpu.yml', request=request), 720)
        self.assertIn('branch=main', paths[0])
        self.assertIn('status=success', paths[0])
        self.assertEqual(schedule.gpu_estimate('bakes-gpu.yml', request=lambda *a: {'workflow_runs': []}),
                         schedule.BAKES_ESTIMATE_SECONDS)

    def test_pending_includes_queued_waiting_and_running_on_any_branch(self):
        for status in ('queued', 'waiting', 'in_progress', 'pending', 'requested', 'completed'):
            paths = []
            def request(method, path):
                paths.append(path)
                return {'workflow_runs': [dict(status=status)]}
            self.assertEqual(schedule.gpu_pending(request=request), status != 'completed')
            self.assertTrue(all('branch=' not in path for path in paths))


if __name__ == '__main__':
    unittest.main()

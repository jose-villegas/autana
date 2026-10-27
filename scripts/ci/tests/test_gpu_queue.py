"""GPU work queues after board locks and yields to bakes."""
from datetime import datetime, timedelta, timezone
from functools import partial
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from scripts.ci.github_api import PER_PAGE
from scripts.lib import local_schedule as schedule
from scripts.ci.tests.test_local_schedule import Clock
import yaml


class GpuQueueTests(unittest.TestCase):
    def setUp(self):
        root = self.enterContext(tempfile.TemporaryDirectory())
        self.enterContext(patch.dict(os.environ, _AUTANA_HEAVY_LOCK_ROOT=root))

    def test_pending_reads_only_first_recent_page_per_workflow(self):
        request = Mock(side_effect=[
            {'workflow_runs': [{'status': 'completed'}] * PER_PAGE}
            for workflow in schedule.GPU_WORKFLOWS])
        now = datetime(2026, 10, 23, tzinfo=timezone.utc)
        with patch.object(schedule, 'Clock') as clock:
            clock.return_value.now.return_value = now
            self.assertFalse(schedule.gpu_pending(request=request))
        cutoff = (now - schedule.RECENT_RUNS_WINDOW).strftime("%Y-%m-%dT%H:%M:%SZ")
        self.assertEqual(request.call_args_list, [
            unittest.mock.call('GET', f'/actions/workflows/{workflow}/runs?created=>={cutoff}&per_page={PER_PAGE}')
            for workflow in schedule.GPU_WORKFLOWS])

    def test_idle_polls_reset_and_ready_waits_without_deadline(self):
        clock = Clock(1)
        pending = iter((False, True, False, False))
        self.assertTrue(schedule.wait_idle(lambda: next(pending), clock, idle_polls=2))
        self.assertEqual(clock.sleeps, [schedule.QUEUE_POLL_SECONDS] * 3)
        release = clock.now() + timedelta(days=2)
        self.assertTrue(schedule.gpu_ready(clock, board_held=lambda: clock.now() < release))
        self.assertEqual(clock.now(), release)

    def test_ready_refreshes_marker_and_defers_only_doc_images(self):
        import json
        import contextlib
        import io
        for workflow in schedule.GPU_WORKFLOWS:
            clock = Clock(1)
            with patch.dict(os.environ, GPU_WORKFLOW=workflow, RUNNER_NAME='runner'), \
                    patch.object(sys, 'argv', ['schedule', 'ready']), \
                    patch.object(schedule, 'Clock', return_value=clock), \
                    patch.object(schedule, 'gpu_pending', return_value=True), \
                    patch.object(schedule, 'gpu_ready') as ready, \
                    contextlib.redirect_stdout(io.StringIO()):
                code = schedule.main()
            self.assertEqual(code, schedule.EXIT_DEFERRED if workflow == schedule.DOC_IMAGES_WORKFLOW else 0)
            self.assertEqual(ready.called, workflow != schedule.DOC_IMAGES_WORKFLOW)
            marker = json.loads((schedule.heavy_root() / schedule.GPU_HOST_MARKER).read_text())
            self.assertEqual(marker['runner'], 'runner')

    def test_workflows_separate_marker_wait_and_work(self):
        root = Path(__file__).resolve().parents[3]
        for filename, job in (('doc-images-gpu.yml', 'refresh'), ('nightly-perf.yml', 'nightly')):
            workflow = yaml.safe_load((root / '.github/workflows' / filename).read_text())
            self.assertEqual(workflow[True]['schedule'], [{'cron': '17 20 * * *'}])
            self.assertEqual(workflow['jobs']['marker']['runs-on'], 'ubuntu-24.04')
            wait = workflow['jobs']['wait']
            self.assertEqual(wait['runs-on'], 'ubuntu-24.04')
            self.assertIn('marker', wait['needs'])
            self.assertEqual(wait['timeout-minutes'], 355)
            self.assertEqual(wait['strategy'], {'max-parallel': 1, 'matrix': {'leg': [1, 2, 3, 4, 5]}})
            self.assertLess(schedule.HOSTED_WAIT_SECONDS, wait['timeout-minutes'] * 60)
            self.assertIn('wait', workflow['jobs'][job]['needs'])
            if job == 'refresh':
                self.assertEqual(wait['if'], workflow['jobs'][job]['if'])
                self.assertIn('fingerprint', wait['needs'])
            self.assertEqual(workflow['jobs'][job]['timeout-minutes'], 1380)
            self.assertIn('marker', workflow['jobs'][job]['needs'])
            self.assertEqual(workflow['jobs'][job]['env']['MARKER'], '${{ needs.marker.outputs.marker }}')

    def test_pending_counts_only_gpu_jobs_ready_to_run(self):
        for status in ('queued', 'waiting', 'in_progress', 'pending', 'requested', 'completed'):
            for labels in (['self-hosted', 'linux', 'gpu'], ['ubuntu-24.04']):
                with self.subTest(status=status, labels=labels):
                    def request(method, path):
                        if '/jobs?' in path:
                            return {'jobs': [dict(status=status, labels=labels)]}
                        return {'workflow_runs': [dict(id=1, status='in_progress')]}
                    self.assertEqual(schedule.gpu_pending(request=request),
                                     'gpu' in labels and status in ('queued', 'waiting', 'in_progress'))

    def test_tomorrow_marker_wait_does_not_hold_tonights_board_plan(self):
        def request(method, path):
            if '/jobs?' in path:
                return {'jobs': [dict(name='marker', status='completed', labels=['ubuntu-24.04']),
                                 dict(name='wait (1)', status='in_progress', labels=['ubuntu-24.04']),
                                 dict(name='refresh', status='pending', labels=['gpu'])]}
            return {'workflow_runs': [dict(id=1, status='in_progress')]}
        self.assertFalse(schedule.gpu_pending(request=request))


    def test_board_night_waits_for_board_start(self):
        root = Path(__file__).resolve().parents[3]
        workflow = yaml.safe_load((root / '.github/workflows/nightly-perf.yml').read_text())
        marker = workflow['jobs']['marker']
        self.assertIn('--start BOARD_START marker', marker['steps'][-1]['run'])
        self.assertEqual(marker['env']['BOARD_START'], '${{ vars.BOARD_START }}')
        trigger = datetime(2026, 10, 27, 20, 17, tzinfo=timezone.utc)
        with patch.dict(os.environ, NIGHT_ZONE='UTC'):
            os.environ.pop('BOARD_START', None)
            board = schedule.next_marker(trigger, start='BOARD_START')
            gpu = schedule.next_marker(trigger, schedule.NIGHT_START)
        self.assertEqual(board.strftime('%H:%M'), schedule.BOARD_START)
        self.assertLess(gpu, board)


if __name__ == '__main__':
    unittest.main()

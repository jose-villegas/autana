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
            self.assertEqual(workflow[True]['schedule'], [{'cron': '0 22 * * *'}])
            self.assertEqual(workflow['jobs']['marker']['timeout-minutes'], 1500)
            self.assertEqual(workflow['jobs'][job]['timeout-minutes'], 1380)
            self.assertIn('marker', workflow['jobs'][job]['needs'])
            self.assertEqual(workflow['jobs'][job]['env']['MARKER'], '${{ needs.marker.outputs.marker }}')

    def test_pending_includes_queued_waiting_and_running_on_any_branch(self):
        for status in ('queued', 'waiting', 'in_progress', 'pending', 'requested', 'completed'):
            paths = []
            def request(method, path):
                paths.append(path)
                return {'workflow_runs': [dict(status=status)]}
            self.assertEqual(schedule.gpu_pending(request=request), status != 'completed')
            self.assertTrue(all('branch=' not in path and 'status=' not in path for path in paths))
            self.assertEqual(len(paths), 1 if status != 'completed' else len(schedule.GPU_WORKFLOWS))


if __name__ == '__main__':
    unittest.main()

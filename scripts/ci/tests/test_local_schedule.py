"""Night starts use an injected local clock."""
from datetime import datetime, timedelta
import contextlib
import io
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
import yaml
from unittest.mock import patch

from scripts.lib import local_schedule
from scripts.lib.local_schedule import NIGHT_START, NIGHT_END


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
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.marker_root = Path(temporary.name) / "heavy"
        environment = patch.dict(os.environ, _AUTANA_HEAVY_LOCK_ROOT=str(self.marker_root))
        environment.start()
        self.addCleanup(environment.stop)

    def test_gpu_stage_refreshes_host_marker_before_refusing(self):
        import json
        for workflow in local_schedule.GPU_ESTIMATES:
            for runner in ("gpu-runner-a", "gpu-runner-b"):
                env = dict(os.environ, GPU_WORKFLOW=workflow, RUNNER_NAME=runner,
                           GITHUB_EVENT_NAME="workflow_dispatch", OVERRIDE="false")
                with patch.dict(os.environ, env), \
                        patch.object(sys, "argv", ["local_schedule", "ready"]), \
                        patch.object(local_schedule, "gpu_queue", return_value=None), \
                        patch.object(local_schedule, "gpu_ready", return_value=False), \
                        patch.object(local_schedule, "Clock", return_value=Clock(12)), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(local_schedule.main(), local_schedule.EXIT_REFUSED)
                record = json.loads((self.marker_root / local_schedule.GPU_HOST_MARKER).read_text())
                self.assertEqual(record["runner"], runner)
                self.assertEqual(record["refreshed_at"], Clock(12).now().isoformat())

    def test_gpu_marker_read_needs_no_lock_or_directory(self):
        with patch.object(local_schedule.LockStore, "guard", side_effect=AssertionError("reader took lock")):
            self.assertFalse(local_schedule.gpu_on_host(Clock(12)))
            self.assertFalse(self.marker_root.exists())
            local_schedule.refresh_gpu_host("runner", Clock(12))
            self.assertTrue(local_schedule.gpu_on_host(Clock(12)))
            self.assertTrue(local_schedule.gpu_on_host(Clock(12, day=21 + 7)))
            self.assertFalse(local_schedule.gpu_on_host(Clock(11)))

    def test_missing_gpu_workflow_refuses_before_running(self):
        env = dict(os.environ, OVERRIDE="true")
        env.pop("GPU_WORKFLOW", None)
        with patch.dict(os.environ, env, clear=True), \
                patch.object(sys, "argv", ["local_schedule", "start", "--", "render"]), \
                patch.object(local_schedule.subprocess, "call") as run:
            with self.assertRaisesRegex(KeyError, "GPU_WORKFLOW"):
                local_schedule.main()
            run.assert_not_called()

    def test_gpu_stage_busy_dispatch_refuses_and_schedule_skips(self):
        root = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            tools = fixture / "tools"
            tools.mkdir()
            autana = tools / "autana"
            autana.write_text("#!/bin/sh\necho 'heavy held by render-owner' >&2\nexit 75\n")
            autana.chmod(0o755)
            python = fixture / "python3"
            python.write_text("#!/bin/sh\ncase \"$2\" in\n"
                              "budget) echo 60; exit 0;;\n"
                              "ready) echo ready >> order; exit 0;;\nesac\n"
                              f'exec "{sys.executable}" "$@"\n')
            python.chmod(0o755)
            autana.write_text(autana.read_text().replace("echo 'heavy", "echo heavy >> order\necho 'heavy"))
            env = dict(os.environ, PYTHONPATH=str(root), OVERRIDE="false",
                       GPU_WORKFLOW=local_schedule.DOC_IMAGES_WORKFLOW,
                       PATH=str(fixture) + os.pathsep + os.environ["PATH"])
            for event in ("workflow_dispatch", "schedule", "workflow_run"):
                env["GITHUB_EVENT_NAME"] = event
                result = subprocess.run(["sh", str(root / "scripts/ci/gpu_stage.sh"), "render"],
                                        cwd=fixture, env=env, capture_output=True, text=True)
                self.assertIn("render-owner", result.stderr)
                self.assertEqual((fixture / "order").read_text().splitlines()[-2:], ["ready", "heavy"])
                if event in ("schedule", "workflow_run"):
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(local_schedule.SKIPPED, result.stdout)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(local_schedule.REFUSED, result.stdout)

    def test_gpu_cron_starts_before_local_window_in_both_seasons(self):
        workflow = Path(__file__).resolve().parents[3] / ".github/workflows/doc-images-gpu.yml"
        data = yaml.safe_load(workflow.read_text())
        self.assertEqual(data[True]['schedule'], [{'cron': '0 16 * * *'}])
        start_hour, start_minute = map(int, NIGHT_START.split(":"))
        for offset in (1, 2):
            self.assertLessEqual(16 + offset, start_hour + start_minute / 60)
        self.assertEqual(data["env"]["NIGHT_START"], "${{ vars.NIGHT_START }}")

    def test_nightly_window_waits_runs_or_skips(self):
        for hour, minute, wait, allowed in ((18, 0, 270 * 60, True),
                (22, 30, 0, True), (23, 52, 0, True), (0, 30, 0, True),
                (1, 52, 0, True), (6, 59, 0, True), (7, 0, 0, False),
                (8, 0, 0, False), (12, 30, 600 * 60, True)):
            with self.subTest(hour=hour, minute=minute):
                clock = Clock(hour, minute)
                deadline = local_schedule.night_window("22:30", "07:00", scheduled=True, clock=clock)
                self.assertEqual(deadline is not None, allowed)
                self.assertEqual(clock.sleeps, [wait] if wait else [])
                if allowed:
                    expected_day = 22 if hour >= 12 else 21
                    self.assertEqual(deadline, (datetime(2026, 10, expected_day - 1, 22, 30),
                                                datetime(2026, 10, expected_day, 7)))

    def test_nightly_cron_and_timeout_cover_both_seasons(self):
        workflow = Path(__file__).resolve().parents[3] / ".github/workflows/nightly-perf.yml"
        data = yaml.safe_load(workflow.read_text())
        cron_hour = 18
        queue_hour, queue_minute = map(int, NIGHT_START.split(":"))
        end_hour, end_minute = map(int, NIGHT_END.split(":"))
        queue_hour += queue_minute / 60
        end_hour += end_minute / 60
        self.assertEqual(data[True]["schedule"], [{"cron": f"0 {cron_hour} * * *"}])
        longest_wait_minutes = max((queue_hour - cron_hour - offset) * 60 for offset in (1, 2))
        window_minutes = (24 - queue_hour + end_hour) * 60
        publish_minutes = 60
        timeout_minutes = int(longest_wait_minutes + window_minutes + publish_minutes)
        self.assertEqual(data["jobs"]["nightly"]["timeout-minutes"], timeout_minutes)

    def test_gate_and_post_lock_check_distinguish_dispatch_from_schedule(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            summary = Path(directory) / "summary"
            env = dict(GPU_WORKFLOW=local_schedule.DOC_IMAGES_WORKFLOW, GITHUB_OUTPUT=str(output), GITHUB_STEP_SUMMARY=str(summary), OVERRIDE="false")
            refusal = local_schedule.REFUSED
            for event, hour in (("schedule", 7), ("workflow_dispatch", 18),
                                ("workflow_dispatch", 7), ("workflow_dispatch", 15)):
                env["GITHUB_EVENT_NAME"] = event
                for mode in ("gate", "start"):
                    output.write_text("")
                    summary.write_text("")
                    with self.subTest(event=event, hour=hour, mode=mode), patch.dict(os.environ, env), \
                            patch.object(local_schedule, "Clock", return_value=Clock(hour)), \
                            patch.object(sys, "argv", ["local_schedule", mode, "--", "render"]), \
                            patch.object(local_schedule.subprocess, "call") as run, \
                            contextlib.redirect_stdout(io.StringIO()) as notice:
                        code = local_schedule.main()
                        if event in ("schedule", "workflow_run"):
                            self.assertEqual(code, local_schedule.EXIT_SKIPPED if mode == "gate" else 0)
                            message = local_schedule.SKIPPED
                        else:
                            self.assertNotEqual(code, 0)
                            self.assertNotEqual(code, local_schedule.EXIT_SKIPPED)
                            message = refusal
                        self.assertEqual(notice.getvalue(), message + "\n")
                        self.assertEqual(summary.read_text(), message + "\n")
                        self.assertEqual(output.read_text(), "render=false\n" if mode == "gate" else "")
                        run.assert_not_called()
            env["OVERRIDE"] = "true"
            with patch.dict(os.environ, env), patch.object(local_schedule, "Clock", return_value=Clock(12)), \
                    patch.object(sys, "argv", ["local_schedule", "start", "--", "render"]), \
                    patch.object(local_schedule.subprocess, "call", return_value=0) as run:
                self.assertEqual(local_schedule.main(), 0)
                run.assert_called_once_with(["render"])
            self.assertIn("rendered=true", output.read_text())

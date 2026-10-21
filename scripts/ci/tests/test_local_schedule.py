"""Night starts use an injected local clock."""
from datetime import datetime, timedelta
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
import local_schedule
from local_schedule import gpu_window, local_time, wait_seconds, GPU_START, GPU_END


class Clock:
    def __init__(self, hour):
        self.value = datetime(2026, 10, 21, hour)
        self.sleeps = []

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.value += timedelta(seconds=seconds)


class LocalScheduleTests(unittest.TestCase):
    def test_dispatch_window_and_override(self):
        for hour, allowed in ((0, False), (1, True), (2, True), (3, False), (12, False)):
            with self.subTest(hour=hour):
                clock = Clock(hour)
                self.assertEqual(gpu_window(clock=clock), allowed)
                self.assertTrue(gpu_window(override=True, clock=clock))
                self.assertEqual(clock.sleeps, [])

    def test_schedule_waits_until_start_and_never_rolls_cap_forward(self):
        clock = Clock(0)
        self.assertTrue(gpu_window(scheduled=True, clock=clock))
        self.assertEqual(clock.now(), local_time(GPU_START, clock.now()))
        self.assertEqual(wait_seconds(GPU_END, clock.now()), 2 * 60 * 60)
        self.assertFalse(gpu_window(scheduled=True, clock=Clock(3)))
        self.assertEqual(wait_seconds(GPU_END, Clock(4).now()), 0)

    def test_gate_and_post_lock_check_skip_without_a_rendered_record(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            env = dict(GITHUB_OUTPUT=str(output), GITHUB_EVENT_NAME="workflow_dispatch", OVERRIDE="false")
            for mode in ("gate", "start"):
                with self.subTest(mode=mode), patch.dict(os.environ, env), \
                        patch.object(local_schedule, "Clock", return_value=Clock(3)), \
                        patch.object(sys, "argv", ["local_schedule", mode, "--", "render"]), \
                        patch.object(local_schedule.subprocess, "call") as run, \
                        contextlib.redirect_stdout(io.StringIO()) as notice:
                    code = local_schedule.main()
                    self.assertEqual(code, local_schedule.EXIT_SKIPPED if mode == "gate" else 0)
                    self.assertIn(local_schedule.SKIPPED, notice.getvalue())
                    run.assert_not_called()
            self.assertEqual(output.read_text(), "render=false\n")
            env["OVERRIDE"] = "true"
            with patch.dict(os.environ, env), patch.object(local_schedule, "Clock", return_value=Clock(12)), \
                    patch.object(sys, "argv", ["local_schedule", "start", "--", "render"]), \
                    patch.object(local_schedule.subprocess, "call", return_value=0) as run:
                self.assertEqual(local_schedule.main(), 0)
                run.assert_called_once_with(["render"])
            self.assertIn("rendered=true", output.read_text())

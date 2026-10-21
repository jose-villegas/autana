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
    def __init__(self, hour, minute=0):
        self.value = datetime(2026, 10, 21, hour, minute)
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

    def test_schedule_before_midnight_waits_for_next_start(self):
        clock = Clock(23, 30)
        self.assertTrue(gpu_window(scheduled=True, clock=clock))
        self.assertEqual(clock.now(), datetime(2026, 10, 22, 1))
        self.assertEqual(clock.sleeps, [90 * 60])
        for hour in (3, 4, 11, 12):
            with self.subTest(hour=hour):
                clock = Clock(hour)
                self.assertFalse(gpu_window(scheduled=True, clock=clock))
                self.assertEqual(clock.sleeps, [])

    def test_gate_and_post_lock_check_distinguish_dispatch_from_schedule(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            summary = Path(directory) / "summary"
            env = dict(GITHUB_OUTPUT=str(output), GITHUB_STEP_SUMMARY=str(summary), OVERRIDE="false")
            refusal = "GPU stage renders 01:00-03:00 local time; dispatch with override to render now"
            for event, hour in (("schedule", 3), ("workflow_dispatch", 0),
                                ("workflow_dispatch", 3), ("workflow_dispatch", 15)):
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
                        if event == "schedule":
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

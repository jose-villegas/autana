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
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
import local_schedule
from local_schedule import gpu_window, local_time, wait_seconds, NIGHT_START, NIGHT_END


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
                              "estimate|budget) echo 60; exit 0;;\n"
                              "ready) echo ready >> order; exit 0;;\nesac\n"
                              f'exec "{sys.executable}" "$@"\n')
            python.chmod(0o755)
            autana.write_text(autana.read_text().replace("echo 'heavy", "echo heavy >> order\necho 'heavy"))
            env = dict(os.environ, PYTHONPATH=str(root), OVERRIDE="false",
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
        text = workflow.read_text()
        self.assertIn("cron: '0 18 * * *'", text)
        start_hour, start_minute = map(int, NIGHT_START.split(":"))
        for offset in (1, 2):
            self.assertLess(18 + offset, start_hour + start_minute / 60)
        self.assertIn("NIGHT_START: ${{ vars.NIGHT_START }}", text)

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
        text = workflow.read_text()
        cron_hour = 18
        queue_hour, queue_minute = map(int, NIGHT_START.split(":"))
        end_hour, end_minute = map(int, NIGHT_END.split(":"))
        queue_hour += queue_minute / 60
        end_hour += end_minute / 60
        self.assertIn(f"cron: '0 {cron_hour} * * *'", text)
        longest_wait_minutes = max((queue_hour - cron_hour - offset) * 60 for offset in (1, 2))
        window_minutes = (24 - queue_hour + end_hour) * 60
        publish_minutes = 60
        timeout_minutes = int(longest_wait_minutes + window_minutes + publish_minutes)
        self.assertIn(f"timeout-minutes: {timeout_minutes}", text)

    def test_dispatch_window_and_override(self):
        for hour, allowed in ((0, True), (1, True), (6, True), (7, False), (12, False), (23, True)):
            with self.subTest(hour=hour):
                clock = Clock(hour)
                self.assertEqual(gpu_window(clock=clock), allowed)
                self.assertTrue(gpu_window(override=True, clock=clock))
                self.assertEqual(clock.sleeps, [])

    def test_schedule_waits_until_start_and_never_rolls_cap_forward(self):
        clock = Clock(18)
        self.assertTrue(gpu_window(scheduled=True, clock=clock))
        self.assertEqual(clock.now(), local_time(NIGHT_START, clock.now()))
        self.assertEqual(wait_seconds(NIGHT_END, clock.now(), next_occurrence=True), 510 * 60)
        self.assertFalse(gpu_window(scheduled=True, clock=Clock(7)))
        self.assertEqual(wait_seconds(NIGHT_END, Clock(8).now()), 0)

    def test_schedule_inside_night_starts_immediately(self):
        clock = Clock(23, 30)
        self.assertTrue(gpu_window(scheduled=True, clock=clock))
        self.assertEqual(clock.sleeps, [])
        for hour in (7, 8, 11, 12):
            with self.subTest(hour=hour):
                clock = Clock(hour)
                self.assertFalse(gpu_window(scheduled=True, clock=clock))
                self.assertEqual(clock.sleeps, [])

    def test_gate_and_post_lock_check_distinguish_dispatch_from_schedule(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            summary = Path(directory) / "summary"
            env = dict(GITHUB_OUTPUT=str(output), GITHUB_STEP_SUMMARY=str(summary), OVERRIDE="false")
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

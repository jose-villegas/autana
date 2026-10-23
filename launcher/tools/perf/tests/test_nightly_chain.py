"""Board-free replays of nightly planning, acquisition and publication."""
from datetime import datetime, timedelta, timezone
import io
from functools import partial
from itertools import repeat
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PERF = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PERF))
sys.path.insert(0, str(PERF.parents[2] / "scripts/device/tests"))
sys.path.insert(0, str(PERF.parents[2] / "scripts/ci/tests"))
import port_guard
from test_local_schedule import Clock as ScheduleClock
from nightly import scoped_chain as chain, lnt
from test_perf_compare import FakeAutana
from layout_measure import make_table, parse_report, parse_timings
import layout_measure
import autana_config

MACHINE = "00:11:22:33:20:E8"
OTHER_MACHINE = "00:11:22:33:21:24"


IMAGE_HASH_APPENDED_OFFSET = 23
IMAGE_CHECKSUM_BYTES = 1
IMAGE_SHA256_BYTES = 32
IMAGE_MAX_PADDING_BYTES = 15
IMAGE_TEST_PADDING_BYTES = 4


def app_image(code=b"code", trailer=None):
    if trailer is None:
        trailer = bytes(IMAGE_MAX_PADDING_BYTES + IMAGE_CHECKSUM_BYTES + IMAGE_SHA256_BYTES)
    header = bytearray(chain.IMAGE_HEADER_BYTES)
    segments = (b"a" * chain.APP_DESC_BYTES, code)
    header[chain.IMAGE_SEGMENT_COUNT_OFFSET] = len(segments)
    header[IMAGE_HASH_APPENDED_OFFSET] = 1
    def segment(data):
        return bytes(chain.SEGMENT_DATA_LENGTH_OFFSET) + len(data).to_bytes(chain.SEGMENT_DATA_LENGTH_BYTES, "little") + data
    return header + b"".join(segment(data) for data in segments) + trailer


Clock = partial(ScheduleClock, day=18, tzinfo=timezone.utc)


class Replay(FakeAutana):
    def __init__(self, root, clock, builds=(), captures=(), durations=(), selftest_code=0, heavy_wait=0, capture_wait=0):
        super().__init__(root)
        self.clock = clock
        self.builds, self.captures, self.durations = iter(builds), iter(captures), iter(durations)
        self.selftest_code = selftest_code
        self.heavy_wait = heavy_wait
        self.capture_wait = iter(capture_wait) if isinstance(capture_wait, tuple) else repeat(capture_wait)
        self.boundaries, self.tokens, self.timeouts = [], [], []

    def __call__(self, command, log, timeout, **options):
        self.tokens.append(os.environ.get("_AUTANA_DEVICE_LOCK_TOKEN"))
        self.timeouts.append((command, timeout))
        if command == ["selftest-replay"]:
            self.boundaries.append(self.clock.now())
            self.clock.sleep(60)
            return self.selftest_code, [(0, "report: first.md"), (60, "report: selftest.md")], 60
        if "build" in command:
            self.clock.sleep(self.heavy_wait + next(self.durations, 60))
            code = next(self.builds, 0)
            project = Path(command[command.index("--project") + 1])
            build = project / "launcher/build.diag"
            build.mkdir(parents=True, exist_ok=True)
            (build / "launcher.bin").write_bytes(app_image((project / "code").read_bytes()))
            (build / "assets.bin").write_bytes(b"assets")
            (build / "build_id.txt").write_text("0-diag")
            return code, [(0, "heavy wait_seconds=" + str(self.heavy_wait))] if self.heavy_wait else [], 60
        if "suite" in command:
            capture_wait = next(self.capture_wait)
            self.clock.sleep(capture_wait + 60)
            if next(self.captures, False):
                raise RuntimeError("capture failed")
        code, lines, wall = super().__call__(command, log, timeout)
        if "suite" in command:
            if capture_wait:
                lines.append((0, "heavy wait_seconds=" + str(capture_wait)))
        return code, lines, wall


class NightlyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        cache = patch.object(autana_config, "user_cache_dir", return_value=self.root / "cache")
        cache.start()
        self.addCleanup(cache.stop)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        chain.git(self.repo, "init")
        chain.git(self.repo, "config", "user.email", "test@example.invalid")
        chain.git(self.repo, "config", "user.name", "Test")
        chain.git(self.repo, "config", "core.hooksPath", str(self.root / "no-hooks"))
        self.commits = []
        header = self.repo / "launcher/test/suites.h"
        header.parent.mkdir(parents=True)
        header.write_text("#define SUITE_FILTER_LEN 24\n#define SUITE_FILTER_MAX 4\n")
        for number in range(6):
            (self.repo / "code").write_text(str(number))
            chain.git(self.repo, "add", ".")
            chain.git(self.repo, "commit", "-qm", f"change (#{100 + number})")
            self.commits.append(chain.git(self.repo, "rev-parse", "HEAD"))
        self.clock = Clock(22, 30)
        self.args = SimpleNamespace(repo=self.repo, out=self.root / "out", data=self.root / "data",
                                    board="20E8", lane="0/2", until="07:00", previous=None,
                                    suite=[("suite", "-", "-")], selftest=None)

    def steps(self, indices=(1, 3, 5), anchor=False):
        return [chain.Step(self.commits[i], 100 + i, "anchor" if anchor and n == 0 else "new", i + 1)
                for n, i in enumerate(indices)]

    def run_chain(self, steps=None, **options):
        replay = Replay(self.root, self.clock, **options)
        writes = []
        original = chain.write_json

        def record(path, value):
            original(path, value)
            if path.name == "night.json":
                writes.append(json.loads(path.read_text()))

        with patch("layout_measure.capture_board", return_value=MACHINE), \
                patch.dict(os.environ, _AUTANA_DEVICE_LOCK_TOKEN="outer-hold"), \
                patch.object(chain, "write_json", side_effect=record):
            results = chain.run_chain(self.steps() if steps is None else steps, self.args, replay, self.clock)
        self.assertTrue(all(token == "outer-hold" for token in replay.tokens))
        self.assertEqual(chain.git(self.repo, "worktree", "list", "--porcelain").count("worktree "), 1)
        self.assertEqual(len(writes[-1]["steps"]), len(results))
        return results, replay, writes

    def capture_with(self, **fields):
        original = chain.run_flash
        def capture(*args, **kwargs):
            return dict(original(*args, **kwargs), **fields)
        return patch.object(chain, "run_flash", side_effect=capture)

    def test_worktree_git_commands_enable_longpaths(self):
        commands = []
        original = subprocess.check_output

        def record(command, **options):
            if command[0] == "git":
                commands.append(command)
            return original(command, **options)

        with patch.object(chain.subprocess, "check_output", side_effect=record):
            self.run_chain()
        for verb in ("add", "checkout", "submodule", "remove"):
            selected = [command for command in commands if verb in command]
            self.assertTrue(selected, verb)
            for command in selected:
                self.assertIn(["-c", "core.longpaths=true"],
                              [command[i:i + 2] for i in range(len(command) - 1)])

    def test_cleanup_failure_saves_manifest_and_preserves_results(self):
        original = subprocess.check_output
        commands = []

        def fail_remove(command, **options):
            commands.append(command)
            if "worktree" in command and "remove" in command:
                raise subprocess.CalledProcessError(255, command, stderr="Filename too long")
            return original(command, **options)

        with patch.object(chain.subprocess, "check_output", side_effect=fail_remove):
            results, _, writes = self.run_chain()
        self.assertEqual([result.outcome for result in results], ["measured"] * 3)
        self.assertTrue(writes[-1]["cleanup"].startswith("failed:"))
        self.assertIn("Filename too long", (self.args.out / "cleanup.log").read_text())
        self.assertTrue(any("prune" in command for command in commands))
        self.assertFalse(list(self.args.out.glob("nightly-*")))

    def test_directory_cleanup_failure_still_saves_manifest(self):
        with patch.object(chain, "remove_output", side_effect=OSError("directory busy")):
            results, _, writes = self.run_chain()
        self.assertEqual([result.outcome for result in results], ["measured"] * 3)
        self.assertIn("directory busy", writes[-1]["cleanup"])
        self.assertIn("directory busy", (self.args.out / "cleanup.log").read_text())

    def test_cleanup_warning_does_not_change_lane_exit_status(self):
        plan = self.root / "plan.json"
        plan.write_text(json.dumps(dict(night="2026-10-18", started=self.clock.now().isoformat(),
                                       lanes={"20E8": dict(lane="0/2", previous=None,
                                                          steps=[vars(self.steps()[0])])})))
        command = ["scoped_chain", "run", "--plan", str(plan), "--board", "20E8", "--lane", "0/2",
                   "--until", "07:00", "--repo", str(self.repo), "--data", str(self.args.data),
                   "--out", str(self.args.out), "--suite", "suite", "-", "-",
                    "--selftest", "selftest-replay"]
        original = subprocess.check_output

        def fail_remove(command, **options):
            if "worktree" in command and "remove" in command:
                raise subprocess.CalledProcessError(255, command)
            return original(command, **options)

        for build, capture, selftest, expected in ((0, False, 0, 0), (1, False, 0, 1),
                                                  (0, True, 0, 1), (0, False, 1, 1)):
            clock = Clock(22, 30)
            replay = Replay(self.root, clock, builds=(build,), captures=(capture,), selftest_code=selftest)
            with self.subTest(build=build, capture=capture, selftest=selftest), \
                    patch.object(sys, "argv", command), \
                    patch.dict(os.environ, _AUTANA_DEVICE_LOCK_TOKEN="outer-hold"), \
                    patch("layout_measure.capture_board", return_value=MACHINE), \
                    patch.object(chain, "Clock", return_value=clock), \
                    patch.object(chain, "run_stamped", replay), \
                    patch.object(chain.subprocess, "check_output", side_effect=fail_remove):
                self.assertEqual(chain.main(), expected)
                manifest = lnt.read_last_night(self.args.data, "20E8")
                self.assertTrue(manifest["cleanup"].startswith("failed:"))

    def test_image_hash_excludes_metadata_but_includes_code_and_assets(self):
        build = self.root / "image"
        build.mkdir()
        trailer_bytes = IMAGE_TEST_PADDING_BYTES + IMAGE_CHECKSUM_BYTES + IMAGE_SHA256_BYTES
        app = bytearray(app_image(trailer=bytes(trailer_bytes)))
        (build / "launcher.bin").write_bytes(app)
        (build / "assets.bin").write_bytes(b"assets")
        expected = chain.image_hash(build)
        for index in (chain.APP_DESC_OFFSET, chain.APP_DESC_OFFSET + chain.APP_DESC_BYTES - 1,
                      *range(len(app) - trailer_bytes, len(app))):
            app[index] ^= 1
        (build / "launcher.bin").write_bytes(app)
        self.assertEqual(chain.image_hash(build), expected)
        app[len(app) - trailer_bytes - 1] ^= 1
        (build / "launcher.bin").write_bytes(app)
        self.assertNotEqual(chain.image_hash(build), expected)
        changed = chain.image_hash(build)
        (build / "assets.bin").write_bytes(b"other")
        self.assertNotEqual(chain.image_hash(build), changed)

    def test_plan_parity_anchor_order_baseline_and_carryover(self):
        tip = self.commits[-1]
        for lane in range(2):
            baseline = chain.plan_steps(self.repo, tip, None, lane, 2)
            self.assertEqual(len(baseline), 1)
            self.assertEqual(baseline[0].position, 6 if lane == 0 else 5)
            previous = {"last_measured": self.commits[lane + 1]}
            steps = chain.plan_steps(self.repo, tip, previous, lane, 2)
            self.assertEqual(steps[0].role, "anchor")
            self.assertEqual(steps[0].commit, previous["last_measured"])
            self.assertTrue(all(step.position % 2 == lane for step in steps[1:]))
            self.assertEqual([s.position for s in steps], sorted(s.position for s in steps))
            self.assertEqual(steps[-1].pr, 105 if lane == 0 else 104)
            carried = chain.plan_steps(self.repo, tip, previous, lane, 2)
            self.assertEqual(steps, carried)

    def test_plan_uses_first_parent_and_merge_pr(self):
        chain.git(self.repo, "checkout", "-qb", "side", self.commits[2])
        (self.repo / "side").write_text("side")
        chain.git(self.repo, "add", ".")
        chain.git(self.repo, "commit", "-qm", "side commit")
        side = chain.git(self.repo, "rev-parse", "HEAD")
        chain.git(self.repo, "checkout", "--detach", self.commits[-1])
        chain.git(self.repo, "merge", "--no-ff", "-m", "Merge pull request #999", "side")
        steps = chain.plan_steps(self.repo, "HEAD", {"last_measured": self.commits[0]}, 1, 2)
        self.assertNotIn(side, [s.commit for s in steps])
        self.assertEqual((steps[-1].position, steps[-1].pr), (7, 999))

    def test_shared_tip_and_explicit_range_limit(self):
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=["20E8", "2124"], data=self.args.data,
                               out=self.root / "plan.json", queue_at="22:00", until="07:00", from_sha=self.commits[1],
                               max_steps=2, selftest_board="2124")
        chain.plan(args, self.clock)
        plan = json.loads(args.out.read_text())
        self.assertEqual(plan["tip"], self.commits[-1])
        self.assertEqual(self.clock.sleeps, [])
        for board, lane in plan["lanes"].items():
            reserve = chain.SELFTEST_ALLOWANCE_SECONDS if board == args.selftest_board else 0
            self.assertEqual(lane["estimate"], len(lane["steps"]) * chain.FIRST_STEP_SECONDS + reserve)
            self.assertEqual(len(lane["steps"]), 2)
            self.assertLessEqual(lane["steps"][-1]["position"], 6)
        args.queue_at = None
        self.clock.sleeps.clear()
        chain.plan(args, self.clock)
        self.assertEqual(self.clock.sleeps, [])
        previous = {"last_measured": self.commits[0]}
        limited = chain.select_steps(self.repo, "HEAD", previous, 0, 2, self.commits[1], 1)
        self.assertEqual([step.role for step in limited], ["anchor", "new"])
        self.assertEqual(limited[-1].commit, self.commits[1])

    def test_equal_image_skips_but_anchor_measures(self):
        steps = self.steps(indices=(1, 1, 1), anchor=True)
        results, replay, writes = self.run_chain(steps)
        self.assertEqual([r.outcome for r in results], ["measured", "skipped_same_image", "skipped_same_image"])
        self.assertEqual(sum("suite" in cmd for cmd in replay.calls), 1)
        self.assertEqual([len(w["steps"]) for w in writes], [0, 1, 2, 3, 3])
        previous = writes[-1]
        self.args.previous = previous
        results, _, _ = self.run_chain([chain.Step(steps[0].commit, 101, "anchor", 2)])
        self.assertEqual(results[0].outcome, "measured")

    def test_build_failure_continues_without_updating_hash(self):
        results, _, _ = self.run_chain(self.steps(indices=(1, 3, 1)), builds=(0, 1, 0))
        self.assertEqual([r.outcome for r in results], ["measured", "build_failed", "skipped_same_image"])

    def test_two_capture_failures_stop_and_success_resets_streak(self):
        results, _, writes = self.run_chain(captures=(True, True))
        self.assertEqual([r.outcome for r in results], ["capture_failed", "capture_failed", "not_run_stopped"])
        self.assertEqual(writes[-1]["stop_reason"], "capture_failures")
        results, _, _ = self.run_chain(captures=(True, False, True))
        self.assertEqual([r.outcome for r in results], ["capture_failed", "measured", "capture_failed"])

    def test_heavy_wait_is_recorded_without_inflating_build_or_worst_step(self):
        self.args.until = "02:00"
        results, _, writes = self.run_chain(heavy_wait=60 * 60)
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual([r.build_seconds for r in results], [60] * 3)
        self.assertEqual([s["wait_seconds"] for s in writes[-1]["steps"]], [60 * 60] * 3)

    def test_flash_rebuild_wait_is_recorded_without_inflating_worst_step(self):
        self.args.until = "02:00"
        results, _, writes = self.run_chain(capture_wait=(2 * 60 * 60, 0, 0))
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual([r.build_seconds for r in results], [60] * 3)
        self.assertEqual([s["wait_seconds"] for s in writes[-1]["steps"]], [2 * 60 * 60, 0, 0])

    def test_deadline_and_observed_worst_step(self):
        self.args.until = "22:52"
        results, _, _ = self.run_chain()
        self.assertEqual([r.outcome for r in results], ["not_run_window"] * 3)
        self.args.until = "23:33"
        results, _, _ = self.run_chain(durations=(30 * 60, 60))
        self.assertEqual([r.outcome for r in results], ["measured", "measured", "not_run_window"])

    def test_selftest_failure_continues_inside_hold(self):
        self.args.selftest = ["selftest-replay"]
        results, replay, writes = self.run_chain(selftest_code=1)
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual(replay.boundaries, [datetime(2026, 10, 18, 22, 30, tzinfo=timezone.utc)])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "failed")
        self.assertTrue(Path(writes[-1]["selftest"]["report"]).is_file())

    def test_selftest_runs_first_without_waiting_and_respects_deadline(self):
        self.args.selftest = ["selftest-replay"]
        results, replay, writes = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(replay.boundaries, [Clock(22, 30).now()])
        self.assertEqual(replay.timeouts[0][0], ["selftest-replay"])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "passed")
        self.assertEqual(results[0].outcome, "measured")
        self.clock = Clock(22, 30)
        self.args.until = "00:00"
        _, replay, writes = self.run_chain()
        self.assertEqual(replay.boundaries, [])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "not_run")

    def test_partial_manifest_retains_board_hash(self):
        self.args.previous = dict(last_measured=self.commits[1], machine=MACHINE,
                                  last_image_hash="previous-hash", steps=[])
        self.args.until = "22:31"
        _, _, writes = self.run_chain()
        self.assertEqual(writes[-1]["last_image_hash"], "previous-hash")
        self.assertEqual(writes[-1]["last_measured"], self.commits[1])

    def test_lnt_shape_seconds_quarter_and_latest_night(self):
        report = lnt.lnt_report(MACHINE, "20E8", self.commits[0], None, "anchor", "2026-10-18",
                                "build-diag", "hash", "start", "end", {"suite/frame quarter=landscape": [412]})
        self.assertEqual(set(report), {"format_version", "machine", "run", "tests"})
        self.assertEqual(report["format_version"], "2")
        self.assertEqual(report["machine"], {"name": MACHINE, "board": "20E8", "image": lnt.IMAGE})
        self.assertEqual(set(report["run"]), {"start_time", "end_time", "llvm_project_revision", "pr",
                                            "build_id", "image_hash", "night", "role"})
        self.assertEqual(report["tests"], [{"name": "suite/frame quarter=landscape", "execution_time": [0.000412]}])
        self.assertIsNone(lnt.read_last_night(self.args.data, "20E8"))
        for night in ("2026-10-18-2230", "2026-10-18-0900"):
            chain.write_json(self.args.data / "scoped/20E8" / night / "night.json", {"night": night})
        self.assertEqual(lnt.read_last_night(self.args.data, "20E8")["night"], "2026-10-18-2230")

    def test_attempted_tail_not_planned_again(self):
        for outcome in ("skipped_same_image", "build_failed"):
            with self.subTest(outcome=outcome):
                self.args.previous = dict(last_measured=self.commits[1], last_attempted=self.commits[1],
                                          machine=MACHINE, last_image_hash="same")
                with patch.object(chain, "image_hash", return_value="same"):
                    _, _, writes = self.run_chain(self.steps(indices=(3, 5)),
                                                   builds=(1, 1) if outcome == "build_failed" else ())
                previous = writes[-1]
                self.assertEqual([row["outcome"] for row in previous["steps"]], [outcome] * 2)
                self.assertEqual(previous["last_attempted"], self.commits[5])
                self.assertEqual(previous["last_measured"], self.commits[1])
                planned = chain.plan_steps(self.repo, "HEAD", previous, 0, 2)
                self.assertEqual([(step.commit, step.role) for step in planned], [(self.commits[1], "anchor")])

    def test_explicit_earlier_range_preserves_cursors_and_hash(self):
        self.args.previous = dict(last_measured=self.commits[3], last_attempted=self.commits[5],
                                  machine=MACHINE, last_image_hash="newest")
        selected = chain.select_steps(self.repo, "HEAD", self.args.previous, 0, 2, self.commits[0])
        self.assertEqual(sum(step.commit == self.commits[3] for step in selected), 1)
        _, _, writes = self.run_chain(self.steps(indices=(1,)))
        for key in ("last_measured", "last_attempted", "last_image_hash"):
            self.assertEqual(writes[-1][key], self.args.previous[key])

    def test_same_day_nights_have_distinct_directories(self):
        self.run_chain(self.steps(indices=(1,)))
        self.clock.value += timedelta(hours=1)
        self.run_chain(self.steps(indices=(3,)))
        paths = list((self.args.data / "scoped/20E8").glob("*/night.json"))
        self.assertEqual(len(paths), 2)
        latest = lnt.read_last_night(self.args.data, "20E8")
        self.assertEqual(latest["night"], "2026-10-18")
        self.assertEqual(latest["last_measured"], self.commits[3])

    def test_flashing_different_build_fails_capture(self):
        with self.capture_with(build_id="different-diag"):
            results, _, _ = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(results[0].outcome, "capture_failed")
        self.assertIn("differs from hashed build", (self.args.out / "00-error.log").read_text())

    def test_capture_failure_retries_cursor_and_fourth_step_runs(self):
        self.args.previous = dict(last_measured=self.commits[0], last_attempted=self.commits[0])
        results, _, writes = self.run_chain(self.steps(indices=(1,)), captures=(True,))
        self.assertEqual(results[0].outcome, "capture_failed")
        self.assertEqual(writes[-1]["last_attempted"], self.commits[0])
        planned = chain.plan_steps(self.repo, "HEAD", writes[-1], 0, 2)
        self.assertIn(self.commits[1], [step.commit for step in planned])
        results, _, _ = self.run_chain(self.steps(indices=(1, 2, 3, 4)), captures=(True, False, True, False))
        self.assertEqual([r.outcome for r in results], ["capture_failed", "measured", "capture_failed", "measured"])

    def test_stopped_reason_survives_later_window(self):
        self.args.until = "23:20"
        results, _, writes = self.run_chain(self.steps(indices=(1, 2, 3, 4)),
                                           captures=(True, True), durations=(60, 26 * 60))
        self.assertEqual([r.outcome for r in results],
                         ["capture_failed", "capture_failed", "not_run_stopped", "not_run_stopped"])
        self.assertEqual(writes[-1]["stop_reason"], "capture_failures")

    def test_exact_budget_plan_clock_and_night(self):
        self.args.started = self.clock.now().isoformat()
        self.args.night = "planned-night"
        self.args.until = "22:53"
        for extra, outcome in ((0, "measured"), (1, "not_run_window")):
            self.clock = Clock(22, 30)
            self.clock.sleep(extra)
            results, _, writes = self.run_chain(self.steps(indices=(1,)), durations=(22 * 60,))
            self.assertEqual(results[0].outcome, outcome)
            self.assertEqual(writes[-1]["night"], "planned-night")
        self.clock = Clock(0, 0)
        self.clock.value += timedelta(days=1)
        self.args.until = "23:00"
        results, _, _ = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(results[0].outcome, "not_run_window")

    def test_window_after_build_and_build_lock_timeout_preserves_cursor(self):
        self.args.until = "23:00"
        for duration, code in ((61 * 60, 0), (8 * 60, autana_config.EXIT_BUSY)):
            self.clock = Clock(22, 30)
            results, replay, writes = self.run_chain(self.steps(indices=(1,)), durations=(duration,), builds=(code,))
            self.assertEqual(results[0].outcome, "not_run_window")
            self.assertIsNone(writes[-1]["last_attempted"])
            self.assertFalse(any("suite" in cmd for cmd in replay.calls))

    def test_failed_selftest_log_exists_and_remaining_steps_continue(self):
        self.args.selftest = ["selftest-replay"]
        with patch.object(chain, "REPORT_LINE", re.compile(r"never matches")):
            results, _, writes = self.run_chain(builds=(1, 0, 0), captures=(True, False), selftest_code=1)
        self.assertEqual([result.outcome for result in results],
                         ["build_failed", "capture_failed", "measured"])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "failed")
        self.assertTrue(Path(writes[-1]["selftest"]["report"]).is_file())

    def test_plan_delegates_window_and_writes_skip_summary(self):
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=["20E8"],
            data=self.args.data, out=self.root / "plan.json", queue_at="22:30", until="06:00",
            from_sha=None, max_steps=None, selftest_board="20E8")
        summary = self.root / "summary"
        with patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)), \
                patch.object(chain, "night_window", return_value=None) as window, \
                patch.object(chain, "plugged_boards") as discover:
            chain.plan(args, self.clock)
        window.assert_called_once_with(args.queue_at, args.until, scheduled=True, clock=self.clock)
        discover.assert_not_called()
        self.assertEqual(json.loads(args.out.read_text())["lanes"], {})
        self.assertEqual(summary.read_text(), chain.SKIPPED + "\n")

    def test_selftest_limits_timeout_and_saves_metadata(self):
        self.args.selftest = ["selftest-replay"]
        self.args.until = "00:30"
        self.args.started = Clock(22, 0).now().isoformat()
        _, replay, writes = self.run_chain(steps=[])
        saved = writes[1]["selftest"]
        self.assertEqual(saved["outcome"], "passed")
        self.assertEqual(saved["started"], replay.boundaries[0].isoformat())
        self.assertEqual(saved["seconds"], 60)
        self.assertEqual(saved["report"], str((self.args.out / "selftest.log").resolve()))
        self.assertEqual(replay.timeouts[0][1], chain.SELFTEST_ALLOWANCE_SECONDS)

    def test_capture_identity_rows_start_and_empty_capture(self):
        for previous in (None, dict(machine=MACHINE)):
            self.args.previous = previous
            with self.capture_with(board=OTHER_MACHINE):
                results, _, _ = self.run_chain(self.steps(indices=(1,)))
            self.assertEqual(results[0].outcome, "capture_failed")
        self.args.previous = dict(machine=OTHER_MACHINE)
        results, _, _ = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(results[0].outcome, "capture_failed")
        self.args.previous = dict(machine=MACHINE)
        self.clock = Clock(22, 30)
        results, replay, writes = self.run_chain(self.steps(indices=(1,)))
        manifest = self.args.data / "scoped/20E8" / Clock(22, 30).now().strftime("%Y-%m-%d-%H%M") / "night.json"
        report = json.loads((manifest.parent / results[0].report).read_text())
        self.assertEqual(report["machine"]["name"], MACHINE)
        self.assertEqual(report["run"]["start_time"], (Clock(22, 30).now() + timedelta(seconds=60)).isoformat())
        self.assertTrue(all(test["name"].startswith("suite/") for test in report["tests"]))
        with self.capture_with(suites={}):
            results, _, _ = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(results[0].outcome, "capture_failed")
        self.assertTrue(all(timeout > chain.BUILD_TIMEOUT_SECONDS for cmd, timeout in replay.timeouts if "build" in cmd))
        self.assertTrue(all(timeout > chain.CAPTURE_TIMEOUT_SECONDS for cmd, timeout in replay.timeouts if "suite" in cmd))
        suites = [cmd for cmd, _ in replay.timeouts if "suite" in cmd]
        self.assertTrue(suites)
        self.assertEqual(suites[0][suites[0].index("--runs") + 1], "1")

    def test_invalid_history_lane_and_range(self):
        for previous in ({"last_measured": "missing"}, {"last_attempted": "missing"}):
            with self.assertRaisesRegex(ValueError, next(iter(previous))):
                chain.plan_steps(self.repo, "HEAD", previous, 0, 2)
        with self.assertRaises(ValueError):
            chain.plan_steps(self.repo, "HEAD", None, 2, 2)
        with self.assertRaises(ValueError):
            chain.select_steps(self.repo, "HEAD", None, 0, 2, max_steps=-1)
        chain.git(self.repo, "checkout", "-qb", "off", self.commits[0])
        chain.git(self.repo, "commit", "--allow-empty", "-qm", "off history")
        off = chain.git(self.repo, "rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "first-parent"):
            chain.select_steps(self.repo, self.commits[-1], None, 0, 2, from_sha=off)
        selected = chain.select_steps(self.repo, self.commits[-1], {"last_measured": self.commits[0]}, 0, 2, self.commits[0])
        self.assertEqual([s.position for s in selected], [1, 2, 4, 6])

    def test_invalid_images_fail_build_and_hash_length(self):
        valid = app_image()
        oversized = bytearray(valid)
        oversized[chain.IMAGE_HEADER_BYTES + chain.SEGMENT_DATA_LENGTH_OFFSET:chain.APP_DESC_OFFSET] = (len(valid) * 2).to_bytes(chain.SEGMENT_DATA_LENGTH_BYTES, "little")
        no_descriptor = bytearray(valid)
        no_descriptor[chain.IMAGE_SEGMENT_COUNT_OFFSET] = 0
        original = Replay.__call__
        for image, message in ((b"short", "too short"), (oversized, "exceeds image"), (no_descriptor, "omit app descriptor")):
            def truncated(replay, command, *args, **kwargs):
                result = original(replay, command, *args, **kwargs)
                if "build" in command:
                    project = Path(command[command.index("--project") + 1])
                    (project / "launcher/build.diag/launcher.bin").write_bytes(image)
                return result
            with patch.object(Replay, "__call__", truncated):
                results, _, _ = self.run_chain(self.steps(indices=(1,)))
            self.assertEqual(results[0].outcome, "build_failed")
            self.assertIn(message, next(self.args.out.glob("*-build.log")).read_text())
        results, _, writes = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(len(results[0].image_hash), chain.IMAGE_HASH_DIGITS)
        self.assertEqual(writes[0]["cleanup"], "pending")
        self.assertEqual({key: writes[-1]["steps"][0][key] for key in ("commit", "pr", "role", "position")},
                         vars(self.steps(indices=(1,))[0]))
        chain.write_json(self.args.data / "scoped/20E8/not-a-night/night.json", {"night": "invalid"})
        self.assertNotEqual(lnt.read_last_night(self.args.data, "20E8")["night"], "invalid")

    def test_main_refusals_replan_and_wait_rollover(self):
        planned = dict(tip=self.commits[-1], night="2026-10-18", started=self.clock.now().isoformat(),
                       lanes={"20E8": dict(lane="0/2", previous=None, steps=[])})
        path = self.root / "plan.json"
        path.write_text(json.dumps(planned))
        base = ["chain", "run", "--plan", str(path), "--board", "20E8", "--lane", "0/2",
                "--until", "07:00", "--data", str(self.args.data), "--out", str(self.args.out)]
        suite = ["--suite", "suite", "-", "-"]
        for command, token in ((base + suite, ""), (base, "token"),
                               (base + suite + ["--selftest-at", "01:00"], "token"),
                               (base + suite + ["--lane", "1/2"], "token")):
            with patch.object(sys, "argv", command), patch.dict(os.environ, _AUTANA_DEVICE_LOCK_TOKEN=token), \
                    patch.object(chain, "run_chain", return_value=[]), \
                    patch.object(chain, "read_last_night", return_value={"selftest": {"outcome": "not_run"}}), \
                    self.assertRaises(SystemExit) as caught:
                chain.main()
            self.assertEqual(caught.exception.code, 2)
        with patch.object(sys, "argv", base + suite + ["--max-steps", "0"]), \
                patch.dict(os.environ, _AUTANA_DEVICE_LOCK_TOKEN="token"), \
                patch.object(chain, "select_steps", return_value=[]) as select, \
                patch.object(chain, "run_chain", return_value=[]), \
                patch.object(chain, "read_last_night", return_value={"selftest": {"outcome": "not_run"}}):
            self.assertEqual(chain.main(), 0)
            self.assertEqual(select.call_args.args[-1], 0)
        with patch.object(sys, "argv", ["chain", "wait-seconds", "--until", "22:00"]), \
                patch.object(chain, "Clock", return_value=self.clock), patch("sys.stdout", new_callable=io.StringIO) as output:
            chain.main()
        self.assertEqual(int(output.getvalue()), int(timedelta(hours=23, minutes=30).total_seconds()))

    def test_plan_reads_each_boards_manifest(self):
        previous = {"20E8": {"last_measured": self.commits[0]}, "2124": {"last_measured": self.commits[2]}}
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=list(previous), data=self.args.data,
                               out=self.root / "plan.json", queue_at=None, from_sha=None, max_steps=None,
                               selftest_board=None)
        with patch.object(chain, "read_last_night", side_effect=lambda data, board: previous[board]):
            chain.plan(args, self.clock)
        planned = json.loads(args.out.read_text())
        self.assertEqual({board: lane["previous"] for board, lane in planned["lanes"].items()}, previous)

    def test_plan_without_boards_takes_every_plugged_board(self):
        listing = {"boards": [{"board": "44:BD:8D:78:20:E8", "port": "COM3"},
                              {"board": "44:BD:8D:78:21:24", "port": None},
                              {"board": "90:70:69:FE:A3:08", "port": "COM5"}]}
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=None, data=self.args.data,
                               out=self.root / "plan.json", queue_at=None, from_sha=None, max_steps=None,
                               selftest_board=None)
        real = chain.subprocess.check_output
        def devices(command, **kwargs):
            return json.dumps(listing) if "devices" in command else real(command, **kwargs)
        with patch.object(chain.subprocess, "check_output", side_effect=devices), \
                patch.object(chain, "read_last_night", return_value=None), \
                patch("sys.stdout", new_callable=io.StringIO) as output:
            chain.plan(args, self.clock)
        planned = json.loads(args.out.read_text())
        self.assertEqual({board: lane["lane"] for board, lane in planned["lanes"].items()},
                         {"20E8": "0/2", "A308": "1/2"})
        self.assertEqual(output.getvalue().splitlines(), ["SCOPED_BOARDS=20E8 A308", "SELFTEST_BOARD=20E8"])
        listing["boards"] = [dict(board, port=None) for board in listing["boards"]]
        args.boards = None
        with patch.object(chain.subprocess, "check_output", side_effect=devices), \
                self.assertRaises(SystemExit):
            chain.plan(args, self.clock)

    def test_cleanup_fallback_deletes_tree_and_copies_records(self):
        original_git, original_replay, original_remove = chain.git, Replay.__call__, chain.remove_output
        def records(replay, command, *args, **kwargs):
            result = original_replay(replay, command, *args, **kwargs)
            if "build" in command:
                project = Path(command[command.index("--project") + 1])
                record = project / ".records/device/record.json"
                record.parent.mkdir(parents=True, exist_ok=True)
                record.write_text("device record")
            return result
        def fail_remove(repo, *words):
            if "worktree" in words and "remove" in words:
                raise subprocess.CalledProcessError(1, words)
            return original_git(repo, *words)
        def remove(path):
            path = Path(path)
            if path.name != "tree":
                self.assertFalse((path / "tree").exists())
            original_remove(path)
        with patch.object(chain, "git", side_effect=fail_remove), patch.object(Replay, "__call__", records), \
                patch.object(chain, "remove_output", side_effect=remove):
            _, _, writes = self.run_chain(self.steps(indices=(1,)))
        self.assertFalse(list(self.args.out.glob("nightly-*")))
        self.assertEqual((self.args.out / "device/record.json").read_text(), "device record")
        self.assertEqual(writes[0]["cleanup"], "pending")

    def test_latest_build_and_missing_booted_build(self):
        for name, stamp in (("build.new", 300), ("build.old", 100), ("build.middle", 200)):
            path = self.repo / "launcher" / name / "build_id.txt"
            path.parent.mkdir(parents=True)
            path.write_text(name)
            os.utime(path, (stamp, stamp))
        self.assertEqual(layout_measure.latest_build(self.repo).name, "build.new")
        with self.assertRaises(RuntimeError):
            layout_measure.booted_build_id(self.root / "missing", [])

    def test_generic_table_preserves_stage_rows(self):
        capture = self.root / "capture.log"
        text = "stage=frame paused=0 quarter=portrait mean_us=412\nstage=frame paused=1 quarter=landscape mean_us=413"
        capture.write_text(text)
        table = make_table("-", capture, self.root / "table.md")
        self.assertEqual(parse_report(table), parse_timings(text))
        self.assertEqual(len(parse_report(table)), 2)

    def test_full_serial_board_selection(self):
        self.args.board = MACHINE.lower()
        results, _, _ = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(results[0].outcome, "measured")

    def test_squash_pr_is_last_subject_match(self):
        chain.git(self.repo, "commit", "--allow-empty", "-qm", "fix (#7) and (#8) (#999)")
        self.assertEqual(chain.history_steps(self.repo, "HEAD")[-1].pr, 999)

    def test_lane_info_reports_share_and_rejects_unknown_board(self):
        plan = self.root / "plan.json"
        plan.write_text(json.dumps({"lanes": {"20E8": {"lane": "0/2", "estimate": 42}}}))
        for board, expected in (("20E8", 0), (MACHINE.lower(), 0), ("missing", 2)):
            result = subprocess.run([sys.executable, str(PERF / "nightly/scoped_chain.py"),
                                     "lane-info", "--plan", str(plan), "--board", board],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stderr)
            self.assertIn("0/2 42" if expected == 0 else "unknown board", result.stdout + result.stderr)


class LaneSettingsTests(unittest.TestCase):
    def test_lanes_run_after_prepare_failure_and_wait_for_failed_lane(self):
        workflow = (PERF.parents[2] / ".github/workflows/nightly-perf.yml").read_text()
        section = workflow.split("      - name: Run each board lane and wait for all\n", 1)[1].split("      - name:", 1)[0]
        self.assertIn("if: always() && env.SCOPED_BOARDS != ''", section)
        script = section.split("        run: |\n", 1)[1]
        script = "\n".join(line[10:] for line in script.splitlines())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lane = root / "launcher/tools/perf/nightly/lane.sh"
            lane.parent.mkdir(parents=True)
            lane.write_text('echo "$1" >> "$EVENTS"\n[ "$1" != first ]\n')
            events = root / "events"
            env = dict(os.environ, GITHUB_WORKSPACE=directory, NIGHTLY_SUITES="", SCOPED_BOARDS="first second",
                       PERF_OUT=directory, PERF_DATA=directory, EVENTS=str(events))
            result = subprocess.run(["bash", "-eo", "pipefail", "-c",
                'cygpath() { echo "$2"; }; ' + script], cwd=root, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(sorted(events.read_text().splitlines()), ["first", "second"])
        for step in ("Summarize board outcomes", "Publish night manifests and LNT reports"):
            self.assertIn("if: always()", workflow.split("- name: " + step, 1)[1].split("run:", 1)[0])

    def test_lane_wait_uses_plan_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            plan = Path(directory) / "plan.json"
            plan.write_text(json.dumps(dict(started=Clock(22, 30).now().isoformat())))
            command = ["chain", "wait-seconds", "--until", "07:00", "--plan", str(plan)]
            clock = Clock(8, 30)
            clock.value += timedelta(days=1)
            with patch.object(sys, "argv", command), patch.object(chain, "Clock", return_value=clock), \
                    patch("sys.stdout", new_callable=io.StringIO) as output:
                self.assertEqual(chain.main(), 0)
            self.assertEqual(output.getvalue(), "0\n")

    def test_lane_uses_configured_window_and_selftest_command(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "scripts/lib/python.sh"
            library.parent.mkdir(parents=True)
            library.write_text('find_python() { printf "%s\\n" "$TEST_PYTHON"; }\n')
            driver = root / "launcher/tools/perf/nightly/scoped_chain.py"
            driver.parent.mkdir(parents=True)
            driver.write_text('import sys\nprint("123" if sys.argv[1] == "wait-seconds" else "0/1 42")\n')
            plan = root / "plan.json"
            plan.write_text(json.dumps({"lanes": {"board": {"lane": "0/1", "estimate": 42}}}))
            script = PERF / "nightly/lane.sh"
            command = r"""
autana() { printf '%s\n' "$@"; printf 'BOARD_CI_LOG=%s\n' "${BOARD_CI_LOG:-}"; }
export -f autana
bash "$1" board "$2" data out -- --suite suite - "table with spaces"
"""
            env = dict(os.environ, TEST_PYTHON=sys.executable, SELFTEST_BOARD="board",
                       WINDOW_END="05:45")
            result = subprocess.run(["bash", "-c", command, "lane-test", script.as_posix(), plan.as_posix()],
                                    cwd=root, env=env, capture_output=True, text=True, check=True)
            args = result.stdout.splitlines()
            self.assertEqual(args[args.index("--suite") + 1:args.index("--suite") + 4],
                             ["suite", "-", "table with spaces"])
            self.assertEqual(args[args.index("--estimate") + 1], "42")
            env["SELFTEST_BOARD"] = "another-board"
            env["BOARD_CI_LOG"] = "inherited-log"
            other = subprocess.run(["bash", "-c", command, "lane-test", script.as_posix(), plan.as_posix()],
                                   cwd=root, env=env, capture_output=True, text=True, check=True).stdout.splitlines()
            self.assertNotIn("--selftest", other)
            self.assertNotIn("--selftest-at", other)
            self.assertIn("BOARD_CI_LOG=", other)
            env["SELFTEST_BOARD"] = "board"

            self.assertNotIn("--selftest-at", args)
            self.assertIn("--selftest", args)
            for option, setting in (("--until", "WINDOW_END"),):
                with self.subTest(option=option):
                    self.assertEqual(args[args.index(option) + 1], env[setting])

    def test_workflow_queue_input_can_start_immediately(self):
        workflow = (PERF.parents[2] / ".github/workflows/nightly-perf.yml").read_text()
        start = workflow.index("          args=()")
        end = workflow.index("\n      - name:", start)
        script = "\n".join(line[10:] for line in workflow[start:end].splitlines())
        command = 'driver() { printf "%s\\n" "$@"; }; PY=driver; ' + script
        for event, requested, expected in (("schedule", "", "22:30"),
                                           ("workflow_dispatch", "", None),
                                           ("workflow_dispatch", "21:15", "21:15")):
            with self.subTest(event=event, requested=requested), tempfile.TemporaryDirectory() as tmp:
                # The plan's output goes to the job environment, not the log.
                github_env = Path(tmp) / "github_env"
                env = dict(os.environ, GITHUB_EVENT_NAME=event, DISPATCH_QUEUE_AT=requested,
                           QUEUE_AT="22:30", FROM_SHA="", MAX_STEPS="", GITHUB_ENV=str(github_env))
                subprocess.run(["bash", "-c", command], env=env, capture_output=True, text=True, check=True)
                args = github_env.read_text().splitlines()
                self.assertNotIn("--boards", args)
                if expected is None:
                    self.assertNotIn("--queue-at", args)
                else:
                    self.assertEqual(args[args.index("--queue-at") + 1], expected)
                self.assertNotIn("--suite", args)


if __name__ == "__main__":
    unittest.main()

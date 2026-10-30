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
import yaml
from unittest.mock import Mock, call, patch

PERF = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PERF))
sys.path.insert(0, str(PERF.parents[2] / "scripts/device/tests"))
sys.path.insert(0, str(PERF.parents[2] / "scripts/ci/tests"))
import port_guard
from test_local_schedule import Clock as ScheduleClock
from scripts.lib.local_schedule import QUEUE_POLL_SECONDS
import local_schedule as schedule
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
    def __init__(self, root, clock, builds=(), captures=(), durations=(), selftest_code=0, heavy_wait=0, capture_wait=0,
                 selftest_complete=True):
        super().__init__(root)
        self.selftest_complete = selftest_complete
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
            ended = [(60, layout_measure.SELFTEST_COMPLETE_LINE)] if self.selftest_complete else []
            return self.selftest_code, [(0, "report: first.md"), *ended, (60, "report: selftest.md")], 60
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
        marker_env = patch.dict(os.environ, _AUTANA_HEAVY_LOCK_ROOT=str(Path(self.temp.name) / "heavy"))
        marker_env.start()
        self.addCleanup(marker_env.stop)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        cache = patch.object(autana_config, "user_cache_dir", return_value=self.root / "cache")
        cache.start()
        self.addCleanup(cache.stop)
        for owner in (chain, layout_measure):
            guard = patch.object(owner, "launch_process_tree",
                                 side_effect=AssertionError("test attempted a real process launch"))
            guard.start()
            self.addCleanup(guard.stop)
        stop = patch.object(chain, "stop_process_tree")
        self.stop_tree = stop.start()
        self.addCleanup(stop.stop)
        close = patch.object(chain, "close_process_tree")
        close.start()
        self.addCleanup(close.stop)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        chain.git(self.repo, "init")
        chain.git(self.repo, "config", "user.email", "test@example.invalid")
        chain.git(self.repo, "config", "user.name", "Test")
        chain.git(self.repo, "config", "core.hooksPath", str(self.root / "no-hooks"))
        plugged = patch.object(chain, "plugged_boards", return_value=["20E8", "2124"])
        self.plugged_patch = plugged
        plugged.start()
        self.addCleanup(plugged.stop)
        # The plan lists this machine's boards; a test gives it some, or none.
        self.local_patch = patch.object(chain, "local_boards", return_value=[])
        self.local_patch.start()
        self.addCleanup(self.local_patch.stop)
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
                                    board="20E8", lane="0/2", previous=None,
                                    suite=[("suite", "-", "-")], selftest=None)

    def before_cutoff(self, scans):
        """Puts the test clock `scans` fleet scans before the night's BOARD_LAST_START."""
        self.clock.value = schedule.night_cutoff(self.clock.now()) - timedelta(seconds=scans * chain.RESCAN_SECONDS)

    def window(self, *listed):
        """What a plan hands its night: the clock's BOARD_LAST_START as the cut-off, and the boards it listed."""
        return dict(cutoff=schedule.night_cutoff(self.clock.now()).isoformat(), listed=list(listed))

    @staticmethod
    def row(board, **fields):
        """A board as the fleet lists it, free unless `fields` say otherwise."""
        return dict(dict(board=board, port="COM5", state="unlocked", holder=None, waiting=[]), **fields)

    def fleet(self, *listings):
        """Each fleet scan answers the next (rows, unreachable hosts) listing; the last one repeats."""
        answers = iter(listings)
        last = [([], [])]

        def listing():
            last[0] = next(answers, last[0])
            boards, unreachable = last[0]
            return dict(boards=list(boards), unreachable=list(unreachable))
        return patch.object(chain, "board_listing", side_effect=listing)

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
        for command in replay.calls:
            if "suite" in command:
                self.assertRegex(command[command.index("--wait") + 1], r"^[0-9]+$")
        self.assertEqual(chain.git(self.repo, "worktree", "list", "--porcelain").count("worktree "), 1)
        self.assertEqual(len(writes[-1]["steps"]), len(results))
        return results, replay, writes

    def capture_with(self, **fields):
        original = chain.run_flash
        def capture(*args, **kwargs):
            return dict(original(*args, **kwargs), **fields)
        return patch.object(chain, "run_flash", side_effect=capture)

    def test_lane_timestamps_and_timeline(self):
        marker = self.clock.now().isoformat()
        _, _, writes = self.run_chain()
        self.assertEqual(writes[-1]['started'], marker)
        self.assertEqual(writes[-1]['ended'], self.clock.now().isoformat())
        plan = self.root / 'plan.json'
        chain.write_json(plan, dict(marker=marker, started=marker, lanes={'20E8': {}}))
        gpu_end = (self.clock.now() + timedelta(hours=1)).isoformat()
        key = datetime.fromisoformat(marker).strftime(schedule.MARKER_KEY_FORMAT)
        chain.write_json(schedule.heavy_root() / 'night-steps' / key / 'gpu.json',
                         dict(workflow=schedule.DOC_IMAGES_WORKFLOW, marker=marker,
                              started=marker, ended=gpu_end, exit=0))
        summary = self.root / 'summary'
        chain.timeline(SimpleNamespace(plan=plan, data=self.args.data, summary=summary))
        record = json.loads((self.args.data / 'scoped/timeline' / (key + '.json')).read_text())
        self.assertEqual(record['free_at'], gpu_end)
        self.assertEqual([step['name'] for step in record['steps']],
                         ['gpu ' + schedule.DOC_IMAGES_WORKFLOW, 'board 20E8'])
        self.assertEqual(len(summary.read_text().splitlines()), 1)
        self.assertIn('host free', summary.read_text())
        self.assertIn('1h 06min after the marker', summary.read_text())

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

    def test_interrupted_lane_saves_its_manifest_and_cleans_up(self):
        def stopped(command, log, timeout, **options):
            raise KeyboardInterrupt
        with patch.dict(os.environ, _AUTANA_DEVICE_LOCK_TOKEN="outer-hold"), self.assertRaises(KeyboardInterrupt):
            chain.run_chain(self.steps(), self.args, stopped, self.clock)
        manifest = lnt.read_last_night(self.args.data, "20E8")
        self.assertEqual((manifest["stop_reason"], manifest["cleanup"]), ("interrupted", "passed"))
        self.assertTrue(manifest["ended"])
        self.assertEqual(chain.git(self.repo, "worktree", "list", "--porcelain").count("worktree "), 1)

    def test_captures_land_in_the_lanes_output(self):
        settings = []
        original = Replay.__call__

        def build(replay, command, log, timeout, **options):
            if "build" in command:
                project = Path(command[command.index("--project") + 1])
                settings.append(autana_config.load(project)["records"])
            return original(replay, command, log, timeout, **options)

        with patch.object(Replay, "__call__", build):
            self.run_chain()
        self.assertEqual(set(settings), {(self.args.out.resolve() / "device").as_posix()})

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
        self.assertFalse(list(self.args.out.parent.glob("nightly-*")))

    def test_directory_cleanup_failure_still_saves_manifest(self):
        with patch.object(chain, "remove_output", side_effect=OSError("directory busy")):
            results, _, writes = self.run_chain()
        self.assertEqual([result.outcome for result in results], ["measured"] * 3)
        self.assertIn("directory busy", writes[-1]["cleanup"])
        self.assertIn("directory busy", (self.args.out / "cleanup.log").read_text())

    def test_cleanup_warning_does_not_change_lane_exit_status(self):
        plan = self.root / "plan.json"
        plan.write_text(json.dumps(dict(night="2026-10-18", started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(),
                                       lanes={"20E8": dict(lane="0/2", previous=None,
                                                          steps=[vars(self.steps()[0])])})))
        command = ["scoped_chain", "run", "--plan", str(plan), "--board", "20E8", "--lane", "0/2",
                   "--repo", str(self.repo), "--data", str(self.args.data),
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

    def test_unplugged_before_second_step_keeps_result(self):
        with patch.object(chain, "plugged_boards", side_effect=[["20E8"], []]):
            results, _, writes = self.run_chain()
        self.assertEqual([result.outcome for result in results],
                         ["measured", "not_run_unplugged", "not_run_unplugged"])
        self.assertEqual(writes[-1]["stop_reason"], "unplugged")
        self.assertEqual(writes[-1]["last_measured"], self.commits[1])

    def test_error_after_unplug_does_not_count_as_capture_failure(self):
        with patch.object(chain, "plugged_boards", side_effect=[["20E8"], []]):
            results, _, writes = self.run_chain(captures=(True,))
        self.assertEqual([result.outcome for result in results], ["not_run_unplugged"] * 3)
        self.assertIsNone(writes[-1]["last_attempted"])

    def test_failed_build_before_unplug_keeps_failure(self):
        with patch.object(chain, "plugged_boards", side_effect=[["20E8"], []]):
            results, _, writes = self.run_chain(builds=(1,))
        self.assertEqual([result.outcome for result in results],
                         ["build_failed", "not_run_unplugged", "not_run_unplugged"])
        self.assertEqual(writes[-1]["last_attempted"], self.commits[1])

    def test_presence_query_failure_keeps_lane_running(self):
        with patch.object(chain, "plugged_boards", side_effect=OSError("devices failed")):
            results, _, _ = self.run_chain(self.steps((5,)))
        self.assertEqual(results[0].outcome, "measured")

    def test_unplugged_lane_preserves_failure_exit(self):
        path = self.root / "plan.json"
        path.write_text(json.dumps(dict(night="2026-10-18", started=self.clock.now().isoformat(),
                                       marker=self.clock.now().isoformat(), lanes={"20E8": dict(
                                           lane=self.args.lane, previous=None,
                                           steps=[vars(step) for step in self.steps()])})))
        command = ["chain", "run", "--plan", str(path), "--board", "20E8", "--lane", self.args.lane,
                   "--repo", str(self.repo), "--data", str(self.args.data), "--out", str(self.args.out),
                   "--suite", "suite", "-", "-"]
        replay = Replay(self.root, self.clock, builds=(1,))
        with patch.object(sys, "argv", command), patch.object(chain, "Clock", return_value=self.clock), \
                patch.object(chain, "run_stamped", replay), \
                patch.object(chain, "plugged_boards", side_effect=[["20E8"], ["20E8"], []]), \
                patch.dict(os.environ, _AUTANA_DEVICE_LOCK_TOKEN="hold"):
            self.assertEqual(chain.main(), 1)

    def supervise(self, *, failed=False, unplugged=False):
        # The replayed first lane moves the clock: the joiner's scan must still come before the cut-off.
        self.before_cutoff(30)
        previous = dict(last_measured=self.commits[2], last_attempted=self.commits[4], machine=OTHER_MACHINE)
        chain.write_json(self.args.data / "scoped/2124/2026-10-17-2230/night.json", previous)
        planned = dict(tip=self.commits[-1], night="2026-10-18", started=self.clock.now().isoformat(),
                       marker=self.clock.now().isoformat(), **self.window(),
                       lanes={"20E8": dict(lane="0/2", previous=None,
                                          steps=[vars(step) for step in self.steps((1, 3, 5) if unplugged else (5,))],
                                          estimate=chain.FIRST_STEP_SECONDS)})
        path = self.root / "plan.json"
        chain.write_json(path, planned)
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data, out=self.args.out,
                               suite=self.args.suite)
        commands = []
        manifests = {}

        def launch(command, **options):
            commands.append(command)
            board = command[2]
            current = json.loads(Path(command[command.index("--plan") + 1]).read_text())
            lane = current["lanes"][board]
            lane_args = SimpleNamespace(**vars(self.args))
            lane_args.board, lane_args.lane, lane_args.previous = board, lane["lane"], lane["previous"]
            lane_args.started = current["started"]
            lane_args.out = self.args.out / board
            steps = [chain.Step(**step) for step in lane["steps"]]
            replay = Replay(self.root, self.clock, builds=(1,) if failed and board == "20E8" else ())
            machine = MACHINE if board == "20E8" else OTHER_MACHINE
            plugged = iter(([board], [])) if unplugged and board == "20E8" else repeat([board])
            with patch.object(chain, "plugged_boards", side_effect=lambda: next(plugged)), \
                    patch("layout_measure.capture_board", return_value=machine):
                results = chain.run_chain(steps, lane_args, replay, self.clock)
            manifests[board] = lnt.read_last_night(self.args.data, board)
            polls = iter((None, None))
            return SimpleNamespace(poll=lambda: next(polls, int(any(r.outcome == "build_failed" for r in results))),
                                   wait=lambda: int(any(r.outcome == "build_failed" for r in results)))

        with self.fleet(([self.row("20E8"), self.row("2124")], [])):
            status = chain.run_night(args, clock=self.clock, launch=launch)
        return status, commands, json.loads(path.read_text()), manifests

    def test_supervisor_empty_lane_can_finish_after_multiple_rescans(self):
        path = self.root / "plan.json"
        chain.write_json(path, dict(started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(), **self.window(),
                                   lanes={"FAK1": dict(lane="0/1", steps=[], estimate=0)}))
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data, out=self.args.out,
                               suite=self.args.suite)
        finish = self.clock.now() + timedelta(seconds=chain.RESCAN_SECONDS * 2)
        process = SimpleNamespace(poll=lambda: 0 if self.clock.now() >= finish else None, wait=lambda: 0)
        with patch.object(chain, "plugged_boards", return_value=["FAK1"]):
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=lambda *a, **kw: process), 0)
        self.assertGreaterEqual(self.clock.now(), finish)
        self.stop_tree.assert_not_called()

    def test_supervisor_hang_guard_stops_each_lane_and_waits_for_all(self):
        path = self.root / "plan.json"
        chain.write_json(path, dict(started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(), **self.window(),
                                   lanes={board: dict(lane=f"{i}/2", estimate=42) for i, board in enumerate(("FAK1", "FAK2"))}))
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data, out=self.args.out,
                               suite=self.args.suite)
        events = []

        def launch(command, **options):
            board = command[2]
            events.append("start " + board)
            def wait():
                events.append("finish " + board)
                return int(board == "FAK1")
            return SimpleNamespace(poll=lambda: None, wait=wait)

        with patch.object(chain, "plugged_boards", return_value=["FAK1", "FAK2"]):
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 1)
        self.assertEqual(self.stop_tree.call_count, 2)
        self.assertEqual(events, ["start FAK1", "start FAK2", "finish FAK1", "finish FAK2"])

    def test_supervisor_does_not_publish_joiner_when_launch_fails(self):
        self.before_cutoff(2)
        path = self.root / "plan.json"
        planned = dict(tip=self.commits[-1], started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(), **self.window(),
                       from_sha=self.commits[3], max_steps=1,
                       lanes={"FAK1": dict(lane="0/1", estimate=chain.FIRST_STEP_SECONDS)})
        chain.write_json(path, planned)
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data,
                               out=self.args.out, suite=self.args.suite)
        def launch(command, **options):
            self.assertEqual(json.loads(path.read_text()), planned)
            if command[2] == "FAK2":
                raise OSError("cannot start joining lane")
            polls = iter((None, None))
            return SimpleNamespace(poll=lambda: next(polls, 0), wait=lambda: 0)
        with self.fleet(([self.row("FAK1"), self.row("FAK2")], [])):
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 1)
        self.assertEqual(json.loads(path.read_text()), planned)

    def test_supervisor_default_launch_owns_and_closes_process_tree(self):
        args = self.night(self.fak1_lane())
        process = SimpleNamespace(poll=lambda: 0, wait=lambda: 0)
        with patch.object(chain, "launch_process_tree", return_value=process) as launch, \
                patch.object(chain, "close_process_tree") as close:
            self.assertEqual(chain.run_night(args, clock=self.clock), 0)
        launch.assert_called_once()
        close.assert_called_once_with(process)

    def test_supervisor_lane_plan_error_skips_the_board_for_the_night(self):
        self.before_cutoff(2)
        path = self.root / "plan.json"
        planned = dict(tip=self.commits[-1], started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(), **self.window(),
                       lanes={"20E8": dict(lane="0/1", estimate=42)})
        chain.write_json(path, planned)
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data,
                               out=self.args.out, suite=self.args.suite)
        process = Mock()
        process.poll.return_value = 0
        process.wait.return_value = 0
        with self.fleet(([self.row("20E8"), self.row("2124")], [])), \
                patch.object(chain, "plan_lane", side_effect=ValueError("bad history")) as lane:
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=lambda *a, **kw: process), 1)
        lane.assert_called_once()
        process.wait.assert_called_once()
        self.assertEqual(self.clock.sleeps, [chain.RESCAN_SECONDS] * 2)

    def test_supervisor_interrupt_terminates_and_waits_every_child(self):
        path = self.root / "plan.json"
        chain.write_json(path, dict(marker=self.clock.now().isoformat(), **self.window(),
                                   lanes={"20E8": dict(lane="0/1", estimate=42)}))
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data,
                               out=self.args.out, suite=self.args.suite)
        process = Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        with patch.object(self.clock, "sleep", side_effect=KeyboardInterrupt), \
                self.assertRaises(KeyboardInterrupt):
            chain.run_night(args, clock=self.clock, launch=lambda *a, **kw: process)
        self.stop_tree.assert_called_once_with(process)
        process.wait.assert_called_once()

    def test_supervisor_lane_settings_and_plan_selection(self):
        self.before_cutoff(2)
        path = self.root / "plan.json"
        chain.write_json(path, dict(tip=self.commits[-1], started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(), **self.window(),
            from_sha=self.commits[3], max_steps=1, lanes={"20E8": dict(lane="0/2", estimate=42)}))
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data,
                               out=self.args.out, suite=[("suite", "-", "table with spaces")])
        polls = iter((None, None))
        launch = Mock(side_effect=lambda *a, **kw: SimpleNamespace(poll=lambda: next(polls, 0), wait=lambda: 0))
        original = subprocess.check_output
        def converted(command, **options):
            if command[0] == "/tools/cygpath":
                return "C:/bash.exe\n" if command[1] == "-w" else "/captures/ci.log\n"
            return original(command, **options)
        with patch.dict(os.environ, SELFTEST_BOARD="20E8", BOARD_CI_LOG="inherited"), \
                self.fleet(([self.row("20E8"), self.row("2124")], [])), \
                patch.object(chain.shutil, "which", side_effect=lambda name: "/tools/" + name), \
                patch.object(chain.subprocess, "check_output", side_effect=converted):
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 0)
        first, second = launch.call_args_list
        command = first.args[0]
        self.assertEqual(command[command.index("--selftest") + 1:],
                         ["C:/bash.exe", str(self.repo.resolve() / "scripts/ci/board-steps.sh")])
        self.assertEqual(first.kwargs["env"]["BOARD_CI_LOG"], "/captures/ci.log")
        self.assertNotIn("BOARD_CI_LOG", second.kwargs["env"])
        self.assertNotIn("--selftest", second.args[0])
        self.assertEqual(command[command.index("--suite") + 1:command.index("--suite") + 4], list(args.suite[0]))
        joining = json.loads(path.read_text())["lanes"]["2124"]
        self.assertEqual([step["commit"] for step in joining["steps"]], [self.commits[3]])

    def test_joiner_uses_own_anchor_and_is_in_summary(self):
        status, commands, planned, manifests = self.supervise()
        self.assertEqual(status, 0)
        self.assertEqual([command[2] for command in commands], ["20E8", "2124"])
        joiner = planned["lanes"]["2124"]
        self.assertEqual(joiner["lane"], "0/1")
        self.assertEqual(joiner["steps"][0]["commit"], self.commits[2])
        self.assertEqual(joiner["steps"][0]["role"], "anchor")
        self.assertEqual([step["commit"] for step in joiner["steps"]], [self.commits[2], self.commits[5]])
        self.assertEqual(planned["lanes"]["20E8"]["lane"], "0/2")
        self.assertEqual(manifests["2124"]["machine"], OTHER_MACHINE)
        workflow = (PERF.parents[2] / ".github/workflows/nightly-perf.yml").read_text()
        script = workflow.split("          \"$PY\" - <<'PY'\n", 1)[1].split('          PY', 1)[0]
        script = "\n".join(line[10:] for line in script.splitlines())
        summary = self.root / "summary"
        with patch.dict(os.environ, PERF_OUT=str(self.root), PERF_DATA=str(self.args.data),
                        GITHUB_STEP_SUMMARY=str(summary)):
            exec(script, {})
        self.assertIn("2124: measured=2", summary.read_text())

    def test_joiner_has_no_time_cutoff(self):
        status, commands, planned, _ = self.supervise()
        self.assertEqual(status, 0)
        self.assertEqual(len(commands), 2)
        self.assertIn("2124", planned["lanes"])

    def test_supervisor_unplugged_lane_keeps_result_and_other_lane_finishes(self):
        status, commands, _, manifests = self.supervise(unplugged=True)
        self.assertEqual(status, 0)
        self.assertEqual(len(commands), 2)
        self.assertEqual(manifests["20E8"]["stop_reason"], "unplugged")
        self.assertEqual([step["outcome"] for step in manifests["20E8"]["steps"]],
                         ["measured", "not_run_unplugged", "not_run_unplugged"])
        self.assertEqual([step["outcome"] for step in manifests["2124"]["steps"]], ["measured"] * 2)

    def boards_ran(self, **options):
        """The night's exit status and the boards_ran line it hands the workflow."""
        output = self.root / "github-output"
        with patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
            status = self.supervise(**options)[0]
        return status, output.read_text()

    def test_night_whose_lanes_ran_reports_boards_ran_even_with_failed_tests(self):
        self.assertEqual(self.boards_ran(failed=True), (1, "boards_ran=true\n"))

    def test_night_with_an_unplugged_lane_reports_boards_did_not_run(self):
        self.assertEqual(self.boards_ran(unplugged=True), (0, "boards_ran=false\n"))

    def test_supervisor_failed_lane_does_not_stop_joiner(self):
        status, commands, _, manifests = self.supervise(failed=True)
        self.assertEqual(status, 1)
        self.assertEqual(len(commands), 2)
        self.assertEqual([step["outcome"] for step in manifests["2124"]["steps"]], ["measured"] * 2)

    def night(self, lanes=None, window=None):
        """The night's arguments, with a plan written from the clock: `lanes` (none) and `window` (self.window())."""
        path = self.root / "plan.json"
        chain.write_json(path, dict(tip=self.commits[-1], night="2026-10-18", started=self.clock.now().isoformat(),
                                    marker=self.clock.now().isoformat(), **(window or self.window()),
                                    lanes=lanes or {}))
        return SimpleNamespace(plan=path, repo=self.repo, data=self.args.data, out=self.args.out,
                               suite=self.args.suite)

    def empty_night(self, scans):
        self.before_cutoff(scans)
        return self.night()

    @staticmethod
    def fak1_lane():
        return {"FAK1": dict(lane="0/1", estimate=chain.FIRST_STEP_SECONDS)}

    def running_until(self, finish, launched):
        """A launch whose lanes run until `finish`, recording each board it starts in `launched`."""
        def launch(command, **options):
            launched.append(self.launched_board(command))
            return SimpleNamespace(poll=lambda: 0 if self.clock.now() >= finish else None, wait=lambda: 0)
        return launch

    def test_night_starts_lanes_on_any_hosts_boards_as_they_free_until_the_cutoff(self):
        args = self.empty_night(4)
        cutoff = schedule.night_cutoff(self.clock.now())
        launched = {}

        def launch(command, **options):
            remote = "remote-lane.sh" in command
            board = command[4] if remote else command[command.index("--board") + 1]
            wait = command[6] if remote else command[command.index("--wait") + 1]
            launched[board] = (self.clock.now(), wait)
            return SimpleNamespace(poll=lambda: 0, wait=lambda: 0)
        book2, legion = dict(host="book2", name="Book2"), dict(host="legion", name="Legion")
        held = self.row("2124", state="held", holder=dict(owner="day@job:1"))
        gated = self.row("20E8", gated="person at the desk", **book2)
        queued = self.row("A308", waiting=[dict(owner="day@job:2")], **legion)
        free = [self.row("2124"), self.row("20E8", **book2), self.row("A308", **legion)]
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="bash remote-lane.sh"), \
                patch.object(chain.shutil, "which", side_effect=lambda name: name), \
                self.fleet(([held, gated], ["legion"]), ([held, gated, queued], []), (free, [])) as scans, \
                patch("builtins.print") as said:
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 0)
        # Each board starts at the first scan that finds it free, and queues on its lock no later than the cut-off.
        last_scan = cutoff - timedelta(seconds=chain.RESCAN_SECONDS)
        self.assertEqual(launched, {board: (last_scan, str(chain.RESCAN_SECONDS)) for board in ("2124", "20E8", "A308")})
        lanes = json.loads(args.plan.read_text())["lanes"]
        self.assertEqual({board: (lane["lane"], lane.get("host")) for board, lane in lanes.items()},
                         {"2124": ("0/1", None), "20E8": ("0/1", "book2"), "A308": ("0/1", "legion")})
        self.assertEqual(scans.call_count, 3)
        said = [c.args[0] for c in said.call_args_list]
        self.assertEqual([line for line in said if "legion" in line],
                         ["::warning::remote boards not listed from legion",
                          "::notice::remote boards listed again from legion"])

    def test_night_with_no_free_board_waits_to_the_cutoff_and_fails_only_with_no_board_at_all(self):
        # Busy boards are working, not down: the night warns. No board listed at all: it fails.
        for listed, status in (([self.row("2124", state="held")], 0), ([], 1)):
            with self.subTest(listed=listed):
                self.clock = Clock(22, 30)
                args = self.empty_night(3)
                cutoff = schedule.night_cutoff(self.clock.now())
                launch = Mock(side_effect=AssertionError("no board was free"))
                with self.fleet((listed, [])) as scans, patch("builtins.print"):
                    self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), status)
                self.assertEqual(self.clock.now(), cutoff)
                self.assertEqual(scans.call_count, 2)
                self.assertEqual(json.loads(args.plan.read_text())["lanes"], {})

    @staticmethod
    def launched_board(command):
        return command[4] if "remote-lane.sh" in command else command[command.index("--board") + 1]

    def test_night_listing_error_warns_and_scans_on_and_warns_once_per_lost_host(self):
        args = self.empty_night(4)
        launched = []
        listings = iter((OSError("devices failed"),
                         dict(boards=[self.row("2124")], unreachable=["legion"]),
                         dict(boards=[self.row("2124")], unreachable=["legion"])))

        def listing():
            answer = next(listings)
            if isinstance(answer, Exception):
                raise answer
            return answer

        def launch(command, **options):
            launched.append(self.launched_board(command))
            return SimpleNamespace(poll=lambda: 0, wait=lambda: 0)
        with patch.object(chain, "board_listing", side_effect=listing), patch("builtins.print") as said:
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 0)
        self.assertEqual(launched, ["2124"])
        self.assertIn(call("::warning::devices: devices failed", file=sys.stderr), said.call_args_list)
        self.assertEqual(self.clock.now(), schedule.night_cutoff(self.clock.now()))
        # A host still down at the next scan is not warned of again.
        self.assertEqual(sum("not listed from legion" in c.args[0] for c in said.call_args_list), 1)

    def test_lane_started_before_the_cutoff_runs_on_after_it_and_no_lane_starts_after(self):
        self.before_cutoff(2)
        args = self.night(self.fak1_lane())
        finish = schedule.night_cutoff(self.clock.now()) + timedelta(seconds=3 * chain.RESCAN_SECONDS)
        launched = []
        launch = self.running_until(finish, launched)
        with self.fleet(([self.row("2124", state="held")], []), ([self.row("2124")], [])) as scans:
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 0)
        self.assertEqual(launched, ["FAK1"])
        self.assertEqual(scans.call_count, 1)
        self.assertEqual(self.clock.now(), finish)
        self.stop_tree.assert_not_called()

    def test_joiner_lock_wait_rounds_up_to_the_cutoff_and_a_failed_lock_fails_the_night_once(self):
        args = self.empty_night(1)
        # The scan lands half a second before the cut-off.
        self.clock.value -= timedelta(milliseconds=500)
        launched = []

        def launch(command, **options):
            launched.append(command[command.index("--wait") + 1])
            # The board was taken after the scan: the lane's lock wait runs out.
            return SimpleNamespace(poll=lambda: autana_config.EXIT_BUSY, wait=lambda: autana_config.EXIT_BUSY)
        with self.fleet(([self.row("2124")], [])):
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 1)
        self.assertEqual(launched, ["1"])

    def test_night_keeps_the_plans_cutoff(self):
        # A named plan's cut-off is its start: well before BOARD_LAST_START it still takes no joiner.
        for cutoff, joined in (("start", []), ("window", ["2124"])):
            with self.subTest(cutoff=cutoff):
                self.clock = Clock(22, 30)
                self.before_cutoff(3)
                window = self.window() if cutoff == "window" else dict(cutoff=self.clock.now().isoformat(), listed=[])
                args = self.night(self.fak1_lane(), window)
                launched = []
                launch = self.running_until(self.clock.now() + timedelta(seconds=2 * chain.RESCAN_SECONDS), launched)
                with self.fleet(([self.row("2124")], [])):
                    self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 0)
                self.assertEqual(launched, ["FAK1", *joined])

    def test_plans_listed_boards_count_as_seen(self):
        # Boards the plan listed busy are working, not down, even when no later scan lists them.
        for listed, status in ((["2124"], 0), ([], 1)):
            with self.subTest(listed=listed):
                args = self.night(window=dict(cutoff=self.clock.now().isoformat(), listed=listed))
                with self.fleet() as scans, patch("builtins.print"):
                    self.assertEqual(chain.run_night(args, clock=self.clock, launch=Mock()), status)
                scans.assert_not_called()

    def test_scan_that_reaches_the_cutoff_starts_no_lane(self):
        args = self.empty_night(1)
        # The scan starts half a scan before the cut-off and takes a whole one.
        self.clock.value -= timedelta(seconds=chain.RESCAN_SECONDS / 2)

        def slow_listing():
            self.clock.sleep(chain.RESCAN_SECONDS)
            return dict(boards=[self.row("2124")], unreachable=[])
        launch = Mock(side_effect=AssertionError("started after the cut-off"))
        with patch.object(chain, "board_listing", side_effect=slow_listing) as scans, patch("builtins.print"):
            self.assertEqual(chain.run_night(args, clock=self.clock, launch=launch), 0)
        self.assertEqual(scans.call_count, 1)
        self.assertEqual(json.loads(args.plan.read_text())["lanes"], {})

    def test_busy_names_why_a_board_takes_no_lane(self):
        cases = {"person at the desk": self.row("A308", state="held", gated="person at the desk"),
                 "held by day@job:1": self.row("A308", state="held", holder=dict(owner="day@job:1")),
                 "1 job(s) queued for it": self.row("A308", waiting=[dict(owner="day@job:2")]),
                 # A listing without lock state cannot show the board free.
                 "held by None": {"board": "A308", "port": "COM5"},
                 None: self.row("A308")}
        for why, board in cases.items():
            with self.subTest(why=why):
                self.assertEqual(chain.busy(board), why)

    def test_named_boards_plan_only_while_free(self):
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=["20E8", "A308", "B0B0", "2124"], data=self.args.data,
                               out=self.root / "plan.json", marker=self.clock.now().isoformat(),
                               from_sha=None, max_steps=None, selftest_board=None)
        listing = json.dumps({"boards": [self.row("20E8", host="book2", name="Book2", state="held"),
                                         self.row("A308", host="legion", name="Legion", gated="person at the desk")],
                              "unreachable": ["helios"]})
        self.local_patch.stop()
        local = patch.object(chain, "local_boards",
                             return_value=[self.row("44:BD:8D:78:B0:B0", waiting=[dict(owner="day@job:2")]),
                                           self.row("44:BD:8D:78:21:24")])
        real = subprocess.check_output
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="remote-lane"), \
                patch.object(chain.subprocess, "check_output",
                             side_effect=lambda command, **o: listing if command[1:] == ["boards"] else real(command, **o)), \
                patch.object(chain, "gpu_on_host", return_value=True), patch.object(chain, "wait_idle"), \
                local, patch("builtins.print") as said:
            chain.plan(args, self.clock)
        # Held, gated and queued boards get no lane whether named or discovered, here or remote; named boards
        # are the whole night, so it takes none later either.
        planned = json.loads(args.out.read_text())
        self.assertEqual(list(planned["lanes"]), ["2124"])
        self.assertEqual(planned["cutoff"], planned["started"])
        self.assertIn(call("::notice::board 20E8 on Book2 sits this night out: held by held", file=sys.stderr),
                      said.call_args_list)
        self.assertIn(call("::warning::remote boards not listed from helios", file=sys.stderr), said.call_args_list)

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
        schedule.refresh_gpu_host("gpu-runner", self.clock)
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=["20E8", "2124"], data=self.args.data,
                               out=self.root / "plan.json", marker=self.clock.now().isoformat(), from_sha=self.commits[1],
                               max_steps=2, selftest_board="2124")
        chain.plan(args, self.clock, request=lambda *a: {"workflow_runs": []})
        plan = json.loads(args.out.read_text())
        self.assertEqual(plan["tip"], self.commits[-1])
        self.assertEqual((plan["from_sha"], plan["max_steps"]), (args.from_sha, args.max_steps))
        self.assertEqual(self.clock.sleeps, [QUEUE_POLL_SECONDS])
        for board, lane in plan["lanes"].items():
            reserve = chain.SELFTEST_ALLOWANCE_SECONDS if board == args.selftest_board else 0
            self.assertEqual(lane["estimate"], len(lane["steps"]) * chain.FIRST_STEP_SECONDS + reserve)
            self.assertEqual(len(lane["steps"]), 2)
            self.assertLessEqual(lane["steps"][-1]["position"], 6)
        self.assertEqual(plan["marker"], args.marker)
        self.assertNotIn("deadline", plan)
        previous = {"last_measured": self.commits[0]}
        limited = chain.select_steps(self.repo, "HEAD", previous, 0, 2, self.commits[1], 1)
        self.assertEqual([step.role for step in limited], ["anchor", "new"])
        self.assertEqual(limited[-1].commit, self.commits[1])

    def test_equal_image_skips_but_anchor_measures(self):
        steps = self.steps(indices=(1, 1, 1), anchor=True)
        build_seconds = 60.75
        expected_wait = chain.NIGHT_STEP_HANG_SECONDS
        results, replay, writes = self.run_chain(steps, durations=(build_seconds,))
        capture = next(command for command in replay.calls if "suite" in command)
        self.assertEqual(capture[capture.index("--wait") + 1], str(expected_wait))
        self.assertEqual([r.outcome for r in results], ["measured", "skipped_same_image", "skipped_same_image"])
        self.assertEqual(sum("suite" in cmd for cmd in replay.calls), 1)
        self.assertEqual([len(w["steps"]) for w in writes], [0, 1, 2, 3, 3])
        previous = writes[-1]
        self.args.previous = previous
        results, _, _ = self.run_chain([chain.Step(steps[0].commit, 101, "anchor", 2)])
        self.assertEqual(results[0].outcome, "measured")

    def test_busy_build_stops_without_advancing_cursor(self):
        self.args.previous = dict(last_measured=self.commits[0], last_attempted=self.commits[0])
        results, replay, writes = self.run_chain(builds=(autana_config.EXIT_BUSY,))
        self.assertEqual([r.outcome for r in results], ['not_run_busy'] * 3)
        self.assertEqual(writes[-1]['stop_reason'], 'busy')
        self.assertEqual(writes[-1]['last_attempted'], self.commits[0])
        self.assertEqual(writes[-1]['last_measured'], self.commits[0])
        self.assertEqual(sum('build' in command for command, _ in replay.timeouts), 1)

    def test_build_failure_continues_without_updating_hash(self):
        results, _, _ = self.run_chain(self.steps(indices=(1, 3, 1)), builds=(0, 1, 0))
        self.assertEqual([r.outcome for r in results], ["measured", "build_failed", "skipped_same_image"])

    def test_two_capture_failures_stop_and_success_resets_streak(self):
        results, _, writes = self.run_chain(captures=(True, True))
        self.assertEqual([r.outcome for r in results], ["capture_failed", "capture_failed", "not_run_stopped"])
        self.assertEqual(writes[-1]["stop_reason"], "capture_failures")
        results, _, _ = self.run_chain(captures=(True, False, True))
        self.assertEqual([r.outcome for r in results], ["capture_failed", "measured", "capture_failed"])

    def test_heavy_wait_is_recorded_without_inflating_build(self):
        results, _, writes = self.run_chain(heavy_wait=60 * 60)
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual([r.build_seconds for r in results], [60] * 3)
        self.assertEqual([s["wait_seconds"] for s in writes[-1]["steps"]], [60 * 60] * 3)

    def test_flash_rebuild_wait_is_recorded(self):
        results, _, writes = self.run_chain(capture_wait=(2 * 60 * 60, 0, 0))
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual([r.build_seconds for r in results], [60] * 3)
        self.assertEqual([s["wait_seconds"] for s in writes[-1]["steps"]], [2 * 60 * 60, 0, 0])

    def test_selftest_failure_continues_inside_hold(self):
        self.args.selftest = ["selftest-replay"]
        results, replay, writes = self.run_chain(selftest_code=1)
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual(replay.boundaries, [datetime(2026, 10, 18, 22, 30, tzinfo=timezone.utc)])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "failed")
        self.assertEqual(writes[-1]["selftest"]["report"], "selftest.md")
        self.assertTrue(lnt.night_ran(writes[-1]))

    def test_selftest_that_never_completed_means_the_board_did_not_run(self):
        self.args.selftest = ["selftest-replay"]
        _, _, writes = self.run_chain(selftest_code=1, selftest_complete=False)
        self.assertEqual(writes[-1]["selftest"]["outcome"], "incomplete")
        self.assertFalse(lnt.night_ran(writes[-1]))

    def test_selftest_runs_first_without_waiting(self):
        self.args.selftest = ["selftest-replay"]
        results, replay, writes = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(replay.boundaries, [Clock(22, 30).now()])
        self.assertEqual(replay.timeouts[0][0], ["selftest-replay"])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "passed")
        self.assertEqual(results[0].outcome, "measured")
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

    def test_stopped_reason_survives_long_step(self):
        results, _, writes = self.run_chain(self.steps(indices=(1, 2, 3, 4)),
                                           captures=(True, True), durations=(60, 26 * 60))
        self.assertEqual([r.outcome for r in results],
                         ["capture_failed", "capture_failed", "not_run_stopped", "not_run_stopped"])
        self.assertEqual(writes[-1]["stop_reason"], "capture_failures")

    def test_failed_selftest_log_exists_and_remaining_steps_continue(self):
        self.args.selftest = ["selftest-replay"]
        with patch.object(chain, "REPORT_LINE", re.compile(r"never matches")):
            results, _, writes = self.run_chain(builds=(1, 0, 0), captures=(True, False), selftest_code=1)
        self.assertEqual([result.outcome for result in results],
                         ["build_failed", "capture_failed", "measured"])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "failed")
        self.assertTrue((self.args.out / writes[-1]["selftest"]["report"]).is_file())

    def test_plan_waits_for_gpu_queue_to_finish(self):
        for release_hour, release_minute in ((22, 30), (1, 30), (12, 45)):
            with self.subTest(release_hour=release_hour):
                clock = ScheduleClock(18)
                schedule.refresh_gpu_host("gpu-runner", clock)
                day = 21 if release_hour > 12 else 22
                released = datetime(2026, 10, day, release_hour, release_minute)
                args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=["20E8"],
                    data=self.args.data, out=self.root / "plan.json", marker=self.clock.now().isoformat(),
                    from_sha=None, max_steps=None, selftest_board="20E8")
                def request(method, path):
                    if '/jobs?' in path:
                        return {"jobs": [dict(status="in_progress", labels=[schedule.GPU_LABEL])]}
                    return {"workflow_runs": [dict(id=1, status="in_progress")] if clock.now() < released else []}
                with patch.object(chain, "read_last_night", return_value=None), \
                        patch.object(chain, "select_steps", return_value=[]):
                    chain.plan(args, clock, request=request)
                planned = json.loads(args.out.read_text())
                self.assertTrue(planned["lanes"])
                self.assertNotIn("deadline", planned)
                expected = released + timedelta(seconds=QUEUE_POLL_SECONDS)
                self.assertEqual(planned["started"], expected.isoformat())

    def test_plan_missing_or_stale_gpu_host_skips_queue_and_reports(self):
        for age in (None, schedule.GPU_HOST_MARKER_DAYS + 1):
            with self.subTest(age=age):
                clock = ScheduleClock(23)
                if age is not None:
                    schedule.refresh_gpu_host("other-runner", ScheduleClock(23, day=21 - age))
                args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=["20E8"],
                    data=self.args.data, out=self.root / "plan.json", marker=self.clock.now().isoformat(),
                    from_sha=None, max_steps=None, selftest_board="20E8")
                summary = self.root / "gpu-summary"
                summary.unlink(missing_ok=True)
                with patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)), \
                        patch.object(chain, "read_last_night", return_value=None), \
                        patch.object(chain, "select_steps", return_value=[]), \
                        patch.object(chain, "gpu_pending", side_effect=AssertionError("remote GPU queue queried")):
                    chain.plan(args, clock)
                self.assertEqual(clock.sleeps, [])
                self.assertEqual(summary.read_text(), schedule.GPU_NOT_ON_HOST + "\n")
                self.assertTrue(json.loads(args.out.read_text())["lanes"])

    def remote_listing(self, *boards, unreachable=()):
        listing = json.dumps({"boards": [self.row(board, host=host, name=host.title())
                                         for board, host in boards], "unreachable": list(unreachable)})
        original = subprocess.check_output

        def answer(command, **options):
            if command[1:] == ["boards"]:
                return listing
            return original(command, **options)
        return patch.object(chain.subprocess, "check_output", side_effect=answer)

    def test_plan_spans_remote_boards_and_keeps_the_selftest_local(self):
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=None, data=self.args.data,
                               out=self.root / "plan.json", marker=self.clock.now().isoformat(),
                               from_sha=None, max_steps=None, selftest_board=None)
        # A host that sees this machine's board too (a loop back) does not take its lane.
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="remote-lane"), \
                patch.object(chain, "local_boards", return_value=[self.row("44:BD:8D:78:21:24", port="COM4")]), \
                self.remote_listing(("44:BD:8D:78:20:E8", "book2"), ("2124", "gpu-runner")), \
                patch.object(chain, "gpu_on_host", return_value=True), \
                patch.object(chain, "wait_idle"), \
                patch("builtins.print") as said:
            chain.plan(args, self.clock)
        lanes = json.loads(args.out.read_text())["lanes"]
        self.assertEqual({board: (lane["lane"], lane.get("host"), lane.get("host_name")) for board, lane in lanes.items()},
                         {"2124": ("0/2", None, None), "20E8": ("1/2", "book2", "Book2")})
        self.assertIn(call("SCOPED_BOARDS=2124 20E8"), said.call_args_list)
        self.assertIn(call("SELFTEST_BOARD=2124"), said.call_args_list)

    def test_plan_skips_a_gated_hosts_boards(self):
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=None, data=self.args.data,
                               out=self.root / "plan.json", marker=self.clock.now().isoformat(),
                               from_sha=None, max_steps=None, selftest_board=None)
        listing = json.dumps({"boards": [self.row("A308", host="legion", name="Legion",
                                                  gated="person at the desk")], "unreachable": []})
        real = subprocess.check_output
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="remote-lane"), \
                patch.object(chain, "local_boards", return_value=[self.row("2124", port="COM4")]), \
                patch.object(chain.subprocess, "check_output",
                             side_effect=lambda command, **o: listing if command[1:] == ["boards"] else real(command, **o)), \
                patch.object(chain, "gpu_on_host", return_value=True), \
                patch.object(chain, "wait_idle"), \
                patch("builtins.print") as said:
            chain.plan(args, self.clock)
        self.assertEqual(list(json.loads(args.out.read_text())["lanes"]), ["2124"])
        self.assertTrue(any("A308 on Legion sits this night out: person at the desk" in c.args[0] for c in said.call_args_list))

    def test_unreachable_remote_hosts_leave_this_machines_lanes_and_warn(self):
        failure = subprocess.CalledProcessError(1, ["remote-lane", "boards"])
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="remote-lane"), \
                patch.object(chain.subprocess, "check_output", side_effect=failure), \
                patch("builtins.print"):
            self.assertEqual(chain.remote_boards(), ([], [chain.REMOTE_LANE_ENV]))
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="remote-lane"), self.remote_listing(unreachable=["book2"]), \
                patch("builtins.print"):
            self.assertEqual(chain.remote_boards(), ([], ["book2"]))
        with patch.dict(os.environ, REMOTE_LANE_COMMAND=""):
            self.assertEqual(chain.remote_boards(), ([], []))

    def test_remote_command_resolves_its_program_on_path(self):
        with patch.dict(os.environ, REMOTE_LANE_COMMAND='"/a b/bash" /x/remote-lane.sh'), \
                patch.object(chain.shutil, "which", side_effect=lambda name: "/git/" + Path(name).name):
            self.assertEqual(chain.remote_command(), ["/git/bash", "/x/remote-lane.sh"])

    def test_boards_lists_this_machines_and_remote_boards_once(self):
        local = [self.row("44:BD:8D:78:21:24", port="COM4")]
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="remote-lane"), \
                patch.object(chain, "local_boards", return_value=local), \
                self.remote_listing(("20E8", "book2"), ("2124", "gpu-runner"), unreachable=["legion"]):
            listing = chain.board_listing()
        self.assertEqual([(board["board"], board.get("host")) for board in listing["boards"]],
                         [("44:BD:8D:78:21:24", None), ("20E8", "book2")])
        self.assertEqual(listing["unreachable"], ["legion"])

    def remote_night(self, launch, lanes=None):
        lane = dict(lane="1/2", previous=None, steps=[], estimate=chain.FIRST_STEP_SECONDS, host="book2",
                    host_name="Book2")
        path = self.root / "plan.json"
        chain.write_json(path, dict(tip=self.commits[-1], night="2026-10-18", started=self.clock.now().isoformat(),
                                    marker=self.clock.now().isoformat(), **self.window(), lanes=lanes or {"20E8": lane}))
        args = SimpleNamespace(plan=path, repo=self.repo, data=self.args.data, out=self.args.out,
                               suite=[("suite", "-", "table with spaces")])
        with patch.dict(os.environ, REMOTE_LANE_COMMAND="bash remote-lane.sh", SELFTEST_BOARD="20E8",
                        BOARD_CI_LOG="inherited"), \
                patch.object(chain.shutil, "which", side_effect=lambda name: name), \
                patch.object(chain, "plugged_boards", return_value=[]):
            return chain.run_night(args, clock=self.clock, launch=launch), lane

    def test_remote_lane_ships_its_folder_and_returns_its_data(self):
        launched = []

        def launch(command, **options):
            launched.append((command, options))
            folder = Path(command[5])
            # What the lane writes on the remote host, as the remote-lane command brings it back.
            chain.write_json(folder / "data/scoped/20E8/2026-10-18-2230/night.json", dict(board="20E8"))
            return SimpleNamespace(poll=lambda: 0, wait=lambda: 0)
        status, lane = self.remote_night(launch)
        self.assertEqual(status, 0)
        (command, options), = launched
        folder = self.args.out.resolve() / "20E8"
        self.assertEqual(command[:8], ["bash", "remote-lane.sh", "run", "book2", "20E8", str(folder),
                                       str(chain.NIGHT_STEP_HANG_SECONDS), "--"])
        self.assertEqual(command[8:10], [str(self.repo.resolve() / "launcher/tools/perf/nightly/scoped_chain.py"), "run"])
        self.assertEqual(command[command.index("--plan") + 1], str(folder / "lane.json"))
        self.assertEqual(command[command.index("--data") + 1], str(folder / "data"))
        self.assertEqual(command[command.index("--out") + 1], str(folder))
        self.assertEqual(command[command.index("--lane") + 1], "1/2")
        self.assertEqual(command[command.index("--suite") + 1:], ["suite", "-", "table with spaces"])
        self.assertNotIn("--selftest", command)
        self.assertNotIn("BOARD_CI_LOG", options["env"])
        self.assertEqual(json.loads((folder / "lane.json").read_text())["lanes"], {"20E8": lane})
        self.assertEqual(lnt.read_last_night(self.args.data, "20E8"), dict(board="20E8"))

    def test_remote_lanes_start_before_local_ones(self):
        order = []
        local = dict(lane="0/2", previous=None, steps=[], estimate=chain.FIRST_STEP_SECONDS)
        remote = dict(local, lane="1/2", host="book2", host_name="Book2")

        def launch(command, **options):
            order.append("remote" if "remote-lane.sh" in command else "local")
            return SimpleNamespace(poll=lambda: 0, wait=lambda: 0)
        self.remote_night(launch, lanes={"2124": local, "20E8": remote})
        self.assertEqual(order, ["remote", "local"])

    def test_hung_remote_lane_is_asked_to_stop_and_brings_its_data_back(self):
        process = Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        stops = []

        def stopped(command, **options):
            stops.append(command)
            process.poll.return_value = 0
        with patch.object(chain.subprocess, "run", side_effect=stopped):
            status, _ = self.remote_night(lambda *a, **kw: process)
        self.assertEqual(status, 1)
        self.assertEqual(stops, [["bash", "remote-lane.sh", "stop", "book2", "20E8"]])
        process.wait.assert_any_call(timeout=chain.REMOTE_STOP_SECONDS)
        self.stop_tree.assert_not_called()

    def test_remote_stop_that_times_out_kills_the_local_tree(self):
        process = Mock()
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired("lane", 0), 0]
        with patch.object(chain.subprocess, "run"), \
                patch.object(self.clock, "sleep", side_effect=KeyboardInterrupt), \
                self.assertRaises(KeyboardInterrupt):
            self.remote_night(lambda *a, **kw: process)
        self.stop_tree.assert_called_once_with(process)

    def test_selftest_limits_timeout_and_saves_metadata(self):
        self.args.selftest = ["selftest-replay"]
        self.args.started = Clock(22, 0).now().isoformat()
        _, replay, writes = self.run_chain(steps=[])
        saved = writes[1]["selftest"]
        self.assertEqual(saved["outcome"], "passed")
        self.assertEqual(saved["started"], replay.boundaries[0].isoformat())
        self.assertEqual(saved["seconds"], 60)
        self.assertEqual(saved["report"], "selftest.md")
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
        self.assertTrue(all(timeout == chain.BUILD_TIMEOUT_SECONDS for cmd, timeout in replay.timeouts if "build" in cmd))
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
        planned = dict(tip=self.commits[-1], night="2026-10-18", started=self.clock.now().isoformat(), marker=self.clock.now().isoformat(),
                       lanes={"20E8": dict(lane="0/2", previous=None, steps=[])})
        path = self.root / "plan.json"
        path.write_text(json.dumps(planned))
        base = ["chain", "run", "--plan", str(path), "--board", "20E8", "--lane", "0/2",
                "--data", str(self.args.data), "--out", str(self.args.out)]
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
    def test_plan_reads_each_boards_manifest(self):
        previous = {"20E8": {"last_measured": self.commits[0]}, "2124": {"last_measured": self.commits[2]}}
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=list(previous), data=self.args.data,
                               out=self.root / "plan.json", marker=self.clock.now().isoformat(), from_sha=None, max_steps=None,
                               selftest_board=None)
        with patch.object(chain, "read_last_night", side_effect=lambda data, board: previous[board]):
            chain.plan(args, self.clock, request=lambda *a: {"workflow_runs": []})
        planned = json.loads(args.out.read_text())
        self.assertEqual({board: lane["previous"] for board, lane in planned["lanes"].items()}, previous)

    def test_plan_without_boards_takes_every_plugged_board(self):
        self.plugged_patch.stop()
        self.local_patch.stop()
        listing = {"boards": [self.row("44:BD:8D:78:20:E8", port="COM3"),
                              self.row("44:BD:8D:78:21:24", port=None),
                              self.row("90:70:69:FE:A3:08"),
                              # A held board waits for the night's rescan instead of queueing behind its holder.
                              self.row("44:BD:8D:78:B0:B0", state="held", holder=dict(owner="day@job:1"))]}
        args = SimpleNamespace(repo=self.repo, tip="HEAD", boards=None, data=self.args.data,
                               out=self.root / "plan.json", marker=self.clock.now().isoformat(), from_sha=None, max_steps=None,
                               selftest_board=None)
        real = chain.subprocess.check_output
        def devices(command, **kwargs):
            return json.dumps(listing) if "devices" in command else real(command, **kwargs)
        with patch.object(chain.subprocess, "check_output", side_effect=devices), \
                patch.object(chain, "read_last_night", return_value=None), \
                patch("sys.stdout", new_callable=io.StringIO) as output:
            chain.plan(args, self.clock, request=lambda *a: {"workflow_runs": []})
        planned = json.loads(args.out.read_text())
        self.assertEqual({board: lane["lane"] for board, lane in planned["lanes"].items()},
                         {"20E8": "0/2", "A308": "1/2"})
        self.assertEqual(output.getvalue().splitlines(), ["SCOPED_BOARDS=20E8 A308", "SELFTEST_BOARD=20E8"])
        planned = json.loads(args.out.read_text())
        self.assertEqual(planned["cutoff"], schedule.night_cutoff(self.clock.now()).isoformat())
        self.assertEqual(planned["listed"], ["20E8", "A308", "B0B0"])
        # No free board: the plan is empty and the night waits for one.
        listing["boards"] = [dict(board, port=None) for board in listing["boards"]]
        args.boards = args.selftest_board = None
        with patch.object(chain.subprocess, "check_output", side_effect=devices), \
                patch("sys.stdout", new_callable=io.StringIO) as output:
            chain.plan(args, self.clock, request=lambda *a: {"workflow_runs": []})
        self.assertEqual(json.loads(args.out.read_text())["lanes"], {})
        self.assertEqual(output.getvalue().splitlines(), ["SCOPED_BOARDS=", "SELFTEST_BOARD="])

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
        self.assertFalse(list(self.args.out.parent.glob("nightly-*")))
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

class LaneSettingsTests(unittest.TestCase):
    def test_workflow_lanes_run_after_prepare_failure_and_publish_after_failure(self):
        workflow = yaml.safe_load((PERF.parents[2] / ".github/workflows/nightly-perf.yml").read_text())
        steps = {step.get("name"): step for step in workflow["jobs"]["nightly"]["steps"]}
        lane_step = steps["Run each board lane and wait for all"]
        self.assertEqual(lane_step["if"], "${{ !cancelled() && steps.plan.outcome == 'success' }}")
        self.assertEqual(steps["Prepare the data branch and shared plan"]["id"], "plan")
        self.assertEqual(workflow["env"]["BOARD_LAST_START"], "${{ vars.BOARD_LAST_START }}")
        for step in ("Summarize board outcomes", "Publish night manifests and LNT reports"):
            self.assertEqual(steps[step]["if"], "always() && env.PERF_DATA != ''")

    def test_workflow_passes_marker_to_plan(self):
        workflow = yaml.safe_load((PERF.parents[2] / ".github/workflows/nightly-perf.yml").read_text())
        step = next(step for step in workflow["jobs"]["nightly"]["steps"]
                    if step.get("name") == "Prepare the data branch and shared plan")
        self.assertIn('--marker "$MARKER"', step['run'])
        self.assertEqual(workflow['jobs']['nightly']['needs'], ['marker', 'wait'])


if __name__ == "__main__":
    unittest.main()

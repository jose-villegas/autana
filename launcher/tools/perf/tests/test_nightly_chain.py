"""Board-free replays of nightly planning, acquisition and publication."""
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

PERF = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PERF))
from nightly import scoped_chain as chain, lnt
from test_perf_compare import FakeAutana
from layout_measure import parse_report, parse_timings

MACHINE = "00:11:22:33:20:E8"


class Clock:
    def __init__(self, hour=22, minute=30):
        self.value = datetime(2026, 10, 18, hour, minute, tzinfo=timezone.utc)
        self.sleeps = []

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.value += timedelta(seconds=seconds)


class Replay(FakeAutana):
    def __init__(self, root, clock, builds=(), captures=(), durations=(), selftest_code=0):
        super().__init__(root)
        self.clock = clock
        self.builds, self.captures, self.durations = iter(builds), iter(captures), iter(durations)
        self.selftest_code = selftest_code
        self.boundaries, self.tokens = [], []

    def __call__(self, command, log, timeout):
        self.tokens.append(os.environ.get("_AUTANA_DEVICE_LOCK_TOKEN"))
        if command == ["selftest-replay"]:
            self.boundaries.append(self.clock.now())
            self.clock.sleep(60)
            return self.selftest_code, [(0, "report: selftest.md")], 60
        if "build" in command:
            self.clock.sleep(next(self.durations, 60))
            code = next(self.builds, 0)
            project = Path(command[command.index("--project") + 1])
            build = project / "launcher/build.diag"
            build.mkdir(parents=True, exist_ok=True)
            (build / "launcher.bin").write_bytes(b"a" * 512 + (project / "code").read_bytes() + b"t" * chain.IMAGE_TRAILER_BYTES)
            (build / "assets.bin").write_bytes(b"assets")
            (build / "build_id.txt").write_text("0-diag")
            return code, [], 60
        if "suite" in command:
            self.clock.sleep(60)
            if next(self.captures, False):
                raise RuntimeError("capture failed")
        return super().__call__(command, log, timeout)


class NightlyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
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
        self.clock = Clock()
        self.args = SimpleNamespace(repo=self.repo, out=self.root / "out", data=self.root / "data",
                                    board="20E8", lane="0/2", until="07:00", previous=None,
                                    suite=[("suite", "-", "-")], selftest_at=None, selftest=None)

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
        self.assertEqual([len(item["steps"]) for item in writes if item["steps"]][-1], len(results))
        return results, replay, writes

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
                   "--selftest-at", "22:31", "--selftest", "selftest-replay"]
        original = subprocess.check_output

        def fail_remove(command, **options):
            if "worktree" in command and "remove" in command:
                raise subprocess.CalledProcessError(255, command)
            return original(command, **options)

        for build, capture, selftest, expected in ((0, False, 0, 0), (1, False, 0, 1),
                                                  (0, True, 0, 1), (0, False, 1, 1)):
            clock = Clock()
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
        app = bytearray(b"a" * 1024)
        (build / "launcher.bin").write_bytes(app)
        (build / "assets.bin").write_bytes(b"assets")
        expected = chain.image_hash(build)
        for index in (chain.APP_DESC_OFFSET, chain.APP_DESC_OFFSET + chain.APP_DESC_BYTES - 1,
                      len(app) - chain.IMAGE_TRAILER_BYTES, len(app) - 1):
            app[index] ^= 1
        (build / "launcher.bin").write_bytes(app)
        self.assertEqual(chain.image_hash(build), expected)
        app[chain.APP_DESC_OFFSET + chain.APP_DESC_BYTES] ^= 1
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
                               out=self.root / "plan.json", queue_at="22:00", from_sha=self.commits[1],
                               max_steps=2, selftest_board="2124")
        chain.plan(args, self.clock)
        plan = json.loads(args.out.read_text())
        self.assertEqual(plan["tip"], self.commits[-1])
        self.assertEqual(self.clock.sleeps, [0])
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

    def test_deadline_and_observed_worst_step(self):
        self.args.until = "22:52"
        results, _, _ = self.run_chain()
        self.assertEqual([r.outcome for r in results], ["not_run_window"] * 3)
        self.args.until = "23:33"
        results, _, _ = self.run_chain(durations=(30 * 60, 60))
        self.assertEqual([r.outcome for r in results], ["measured", "measured", "not_run_window"])

    def test_selftest_at_boundary_and_failure_continues_inside_hold(self):
        self.args.selftest_at = "22:31"
        self.args.selftest = ["selftest-replay"]
        results, replay, writes = self.run_chain(selftest_code=1)
        self.assertEqual([r.outcome for r in results], ["measured"] * 3)
        self.assertEqual(replay.boundaries, [datetime(2026, 10, 18, 22, 32, tzinfo=timezone.utc)])
        self.assertEqual(writes[-1]["selftest"]["outcome"], "failed")
        self.assertEqual(writes[-1]["selftest"]["report"], "selftest.md")

    def test_selftest_waits_after_chain_and_reserves_deadline(self):
        self.args.selftest_at, self.args.selftest = "01:00", ["selftest-replay"]
        results, replay, writes = self.run_chain(self.steps(indices=(1,)))
        self.assertEqual(replay.boundaries[0].hour, 1)
        self.assertIn(8880, self.clock.sleeps)
        self.assertEqual(writes[-1]["selftest"]["outcome"], "passed")
        self.clock = Clock()
        self.args.until = "00:00"
        self.args.selftest_at = "23:00"
        results, replay, writes = self.run_chain()
        self.assertEqual([r.outcome for r in results], ["not_run_window"] * 3)
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
        for night in ("2026-10-18", "2026-10-16"):
            chain.write_json(self.args.data / "scoped/20E8" / night / "night.json", {"night": night})
        self.assertEqual(lnt.read_last_night(self.args.data, "20E8")["night"], "2026-10-18")

    def test_table_mode_preserves_all_stage_rows(self):
        path = PERF.parents[1] / "main/apps/render_lab/tools/report_performance.py"
        capture = self.root / "fixture.log"
        text = "\n".join(f"capybara_perf: stage=frame paused={paused} quarter={quarter} mean_us={412+paused}"
                         for quarter in ("portrait", "landscape") for paused in (0, 1))
        text += "\ncapybara_perf: stage=skin cores=2 mean_us=17\ncapybara_perf: stage=custom mean_us=19\n"
        capture.write_text(text)
        table = self.root / "table.md"
        subprocess.run([sys.executable, str(path), str(capture), str(table), "--table"], check=True, capture_output=True)
        self.assertEqual(parse_report(table), parse_timings(text))
        self.assertEqual(len(parse_report(table)), 6)


class LaneSettingsTests(unittest.TestCase):
    def test_lane_uses_configured_window_and_selftest_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "scripts/lib/python.sh"
            library.parent.mkdir(parents=True)
            library.write_text('find_python() { printf "%s\\n" "$TEST_PYTHON"; }\n')
            driver = root / "launcher/tools/perf/nightly/scoped_chain.py"
            driver.parent.mkdir(parents=True)
            driver.write_text('import sys\nprint("123" if sys.argv[1] == "wait-seconds" else "")\n')
            plan = root / "plan.json"
            plan.write_text(json.dumps({"lanes": {"board": {"lane": "0/1", "estimate": 42}}}))
            script = PERF / "nightly/lane.sh"
            command = r"""
cygpath() { printf '%s\n' "${@: -1}"; }
autana() { printf '%s\n' "$@"; }
export -f cygpath autana
bash "$1" board "$2" data out
"""
            env = dict(os.environ, TEST_PYTHON=sys.executable, SELFTEST_BOARD="board",
                       WINDOW_END="05:45", SELFTEST_AT="23:15")
            result = subprocess.run(["bash", "-c", command, "lane-test", script.as_posix(), plan.as_posix()],
                                    cwd=root, env=env, capture_output=True, text=True, check=True)
            args = result.stdout.splitlines()
            for option, setting in (("--until", "WINDOW_END"), ("--selftest-at", "SELFTEST_AT")):
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
            with self.subTest(event=event, requested=requested):
                env = dict(os.environ, GITHUB_EVENT_NAME=event, DISPATCH_QUEUE_AT=requested,
                           QUEUE_AT="22:30", FROM_SHA="", MAX_STEPS="", SCOPED_BOARDS="board",
                           SELFTEST_BOARD="board")
                result = subprocess.run(["bash", "-c", command], env=env, capture_output=True,
                                        text=True, check=True)
                args = result.stdout.splitlines()
                if expected is None:
                    self.assertNotIn("--queue-at", args)
                else:
                    self.assertEqual(args[args.index("--queue-at") + 1], expected)
                self.assertNotIn("--suite", args)


if __name__ == "__main__":
    unittest.main()

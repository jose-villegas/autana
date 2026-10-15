"""Shared shell-job replay; all external commands are local fakes."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

PERF = Path(__file__).resolve().parents[1]
TOKEN_ENV = "_AUTANA_DEVICE_LOCK_TOKEN"


def replay(case, kind, mode="ok", extra=(), borrowed=False, script=None, same=False):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for side in ("a", "b"):
            header = root / side / "launcher/test/suites.h"
            header.parent.mkdir(parents=True)
            header.write_text("#define SUITE_FILTER_LEN 24\n#define SUITE_FILTER_MAX 4\n")
        fake = PERF / "tests/fixtures/job_autana.py"
        override = shlex.join([Path(sys.executable).as_posix(), fake.as_posix()])
        # Only the restore checkout uses git; no real worktree is modified.
        git = root / "bin/git"
        git.parent.mkdir()
        git.write_text("#!/bin/sh\n[ \"$PERF_JOB_MODE\" != checkout_fail ] || exit 1\ncase \"$*\" in\n"
                       " *'worktree add'*) mkdir -p \"$6\" ;;\n"
                       "esac\nexit 0\n")
        git.chmod(0o755)
        env = dict(os.environ, PERF_JOB_TEST_ROOT=str(root), PERF_JOB_KIND=kind,
                   PERF_JOB_MODE=mode, PATH=str(git.parent) + os.pathsep + os.environ["PATH"],
                   _AUTANA_DEVICE_LOCK_ROOT=str(root / "locks"))
        env.pop(TOKEN_ENV, None)
        if borrowed:
            env[TOKEN_ENV] = "outer-token"
        command = ["sh", str(script or PERF / ("perf_" + kind + ".sh"))]
        if kind == "compare":
            command += [(root / "a").as_posix(), (root / ("a" if same else "b")).as_posix(), "--max-seeds", "8"]
        else:
            command += ["--build", "A=" + (root / "a").as_posix(), "--knob", "knob=1,2", "--runs", "1"]
        command += ["-o", (root / "out").as_posix(), "--suite", "suite", "-", "-", "--autana", override, *extra]
        if mode in ("interrupt", "terminate"):
            env["PERF_JOB_MODE"] = "interrupt"
            pid_file = root / "shell.pid"
            launch = ["sh", "-c", 'echo "$$" > "$1"; shift; exec "$@"',
                      "perf-job", pid_file.as_posix(), *command]
            process = subprocess.Popen(launch, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 30
                while not (root / "ready").exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                case.assertTrue((root / "ready").exists(), "job never reached capture")
                signame = "TERM" if mode == "terminate" else "INT"
                subprocess.run(["sh", "-c", 'kill -s "$1" "$2"', "signal", signame,
                                pid_file.read_text().strip()], env=env, check=True, timeout=10)
                stdout, stderr = process.communicate(timeout=60)
                result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        else:
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=90)
        calls_file = root / "calls.jsonl"
        calls = [json.loads(line) for line in calls_file.read_text().splitlines()] if calls_file.exists() else []
        return result, calls


def assert_job(case, kind, mode="ok", borrowed=False):
    result, calls = replay(case, kind, mode, borrowed=borrowed)
    case.assertEqual(result.returncode == 0, mode == "ok", result.stderr)
    if mode in ("interrupt", "terminate"):
        case.assertEqual(result.returncode, 130, result.stderr)
    holds = [call for call in calls if "take" in call["words"] or "run" in call["words"]]
    releases = [call for call in calls if "release" in call["words"]]
    case.assertEqual(len(holds), 0 if borrowed else 1, calls)
    case.assertEqual(len(releases), 0 if borrowed else 1, calls)
    if holds:
        case.assertIn("--estimate", holds[0]["words"])
        case.assertGreater(float(holds[0]["words"][holds[0]["words"].index("--estimate") + 1]), 0)
        case.assertIn("perf_" + kind, " ".join(holds[0]["words"]))
    board = [call for call in calls if "lock" not in call["words"]]
    case.assertTrue(any("suite" in call["words"] for call in board), calls)
    case.assertIn("flash", board[-1]["words"], calls)
    case.assertTrue(all(call["token"] == ("outer-token" if borrowed else "job-token") for call in board), calls)
    if releases:
        case.assertEqual(calls[-1], releases[0])


class ShellJobTests:
    def test_shell_job_holds_through_restore(self):
        for mode in ("ok", "fail", "interrupt", "terminate"):
            with self.subTest(mode=mode):
                assert_job(self, self.job_kind, mode)

    def test_shell_checkout_failure_takes_no_hold(self):
        result, calls = replay(self, self.job_kind, mode="checkout_fail")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_shell_job_borrows_existing_hold(self):
        assert_job(self, self.job_kind, borrowed=True)

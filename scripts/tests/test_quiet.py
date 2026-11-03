"""quiet_end's failure summary lists the pattern's lines, not text a passing test printed."""

import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
RUN_TOOL_TESTS = SCRIPTS / "run-tool-tests.sh"
FIXTURE = "test_clip:FAIL: Expected 3 Was 4"
FAILURE = "FAIL: test_real (module.Class)"


def summary(log_text, pattern):
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "run.log"
        script = (f'. "{SCRIPTS / "quiet.sh"}"\nquiet_begin "{log}" >/dev/null\n'
                  f'printf %s "$LOG" >>"{log}"\n{pattern}quiet_end sample 1\n')
        done = subprocess.run(["sh", "-c", script], capture_output=True, text=True,
                              env={"LOG": log_text, "PATH": "/usr/bin:/bin"})
        return done.stdout


class QuietSummaryTests(unittest.TestCase):
    LOG = f"{FIXTURE}\nerror: argparse fixture\n{FAILURE}\n"

    def test_default_pattern_lists_fixture_text(self):
        self.assertIn(FIXTURE, summary(self.LOG, ""))

    def test_narrowed_pattern_lists_only_the_failure_header(self):
        shown = summary(self.LOG, "QUIET_ERROR_PATTERN='^FAIL: '\n")
        self.assertIn(FAILURE, shown)
        self.assertNotIn(FIXTURE, shown)
        self.assertNotIn("argparse fixture", shown)

    def test_run_tool_tests_narrows_the_pattern(self):
        text = RUN_TOOL_TESTS.read_text(encoding="utf-8")
        self.assertIn("QUIET_ERROR_PATTERN=", text)


class QuietTeeTests(unittest.TestCase):
    def test_the_command_status_survives_the_pipe_and_both_streams_are_kept(self):
        for code in (0, 3):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                log = Path(directory) / "run.log"
                log.write_text("before\n", encoding="utf-8")
                script = (f'set -eu\n. "{SCRIPTS / "quiet.sh"}"\n'
                          f'quiet_tee "{log}" sh -c "echo out; echo err >&2; exit {code}"\n'
                          'echo "status=$QUIET_TEE_STATUS"\n')
                done = subprocess.run(["sh", "-c", script], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0)
                self.assertEqual(done.stdout.splitlines(), ["out", "err", f"status={code}"])
                self.assertEqual(log.read_text(encoding="utf-8"), "before\nout\nerr\n")
                self.assertEqual(list(Path(directory).iterdir()), [log])


if __name__ == "__main__":
    unittest.main()

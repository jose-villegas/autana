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


if __name__ == "__main__":
    unittest.main()

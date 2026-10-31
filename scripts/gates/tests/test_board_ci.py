"""Exercise the held Board CI sequence without accessing a board."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
STEPS = ROOT / "scripts/ci/board-steps.sh"
sys.path.insert(0, str(ROOT / "launcher/tools/perf/nightly"))
import scoped_chain  # noqa: E402


def selftest_cap():
    """The capture cap board-steps.sh gives the self-test, in seconds."""
    script = STEPS.read_text(encoding="utf-8").split("selftest_status=0")[0]
    done = subprocess.run([shutil.which("bash"), "-c", script + 'echo "$SELFTEST_MAX_SECONDS"'],
                          capture_output=True, text=True, check=True, stdin=subprocess.DEVNULL)
    return int(done.stdout)


class BoardSequenceTests(unittest.TestCase):
    def test_both_results_decide_the_sequence_verdict(self):
        steps = STEPS
        stub = '''
autana() {
    echo "$*" >> "$RUNNER_TEMP/calls"
    case "$1" in
        selftest) return "$CI_SELFTEST_EXIT" ;;
        flash) return 0 ;;
    esac
}
'''
        # The run's venv Python: answers resolve-port, records a pytest run.
        venv_stub = '''#!/bin/sh
case "$*" in
    *resolve-port*) printf 'COM7\\r\\n' ;;
    *pytest*) echo "pytest $*" >> "$RUNNER_TEMP/calls"; exit "$CI_PYTEST_EXIT" ;;
esac
'''
        for selftest, console in ((0, 0), (0, 1), (1, 0), (1, 1)):
            with self.subTest(selftest=selftest), tempfile.TemporaryDirectory() as directory:
                env = dict(os.environ, RUNNER_TEMP=Path(directory).as_posix(),
                           CI_SELFTEST_EXIT=str(selftest), CI_PYTEST_EXIT=str(console),
                           GITHUB_STEP_SUMMARY=str(Path(directory) / "summary"))
                venv_python = Path(directory) / "device-tests/Scripts/python"
                venv_python.parent.mkdir(parents=True)
                venv_python.write_text(venv_stub)
                venv_python.chmod(0o755)
                result = subprocess.run([shutil.which("bash"), "-c", stub + ". " + steps.as_posix()],
                                        env=env, cwd=directory, capture_output=True, text=True,
                                        stdin=subprocess.DEVNULL)
                self.assertEqual(result.returncode == 0, selftest == console == 0, result.stderr)
                calls = (Path(directory) / "calls").read_text().splitlines()
                self.assertEqual(calls[:2], [f"selftest {selftest_cap()}", "flash diag"])
                self.assertIn("--embedded-services idf,serial", calls[2])
                self.assertIn("--port COM7", calls[2])
                self.assertIn("--skip-autoflash y", calls[2])
                self.assertIn("--root-logdir", calls[2])


class SelftestCap(unittest.TestCase):
    def test_cap_fits_inside_each_callers_budget(self):
        workflow = (ROOT / ".github/workflows/board-ci.yml").read_text(encoding="utf-8")
        job_minutes = int(re.search(r"timeout-minutes: (\d+)", workflow)[1])
        self.assertLess(selftest_cap(), scoped_chain.SELFTEST_ALLOWANCE_SECONDS)
        self.assertLess(selftest_cap(), job_minutes * 60)

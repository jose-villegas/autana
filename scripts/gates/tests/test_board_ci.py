"""Exercise the held Board CI sequence without accessing a board."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


class BoardSequenceTests(unittest.TestCase):
    def test_both_results_decide_the_sequence_verdict(self):
        workflow = Path(__file__).resolve().parents[3] / ".github/workflows/board-ci.yml"
        sequence = workflow.read_text().split("<<'SH'\n", 1)[1].split("\n          SH", 1)[0]
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
                result = subprocess.run([shutil.which("bash"), "-c", stub + textwrap.dedent(sequence)],
                                        env=env, cwd=directory, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, selftest == console == 0, result.stderr)
                calls = (Path(directory) / "calls").read_text().splitlines()
                self.assertEqual(calls[:2], ["selftest", "flash diag"])
                self.assertIn("--embedded-services idf,serial", calls[2])
                self.assertIn("--port COM7", calls[2])
                self.assertIn("--skip-autoflash y", calls[2])
                self.assertIn("--root-logdir", calls[2])

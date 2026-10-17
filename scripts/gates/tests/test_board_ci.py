"""Exercise the held Board CI sequence without accessing a board."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


class BoardSequenceTests(unittest.TestCase):
    def test_selftest_failure_reaches_the_sequence_verdict(self):
        workflow = Path(__file__).resolve().parents[3] / ".github/workflows/board-ci.yml"
        sequence = workflow.read_text().split("<<'SH'\n", 1)[1].split("\n          SH", 1)[0]
        stub = '''
autana() {
    echo "$*" >> "$RUNNER_TEMP/calls"
    case "$1" in
        selftest) return "$CI_SELFTEST_EXIT" ;;
    esac
}
'''
        for selftest in (0, 1):
            with self.subTest(selftest=selftest), tempfile.TemporaryDirectory() as directory:
                env = dict(os.environ, RUNNER_TEMP=Path(directory).as_posix(),
                           CI_SELFTEST_EXIT=str(selftest))
                result = subprocess.run([shutil.which("bash"), "-c", stub + textwrap.dedent(sequence)],
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, selftest == 0, result.stderr)
                self.assertEqual((Path(directory) / "calls").read_text().splitlines(),
                                 ["selftest"])

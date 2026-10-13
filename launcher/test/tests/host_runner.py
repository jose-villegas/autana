"""Runs RUNSUITE requests through the real runner, for tests of what it prints.

`host_tests --run "<suite> [patterns]"` is suites_run_request() and
suites_print_run() as the board calls them, with the same RUN_TEST gate in
between, so a test that reads this output is pinned to the firmware's own
text: rewording a printf turns it red.
"""
import os
import subprocess
import sys
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parents[1]
REPO = TEST_DIR.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "device"))

sys.path.insert(0, str(REPO / "scripts" / "device" / "tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402

_binary = None


def binary():
    """The host test binary, built (incrementally) on first use."""
    global _binary
    if _binary is None:
        shell = device.git_bash()
        result = subprocess.run([shell, str(TEST_DIR / "run_tests.sh"), "--build-only"],
                                env=dict(os.environ, QUIET_INNER="1"),
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
        exe = ".exe" if os.name == "nt" else ""
        _binary = TEST_DIR / "build" / f"host_tests{exe}"
    return _binary


def run(*requests):
    """The runner's stdout after each request, in order."""
    command = [str(binary())]
    for request in requests:
        command += ["--run", request]
    return subprocess.run(command, capture_output=True, text=True).stdout

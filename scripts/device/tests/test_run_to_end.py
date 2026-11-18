"""device.run_to_end on real processes: a lost lock, a deadline and the exit code."""
import isolation  # noqa: F401
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device

# The grandchild touches its marker this long after it starts, unless the lost lock stopped its tree first.
GRANDCHILD_SECONDS = 4
LOST_AFTER_SECONDS = 1


class RunToEndTests(unittest.TestCase):
    def test_a_lost_lock_stops_the_command_and_its_grandchild(self):
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / "grandchild-outlived-the-lock"
            grandchild = (f"import time; from pathlib import Path; time.sleep({GRANDCHILD_SECONDS}); "
                          f"Path({str(marker)!r}).touch()")
            child = f"import subprocess, sys, time; subprocess.Popen([sys.executable, '-c', {grandchild!r}]); time.sleep(60)"
            lost = threading.Event()
            threading.Timer(LOST_AFTER_SECONDS, lost.set).start()
            started = time.monotonic()
            with self.assertRaises(device.LockLost):
                device.run_to_end([sys.executable, "-c", child], lost)
            self.assertLess(time.monotonic() - started, GRANDCHILD_SECONDS)
            time.sleep(GRANDCHILD_SECONDS + 1 - (time.monotonic() - started))
            self.assertFalse(marker.exists(), "the grandchild outlived the lost lock")

    def test_the_exit_code_and_the_deadline_reach_the_caller(self):
        device.run_to_end([sys.executable, "-c", "pass"], threading.Event())
        device.run_to_end([sys.executable, "-c", "pass"])
        with self.assertRaises(subprocess.CalledProcessError) as failed:
            device.run_to_end([sys.executable, "-c", "raise SystemExit(7)"], threading.Event())
        self.assertEqual(failed.exception.returncode, 7)
        with self.assertRaises(subprocess.TimeoutExpired):
            device.run_to_end([sys.executable, "-c", "import time; time.sleep(60)"], timeout=LOST_AFTER_SECONDS)


if __name__ == "__main__":
    unittest.main()

"""The CLI accepts tone lines emitted by the firmware's formatter."""
import sys
import unittest
from pathlib import Path

import host_runner

sys.path.insert(0, str(host_runner.REPO / "scripts/device"))
sys.path.insert(0, str(host_runner.REPO / "scripts/device/tests"))
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)

sys.path.insert(0, str(host_runner.REPO / "scripts/autana"))
import autana


class ToneReplyTest(unittest.TestCase):
    def test_firmware_reply_parses(self):
        lines = [line for line in host_runner.run("run_console_tone_suite reply_frequency").splitlines()
                 if line.startswith("TONE ")]
        self.assertEqual(len(lines), 4)
        results = [autana.parse_tone(line) for line in lines]
        self.assertEqual(results[0]["measured_hz"], results[0]["hz"])
        for result in results[1:]:
            self.assertIsNone(result["measured_hz"])
            self.assertIsNone(result["hz_error"])


if __name__ == "__main__":
    unittest.main()

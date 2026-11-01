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
    def test_firmware_gap_replies_parse(self):
        lines = [line for line in host_runner.run("run_console_tone_suite pulse_gap_placement").splitlines()
                 if line.startswith("TONE ")]
        self.assertEqual(len(lines), 6)
        results = [autana.parse_tone(line) for line in lines]
        self.assertEqual([row["imu_gaps"] for row in results], [1, 1, 1, 0, 0, 0])
        self.assertEqual([row["shake_reason"] for row in results],
                         ["gap inside pulse", "gap inside pulse", None] * 2)
        for index, result in enumerate(results):
            silence = result["silence_pulse"]
            tone = result["tone_pulse"]
            inside = index % 3 != 2
            self.assertEqual(silence["status"], "gap" if inside else "clean")
            self.assertEqual(tone["status"], "clean")
            self.assertEqual(tone["take_gaps"], 0)
            self.assertIsNone(tone["take_ms"])
            self.assertIsNone(tone["lost_ms"])
            self.assertEqual(silence["take_gaps"], 1 if index < 3 else 0)
            self.assertEqual(silence["pulse_gaps"], int(index < 3 and inside))
            self.assertEqual(silence["lost_samples"], int(index >= 3 and inside))
            time = silence["pulse_gap_ms"] if index < 3 else silence["lost_ms"]
            if inside:
                self.assertGreaterEqual(time, 0)
                self.assertLessEqual(time, result["pulse_ms"])
            else:
                self.assertIsNone(time)


    def test_broadband_noise_parses(self):
        reply = ("TONE heard 1000 Hz: -12.0 dB (silence -100.0 dB), noise -70.0 dBFS, "
                 "path acoustic, measured 1000.00 Hz, open 0 B DMA, thd -40.0 dB, peak -6.0 dBFS")
        self.assertEqual(autana.parse_tone(reply)["noise_dbfs"], -70.0)

    def test_firmware_reply_parses(self):
        lines = [line for line in host_runner.run("run_console_tone_suite reply_frequency").splitlines()
                 if line.startswith("TONE ")]
        self.assertEqual(len(lines), 4)
        results = [autana.parse_tone(line) for line in lines]
        self.assertEqual([row["noise_dbfs"] for row in results], [-70.0] * 4)
        self.assertEqual(results[0]["measured_hz"], results[0]["hz"])
        for result in results[1:]:
            self.assertIsNone(result["measured_hz"])
            self.assertIsNone(result["hz_error"])


if __name__ == "__main__":
    unittest.main()

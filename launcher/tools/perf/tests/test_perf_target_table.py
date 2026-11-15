"""perf_target_table turns PERF TARGET lines into the table perf_compare reads."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import perf_target_table
from layout_measure import parse_report

CAPTURE = """I (9645) device_tests: PERF TARGET stress-card portrait r3d.draw: measured 1290 us, goal 1300 us, distance -1%
I (9650) device_tests: PERF TARGET stress-card portrait: measured 1757 us, goal 2752 us, distance -36%
I (9660) device_tests: PERF TARGET stress-card portrait: measured 1760 us, goal 2752 us, distance -36%
I (9670) device_tests: PERF TARGET SUMMARY: 0 unmet
"""


class PerfTargetTable(unittest.TestCase):
    def test_each_target_reads_back_as_its_last_measurement(self):
        with tempfile.TemporaryDirectory() as root:
            capture, table = Path(root) / "capture.log", Path(root) / "table.md"
            capture.write_text(CAPTURE, encoding="utf-8")
            self.assertEqual(perf_target_table.main([str(capture), str(table)]), 0)
            self.assertEqual(parse_report(table), {"stress-card portrait r3d.draw": 1290,
                                                   "stress-card portrait": 1760})

    def test_a_capture_with_no_targets_is_refused(self):
        with self.assertRaisesRegex(ValueError, "no PERF TARGET"):
            perf_target_table.render("I (1) device_tests: PERF TARGET SUMMARY: 0 unmet\n")


if __name__ == "__main__":
    unittest.main()

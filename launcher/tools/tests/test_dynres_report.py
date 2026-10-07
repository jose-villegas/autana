"""Per-path prediction error and quality from a small synthetic capture."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import dynres_report as report


class DynresReportTests(unittest.TestCase):
    def test_camera_keys_records_errors_and_missing_quality(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory) / "capture.log"
            capture.write_text("dynres_step: width 0 184x224 upscale 1\n"
                               "dynres_frames: camera predicted width 100 0 0:60:20:4:100 0:130:10:5:100\n"
                               "dynres_frames: tour predicted width 100 0 0:90:10:4:100\n"
                               "dynres_frames: tour fixed half 0 0 -1:90:10:4:0\n")
            _, _, ladders, runs = report.read_captures([str(capture)])
        self.assertEqual(len(runs), 3)
        self.assertEqual(runs[("camera", "predicted", "width", 100)][0], (0, 60, 20, 4, 100))
        rows = report.policy_rows(runs, ladders, {"camera": {(184, 224): [(0, 2.0, 0.9)]}}, {})
        self.assertEqual([r["camera"] for r in rows], ["camera", "tour", "tour"])
        self.assertEqual(rows[0]["delta_e"], 2.0)
        self.assertIsNone(rows[1]["delta_e"])
        table = report.prediction_table(runs)
        self.assertIn("| camera | width | 0.1 | 2 | 30.0% | 40.0% | 50.0% |", table)
        self.assertIn("| tour | width | 0.1 | 1 | 0.0% | 0.0% | 0.0% |", table)


if __name__ == "__main__":
    unittest.main()

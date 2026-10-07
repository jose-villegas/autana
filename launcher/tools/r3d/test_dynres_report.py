import pathlib
import unittest
import tempfile

import dynres_report as report


class DynresReportTests(unittest.TestCase):
    def test_findings_compare_fast_path_steps_and_cost_references(self):
        sizes = [(368, 448), (368, 224), (184, 448), (184, 298),
                 (184, 224), (184, 179), (184, 149), (147, 179), (122, 149)]
        splits = {size: {"mean": 1000} for size in sizes}
        table = report.findings_table(splits, {})
        for pair in ("184x298 minus 184x224", "184x224 minus 184x179",
                     "184x179 minus 147x179", "184x149 minus 122x149"):
            with self.subTest(pair=pair):
                self.assertIn(pair, table)

    def test_refit_metadata_merges_and_prices_the_prediction_note(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for i in range(2):
                capture = pathlib.Path(directory) / f"capture-{i}.log"
                capture.write_text(
                    f"dynres_refit_cost: calls 1000 mean_us {i + 1}.250 max_us {i + 4}\n"
                    f"dynres_refit: orbit width 66000 base {i + 10000} per_triangle 2.5000 "
                    "per_triangle_row 4.0000 per_pixel_share 20000\n", encoding="utf-8")
                paths.append(str(capture))
            refit = {}
            report.read_captures(paths, refit)
            self.assertEqual({"calls": 1000, "mean_us": 2.25, "max_us": 5.0}, refit["cost"])
            self.assertEqual({"base": 10001.0, "per_triangle": 2.5, "per_triangle_row": 4.0,
                              "per_pixel_share": 20000.0}, refit["weights"][("orbit", "width", 66000)])
            self.assertIn("1000 calls, mean 2.250 us, max 5 us per call", report.prediction_table({}, refit))
        self.assertNotIn("Online refit", report.prediction_table({}))

    def test_committed_capture_remains_readable(self):
        capture = pathlib.Path(__file__).resolve().parents[3] / "docs/render/data/dynamic-resolution-board.log"
        splits, spans, ladders, runs = report.read_captures([str(capture)])
        self.assertTrue(report.stages_table(splits, spans))
        self.assertTrue(report.findings_table(splits, spans))
        self.assertTrue(report.policies_table(report.policy_rows(runs, ladders, {}, splits)))


if __name__ == "__main__":
    unittest.main()

import pathlib
import unittest

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

    def test_committed_capture_remains_readable(self):
        capture = pathlib.Path(__file__).resolve().parents[3] / "docs/render/data/dynamic-resolution-board.log"
        splits, spans, ladders, runs = report.read_captures([str(capture)])
        self.assertTrue(report.stages_table(splits, spans))
        self.assertTrue(report.findings_table(splits, spans))
        self.assertTrue(report.policies_table(report.policy_rows(runs, ladders, {}, splits)))


if __name__ == "__main__":
    unittest.main()

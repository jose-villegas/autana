"""Regression tests for the revision performance report comparison.

    python -m unittest discover -s launcher/tools/perf/tests
"""
import pathlib
import sys
import tempfile
import unittest


PERF = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(PERF))

import perf_compare  # noqa: E402


class PerfCompareTest(unittest.TestCase):
    def test_a_markdown_table_uses_its_measured_column(self):
        rows = perf_compare.parse_report(FIXTURES / "sand_a.md")
        self.assertEqual(rows["hot"], 2000)

    def test_sponza_mean_lines_are_generic_named_number_rows(self):
        rows = perf_compare.parse_report(FIXTURES / "sponza.txt")
        self.assertEqual(rows, {"sponza": 51000, "lite": 42000, "flat": 39000})

    def test_worst_of_each_side_is_compared_per_row(self):
        a = [{"full": 100, "lite": 80}, {"full": 110, "lite": 78}]
        b = [{"full": 105, "lite": 82}, {"full": 104, "lite": 88}]
        self.assertEqual(perf_compare.compare(a, b), [
            ("full", 110, 105, -5, "improved"),
            ("lite", 80, 88, 8, "regressed"),
        ])

    def test_controls_turn_a_small_delta_into_no_change(self):
        a = [perf_compare.parse_report(FIXTURES / "sand_a.md")]
        b = [perf_compare.parse_report(FIXTURES / "sand_b.md")]
        controls = ("test_a_full_size_step_fits_in_the_frame_budget",
                    "test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget")
        hot = next(row for row in perf_compare.compare(a, b, controls) if row[0] == "hot")
        self.assertEqual(hot,
                         ("hot", 2000, 2010, 10, "no change"))

    def test_summary_lists_worst_values_and_build_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory) / "summary.md"
            perf_compare.write_summary(
                out, "before", "after", ["before-diag"], ["after-diag"],
                [FIXTURES / "sand_a.md"], [FIXTURES / "sand_b.md"])
            summary = out.read_text(encoding="utf-8")
            aggregate_path = out.parent / "worst.md"
            perf_compare.write_aggregate(
                aggregate_path, [perf_compare.parse_report(FIXTURES / "sand_a.md")])
            aggregate = aggregate_path.read_text(encoding="utf-8")
        self.assertIn("`before-diag`", summary)
        self.assertIn("| `hot` | 2000 | 2010 | +10 | no change |", summary)
        self.assertIn("| `hot` | ? | 2000 | ? | measured |", aggregate)


if __name__ == "__main__":
    unittest.main()

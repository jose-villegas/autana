"""layout_pilot.py: the layout-noise numbers from canned captures, and the
reading of `autana suite` output that feeds them.

    python -m unittest discover -s launcher/tools/perf/tests
"""
import json
import math
import pathlib
import sys
import tempfile
import unittest

PERF = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PERF))

import layout_pilot as pilot  # noqa: E402


class LayoutPilotTest(unittest.TestCase):
    def test_runs_inside_one_flash_are_run_noise_and_flash_to_flash_is_layout(self):
        seeds = [[1000, 1002, 998], [1010, 1012, 1008], [990, 992, 988], [1005, 1007, 1003]]
        result = pilot.analyse(seeds, flash_over_run=10)
        self.assertAlmostEqual(result["sigma_run"], 2 / 1001.25, places=6)
        self.assertGreater(result["sigma_layout"], 0.008)
        self.assertLess(result["sigma_layout"], 0.012)
        self.assertEqual(result["runs"], 3)

    def test_layout_spread_is_what_run_noise_alone_cannot_explain(self):
        quiet_layout = [[100, 102, 98], [100, 102, 98], [100, 102, 98], [100, 102, 98]]
        result = pilot.analyse(quiet_layout, flash_over_run=10)
        self.assertEqual(result["sigma_layout"], 0)
        self.assertIsNone(result["optimum_runs"])

    def test_the_optimum_is_kalibera_jones(self):
        seeds = [[1000, 1004], [1020, 1024], [980, 984], [1010, 1014]]
        result = pilot.analyse(seeds, flash_over_run=8)
        run_variance = 8.0
        layout_variance = result["seed_spread"] ** 2 * result["mean"] ** 2 - run_variance / 2
        expected = math.ceil(math.sqrt(8 * run_variance / layout_variance))
        self.assertEqual(result["optimum_runs"], expected)

    def test_a_tighter_interval_needs_more_seeds(self):
        loose = pilot.required_seeds(0.002, 0.001, 3, 0.005)
        tight = pilot.required_seeds(0.002, 0.001, 3, 0.001)
        self.assertGreater(tight, loose)
        self.assertGreaterEqual(loose, 2)

    def test_no_spread_needs_no_more_than_the_floor(self):
        self.assertIsNone(pilot.required_seeds(0, 0, 3, 0.001))

    def test_the_interval_the_seed_count_buys_is_the_one_asked_for(self):
        layout, run, runs, half_width = 0.003, 0.0005, 3, 0.002
        seeds = pilot.required_seeds(layout, run, runs, half_width)
        width = lambda k: pilot.t95(2 * (k - 1)) * math.sqrt(
            2 * (layout ** 2 + run ** 2 / runs) / k)
        self.assertLessEqual(width(seeds), half_width)
        self.assertGreater(width(seeds - 1), half_width)

    def test_run_seconds_run_from_one_batch_line_to_the_next(self):
        lines = [(1.0, "booted BUILD_ID=abc-diag to its console"),
                 (2.0, "batch: run_x run 1/2"),
                 (50.0, "report: /r/a.md"),
                 (52.0, "batch: run_x run 2/2"),
                 (110.0, "report: /r/b.md")]
        captures, first, seconds = pilot.captured_runs(lines, "run_x", 2)
        self.assertEqual([c.name for c in captures], ["a.log", "b.log"])
        self.assertEqual(first, 2.0)
        self.assertEqual(seconds, [50.0, 58.0])

    def test_a_missing_run_is_an_error_rather_than_a_short_sample(self):
        lines = [(2.0, "batch: run_x run 1/2"), (50.0, "report: /r/a.md")]
        with self.assertRaises(RuntimeError):
            pilot.captured_runs(lines, "run_x", 2)

    def test_load_reads_rows_back_per_suite_and_seed(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = pathlib.Path(temp)
            for seed, values in ((1, (10, 12)), (2, (20, 22))):
                runs = []
                for number, value in enumerate(values):
                    table = directory / f"s{seed}_{number}.md"
                    table.write_text(f"| Test | Measured (us) |\n|---|---:|\n| `a` | {value} |\n",
                                     encoding="utf-8")
                    runs.append({"capture": str(table), "table": str(table)})
                (directory / f"seed_{seed}.json").write_text(json.dumps({
                    "seed": seed, "flash_seconds": 100.0, "build_id": "x",
                    "suites": {"suite": {"run_seconds": [30.0, 30.0], "runs": runs}}}),
                    encoding="utf-8")
            loaded = pilot.load(directory)
        self.assertEqual(loaded["suite"][1]["rows"], {"a": [10, 12]})
        self.assertEqual(loaded["suite"][2]["flash"], 100.0)
        self.assertEqual(loaded["suite"][2]["run"], 30.0)


if __name__ == "__main__":
    unittest.main()

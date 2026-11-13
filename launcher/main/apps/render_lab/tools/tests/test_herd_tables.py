"""herd_tables.py turns run_herd_perf_suite lines into perf_compare's table, averaging repeated runs.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_herd_tables.py
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import herd_tables  # noqa: E402

P = herd_tables.PREFIX
Q = herd_tables.QUARTER
CAPTURE = f"""I (1) herd_perf: {P}n=8 internal_taken=10 internal_free=20 internal_largest=5
I (2) herd_perf: {P}n=8 samples=100 step_mean_us=900 step_p95_us=950
I (3) herd_perf: {P}n=8 samples=100 frame_mean_us=20000 frame_p95_us=21000 in_view_mean=6.00 {Q}portrait
I (4) herd_perf: {P}n=8 anim=tiered samples=100 step_mean_us=400 step_p95_us=450 step_max_us=800
I (5) herd_perf: {P}n=8 anim=tiered samples=100 frame_mean_us=18000 frame_p95_us=19000 {Q}landscape
RUN=2
I (2) herd_perf: {P}n=8 samples=100 step_mean_us=1000 step_p95_us=1050
"""


class HerdTables(unittest.TestCase):
    def test_rows_are_named_by_size_tier_part_and_quarter_and_averaged(self):
        rows = dict(line.strip("| ").split(" | ") for line in herd_tables.perf_table(CAPTURE).splitlines()[2:])
        self.assertEqual({"herd-8 step": "950", "herd-8 frame portrait": "20000", "herd-8 tiered step": "400",
                          "herd-8 tiered frame landscape": "18000"}, rows)

    def test_a_capture_without_timings_is_refused(self):
        with self.assertRaisesRegex(ValueError, "no herd_perf"):
            herd_tables.perf_table("I (1) other: n=1\n")


if __name__ == "__main__":
    unittest.main()

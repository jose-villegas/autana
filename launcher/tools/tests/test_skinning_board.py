"""Board budgets use the last measurements in each run and medians across runs."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "r3d/skin_light"))
from report_skinning_board import parse_capture


def run_capture(number, mean):
    lines = [f"RUN={number}"]
    for phase in ("before_enter", "loaded", "playing"):
        lines.append(f"capybara_perf: phase={phase} internal_free={mean} internal_largest={mean}")
    for stage in ("skin", "animate"):
        for cores in ("1", "2"):
            lines.append(f"capybara_perf: stage={stage} cores={cores} samples=100 vertices=800 "
                         f"mean_us={mean} us_per_vertex={mean / 800:.3f}")
    lines.append(f"capybara_perf: stage=animation_step cores=unknown samples=100 vertices=800 mean_us={mean}")
    for paused in (0, 1):
        lines.append(f"capybara_perf: stage=render paused={paused} samples=100 mean_us={mean - paused}")
    lines.append("capybara_perf: stage=render_delta mean_us=1")
    return "\n".join(lines) + "\n"


class SkinningBoard(unittest.TestCase):
    def test_several_runs_use_median_not_mean(self):
        result = parse_capture("BUILD_ID=sample-diag\n" + run_capture(1, 100)
                               + run_capture(2, 900) + run_capture(3, 300))
        self.assertEqual(result["build"], "sample-diag")
        self.assertEqual(result["run_count"], 3)
        self.assertEqual(result["stages"][("skin", "1")], 300)
        self.assertEqual(result["per_vertex"][("skin", "1")], 0.375)
        self.assertEqual(result["phases"]["playing"], (300, 300))
        self.assertEqual(result["render"], (300, 299, 1))

    def test_last_phase_and_stage_in_each_run_win(self):
        result = parse_capture("BUILD_ID=sample-diag\n" + run_capture(1, 100)
                               + "capybara_perf: phase=loaded internal_free=200 internal_largest=150\n"
                               + "capybara_perf: stage=skin cores=1 samples=100 vertices=800 "
                                 "mean_us=400 us_per_vertex=0.500\n")
        self.assertEqual(result["phases"]["loaded"], (200, 150))
        self.assertEqual(result["stages"][("skin", "1")], 400)

    def test_missing_stage_is_loud_when_no_run_logged_it(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 300) + run_capture(2, 300)
        text = text.replace("capybara_perf: stage=animate cores=2 samples=100 vertices=800 mean_us=300 us_per_vertex=0.375\n", "")
        with self.assertRaisesRegex(ValueError, "animate.*2"):
            parse_capture(text)

    def test_partial_stage_uses_only_logged_runs_and_counts_them(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100) + run_capture(2, 900) + run_capture(3, 300)
        text = text.replace("capybara_perf: stage=render paused=0 samples=100 mean_us=900\n", "")
        result = parse_capture(text)
        self.assertEqual(result["render"][0], 200)
        self.assertEqual(result["render_counts"], (2, 3, 3))

    def test_missing_phase_and_build_fail(self):
        with self.assertRaisesRegex(ValueError, "build"):
            parse_capture(run_capture(1, 100))
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        with self.assertRaisesRegex(ValueError, "playing"):
            parse_capture(text.replace("capybara_perf: phase=playing internal_free=100 internal_largest=100\n", ""))


if __name__ == "__main__":
    unittest.main()

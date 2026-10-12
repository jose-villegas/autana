"""Board budgets use the last measurements in each run and medians across runs."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "r3d/skin_light"))
from report_skinning_board import (PHASES, PHASE_LOADED, PHASE_PLAYING, PREFIX, STAGE_ANIMATE,
                                   STAGE_ANIMATION_STEP, STAGE_RENDER, STAGE_RENDER_DELTA,
                                   STAGE_SKIN, parse_capture)


def run_capture(number, mean):
    lines = [f"RUN={number}"]
    for phase in PHASES:
        lines.append(f"{PREFIX} phase={phase} internal_free={mean} internal_largest={mean}")
    for stage in (STAGE_SKIN, STAGE_ANIMATE):
        for cores in ("1", "2"):
            lines.append(f"{PREFIX} stage={stage} cores={cores} samples=100 vertices=800 "
                         f"mean_us={mean} us_per_vertex={mean / 800:.3f}")
    lines.append(f"{PREFIX} stage={STAGE_ANIMATION_STEP} cores=unknown samples=100 vertices=800 mean_us={mean}")
    for paused in (0, 1):
        lines.append(f"{PREFIX} stage={STAGE_RENDER} paused={paused} samples=100 mean_us={mean - paused}")
    lines.append(f"{PREFIX} stage={STAGE_RENDER_DELTA} mean_us=1")
    return "\n".join(lines) + "\n"


class SkinningBoard(unittest.TestCase):
    def test_several_runs_use_median_not_mean(self):
        result = parse_capture("BUILD_ID=sample-diag\n" + run_capture(1, 100)
                               + run_capture(2, 900) + run_capture(3, 300))
        self.assertEqual(result["build"], "sample-diag")
        self.assertEqual(result["run_count"], 3)
        self.assertEqual(result["stages"][(STAGE_SKIN, "1")], 300)
        self.assertEqual(result["per_vertex"][(STAGE_SKIN, "1")], 0.375)
        self.assertEqual(result["phases"][PHASE_PLAYING], (300, 300))
        self.assertEqual(result["render"], (300, 299, 1))

    def test_last_phase_and_stage_in_each_run_win(self):
        result = parse_capture("BUILD_ID=sample-diag\n" + run_capture(1, 100)
                               + f"{PREFIX} phase={PHASE_LOADED} internal_free=200 internal_largest=150\n"
                               + f"{PREFIX} stage={STAGE_SKIN} cores=1 samples=100 vertices=800 "
                                 "mean_us=400 us_per_vertex=0.500\n")
        self.assertEqual(result["phases"][PHASE_LOADED], (200, 150))
        self.assertEqual(result["stages"][(STAGE_SKIN, "1")], 400)

    def test_missing_stage_is_loud_when_no_run_logged_it(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 300) + run_capture(2, 300)
        text = text.replace(f"{PREFIX} stage={STAGE_ANIMATE} cores=2 samples=100 vertices=800 mean_us=300 us_per_vertex=0.375\n", "")
        with self.assertRaisesRegex(ValueError, f"{STAGE_ANIMATE}.*2"):
            parse_capture(text)

    def test_partial_stage_uses_only_logged_runs_and_counts_them(self):
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100) + run_capture(2, 900) + run_capture(3, 300)
        text = text.replace(f"{PREFIX} stage={STAGE_RENDER} paused=0 samples=100 mean_us=900\n", "")
        result = parse_capture(text)
        self.assertEqual(result["render"][0], 200)
        self.assertEqual(result["render_counts"], (2, 3, 3))

    def test_missing_phase_and_build_fail(self):
        with self.assertRaisesRegex(ValueError, "build"):
            parse_capture(run_capture(1, 100))
        text = "BUILD_ID=sample-diag\n" + run_capture(1, 100)
        with self.assertRaisesRegex(ValueError, PHASE_PLAYING):
            parse_capture(text.replace(f"{PREFIX} phase={PHASE_PLAYING} internal_free=100 internal_largest=100\n", ""))


if __name__ == "__main__":
    unittest.main()

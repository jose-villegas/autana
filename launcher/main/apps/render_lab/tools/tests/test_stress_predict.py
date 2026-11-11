"""stress_predict reads stress_perf captures in the frame profile's replay format and scales each
preset's stage change by a scene's stage times."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[6]
TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TOOLS), str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf")]
import stress_predict
from build.c_header import read_define

REPLAY = read_define(ROOT / "launcher/test/frame_profile.h", "FRAME_PROFILE_REPLAY") + " quarter=%s"


def replay(row, cull, transform, draw, upscale, quarter="portrait", t=0):
    report = (f"r3d.cull {cull:.2f}/0.0  r3d.transform {transform:.2f}/0.0  r3d.draw {draw:.2f}/0.0  "
              f"r3d.upscale {upscale:.2f}/0.0 | total 1.00")
    return "I (1) stress_perf: " + REPLAY % (row, t, "nodes_visited=1", report, quarter) + "\n"


class StressPredictTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def write(self, name, text):
        path = pathlib.Path(self.folder.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_a_scene_moves_by_each_stage_ratio_times_its_own_stage_time(self):
        a = self.write("a.log", replay("stress-open", 1.0, 2.0, 10.0, 1.0) + replay("stress-open", 1.0, 2.0, 10.0, 1.0))
        b = self.write("b.log", replay("stress-open", 0.5, 2.0, 9.0, 1.0))
        scenes = self.write("scenes.log", replay("sponza", 2.0, 4.0, 30.0, 2.5)
                            + replay("sponza", 2.0, 4.0, 30.0, 2.5, quarter="landscape"))
        table = stress_predict.ratios(stress_predict.preset_stages([a], "portrait"), stress_predict.preset_stages([b], "portrait"))
        stages = stress_predict.scene_stages([scenes], [], set(), "portrait")
        # cull halves (-1.0 ms of sponza's 2.0), draw drops a tenth (-3.0 of 30.0).
        self.assertAlmostEqual(stress_predict.predict(table["stress-open"], stages["sponza"]), -4.0)
        self.assertAlmostEqual(stress_predict.predict(table[stress_predict.POOLED], stages["sponza"]), -4.0)

    def test_a_window_scene_averages_the_lines_that_hold_its_text(self):
        log = self.write("water.log", "x config=base row=rest ms/frame avg/worst: r3d.draw 12.00/1.0 | total 9\n"
                                      "x config=base row=tilt ms/frame avg/worst: r3d.draw 14.00/1.0 | total 9\n"
                                      "x config=other row=tilt ms/frame avg/worst: r3d.draw 99.00/1.0 | total 9\n")
        stages = stress_predict.scene_stages([], [("water", log, "config=base")], set(), "portrait")
        self.assertEqual(stages["water"]["r3d.draw"], 13.0)
        self.assertEqual(stages["water"]["r3d.cull"], 0.0)

    def test_the_pooled_change_is_each_stage_sum_over_sum_not_a_mean_of_ratios(self):
        a = self.write("a.log", replay("stress-open", 1.0, 1.0, 1.0, 1.0) + replay("stress-stack", 1.0, 1.0, 9.0, 1.0))
        b = self.write("b.log", replay("stress-open", 1.0, 1.0, 2.0, 1.0) + replay("stress-stack", 1.0, 1.0, 9.0, 1.0))
        table = stress_predict.ratios(stress_predict.preset_stages([a], "portrait"), stress_predict.preset_stages([b], "portrait"))
        # open's draw doubles and stack's holds: pooled 11/10, where a mean of ratios would be 1.5.
        self.assertEqual(table[stress_predict.POOLED]["r3d.draw"], (10.0, 11.0))
        self.assertEqual(list(table)[-1], stress_predict.POOLED)

    def test_every_pose_of_every_capture_in_the_quarter_counts_once(self):
        a = [self.write("a1.log", replay("stress-open", 1, 2, 4, 1)),
             self.write("a2.log", replay("stress-open", 1, 2, 8, 1) + replay("stress-open", 1, 2, 12, 1, t=500)
                        + replay("stress-open", 1, 2, 20, 1) + replay("stress-open", 1, 2, 99, 1, "landscape"))]
        stages = stress_predict.preset_stages(a, "portrait")
        # a2's pose at t=0 is replayed twice (a rerun case): only its last replay counts.
        self.assertEqual(sorted(stages["stress-open"]["r3d.draw"]), [4.0, 12.0, 20.0])

    def test_scene_rows_keep_only_the_named_rows_in_the_asked_quarter(self):
        scenes = self.write("scenes.log", replay("sponza", 1, 1, 30, 1) + replay("sponza", 1, 1, 50, 1, "landscape")
                            + replay("capybara", 1, 1, 6, 1))
        kept = stress_predict.scene_stages([scenes], [], {"sponza"}, "portrait")
        self.assertEqual(list(kept), ["sponza"])
        self.assertEqual(kept["sponza"]["r3d.draw"], 30.0)
        self.assertEqual(set(stress_predict.scene_stages([scenes], [], set(), "portrait")), {"sponza", "capybara"})
        self.assertEqual(stress_predict.scene_stages([scenes], [], set(), "landscape")["sponza"]["r3d.draw"], 50.0)

    def test_a_stage_with_no_time_in_a_adds_nothing(self):
        change = {"r3d.cull": (0.0, 0.5), "r3d.transform": (2.0, 1.0), "r3d.draw": (1.0, 1.0), "r3d.upscale": (1.0, 1.0)}
        scene = {"r3d.cull": 3.0, "r3d.transform": 4.0, "r3d.draw": 9.0, "r3d.upscale": 9.0}
        self.assertAlmostEqual(stress_predict.predict(change, scene), -2.0)

    def test_the_report_lists_presets_then_pooled_and_signs_each_scene(self):
        a = self.write("a.log", replay("stress-open", 1.0, 2.0, 10.0, 1.0) + replay("stress-stack", 1.0, 2.0, 10.0, 1.0))
        b = self.write("b.log", replay("stress-open", 0.5, 2.0, 10.0, 1.0) + replay("stress-stack", 1.0, 2.0, 11.0, 1.0))
        scenes = self.write("scenes.log", replay("sponza", 2.0, 4.0, 30.0, 2.5))
        water = self.write("water.log", "x config=base row=rest ms/frame avg/worst: r3d.draw 10.00/1.0 | total 9\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            stress_predict.main(["--a", str(a), "--b", str(b), "--scenes", str(scenes),
                                 "--window", "water", str(water), "config=base"])
        lines = out.getvalue().splitlines()
        self.assertEqual([line.split(" | ")[0] for line in lines[2:5]], ["| stress-open", "| stress-stack", "| all"])
        self.assertIn("| Scene | stress-open | stress-stack | all |", lines)
        self.assertIn("| sponza | -1.00 | +3.00 | +1.00 |", lines)
        self.assertIn("| water | +0.00 | +1.00 | +0.50 |", lines)

    def test_a_capture_with_no_replays_or_a_window_with_no_match_fails(self):
        empty = self.write("empty.log", "I (1) boot\n")
        with self.assertRaisesRegex(SystemExit, "no frame_replay lines"):
            stress_predict.preset_stages([empty], "portrait")
        log = self.write("water.log", "x config=base row=rest no window here\n")
        with self.assertRaisesRegex(SystemExit, "no frame-cost window"):
            stress_predict.scene_stages([], [("water", log, "config=base")], set(), "portrait")

    def test_a_preset_in_one_arm_only_fails(self):
        a = self.write("a.log", replay("stress-open", 1, 1, 1, 1))
        b = self.write("b.log", replay("stress-stack", 1, 1, 1, 1))
        with self.assertRaisesRegex(SystemExit, "stress-open, stress-stack"):
            stress_predict.ratios(stress_predict.preset_stages([a], "portrait"), stress_predict.preset_stages([b], "portrait"))


if __name__ == "__main__":
    unittest.main()

"""stress_predict reads stress_perf captures in the frame profile's replay format and scales each
preset's stage change by a scene's stage times."""
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


def replay(row, cull, transform, draw, upscale, quarter="portrait"):
    report = (f"r3d.cull {cull:.2f}/0.0  r3d.transform {transform:.2f}/0.0  r3d.draw {draw:.2f}/0.0  "
              f"r3d.upscale {upscale:.2f}/0.0 | total 1.00")
    return "I (1) stress_perf: " + REPLAY % (row, 0, "nodes_visited=1", report, quarter) + "\n"


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
        table = stress_predict.ratios(stress_predict.preset_stages([a]), stress_predict.preset_stages([b]))
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

    def test_a_preset_in_one_arm_only_fails(self):
        a = self.write("a.log", replay("stress-open", 1, 1, 1, 1))
        b = self.write("b.log", replay("stress-stack", 1, 1, 1, 1))
        with self.assertRaisesRegex(SystemExit, "stress-open, stress-stack"):
            stress_predict.ratios(stress_predict.preset_stages([a]), stress_predict.preset_stages([b]))


if __name__ == "__main__":
    unittest.main()

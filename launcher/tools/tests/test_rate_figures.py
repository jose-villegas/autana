"""The update-rate figure's measure (anim/playback/rate_figures.py): a copy posed every N frames is behind its
true pose by the motion since the last posed frame, and each tier is drawn at its threshold plus the hysteresis
the render settings give."""

import pathlib
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "anim" / "playback"))

import rate_figures  # noqa: E402

# One vertex moving one unit along x a frame.
STEP = 1.0
FRAMES = [[(i * STEP, 0.0, 0.0)] for i in range(8)]


class RateFiguresTest(unittest.TestCase):
    def test_a_copy_posed_every_frame_is_never_behind(self):
        self.assertEqual(rate_figures.lag(FRAMES, 1), [0.0] * len(FRAMES))

    def test_a_copy_posed_every_fourth_frame_falls_behind_until_its_next_pose(self):
        self.assertEqual(rate_figures.lag(FRAMES, 4), [0.0, STEP, 2 * STEP, 3 * STEP] * 2)

    def test_each_tier_is_drawn_at_its_threshold_plus_the_hysteresis(self):
        settings = {"anim.hysteresis_px": 4.0, "anim.mid_every": 2, "anim.mid_px": 48.0,
                    "anim.far_every": 4, "anim.far_px": 20.0}
        self.assertEqual([(every, px) for _, every, px in rate_figures.tiers(settings)], [(2, 52.0), (4, 24.0)])

    def test_lag_uses_the_worst_of_several_vertices(self):
        frames = [[(0, 0, 0), (0, 0, 0)], [(1, 0, 0), (0, 3, 4)],
                  [(9, 0, 0), (0, 0, 0)], [(10, 0, 0), (0, 0, 2)]]
        self.assertEqual(rate_figures.lag(frames, 2), [0, 5, 0, 2])

    def test_loop_frame_count_edges(self):
        class Asset:
            def __init__(self, duration):
                self.seconds = duration

            def duration(self, clip):
                return self.seconds

            def sample(self, clip, time):
                return time

            def skin(self, time):
                return [[(time, 0, 0)]], None

        frame = rate_figures.FRAME_MS / rate_figures.MS_PER_S
        for duration, count in ((0, 1), (frame / 2, 1), (frame, 1), (2 * frame, 2), (2.5 * frame, 2)):
            with self.subTest(duration=duration):
                times, frames = rate_figures.loop(Asset(duration), "clip")
                self.assertEqual(times, [i * frame for i in range(count)])
                self.assertEqual(len(frames), count)

    def test_tiers_read_the_default_settings_keys(self):
        settings = rate_figures.render_asset.read(rate_figures.render_asset.DEFAULT)
        got = [(every, px) for _, every, px in rate_figures.tiers(settings)]
        self.assertEqual(got, [(settings[f"anim.{name}_every"],
                               settings[f"anim.{name}_px"] + settings["anim.hysteresis_px"])
                              for name in rate_figures.TIER_NAMES])

    def test_the_radius_is_the_half_diagonal_of_the_box(self):
        self.assertAlmostEqual(rate_figures.bind_radius([(0.0, 0.0, 0.0), (2.0, 2.0, 1.0)]), 1.5)


if __name__ == "__main__":
    unittest.main()

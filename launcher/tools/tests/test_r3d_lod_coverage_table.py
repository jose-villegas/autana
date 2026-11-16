"""coverage_table.py's scoring: a cap's video against the finest level's."""
import pathlib
import sys
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.lod import coverage_table
from render_temporal import TEMPORAL_BLOCK_SIDE, TEMPORAL_POP_THRESHOLD

SIDE = 4 * TEMPORAL_BLOCK_SIDE
FRAMES = 6
FPS = 30
GREY = 100


def still(count, value=GREY):
    return [np.full((SIDE, SIDE, 3), value, dtype=np.uint8) for _ in range(count)]


def videos(**by_name):
    return lambda path: (FPS, iter(by_name[str(path)]))


class ScoredTest(unittest.TestCase):
    def test_a_cap_drawing_what_the_finest_level_draws_scores_nothing(self):
        with mock.patch.object(coverage_table, "read_video", videos(a=still(FRAMES), b=still(FRAMES))):
            self.assertEqual((0.0, 0.0), coverage_table.scored("a", "b"))

    def test_a_block_that_switches_pops_once_per_switch(self):
        def switched(frames):
            b = still(FRAMES)
            for frame in frames:
                b[frame][:TEMPORAL_BLOCK_SIDE, :TEMPORAL_BLOCK_SIDE] = GREY + 2 * TEMPORAL_POP_THRESHOLD
            with mock.patch.object(coverage_table, "read_video", videos(a=still(FRAMES), b=b)):
                return coverage_table.scored("a", "b")

        once, difference = switched(range(FRAMES // 2, FRAMES))
        there_and_back, _ = switched([FRAMES // 2])
        # One switch pops part of one of the FRAMES - 1 changes; a switch back pops it again.
        self.assertGreater(once, 0.0)
        self.assertLess(once, 100 / (FRAMES - 1))
        self.assertAlmostEqual(2 * once, there_and_back)
        self.assertGreater(difference, 0.0)

    def test_a_steady_offset_differs_but_never_pops(self):
        with mock.patch.object(coverage_table, "read_video", videos(a=still(FRAMES), b=still(FRAMES, GREY + 50))):
            popping, difference = coverage_table.scored("a", "b")
        self.assertEqual(0.0, popping)
        self.assertAlmostEqual(50.0, difference)


class CapTextTest(unittest.TestCase):
    def test_caps_read_as_shares_of_the_height_and_the_largest_as_none(self):
        self.assertEqual("25%", coverage_table.cap_text(250))
        self.assertEqual("none", coverage_table.cap_text(coverage_table.UNCAPPED))


if __name__ == "__main__":
    unittest.main()

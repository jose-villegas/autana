"""Checks the fitted-variant recipe's pose handling: which poses it holds
out, and that the poses pruning counts over cover the panel held either way
up. Needs NumPy."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    from r3d.fitted_variant import poses_text, split_poses
    from r3d.poses import either_way, parse_poses
except ImportError:
    parse_poses = None


@unittest.skipIf(parse_poses is None, "needs NumPy")
class FittedVariantTests(unittest.TestCase):
    def test_the_multiples_of_the_held_out_step_are_held_out_but_time_zero_trains(self):
        fit = SimpleNamespace(train_every_ms=1000, held_out_every_ms=5000)
        training, held_out = split_poses(fit, list(range(12)))
        self.assertEqual(held_out, [5, 10])
        self.assertEqual(training, [0, 1, 2, 3, 4, 6, 7, 8, 9, 11])

    def test_the_coverage_view_is_as_wide_as_either_orientation(self):
        width, height, lens, near, poses = either_way(184, 224, 0.62, 6.0, ["pose"])
        self.assertEqual((width, height, near, poses), (224, 224, 6.0, ["pose"]))
        # Portrait spans 0.62 across and 0.62 * 224 / 184 down; landscape the other way round.
        self.assertAlmostEqual(lens, 0.62 * 224 / 184)

    def test_a_written_poses_file_reads_back(self):
        text = poses_text(184, 224, 0.62, 6.0, [[1.0, 2.0, 3.0, 0.0, 0.0, -1.0]])
        width, height, lens, near, poses = parse_poses(text)
        self.assertEqual((width, height, lens, near), (184, 224, 0.62, 6.0))
        self.assertEqual(list(poses[0]), [1.0, 2.0, 3.0, 0.0, 0.0, -1.0])


if __name__ == "__main__":
    unittest.main()

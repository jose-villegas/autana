"""Checks which reference picture texture_figures.py sets beside a path frame: the training pose the viewer draws
that frame at, one path step after the frame's index."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d" / "texture"))

try:
    import texture_figures
except ImportError:
    texture_figures = None

ROOT = TOOLS.parents[1]
BISTRO = ROOT / "launcher/demo/bistro/bistro.scene.toml"


@unittest.skipIf(texture_figures is None, "needs NumPy and Pillow")
class ReferenceMemberTests(unittest.TestCase):
    FIT = SimpleNamespace(train_every_ms=1000, held_out_every_ms=5000)

    def test_frame_n_is_the_pose_one_step_later(self):
        # Frame 0 is drawn at 1000 ms, the second training pose; frame 5 at 6000 ms, the sixth, as 5000 ms is held out.
        self.assertEqual(texture_figures.reference_member(self.FIT, 0, 1000), "reference/0001.png")
        self.assertEqual(texture_figures.reference_member(self.FIT, 5, 1000), "reference/0005.png")

    def test_a_frame_at_a_held_out_pose_is_refused(self):
        with self.assertRaises(SystemExit):
            texture_figures.reference_member(self.FIT, 4, 1000)

    def test_the_bistro_street_sheet_pairs_its_frame_with_its_own_pose(self):
        fit = texture_figures.object_fit(BISTRO, "street_fitted_full")
        # LTEX.md's sheet draws frame 10 at 1000 ms, the pose at 11000 ms: training pose 9.
        self.assertEqual(texture_figures.reference_member(fit, 10, 1000), "reference/0009.png")


if __name__ == "__main__":
    unittest.main()

"""Checks which reference picture texture_figures.py sets beside a path frame: the training pose the viewer draws
that frame at, one path step after the frame's index."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d" / "texture"))

try:
    import texture_figures
except ImportError:
    texture_figures = None

ROOT = TOOLS.parents[1]
BISTRO = ROOT / "launcher/demo/bistro/bistro.scene.toml"
sys.path.insert(0, str(ROOT / "launcher/demo/bistro"))


@unittest.skipIf(texture_figures is None, "needs NumPy and Pillow")
class SquareBoxTests(unittest.TestCase):
    """Every divide sheet square is one cell: a cluster of any shape crops as a square inside the picture."""
    PICTURE = (240, 320)

    @staticmethod
    def cluster(x0, y0, x1, y1):
        return SimpleNamespace(x0=x0, y0=y0, x1=x1, y1=y1)

    def test_a_tall_cluster_crops_a_square_centred_on_it(self):
        self.assertEqual(texture_figures.square_box(self.cluster(100, 100, 110, 140), self.PICTURE),
                         (85, 100, 125, 140))

    def test_a_cluster_at_the_edge_is_moved_inside_the_picture(self):
        self.assertEqual(texture_figures.square_box(self.cluster(230, 300, 240, 320), self.PICTURE),
                         (220, 300, 240, 320))

    def test_a_wide_cluster_crops_a_square_centred_on_it(self):
        self.assertEqual(texture_figures.square_box(self.cluster(100, 100, 140, 110), self.PICTURE),
                         (100, 85, 140, 125))

    def test_a_cluster_larger_than_the_picture_crops_the_picture_s_shorter_side(self):
        self.assertEqual(texture_figures.square_box(self.cluster(0, 0, 240, 320), self.PICTURE), (0, 40, 240, 280))


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
        import bistro_figures
        street = bistro_figures.STREET
        fit = texture_figures.object_fit(BISTRO, street[street.index("--object") + 1])
        # LTEX.md's sheet draws frame 10 at 1000 ms, the pose at 11000 ms: training pose 9.
        self.assertEqual(texture_figures.reference_member(fit, 10, 1000), "reference/0009.png")


@unittest.skipIf(texture_figures is None, "needs NumPy and Pillow")
class MainRefusalTests(unittest.TestCase):
    FIT = SimpleNamespace(train_every_ms=1000, held_out_every_ms=5000)
    BASE = ["--scene", "s.toml", "--object", "o", "--reference", "r", "--dt", "1000", "--title", "t",
            "--image", "a.png", "--out", "out"]
    FAR = ["--far-frame", "0", "--far-image", "far.png", "--far-title", "far"]

    def refused(self, args):
        """Runs main() with the viewer and every subprocess mocked; returns the mocks, the call count of each."""
        with mock.patch.object(texture_figures, "object_fit", return_value=self.FIT),                 mock.patch.object(texture_figures, "draw") as draw,                 mock.patch.object(texture_figures, "run") as run,                 mock.patch.object(texture_figures, "reference") as reference:
            with self.assertRaises(SystemExit):
                texture_figures.main(args)
        self.assertEqual(0, draw.call_count + run.call_count + reference.call_count)

    def test_a_partial_set_of_the_far_flags_is_refused(self):
        for given in range(0, 6, 2):
            for flags in ([self.FAR[given:given + 2]], [self.FAR[:given] + self.FAR[given + 2:]]):
                with self.subTest(flags=flags):
                    self.refused(["--frame", "0", *self.BASE, *flags[0]])

    def test_a_held_out_frame_is_refused_before_any_draw(self):
        self.refused(["--frame", "4", *self.BASE])

    def test_a_held_out_far_frame_is_refused_before_any_draw(self):
        self.refused(["--frame", "0", *self.BASE, "--far-frame", "4", "--far-image", "f.png", "--far-title", "f"])

    def test_a_training_frame_with_all_three_far_flags_is_drawn(self):
        with mock.patch.object(texture_figures, "object_fit", return_value=self.FIT),                 mock.patch.object(texture_figures, "draw", side_effect=RuntimeError("drawn")) as draw:
            with self.assertRaises(RuntimeError):
                texture_figures.main(["--frame", "0", *self.BASE, *self.FAR])
        self.assertEqual(1, draw.call_count)


if __name__ == "__main__":
    unittest.main()

"""Checks the probe figure (r3d/views/probe_figures.py): its views frame the box behind each cut, and its projection
is camera_rays' inverse."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.poses import camera_rays
    from r3d.views import probe_figures
except ImportError:
    np = None


LENS = 0.62


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeFigureViewTests(unittest.TestCase):
    def test_each_view_stands_above_its_own_corner_and_frames_the_box_behind_its_cut(self):
        lo, hi = np.array([-4.0, 0.0, -2.0]), np.array([4.0, 3.0, 2.0])
        centre, views = probe_figures.views(lo, hi, LENS)
        np.testing.assert_allclose(centre, (lo + hi) / 2)
        self.assertEqual(len({(np.sign(eye[0]), np.sign(eye[2])) for eye, _ in views}), 4, "two views share a corner")
        lattice = probe_figures.frame_lattice(lo, hi)
        for eye, forward in views:
            self.assertGreater(eye[1], hi[1], "the view is not from above")
            behind = lattice[(lattice - centre) @ probe_figures.cut_normal(forward) >= 0]
            xy, front = probe_figures.project(behind, eye, forward, LENS)
            self.assertTrue(front.all())
            edge = np.abs(xy / probe_figures.VIEW_SIZE * 2 - 1).max()
            self.assertLessEqual(edge, 1 - probe_figures.FRAME_MARGIN + 1e-6, "the box behind the cut leaves the view")
            self.assertGreater(edge, 1 - 2 * probe_figures.FRAME_MARGIN, "the view stands farther back than it needs")

    def test_a_point_along_a_pixels_ray_projects_to_that_pixel(self):
        eye, forward = np.array([1.0, 5.0, -2.0]), np.array([0.3, -0.6, 0.7])
        width, height = probe_figures.VIEW_SIZE
        origin, direction = camera_rays(width, height, LENS, eye, forward, 1)
        pixels = [(0, 0), (width - 1, 0), (width // 3, height // 2), (width - 1, height - 1)]
        rows = [y * width + x for x, y in pixels]
        xy, front = probe_figures.project(origin[rows] + direction[rows] * 7.0, eye, forward, LENS)
        self.assertTrue(front.all())
        np.testing.assert_allclose(xy, np.array(pixels) + 0.5, atol=1e-6)


if __name__ == "__main__":
    unittest.main()

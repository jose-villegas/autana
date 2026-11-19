"""Checks the probe figure (r3d/views/probe_figures.py): its views frame the box behind each cut, its projection is
camera_rays' inverse, and the fates it finds agree with the selection's record or stop it."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.geometry import face_normals
    from r3d.poses import camera_rays
    from r3d.views import probe_figures, probes
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402
from tests.test_r3d_views_probes import closed_box  # noqa: E402

LENS = 0.62


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeFigureViewTests(unittest.TestCase):
    def test_each_view_stands_above_its_own_corner_and_frames_the_box_behind_its_cut(self):
        lo, hi = np.array([-4.0, 0.0, -2.0]), np.array([4.0, 3.0, 2.0])
        centre, views = probe_figures.views(lo, hi, LENS)
        np.testing.assert_allclose(centre, (lo + hi) / 2)
        self.assertEqual(len({(np.sign(eye[0]), np.sign(eye[2])) for eye, _ in views}), 4, "two views share a corner")
        steps = np.linspace(0, 1, probe_figures.FRAME_SAMPLES)
        lattice = lo + np.stack(np.meshgrid(steps, steps, steps, indexing="ij"), axis=-1).reshape(-1, 3) * (hi - lo)
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


@needs_mitsuba
@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeFigureFateTests(unittest.TestCase):
    def setUp(self):
        from r3d import ray_query

        self.addCleanup(setattr, ray_query, "VARIANT", ray_query.VARIANT)
        ray_query.VARIANT = "scalar_rgb"
        positions, tris = closed_box(2.5)
        self.source = SimpleNamespace(p=positions, tri_v=tris)
        self.query = ray_query.RayQuery(positions, tris, "scalar_rgb")
        # A region around the box: probes in its middle cells lie deeper in it than a probe may move.
        self.lo, self.hi, self.spacing, self.near = [-3.0, -3.0, -3.0], [3.0, 3.0, 3.0], 1.5, 0.05

    def placed(self):
        points, cell = probes.probe_points(self.lo, self.hi, self.spacing, probe_figures.PROBE_SEED)
        return probes.classify(self.query, face_normals(self.source.p, self.source.tri_v)[0], points, cell,
                               self.near)

    def record(self, moved, state):
        return {"region": [self.lo, self.hi], "spacing": self.spacing,
                "train": {"probes": len(moved), "inside": int(np.count_nonzero(state == "inside")),
                          "near": int(np.count_nonzero(state == "near"))}}

    def test_a_probe_with_a_kept_face_is_kept_and_a_live_one_without_is_dropped_by_greedy(self):
        moved, state = self.placed()
        live = np.flatnonzero(state == "kept")
        self.assertGreater(len(live), 1)
        kept_face = np.concatenate([moved[live[0]], probes.FACES[0]])
        found, counts = probe_figures.fates(self.query, self.source, self.record(moved, state), self.near, [kept_face])
        self.assertEqual(found[live[0]][1], "kept")
        self.assertEqual(len(found[live[0]][2]), 1)
        self.assertEqual(counts["kept"], 1)
        self.assertEqual(counts["greedy"], len(live) - 1)
        self.assertGreater(counts["inside"], 0, "no probe of the region fell inside the box")
        self.assertEqual(sum(counts.values()), len(moved))

    def test_fates_that_disagree_with_the_record_stop_the_figure(self):
        moved, state = self.placed()
        record = self.record(moved, state)
        record["train"]["inside"] += 1
        with self.assertRaises(RuntimeError):
            probe_figures.fates(self.query, self.source, record, self.near, [])


if __name__ == "__main__":
    unittest.main()

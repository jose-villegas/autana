"""Checks the fit's probe views (r3d/fit/probes.py), the path-first prune and the closure gate
(r3d/appearance_simplify.py): placement, the inside test on a closed box, the greedy pick, and the ranking."""

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import appearance_simplify, reference_set
    from r3d.appearance_simplify import FitMesh, prune
    from r3d.fit import probes
    from r3d.geometry import face_normals
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402


def closed_box(half):
    """A cube of side 2 `half` at the origin, its faces wound outward."""
    corners = np.array([[x, y, z] for x in (-half, half) for y in (-half, half) for z in (-half, half)], dtype=float)
    quads = ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3))
    tris = np.array([tri for a, b, c, d in quads for tri in ((a, b, c), (a, c, d))])
    return corners, tris


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbePlacementTests(unittest.TestCase):
    def test_one_probe_per_cell_stays_inside_its_cell(self):
        lo, hi, spacing = np.zeros(3), np.array([4.0, 2.0, 1.0]), 1.0
        points, cell = probes.probe_points(lo, hi, spacing, seed=3)
        self.assertEqual(len(points), 4 * 2 * 1)
        self.assertEqual(cell, 1.0)
        cells = np.floor(points / spacing)
        self.assertEqual(len({tuple(c) for c in cells}), len(points), "two probes share a cell")
        offset = points - (cells + 0.5) * spacing
        self.assertTrue(np.all(np.abs(offset) <= probes.JITTER * spacing))
        np.testing.assert_array_equal(probes.probe_points(lo, hi, spacing, seed=3)[0], points)

    def test_the_spacing_defaults_to_near_planes_and_widens_under_the_count_cap(self):
        near = 0.5
        lo, hi = np.zeros(3), np.full(3, probes.PROBE_SPACING_NEAR_PLANES * near * 2)
        self.assertEqual(probes.spacing_for(lo, hi, None, near), probes.PROBE_SPACING_NEAR_PLANES * near)
        self.assertEqual(probes.spacing_for(lo, hi, 7.0, near), 7.0)
        spacing = probes.spacing_for(lo, hi, 1e-3, near)
        cells = np.prod(np.maximum(np.ceil((hi - lo) / spacing), 1))
        self.assertLessEqual(cells, probes.PROBE_COUNT_MAX)
        self.assertGreater(cells, probes.PROBE_COUNT_MAX / 2, "the spacing widened past what the cap needs")

    def test_greedy_faces_pick_the_largest_gain_first_and_stop_at_the_plateau(self):
        sets = [np.array([0, 1]), np.arange(10), np.array([9, 10, 11]), np.array([12])]
        picked, gains = probes.greedy_faces(sets, np.zeros(13, bool), plateau=0.15)
        self.assertEqual(picked, [1, 2])
        self.assertEqual(gains, [10, 2], "a face's gain counted triangles an earlier pick shows")
        seen = np.zeros(13, bool)
        seen[:10] = True
        self.assertEqual(probes.greedy_faces(sets, seen)[0], [2, 3], "what the path shows still counted")
        self.assertEqual(probes.greedy_faces(sets, np.zeros(13, bool), limit=1)[0], [1])


@needs_mitsuba
@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeInsideTests(unittest.TestCase):
    def setUp(self):
        from r3d import ray_query

        self.variant = ray_query.VARIANT
        ray_query.VARIANT = "scalar_rgb"
        self.addCleanup(setattr, ray_query, "VARIANT", self.variant)

    def test_a_probe_in_a_closed_box_is_dropped_and_one_near_a_wall_moves_off_it(self):
        from r3d.ray_query import RayQuery

        positions, tris = closed_box(1.0)
        query, normals = RayQuery(positions, tris, "scalar_rgb"), face_normals(positions, tris)[0]
        near = 0.1
        points = np.array([[0.0, 0.0, 0.0], [1.0 + 0.05, 0.0, 0.0], [3.0, 0.0, 0.0]])
        moved, state = probes.classify(query, normals, points, cell=1.0, near=near)
        self.assertEqual(state.tolist(), ["inside", "kept", "kept"])
        self.assertGreater(moved[1, 0], points[1, 0] + near, "the probe near the wall did not move off it")
        np.testing.assert_array_equal(moved[2], points[2])

    def test_a_cube_face_shows_the_wall_it_faces(self):
        from r3d.ray_query import RayQuery

        positions, tris = closed_box(1.0)
        query = RayQuery(positions, tris, "scalar_rgb")
        poses = probes.cube_poses(np.array([[3.0, 0.0, 0.0]]))
        sets = probes.shown(query, len(tris), poses, 16, 16, probes.FACE_LENS)
        facing = face_normals(positions, tris)[0] @ np.array([1.0, 0.0, 0.0]) > 0.5
        self.assertEqual(set(sets[1].tolist()), set(np.flatnonzero(facing).tolist()), "the -x face missed the wall")
        self.assertEqual(len(sets[0]), 0, "the +x face, looking away, showed the box")


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PathFirstPruneTests(unittest.TestCase):
    def mesh(self, count):
        return FitMesh(np.zeros((3 * count, 3)), np.zeros((3 * count, 3)), np.arange(3 * count).reshape(count, 3),
                       np.zeros(count), 8, np.arange(3 * count))

    def test_one_row_keeps_the_most_shown_and_never_the_unshown(self):
        shown = np.array([0, 50, 3, 0, 900, 7])
        _, kept = prune(self.mesh(6), shown, 3)
        self.assertEqual(kept.tolist(), [1, 4, 5])
        _, ranked = prune(self.mesh(6), shown[None], 3)
        self.assertEqual(ranked.tolist(), kept.tolist(), "one ranked row ordered unlike one row")
        self.assertEqual(len(prune(self.mesh(6), shown, 6)[1]), 4)

    def test_what_the_path_shows_outranks_a_larger_probe_only_triangle(self):
        path = np.array([5, 0, 1, 0])
        probe = np.array([0, 900, 0, 40])
        _, kept = prune(self.mesh(4), np.stack([path, probe]), 3)
        self.assertEqual(kept.tolist(), [0, 1, 2], "a probe-only triangle outranked one the path shows")
        _, kept = prune(self.mesh(4), np.stack([path, probe]), 4)
        self.assertEqual(kept.tolist(), [0, 1, 2, 3])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ClosureTests(unittest.TestCase):
    SIZE = (100, 100)
    PIXELS = SIZE[0] * SIZE[1]
    START_HOLES = 100

    def test_closure_allows_the_limit_and_refuses_one_pixel_more(self):
        growth = int(appearance_simplify.CLOSURE_GROWTH * self.PIXELS)
        check = appearance_simplify.check_closure
        self.assertIn("hole pixels", check(self.START_HOLES, self.START_HOLES + growth, self.PIXELS))
        check(self.START_HOLES, 0, self.PIXELS)
        with self.assertRaisesRegex(ValueError, "closure"):
            check(self.START_HOLES, self.START_HOLES + growth + 1, self.PIXELS)

    def test_main_checks_the_pruned_fit_before_writing(self):
        mesh = PathFirstPruneTests().mesh(2)
        size = self.SIZE
        views = [object()]
        for fitted_holes in (self.START_HOLES, self.PIXELS):
            with self.subTest(fitted_holes=fitted_holes), tempfile.TemporaryDirectory() as directory:
                out = pathlib.Path(directory)
                def write_mesh(*args):
                    (out / "mesh.bundle").touch()
                    return len(mesh.tris)
                with contextlib.ExitStack() as stack:
                    for name, value in dict(load_scene=SimpleNamespace(camera=SimpleNamespace(
                            component=SimpleNamespace(background=0))), start_mesh=mesh,
                            load_views=(views, size), optimise=(mesh.points, mesh.rgb, [1.0]),
                            coverage=np.ones(len(mesh.tris)), stretch_report={
                                "small": dict(stretch=0, area_growth=0, moved=0),
                                "rest": dict(stretch=0, area_growth=0, moved=0),
                                "worst": dict(stretch=[], area_growth=[], moved=[])}).items():
                        stack.enter_context(mock.patch.object(appearance_simplify, name, return_value=value))
                    holes = stack.enter_context(mock.patch.object(appearance_simplify, "hole_pixels",
                                                                 side_effect=[self.START_HOLES, fitted_holes]))
                    stack.enter_context(mock.patch.object(appearance_simplify, "write_mesh", write_mesh))
                    args = [*FitArgumentTests.BASE[:-1], str(out), *FitArgumentTests.HELD, "--budget", "1"]
                    if fitted_holes == self.PIXELS:
                        with self.assertRaisesRegex(ValueError, "closure"):
                            appearance_simplify.main(args)
                        self.assertFalse((out / "mesh.bundle").exists())
                    else:
                        self.assertEqual(appearance_simplify.main(args), 0)
                        self.assertTrue((out / "mesh.bundle").exists())
                    self.assertEqual(holes.call_count, 2)
                    self.assertIs(holes.call_args_list[0].args[0], mesh)
                    self.assertEqual(len(holes.call_args_list[1].args[0].tris), 1)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class FitArgumentTests(unittest.TestCase):
    BASE = ["--scene", "s.toml", "--start", "a.bundle", "--poses", "p.txt", "--reference", "r", "--out", "o"]
    HELD = ["--held-out-poses", "h.txt", "--held-out-reference", "h"]

    def refused(self, *extra):
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit):
            appearance_simplify.main([*self.BASE, *extra])
        return error.getvalue()

    def test_held_out_poses_and_references_must_pair(self):
        self.assertIn("--held-out-reference", self.refused("--held-out-poses", "h.txt"))
        self.assertIn("--held-out-reference", self.refused("--held-out-reference", "h"))

@unittest.skipIf(np is None, "the r3d environment is not installed")
class PrepareReferencesTests(unittest.TestCase):
    PROBES = SimpleNamespace(face=8, spacing=1.0)

    def prepare(self, probes, path=None):
        camera = SimpleNamespace(path=path, region=None, near_z=0.1)
        inputs = SimpleNamespace(scene=lambda: SimpleNamespace(camera=SimpleNamespace(component=camera)),
                                 poses=dict(train_every_ms=1, held_out_every_ms=2, coverage_every_ms=1), size=(4, 4),
                                 probes=probes, job=lambda: None,
                                 sets=lambda: reference_set.fit_sets(path is not None, probes is not None))
        rendered = []
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory) / "work"
            # Three path poses split into two training and one held out; three probe faces into one and two.
            path_poses = (4, 4, 1.0, 0.1, [[0.0] * 6] * 3)
            with mock.patch("r3d.reference_render.render_sets", lambda _inputs, sets: rendered.append(sets)), \
                 mock.patch("r3d.reference_render.lit_source", lambda *a, **k: SimpleNamespace(p=None, tri_v=None)), \
                 mock.patch("r3d.fit.probes.select", lambda *a: ([[1.0] * 6], [[2.0] * 6] * 2, {})), \
                 mock.patch("r3d.mesh_source.camera_path_poses", lambda *a, **k: path_poses):
                count = reference_set.prepare_references(inputs, work)
            written = sorted(p.name for p in work.iterdir())
        return count, written, [[(p.name, r.name) for p, r in sets] for sets in rendered]

    def test_without_a_path_only_the_probe_files_are_written_and_rendered_and_counted(self):
        count, written, rendered = self.prepare(self.PROBES)
        self.assertEqual(written, ["probes.json", "probes.txt", "probes_held_out.txt"])
        self.assertEqual(rendered, [[("probes.txt", "reference_probes"),
                                     ("probes_held_out.txt", "reference_probes_held_out")]])
        self.assertEqual(count, 1)

    def test_with_a_path_and_probes_the_count_adds_both_training_sets(self):
        count, written, rendered = self.prepare(self.PROBES, path=object())
        self.assertEqual(count, 2 + 1)
        self.assertEqual([name for name, _ in rendered[0]],
                         ["train.txt", "train_landscape.txt", "probes.txt", "held_out.txt", "probes_held_out.txt"])


if __name__ == "__main__":
    unittest.main()

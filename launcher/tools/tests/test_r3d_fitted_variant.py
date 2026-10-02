"""Checks the fitted-variant recipe's pose handling: which poses it holds
out, and that the poses pruning counts over cover the panel held either way
up. Needs NumPy."""

import pathlib
import sys
import tempfile
import unittest
import unittest.mock
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    from r3d import fitted_variant
    from r3d.fitted_variant import poses_text, split_poses
    from r3d.poses import either_way, parse_poses
except ImportError:
    fitted_variant = None
    parse_poses = None

try:
    from r3d import lit_mesh, mesh_import, reference_render
except ImportError:
    lit_mesh = mesh_import = reference_render = None


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


@unittest.skipIf(fitted_variant is None, "needs the r3d environment")
class SweepTests(unittest.TestCase):
    def test_mesh_names_map_to_the_host_scene_keys(self):
        self.assertEqual(fitted_variant.host_scene_key("tiny_fitted"), "tiny-fitted")

    def test_board_cost_uses_the_native_held_out_render_size(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / "held_out.txt").write_text(poses_text(2, 3, 0.5, 1.0, [[0, 0, 0, 0, 0, -1]]))
            path = fitted_variant.board_poses(work)
            self.assertEqual(parse_poses(path.read_text())[:2], (2, 3))

    @unittest.skipIf(reference_render is None, "needs the r3d renderer")
    def test_a_sweep_reuses_one_prepared_reference_set(self):
        geometry = SimpleNamespace(positions=[], rgb=[], tris=[0], tri_double=[], scale={})
        poses = (2, 3, 0.5, 1.0, [[0, 0, 0, 0, 0, -1], [1, 0, 0, 0, 0, -1]])
        variant = SimpleNamespace(name="tiny_fitted", fit=SimpleNamespace(train_every_ms=1, held_out_every_ms=2,
                                                                            coverage_every_ms=1), visibility=SimpleNamespace(
                                                                                source="camera_path"))
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            with unittest.mock.patch.object(fitted_variant, "reference_digest", return_value="same"), \
                 unittest.mock.patch.object(mesh_import, "bake_geometry", return_value=geometry), \
                 unittest.mock.patch.object(mesh_import, "camera_path_poses", return_value=poses), \
                 unittest.mock.patch.object(lit_mesh, "write_lit_mesh"), \
                 unittest.mock.patch.object(reference_render, "main") as render:
                fitted_variant.sweep_references("scene", "data", SimpleNamespace(path="settings"), variant, work)
                fitted_variant.sweep_references("scene", "data", SimpleNamespace(path="settings"), variant, work)
        self.assertEqual(render.call_count, 3)

    def test_the_front_and_knee_keep_the_best_tradeoffs(self):
        points = [
            {"predicted_ms": 2.0, "mean_delta_e": 6.0},
            {"predicted_ms": 3.0, "mean_delta_e": 4.0},
            {"predicted_ms": 4.0, "mean_delta_e": 3.0},
            {"predicted_ms": 3.5, "mean_delta_e": 5.0},
            {"predicted_ms": 5.0, "mean_delta_e": 3.0},
        ]
        front, knee = fitted_variant.non_dominated_front(points)
        self.assertEqual(front, points[:3])
        self.assertIs(knee, points[1])

    def test_the_sweep_csv_has_the_public_columns(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "sweep.csv"
            fitted_variant.write_sweep_csv(path, [{name: 1.0 for name in fitted_variant.CSV_FIELDS}])
            self.assertEqual(path.read_text().splitlines()[0].split(","), list(fitted_variant.CSV_FIELDS))

    def test_a_finished_point_is_not_fit_again(self):
        calls = []

        def fit(point, point_dir):
            calls.append(point)
            return {"triangles": point["budget"], "mean_delta_e": 1.0,
                    "p95_delta_e": 2.0, "predicted_ms": 3.0}

        with tempfile.TemporaryDirectory() as directory:
            points = [{"budget": 4, "cost_weight": 0.0}]
            self.assertEqual(fitted_variant.run_sweep_points(pathlib.Path(directory), points, fit), 1)
            self.assertEqual(fitted_variant.run_sweep_points(pathlib.Path(directory), points, fit), 0)
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()

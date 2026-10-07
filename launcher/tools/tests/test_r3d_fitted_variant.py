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
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

try:
    from r3d import fitted_variant
    from r3d.fitted_variant import poses_text, split_poses
    from r3d.poses import either_way, parse_poses
except ImportError:
    fitted_variant = None
    parse_poses = None

try:
    from r3d import mesh_import, reference_render
except ImportError:
    mesh_import = reference_render = None

try:
    from PIL import Image
    import render_compare
except ImportError:
    Image = render_compare = None


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


def fake_job(object_name, asset_name, variant, fitted=True):
    return SimpleNamespace(object=SimpleNamespace(name=object_name), asset_name=asset_name,
                           renderer=SimpleNamespace(fit=object() if fitted else None, variant=SimpleNamespace(name=variant)))


@unittest.skipIf(fitted_variant is None, "needs the r3d environment")
class PlacedVariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from r3d.import_settings import load_scene
        scene = pathlib.Path(__file__).resolve().parents[3] / "launcher/demo/sponza/sponza.scene.toml"
        try:
            cls.sponza = load_scene(scene)
        except Exception as error:
            raise unittest.SkipTest(f"the Sponza scene does not load here: {error}")

    def test_a_scene_object_name_picks_its_own_renderer_even_when_a_variant_is_shared(self):
        for name in ("atrium_fitted_full", "atrium_flat_fitted", "atrium_fitted"):
            job = fitted_variant.placed_variant(self.sponza, name)
            self.assertEqual(job.object.name, name)
        self.assertEqual(fitted_variant.placed_variant(self.sponza, "atrium_flat_fitted").renderer.variant.name,
                         "sponza_fitted_full")

    def test_an_asset_name_picks_its_own_renderer(self):
        job = fitted_variant.placed_variant(self.sponza, "sponza.atrium_flat_fitted")
        self.assertEqual(job.object.name, "atrium_flat_fitted")

    def test_a_variant_only_one_fitted_object_uses_names_that_object(self):
        self.assertEqual(fitted_variant.placed_variant(self.sponza, "sponza_fitted").object.name, "atrium_fitted")

    def test_a_variant_two_fitted_objects_share_is_refused_naming_both(self):
        with self.assertRaises(fitted_variant.SettingsError) as caught:
            fitted_variant.placed_variant(self.sponza, "sponza_fitted_full")
        message = str(caught.exception)
        self.assertIn("atrium_fitted_full", message)
        self.assertIn("atrium_flat_fitted", message)
        self.assertNotIn("atrium_fitted,", message)

    def test_unknown_names_and_objects_that_are_not_fitted_are_refused(self):
        for name in ("no_such_object", "sponza_lite", "atrium", "atrium_flat", "atrium_lite", "sponza"):
            with self.assertRaisesRegex(fitted_variant.SettingsError, "no fitted variant"):
                fitted_variant.placed_variant(self.sponza, name)

    def test_an_object_name_wins_over_another_objects_variant_of_the_same_name(self):
        scene = SimpleNamespace(renderers=[fake_job("wanted", "s.wanted", "other"), fake_job("second", "s.second", "wanted")])
        self.assertEqual(fitted_variant.placed_variant(scene, "wanted").object.name, "wanted")

    def test_a_shared_variant_is_ambiguous_only_among_fitted_renderers(self):
        scene = SimpleNamespace(renderers=[fake_job("a", "s.a", "v"), fake_job("b", "s.b", "v", fitted=False)])
        self.assertEqual(fitted_variant.placed_variant(scene, "v").object.name, "a")


@unittest.skipIf(fitted_variant is None, "needs the r3d environment")
class SweepTests(unittest.TestCase):
    def test_fit_point_default_target_and_recipe_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            point_dir = pathlib.Path(directory) / "budget-42-cost-0.5"
            target = point_dir.parent / (point_dir.name + ".mesh")
            scene_path, scene, job, inputs = "scene.toml", object(), object(), pathlib.Path(directory) / "inputs"
            with unittest.mock.patch.object(fitted_variant, "fit", return_value=target) as fit:
                result = fitted_variant.fit_point(
                    {"budget": 42, "cost_weight": 0.5}, point_dir, scene_path, scene, job, inputs)
            fit.assert_called_once_with(scene_path, scene, job, point_dir, budget=42,
                                        cost_weight=0.5, smoke=False, target=target, inputs=inputs)
            self.assertEqual(result, {"mesh": str(target)})

    def test_fit_point_failure_reports_log_tail_and_preserves_traceback(self):
        def fail_fit(*args, **kwargs):
            for line in range(45):
                print(f"fit output {line}")
            raise ValueError("fit failure sentinel")

        with tempfile.TemporaryDirectory() as directory:
            point_dir = pathlib.Path(directory) / "point"
            with unittest.mock.patch.object(fitted_variant, "fit", side_effect=fail_fit):
                with self.assertRaises(RuntimeError) as caught:
                    fitted_variant.fit_point({"budget": 42}, point_dir, "scene", None, None, directory)
            log_path = point_dir / "fit.log"
            log = log_path.read_text()
            self.assertIn("Traceback (most recent call last):", log)
            self.assertIn("ValueError: fit failure sentinel", log)
            self.assertIn("fit output 0\n", log)
            self.assertEqual(str(caught.exception),
                             f"fit failed: {log_path}\n" + "\n".join(log.splitlines()[-30:]))
            self.assertNotIn("fit output 0\n", str(caught.exception))
            self.assertIsInstance(caught.exception.__cause__, ValueError)

    def test_the_viewer_draws_the_scene_object_alone(self):
        job = SimpleNamespace(asset_name="tiny.walls_fitted")
        self.assertEqual(fitted_variant.viewer_args(job, 3, 5000),
                         "--scene tiny --object walls_fitted --quarter 0 --frames 3 --dt 5000")

    @unittest.skipIf(Image is None, "needs the synthetic render scorer")
    def test_held_out_score_keeps_each_mesh_and_pose_with_its_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            references = work / "reference_held_out"
            references.mkdir()
            (work / "held_out.txt").write_text(poses_text(2, 3, 0.5, 1.0, [[0, 0, 0, 0, 0, -1], [1, 0, 0, 0, 0, -1]]))
            job = SimpleNamespace(asset_name="tiny.fitted", renderer=SimpleNamespace(
                variant=SimpleNamespace(name="tiny_fitted"), fit=SimpleNamespace(held_out_every_ms=5)))
            meshes = [work / name for name in ("source.mesh", "first.mesh", "second.mesh")]
            for value, mesh in enumerate(meshes):
                mesh.write_text(str(value))
            for pose in (1, 2):
                self._synthetic_picture(0, pose).save(references / ("%04d.png" % (pose - 1)))
            with unittest.mock.patch.object(fitted_variant, "_score_mesh", side_effect=self._synthetic_score):
                aligned = fitted_variant.held_out_score(job, meshes[0], work, work / "host")
                first = fitted_variant.held_out_score(job, meshes[1], work, work / "host")
                second = fitted_variant.held_out_score(job, meshes[2], work, work / "host")
                one_pose_late = self._synthetic_score(
                    SimpleNamespace(render_args="--scene tiny --object fitted --frames 3 --dt 5", reference=references,
                                    reference_first=1), job.asset_name, meshes[0], work / "late", work / "host")
        self.assertLess(aligned[0], 0.01)
        self.assertNotAlmostEqual(first[0], second[0], places=3)
        self.assertGreater(one_pose_late[0], aligned[0] + 8.0)

    @staticmethod
    def _synthetic_picture(mesh, pose):
        return Image.new("RGB", (4, 4), (30 + mesh * 50 + pose * 25, 60 + mesh * 40 + pose * 15, 90 + mesh * 30 + pose * 10))

    @classmethod
    def _synthetic_score(cls, args, _name, pack, _work, _host):
        fields = args.render_args.split()
        frames = int(fields[fields.index("--frames") + 1])
        mesh = int(pathlib.Path(pack).read_text())
        rendered = [cls._synthetic_picture(mesh, pose) for pose in range(1, frames + 1)]
        rendered = rendered[getattr(args, "reference_first", 0):]
        references = [Image.open(path) for path in sorted(pathlib.Path(args.reference).glob("*.png"))]
        if len(rendered) != len(references):
            raise ValueError("render and reference frame counts differ")
        values = [render_compare.reference_measure(render, reference).mean_delta_e for render, reference in zip(rendered, references)]
        return sum(values) / len(values), max(values), 1.0, 0.0, 0.0

    def test_board_cost_uses_the_native_held_out_render_size(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / "held_out.txt").write_text(poses_text(2, 3, 0.5, 1.0, [[0, 0, 0, 0, 0, -1]]))
            path = fitted_variant.board_poses(work)
            self.assertEqual(parse_poses(path.read_text())[:2], (2, 3))

    @unittest.skipIf(reference_render is None, "needs the r3d renderer")
    def test_a_sweep_reuses_one_prepared_reference_set(self):
        start = SimpleNamespace(tris=[0])
        poses = (2, 3, 0.5, 1.0, [[0, 0, 0, 0, 0, -1], [1, 0, 0, 0, 0, -1]])
        renderer = SimpleNamespace(variant=SimpleNamespace(name="tiny_fitted"),
                                   fit=SimpleNamespace(train_every_ms=1, held_out_every_ms=2, coverage_every_ms=1),
                                   visibility=SimpleNamespace(source="camera_path"))
        job = SimpleNamespace(settings=SimpleNamespace(path="settings"), renderer=renderer, object=SimpleNamespace(name="tiny"))
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            with unittest.mock.patch.object(fitted_variant, "reference_digest", return_value="same"), \
                 unittest.mock.patch.object(mesh_import, "write_baked", return_value=start), \
                 unittest.mock.patch.object(mesh_import, "camera_path_poses", return_value=poses), \
                 unittest.mock.patch.object(reference_render, "main") as render:
                fitted_variant.sweep_references("scene", "data", job, work)
                fitted_variant.sweep_references("scene", "data", job, work)
        self.assertEqual(render.call_count, 3)
        for call in render.call_args_list:
            argv = call.args[0]
            self.assertEqual(argv[argv.index("--object") + 1], "tiny", "each reference names its object")

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

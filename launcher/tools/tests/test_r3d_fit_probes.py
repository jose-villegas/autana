"""Checks probe placement, face selection and the fit's held-out closure gate."""

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
    from r3d.appearance_simplify import FitMesh
    from r3d.views import probes
    from r3d.geometry import face_normals
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402



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
        mesh = FitMesh(np.zeros((6, 3)), np.zeros((6, 3)), np.arange(6).reshape(2, 3),
                       np.zeros(2), 8, np.arange(6))
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
                            load_views=(views, size), pose_views=(views, size), optimise=(mesh.points, mesh.rgb, [1.0]),
                            stretch_report={
                                "small": dict(stretch=0, area_growth=0, moved=0),
                                "rest": dict(stretch=0, area_growth=0, moved=0),
                                "worst": dict(stretch=[], area_growth=[], moved=[])}).items():
                        stack.enter_context(mock.patch.object(appearance_simplify, name, return_value=value))
                    holes = stack.enter_context(mock.patch.object(appearance_simplify, "hole_pixels",
                                                                 side_effect=[self.START_HOLES, fitted_holes]))
                    stack.enter_context(mock.patch.object(appearance_simplify, "coverage",
                                                         side_effect=[np.array([0, 0]), np.array([1, 0])]))
                    stack.enter_context(mock.patch.object(appearance_simplify, "write_mesh", write_mesh))
                    args = [*FitArgumentTests.BASE[:-1], str(out), *FitArgumentTests.HELD,
                            "--coverage-poses", "path.txt", "--coverage-poses", "probes.txt"]
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
                                 probes=probes, job=lambda: SimpleNamespace(settings=SimpleNamespace(alpha_keep=None)),
                                 sets=lambda: reference_set.fit_sets(path is not None, probes is not None))
        rendered = []
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory) / "work"
            # Three path poses split into two training and one held out; three probe faces into one and two.
            path_poses = (4, 4, 1.0, 0.1, [[0.0] * 6] * 3)
            with mock.patch("r3d.reference_render.render_sets", lambda _inputs, sets: rendered.append(sets)), \
                 mock.patch("r3d.mesh_source.load_source", lambda *a, **k: SimpleNamespace(p=None, tri_v=None)), \
                 mock.patch("r3d.views.probes.select", lambda *a, **k: ([[1.0] * 6], [[2.0] * 6] * 2, {})), \
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


    def test_an_alpha_masked_import_traces_probes_through_cut_texels_of_the_unmasked_source(self):
        from r3d.light import AlphaTest

        source = SimpleNamespace(p="positions", tri_v="triangles", textures=[])
        for keep_alpha in (None, 0.5):
            camera = SimpleNamespace(region=None, near_z=0.1)
            inputs = SimpleNamespace(scene=lambda: SimpleNamespace(camera=SimpleNamespace(component=camera)),
                                     probes=self.PROBES,
                                     job=lambda: SimpleNamespace(settings=SimpleNamespace(alpha_keep=keep_alpha)))
            selected = {}

            def select(*args, **kwargs):
                selected.update(args=args, **kwargs)
                return [], [], {}

            with self.subTest(keep_alpha=keep_alpha), tempfile.TemporaryDirectory() as directory, \
                 mock.patch("r3d.mesh_source.load_source", return_value=source), \
                 mock.patch("r3d.ray_query.RayQuery", lambda *a, **k: SimpleNamespace(args=a, **k)), \
                 mock.patch("r3d.views.probes.select", select):
                reference_set.probe_poses(inputs, pathlib.Path(directory), None)
                self.assertEqual(selected["args"][:2], ("positions", "triangles"), "the source was masked first")
                query = selected["query"]
                if keep_alpha is None:
                    self.assertIsNone(query, "an import without alpha_mask got a cut-out query")
                    continue
                self.assertEqual(query.args, ("positions", "triangles"))
                self.assertIsInstance(query.hit_filter, AlphaTest)
                self.assertEqual(query.hit_filter.keep_alpha, keep_alpha)


@needs_mitsuba
@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeSourceTests(unittest.TestCase):
    def test_an_alpha_masked_obj_keeps_its_cut_card_for_the_probes_and_finds_its_cut_texture(self):
        from PIL import Image
        from r3d import ray_query

        self.addCleanup(setattr, ray_query, "VARIANT", ray_query.VARIANT)
        ray_query.VARIANT = "scalar_rgb"
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            pixels = np.full((4, 4, 4), 255, dtype=np.uint8)
            pixels[..., 3] = 0
            Image.fromarray(pixels).save(root / "leaf.png")
            (root / "card.mtl").write_text("newmtl wall\nKd 1 1 1\nnewmtl leaf\nKd 1 1 1\nmap_Kd leaf.png\n")
            (root / "card.obj").write_text("".join(f"v {x} {y} {z}\n" for z in (0, 1) for x, y in
                                                   ((-1, -1), (1, -1), (1, 1), (-1, 1))) +
                                           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                           "usemtl wall\nf 1/1 2/2 3/3 4/4\nusemtl leaf\nf 5/1 6/2 7/3 8/4\n")
            camera = SimpleNamespace(region=None, near_z=0.1)
            settings = SimpleNamespace(source={"path": root / "card.obj"}, alpha_keep=0.5)
            inputs = SimpleNamespace(scene=lambda: SimpleNamespace(camera=SimpleNamespace(component=camera)),
                                     probes=SimpleNamespace(face=8, spacing=1.0),
                                     job=lambda: SimpleNamespace(settings=settings))
            selected = {}

            def select(positions, tris, *args, **kwargs):
                selected.update(tris=tris, **kwargs)
                return [], [], {}

            with mock.patch("r3d.views.probes.select", select):
                reference_set.probe_poses(inputs, root, None)
        self.assertEqual(len(selected["tris"]), 4, "the probes' source lost the masked card")
        np.testing.assert_array_equal(selected["query"].hit_filter.tested, [False, True])


if __name__ == "__main__":
    unittest.main()

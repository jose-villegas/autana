"""Checks how the fit uses its probe views (r3d/views/probes.py): the path-first prune and the settle rule that pick
a budget (r3d/appearance_simplify.py), and the probe files a reference set writes."""

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
    from r3d.appearance_simplify import FitMesh, prune, settle, settled
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402



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
class SettleTests(unittest.TestCase):
    def test_the_budget_settles_where_holes_stop_falling(self):
        self.assertEqual(settled([1000, 400, 390, 100], 0.05), 1, "settled past the knee")
        self.assertEqual(settled([1000, 400, 0], 0.05), 2)
        self.assertEqual(settled([0, 0], 0.05), 0, "a budget with no holes was passed over")
        self.assertEqual(settled([1000, 500, 250], 0.05), 2, "stopped before the last budget while holes halved")
        self.assertEqual(settled([7], 0.5), 0)
        with self.assertRaises(ValueError):
            settled([], 0.05)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SettleSearchTests(unittest.TestCase):
    """settle() prunes the fit to the budget the held-out holes pick; the hole count is faked per triangle count."""

    COUNT = 10

    def run_settle(self, holes_at, budget, steps, held_out=(("views", "size"),)):
        mesh = FitMesh(np.zeros((3 * self.COUNT, 3)), np.zeros((3 * self.COUNT, 3)),
                       np.arange(3 * self.COUNT).reshape(self.COUNT, 3), np.zeros(self.COUNT), 8,
                       np.arange(3 * self.COUNT))
        evaluated = []

        def holes(drawn, *_rest):
            evaluated.append(len(drawn.tris))
            return holes_at[len(drawn.tris)]
        with mock.patch.object(appearance_simplify, "hole_pixels", holes):
            result = settle(mesh, None, np.arange(1, self.COUNT + 1), budget, steps, list(held_out), (0, 0, 0))
        return result, evaluated

    def test_without_steps_it_prunes_once_to_the_budget_and_does_not_search(self):
        (mesh, kept, record), evaluated = self.run_settle({7: 5}, 7, None)
        self.assertEqual((len(mesh.tris), len(kept), evaluated), (7, 7, [7]))
        self.assertEqual(record, {"budgets": [7], "holes": [5], "budget": 7})

    def test_without_held_out_views_it_prunes_once_and_records_nothing(self):
        (mesh, kept, record), evaluated = self.run_settle({}, 8, None, held_out=())
        self.assertEqual((len(mesh.tris), len(kept), record, evaluated), (8, 8, {}, []))

    def test_the_search_tries_each_step_then_the_budget_even_off_the_step(self):
        holes = {3: 100, 6: 60, 9: 30, 10: 0}
        (mesh, _kept, record), evaluated = self.run_settle(holes, 10, (3, 0.05))
        self.assertEqual(evaluated, [3, 6, 9, 10])
        self.assertEqual(record, {"budgets": [3, 6, 9, 10], "holes": [100, 60, 30, 0], "budget": 10})
        self.assertEqual(len(mesh.tris), 10)

    def test_it_stops_at_the_plateau_and_returns_the_settled_budget_not_the_last_tried(self):
        holes = {2: 100, 4: 50, 6: 49, 8: 0, 10: 0}
        (mesh, kept, record), evaluated = self.run_settle(holes, 10, (2, 0.1))
        self.assertEqual(evaluated, [2, 4, 6], "went on past the plateau")
        self.assertEqual(record, {"budgets": [2, 4, 6], "holes": [100, 50, 49], "budget": 4})
        self.assertEqual((len(mesh.tris), len(kept)), (4, 4), "returned the last budget tried")

    def test_it_stops_when_a_budget_has_no_holes(self):
        holes = {2: 100, 4: 0, 6: 0, 8: 0, 10: 0}
        (mesh, _kept, record), evaluated = self.run_settle(holes, 10, (2, 0.1))
        self.assertEqual((evaluated, len(mesh.tris), record["budget"]), ([2, 4], 4, 4))


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

    def test_settle_needs_a_budget_held_out_poses_a_step_and_a_share_below_one(self):
        for name, extra in (("no budget", [*self.HELD, "--settle", "5", "0.1"]),
                            ("no held-out", ["--budget", "9", "--settle", "5", "0.1"]),
                            ("step below 1", ["--budget", "9", *self.HELD, "--settle", "0.5", "0.1"]),
                            ("share 1", ["--budget", "9", *self.HELD, "--settle", "5", "1"]),
                            ("share negative", ["--budget", "9", *self.HELD, "--settle", "5", "-0.1"])):
            with self.subTest(name):
                self.assertIn("--settle needs", self.refused(*extra))

    def test_a_share_of_zero_passes_the_settle_check(self):
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(Exception):
            appearance_simplify.main([*self.BASE, "--budget", "9", *self.HELD, "--settle", "1", "0"])
        self.assertNotIn("--settle needs", error.getvalue())


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PrepareReferencesTests(unittest.TestCase):
    PROBES = SimpleNamespace(face=8, spacing=1.0)

    def prepare(self, probes, path=None):
        camera = SimpleNamespace(path=path, region=None, near_z=0.1)
        inputs = SimpleNamespace(scene=lambda: SimpleNamespace(camera=SimpleNamespace(component=camera)),
                                 poses=dict(train_every_ms=1, held_out_every_ms=2, coverage_every_ms=1), size=(4, 4),
                                 probes=probes, job=lambda: SimpleNamespace(settings=SimpleNamespace(alpha_keep=None)),
                                 sets=lambda: reference_set.fit_sets(path is not None, probes is not None, False))
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

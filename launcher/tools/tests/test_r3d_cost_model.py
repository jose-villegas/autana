"""Checks the frame-cost model on known geometry: which triangles count as
drawn and how much of them, which clusters are in view, the weights file and
its command line, and the fit's differentiable pieces on the CPU. Needs the
pinned r3d environment (tools/r3d/requirements.txt); the torch checks need
PyTorch, not CUDA."""

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import cost_model
    from r3d.appearance_simplify import projection
    from r3d.cost_model import FEATURES, clusters_in_view, load, predict, triangle_terms, variable_ms
    from r3d.lit_mesh import write_lit_mesh
except ImportError:
    np = None

try:
    import torch
except ImportError:
    torch = None

WEIGHTS = {"constant": 2.0, "submitted": 0.003, "drawn": 0.01, "rows": 0.002, "pixels": 0.0004, "clusters": 0.05}


def terms(screen, double=False, w=1.0, width=100, height=80):
    """triangle_terms of one triangle given in pixels (y down), at clip w."""
    screen = np.asarray(screen, dtype=float)
    x = (screen[:, 0] / width * 2 - 1) * w
    y = (1 - screen[:, 1] / height * 2) * w
    clip = np.stack([x, y, np.zeros(3), np.full(3, w)], axis=1)
    drawn, rows, pixels = triangle_terms(np, clip, np.array([[0, 1, 2]]), np.array([double]), width, height)
    return bool(drawn[0]), float(rows[0]), float(pixels[0])


def card_mesh(directory, count=2):
    side = np.linspace(-1.0, 1.0, count + 1)
    xs, ys = np.meshgrid(side, side)
    positions = np.stack([xs.ravel(), ys.ravel(), np.zeros(xs.size)], axis=1)
    index = np.arange(xs.size).reshape(count + 1, count + 1)
    tris = [[index[r, c], index[r, c + 1], index[r + 1, c + 1]] for r in range(count) for c in range(count)]
    tris += [[index[r, c], index[r + 1, c + 1], index[r + 1, c]] for r in range(count) for c in range(count)]
    write_lit_mesh(directory, "card", positions, np.full((len(positions), 3), 100.0), np.array(tris), np.zeros(len(tris), dtype=int),
                   ["test"], position_scale=64)
    return pathlib.Path(directory) / "card_mesh_generated.c", len(tris)


def poses_file(directory, *poses):
    path = pathlib.Path(directory) / "poses.txt"
    path.write_text("size 32 24\nlens 0.62 0.5\n" + "".join("pose " + " ".join(map(str, pose)) + "\n" for pose in poses))
    return path


@unittest.skipIf(np is None, "the r3d environment is not installed")
class TriangleTermTests(unittest.TestCase):
    def test_a_front_face_on_screen_counts_its_rows_and_area(self):
        self.assertEqual(terms([[10, 10], [10, 50], [50, 10]]), (True, 40.0, 800.0))

    def test_a_back_face_counts_only_when_double_sided(self):
        back = [[10, 10], [50, 10], [10, 50]]
        self.assertEqual(terms(back), (False, 0.0, 0.0))
        self.assertEqual(terms(back, double=True), (True, 40.0, 800.0))

    def test_a_face_behind_the_eye_or_off_screen_is_not_drawn(self):
        self.assertFalse(terms([[10, 10], [10, 50], [50, 10]], w=-1.0)[0])
        self.assertFalse(terms([[110, 10], [110, 50], [150, 10]])[0])

    def test_a_face_partly_off_screen_counts_only_its_clipped_box(self):
        # A 40 x 40 corner triangle with its bottom half below the 80-row screen.
        drawn, rows, pixels = terms([[10, 60], [10, 100], [50, 60]])
        self.assertTrue(drawn)
        self.assertEqual(rows, 20.0)
        self.assertEqual(pixels, 800.0 * 0.5)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ClusterTests(unittest.TestCase):
    def setUp(self):
        self.matrix = projection([0.0, 0.0, 0.0], [0.0, 0.0, -1.0], 32, 24, 0.62, 0.5)

    def test_a_box_straddling_a_plane_is_in_and_beyond_or_behind_it_out(self):
        right = 10.0 * 0.62 * 32 / 24
        boxes = np.array([[[right - 1, -1, -11], [right + 1, 1, -9]], [[right + 2, -1, -11], [right + 4, 1, -9]],
                          [[-1, -1, 4], [1, 1, 6]], [[-1, -1, -11], [1, 1, -9]]])
        self.assertEqual(clusters_in_view(boxes, self.matrix).tolist(), [True, False, False, True])

    def test_submitted_counts_the_triangles_of_the_clusters_in_view(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh, count = card_mesh(directory)
            rows = cost_model.mesh_rows(mesh, poses_file(directory, [0, 0, 3, 0, 0, -1], [0, 0, 3, 0, 0, 1]))
        self.assertEqual(rows[:, FEATURES.index("submitted")].tolist(), [count, 0])
        self.assertEqual(rows[:, FEATURES.index("drawn")].tolist(), [count, 0])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class WeightsFileTests(unittest.TestCase):
    def write(self, directory, rows, ms):
        path = pathlib.Path(directory) / "weights.txt"
        lines = ["# test", "feature " + " ".join(FEATURES), "weight " + " ".join("0" for _ in FEATURES)]
        lines += [f"frame mesh {index} {value:.6f} " + " ".join("%.6f" % v for v in row) for index, (row, value) in enumerate(zip(rows, ms))]
        path.write_text("\n".join(lines) + "\n")
        return path

    def test_fit_on_noisy_frames_predicts_held_out_ones(self):
        try:
            import scipy.optimize  # noqa: F401
        except ImportError:
            self.skipTest("the weight fit needs SciPy")
        rng = np.random.default_rng(5)
        rows = np.array([[1.0, 300.0 * n, 150.0 * n + rng.uniform(0, 40), 900.0 * n, 4000.0 * n * (1 + n % 3), 10.0 + n % 7]
                         for n in range(1, 25)])
        truth = [WEIGHTS[name] for name in FEATURES]
        ms = predict(truth, rows) + rng.normal(0.0, 0.05, len(rows))
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(directory, rows[:18], ms[:18])
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(cost_model.main(["fit", str(path)]), 0)
        fitted = [float(value) for value in out.getvalue().split()[1:]]
        self.assertLess(np.abs(predict(fitted, rows[18:]) - ms[18:]).max(), 0.5)

    def test_predict_reads_the_weights_by_name_and_matches_the_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh, _count = card_mesh(directory)
            poses = poses_file(directory, [0, 0, 3, 0, 0, -1])
            path = pathlib.Path(directory) / "weights.txt"
            shuffled = list(reversed(FEATURES))
            path.write_text("feature " + " ".join(FEATURES) + "\nweight " + " ".join(str(WEIGHTS[name]) for name in FEATURES) + "\n")
            self.assertEqual(load(path)[0], WEIGHTS)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                cost_model.main(["predict", str(mesh), "--poses", str(poses), "--weights", str(path)])
            expected = float((cost_model.mesh_rows(mesh, poses) @ np.array([WEIGHTS[name] for name in FEATURES])).mean())
            self.assertAlmostEqual(float(out.getvalue().split()[-2]), expected, places=3)
            path.write_text("feature " + " ".join(shuffled) + "\n")
            with self.assertRaises(ValueError):
                load(path)

    def test_the_variable_part_is_the_drawn_rows_and_pixels(self):
        drawn, rows, pixels = np.array([True, True]), np.array([3.0, 4.0]), np.array([10.0, 20.0])
        self.assertAlmostEqual(variable_ms(WEIGHTS, drawn, rows, pixels), 0.01 * 2 + 0.002 * 7 + 0.0004 * 30)

    def test_the_committed_weights_file_loads_and_refits_to_itself(self):
        try:
            import scipy.optimize  # noqa: F401
        except ImportError:
            self.skipTest("the weight fit needs SciPy")
        weights, rows, ms, _labels = load(pathlib.Path(cost_model.__file__).with_name("board_cost_weights.txt"))
        refit = dict(zip(FEATURES, cost_model.fit(rows, ms)))
        for name in FEATURES:
            self.assertAlmostEqual(refit[name], weights[name], delta=1e-3 * max(1.0, abs(weights[name])) + 1e-6)


@unittest.skipIf(np is None or torch is None, "needs PyTorch")
class DifferentiablePieceTests(unittest.TestCase):
    def test_the_cost_term_falls_as_a_triangle_shrinks_and_has_a_gradient(self):
        from r3d.appearance_simplify import predicted_ms

        matrix = torch.tensor(projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 32, 24, 0.62, 0.5), dtype=torch.float64)
        points = torch.tensor([[-1.0, -1.0, 0.0], [1.0, -1.0, 0.0], [0.0, 1.0, 0.0]], dtype=torch.float64, requires_grad=True)
        tris = torch.tensor([[0, 1, 2]])

        def cost(scale):
            clip = torch.cat([points * scale, torch.ones(3, 1, dtype=torch.float64)], dim=1) @ matrix.T
            return predicted_ms(torch, WEIGHTS, clip, tris, torch.tensor([False]), (32, 24), 1)

        big = cost(1.0)
        big.backward()
        self.assertGreater(float(points.grad.abs().sum()), 0.0)
        self.assertLess(float(cost(0.5)), float(big))

    def test_vertex_normals_follow_the_surface_and_share_a_welded_position(self):
        from r3d.appearance_simplify import vertex_normals

        points = torch.tensor([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 1.0], [1.0, 1.0, 1.0]])
        tris = torch.tensor([[0, 1, 2], [1, 3, 2], [4, 1, 2]])
        vertex_point = torch.tensor([0, 1, 2, 3, 0])
        normals = vertex_normals(points, tris, vertex_point)
        expected = torch.tensor([0.0, -1.0, 1.0]) / 2**0.5
        self.assertTrue(torch.allclose(normals, expected.expand(5, 3), atol=1e-6))


if __name__ == "__main__":
    unittest.main()

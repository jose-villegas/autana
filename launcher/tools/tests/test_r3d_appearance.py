"""Checks the appearance fit: its camera casts reference_render.py's rays,
a fitted mesh keeps the start's triangle budget and its seams closed, and on
a small synthetic scene the image error falls. The fit itself needs CUDA,
PyTorch and nvdiffrast and is skipped without them; the rest needs the
pinned r3d environment (tools/r3d/requirements.txt)."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.appearance_simplify import point_edges, projection, start_mesh, write_mesh
    from r3d.lit_mesh import finest_triangles, read_lit_mesh, write_lit_mesh
except ImportError:
    np = None

try:
    import nvdiffrast.torch  # noqa: F401
    import torch

    GPU = torch.cuda.is_available()
except ImportError:
    GPU = False


def grid(count, z=0.0):
    """A count x count quad grid in x and y over [-1, 1], facing +z, with a
    colour seam down its middle column of vertices: the left half's vertices
    are duplicated with a second colour."""
    side = np.linspace(-1.0, 1.0, count + 1)
    xs, ys = np.meshgrid(side, side)
    positions = np.stack([xs.ravel(), ys.ravel(), np.full(xs.size, z)], axis=1)
    index = np.arange(xs.size).reshape(count + 1, count + 1)
    tris = []
    for row in range(count):
        for col in range(count):
            a, b, c, d = index[row, col], index[row, col + 1], index[row + 1, col + 1], index[row + 1, col]
            tris += [[a, b, c], [a, c, d]]
    tris = np.array(tris)
    rgb = np.where(positions[:, :1] < 0, [[200.0, 60.0, 40.0]], [[40.0, 90.0, 200.0]])
    left = positions[tris].mean(axis=1)[:, 0] < 0
    copies = np.unique(tris[left])
    duplicate = {int(v): len(positions) + k for k, v in enumerate(copies)}
    positions = np.concatenate([positions, positions[copies]])
    rgb = np.concatenate([rgb, np.tile([[230.0, 200.0, 50.0]], (len(copies), 1))])
    tris[left] = np.vectorize(duplicate.get)(tris[left])
    return positions, rgb, tris


def write_start(directory, count=4):
    positions, rgb, tris = grid(count)
    write_lit_mesh(directory, "card", positions, rgb, tris, np.zeros(len(tris), dtype=int), position_scale=64)
    return pathlib.Path(directory) / "card.mesh"


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AppearanceMeshTests(unittest.TestCase):
    def test_projection_casts_the_reference_pinhole_rays(self):
        try:
            from r3d.reference_render import camera_rays
        except ImportError:
            self.skipTest("reference_render.py needs trimesh and embreex")
        width, height, lens, near = 184, 224, 0.62, 6.0
        eye, forward = np.array([10.0, 20.0, -5.0]), np.array([0.3, -0.2, 1.0])
        matrix = projection(eye, forward, width, height, lens, near)
        _origin, rays = camera_rays(width, height, lens, eye, forward, 1)
        for x, y in ((0, 0), (91, 13), (183, 223)):
            clip = matrix @ np.append(eye + 37.0 * rays[y * width + x], 1.0)
            ndc = clip[:3] / clip[3]
            self.assertAlmostEqual(ndc[0], 2 * (x + 0.5) / width - 1, places=9)
            self.assertAlmostEqual(ndc[1], 1 - 2 * (y + 0.5) / height, places=9)
            self.assertTrue(-1.0 < ndc[2] < 1.0)

    def test_seam_vertices_share_one_position(self):
        with tempfile.TemporaryDirectory() as directory:
            points, rgb, tris, _double, _scale, vertex_point = start_mesh(write_start(directory))
            self.assertLess(len(points), len(rgb))
            self.assertEqual(len(np.unique(points, axis=0)), len(points))
            self.assertTrue(np.all(point_edges(tris, vertex_point) < len(points)))

    def test_the_written_mesh_holds_the_fitted_positions_and_colours(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
            points, rgb = mesh[0] + [0.25, -0.5, 0.125], 1.0 - mesh[1]
            write_mesh(pathlib.Path(directory) / "out", "card", points, rgb, mesh)
            back = read_lit_mesh(pathlib.Path(directory) / "out" / "card.mesh")
        q, colours, _tris, _double, _face = finest_triangles(back)
        written = {(tuple(p), tuple(c)) for p, c in zip(q, colours)}
        wanted = {(tuple(np.rint(p * back.position_scale).astype(int)), tuple(np.rint(c * 255).astype(int)))
                  for p, c in zip(points[mesh[5]], rgb)}
        self.assertEqual(written, wanted)

    def test_fitted_mesh_keeps_the_budget_and_closes_seams(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
            points, rgb = mesh[0] + np.random.default_rng(3).normal(0.0, 0.05, mesh[0].shape), mesh[1][::-1].copy()
            count = write_mesh(pathlib.Path(directory) / "out", "card", points, rgb, mesh)
            self.assertLessEqual(count, len(mesh[2]))
            back = read_lit_mesh(pathlib.Path(directory) / "out" / "card.mesh")
            q, _rgb, tris, _double, _face = finest_triangles(back)
            self.assertLessEqual(len(tris), len(mesh[2]))
            corners = q[tris]
            edges = {}
            for t in corners:
                for i in range(3):
                    key = tuple(sorted((tuple(t[i]), tuple(t[(i + 1) % 3]))))
                    edges[key] = edges.get(key, 0) + 1
            border = sum(1 for n in edges.values() if n == 1)
            self.assertEqual(border, 4 * 4, "a moved seam vertex opened a crack")


@unittest.skipIf(np is None or not GPU, "needs CUDA, PyTorch and nvdiffrast")
class AppearanceFitTests(unittest.TestCase):
    def test_image_error_falls(self):
        from r3d.appearance_simplify import Renderer, optimise

        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
            size = (48, 40)
            matrix = projection([0.2, -0.1, 3.0], [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5)
            truth = Renderer(mesh[2], mesh[3], mesh[5], size, "cuda")
            moved = torch.as_tensor(mesh[0] * [1.3, 0.8, 1.0], dtype=torch.float32, device="cuda")
            target = truth(moved, torch.as_tensor(mesh[1][::-1].copy(), dtype=torch.float32, device="cuda"), torch.as_tensor(
                matrix, dtype=torch.float32, device="cuda")).detach().cpu().numpy()
            _points, _rgb, history = optimise(mesh, [(matrix, target)], size, steps=150, batch=1, lr_position=0.01,
                                              lr_colour=0.05, laplacian=0.0, report=0)
            self.assertLess(np.mean(history[-10:]), 0.5 * np.mean(history[:3]))


if __name__ == "__main__":
    unittest.main()

"""Checks the appearance fit and what feeds it: its camera casts
reference_render.py's rays, a fitted mesh keeps the start's triangle budget
and its seams closed, pruning keeps what shows most, the cost model grows
with triangles and area, path visibility keeps what a pose sees, and on a
small synthetic scene the image error falls. What draws on the GPU needs
CUDA, PyTorch and nvdiffrast and is skipped without them; the rest needs the
pinned r3d environment (tools/r3d/requirements.txt)."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.appearance_simplify import FitMesh, point_edges, projection, prune, refine, start_mesh, write_mesh
    from r3d.cost_model import fit, predict, triangle_terms
    from r3d.lit_mesh import finest_triangles, read_lit_mesh, write_lit_mesh
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402

try:
    from tests import soup

    from r3d.light import visible_from_path
except ImportError:
    soup = None

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


def write_flat_start(directory, count=4):
    """The grid card baked flat: left half one colour, right half another."""
    positions, _rgb, tris = grid(count)
    left = positions[tris].mean(axis=1)[:, 0] < 0
    face = np.where(left[:, None], [[200.0, 60.0, 40.0]], [[40.0, 90.0, 200.0]])
    write_lit_mesh(directory, "flat", positions, None, tris, np.zeros(len(tris), dtype=int), position_scale=64, face_rgb=face)
    return pathlib.Path(directory) / "flat.mesh"


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AppearanceMeshTests(unittest.TestCase):
    def test_projection_casts_the_reference_pinhole_rays(self):
        try:
            from r3d.reference_render import camera_rays
        except ImportError:
            self.skipTest("reference_render.py needs the r3d environment")
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
            mesh = start_mesh(write_start(directory))
            self.assertLess(len(mesh.points), len(mesh.rgb))
            self.assertEqual(len(np.unique(mesh.points, axis=0)), len(mesh.points))
            self.assertTrue(np.all(point_edges(mesh.tris, mesh.vertex_point) < len(mesh.points)))

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

    def test_a_flat_start_carries_a_colour_per_triangle_and_writes_back_flat(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_flat_start(directory))
            self.assertTrue(mesh.flat)
            self.assertEqual(len(mesh.rgb), len(mesh.tris))
            self.assertEqual(len(mesh.points), len(mesh.vertex_point), "a flat start welds by position alone")
            points, rgb = mesh.points + [0.25, 0.0, 0.0], 1.0 - mesh.rgb
            write_mesh(pathlib.Path(directory) / "out", "flat", points, rgb, mesh)
            back = read_lit_mesh(pathlib.Path(directory) / "out" / "flat.mesh")
        q, colours, tris, _double, face = finest_triangles(back)
        self.assertIsNone(colours)
        written = {tuple(map(tuple, q[t])): tuple(c) for t, c in zip(tris, face)}
        expected = {tuple(map(tuple, np.rint(points[mesh.vertex_point][t] * 64).astype(int))): c for t, c in zip(mesh.tris, rgb)}
        self.assertEqual(written.keys(), expected.keys())
        for key, colour in expected.items():
            # Face colours are stored as RGB565, so a channel comes back within its step.
            self.assertTrue(np.all(np.abs(np.array(written[key]) - colour * 255) <= 8), key)

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


@unittest.skipIf(np is None, "the r3d environment is not installed")
class PruneAndCostTests(unittest.TestCase):
    def test_pruning_keeps_the_budget_and_what_shows_most(self):
        mesh = FitMesh(np.zeros((9, 3)), np.zeros((9, 3)), np.arange(18).reshape(6, 3) % 9, np.zeros(6), 8, np.arange(9))
        shown = np.array([0, 50, 3, 0, 900, 7])
        pruned = prune(mesh, shown, 3)
        self.assertEqual(len(pruned[2]), 3)
        kept = {tuple(t) for t in pruned[2]}
        for index in (4, 1, 5):
            self.assertIn(tuple(mesh[2][index]), kept)
        self.assertEqual(len(prune(mesh, shown, 6)[2]), 4, "a triangle no view shows was kept")

    def test_a_flat_mesh_keeps_each_colour_with_its_triangle(self):
        colours = np.arange(18, dtype=float).reshape(6, 3) / 17.0
        mesh = FitMesh(np.zeros((9, 3)), colours, np.arange(18).reshape(6, 3) % 9, np.zeros(6), 8, np.arange(9), True)
        pruned = prune(mesh, np.array([0, 50, 3, 0, 900, 7]), 3)
        self.assertEqual(pruned.rgb.tolist(), colours[[1, 4, 5]].tolist())

    def terms(self, positions, tris):
        matrix = projection([0.0, 0.0, 5.0], [0.0, 0.0, -1.0], 64, 48, 0.62, 0.5)
        clip = np.concatenate([positions, np.ones((len(positions), 1))], axis=1) @ matrix.T
        drawn, rows, pixels = triangle_terms(np, clip, tris, np.zeros(len(tris), dtype=bool), 64, 48)
        return np.array([1.0, 0.0, drawn.sum(), rows.sum(), pixels.sum(), 0.0])

    def test_the_cost_model_grows_with_triangles_and_area(self):
        weights = np.array([1.0, 0.0, 0.01, 0.002, 0.0005, 0.0])
        small, more, larger = (self.terms(*grid_mesh(2, 0.5)), self.terms(*grid_mesh(4, 0.5)), self.terms(*grid_mesh(2, 1.0)))
        self.assertGreater(more[2], small[2])
        self.assertGreater(larger[4], small[4])
        self.assertGreater(larger[3], small[3])
        self.assertGreater(predict(weights, more), predict(weights, small))
        self.assertGreater(predict(weights, larger), predict(weights, small))

    def test_the_fit_recovers_known_non_negative_weights(self):
        try:
            import scipy.optimize  # noqa: F401
        except ImportError:
            self.skipTest("the weight fit needs SciPy")
        rows = np.array([[1.0, 2.0 * n + n % 4, n, 3.0 * n + 5, 40.0 * n * (1 + n % 3), 2.0 + n % 5] for n in range(1, 30)])
        weights = np.array([2.0, 0.003, 0.01, 0.0, 0.0004, 0.05])
        self.assertTrue(np.allclose(fit(rows, predict(weights, rows)), weights, atol=1e-6))


def grid_mesh(count, half):
    """A count x count quad grid of half-width `half` facing +z."""
    side = np.linspace(-half, half, count + 1)
    xs, ys = np.meshgrid(side, side)
    positions = np.stack([xs.ravel(), ys.ravel(), np.zeros(xs.size)], axis=1)
    index = np.arange(xs.size).reshape(count + 1, count + 1)
    tris = [[index[r, c], index[r, c + 1], index[r + 1, c + 1]] for r in range(count) for c in range(count)]
    tris += [[index[r, c], index[r + 1, c + 1], index[r + 1, c]] for r in range(count) for c in range(count)]
    return positions, np.array(tris)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class RefineTests(unittest.TestCase):
    def test_refining_splits_the_worst_triangles_and_leaves_no_crack(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
        error = np.zeros(len(mesh[2]))
        worst = int(np.argmax(mesh[0][mesh[5]][mesh[2]].mean(axis=1)[:, 0] + mesh[0][mesh[5]][mesh[2]].mean(axis=1)[:, 1]))
        error[worst] = 10.0
        refined = refine(mesh, error, len(mesh[2]) + 2)
        self.assertGreaterEqual(len(refined[2]), len(mesh[2]) + 2)
        corners = refined[0][refined[5]][refined[2]]
        a, b, c = corners[:, 0], corners[:, 1], corners[:, 2]
        self.assertTrue(np.all(np.cross(b - a, c - a)[:, 2] > 0), "a split turned a triangle over")
        edges = {}
        for tri in np.round(corners * 64).astype(int):
            for k in range(3):
                key = tuple(sorted((tuple(tri[k]), tuple(tri[(k + 1) % 3]))))
                edges[key] = edges.get(key, 0) + 1
        border = [key for key, count in edges.items() if count == 1]
        on_edge = [all(abs(abs(v) - 64) == 0 for v in (key[0][0], key[1][0])) or
                   all(abs(abs(v) - 64) == 0 for v in (key[0][1], key[1][1])) for key in border]
        self.assertTrue(all(on_edge), "a split left an open edge inside the card")
        before = {tuple(v) for v in np.round(mesh[0] * 64).astype(int)}
        added = [v for v in np.round(refined[0] * 64).astype(int) if tuple(v) not in before]
        self.assertTrue(added and all(v[0] > 0 or v[1] > 0 for v in added), "the split is not on the worst triangle")

    def test_refining_a_flat_mesh_gives_each_half_its_triangles_colour_and_double_flag(self):
        positions, tris = grid_mesh(2, 1.0)
        colours = np.stack([(k + 1) / 20.0 * np.array([1.0, 0.5, 0.25]) + [0.0, 0.01 * k, 0.0] for k in range(len(tris))])
        double = np.arange(len(tris)) % 2 == 0
        mesh = FitMesh(positions, colours, tris, double, 64, np.arange(len(positions)), True)
        error = np.zeros(len(tris))
        error[[0, 5]] = 10.0
        refined = refine(mesh, error, len(tris) + 4)
        self.assertTrue(refined.flat)
        self.assertGreaterEqual(len(refined.tris), len(tris) + 4)
        self.assertEqual(refined.rgb.shape, (len(refined.tris), 3), "one colour per triangle, not per vertex")
        self.assertEqual(len(refined.double), len(refined.tris))
        corners = refined.points[refined.vertex_point][refined.tris]
        old = positions[tris]
        for k, tri in enumerate(corners):
            centre = tri.mean(axis=0)
            parents = []
            for j, parent in enumerate(old):
                a, b, c = parent
                cross = lambda u, v, w: (v[0] - u[0]) * (w[1] - u[1]) - (v[1] - u[1]) * (w[0] - u[0])
                signs = (cross(a, b, centre), cross(b, c, centre), cross(c, a, centre))
                if all(value > 1e-9 for value in signs) or all(value < -1e-9 for value in signs):
                    parents.append(j)
            self.assertEqual(len(parents), 1, "a half's centre lies inside exactly one original triangle")
            self.assertTrue(np.allclose(refined.rgb[k], colours[parents[0]]), f"triangle {k} lost its parent's colour")
            self.assertEqual(bool(refined.double[k]), bool(double[parents[0]]))


@needs_mitsuba
@unittest.skipIf(np is None or soup is None, "the r3d environment is not installed")
class PathVisibilityTests(unittest.TestCase):
    def test_a_pose_keeps_what_it_sees_and_passes_through_culled_faces(self):
        # z = 0 card facing the eye; z = -1 card hidden behind it; z = 6 card behind the eye;
        # z = 2 card facing away, which the rasterizer culls, so the z = 0 card shows through it.
        quads = [(0.0, False), (-1.0, False), (6.0, False), (2.0, True)]
        positions, tris = [], []
        for z, away in quads:
            base = len(positions)
            positions += [[-1, -1, z], [1, -1, z], [1, 1, z], [-1, 1, z]]
            faces = [[0, 1, 2], [0, 2, 3]]
            tris += [[base + (f[0]), base + f[2], base + f[1]] if away else [base + i for i in f] for f in faces]
        positions, tris = np.array(positions, dtype=float), np.array(tris)
        intersector = soup.rays(soup.Soup(positions, tris))
        poses = [np.array([0.0, 0.0, 5.0, 0.0, 0.0, -1.0])]
        seen = visible_from_path(positions, tris, np.zeros(len(tris), dtype=bool), intersector, poses, 16, 12, 0.62, 0.5, 2, 0)
        self.assertEqual(seen.tolist(), [True, True, False, False, False, False, False, False])
        double = np.zeros(len(tris), dtype=bool)
        double[6:] = True
        seen = visible_from_path(positions, tris, double, intersector, poses, 16, 12, 0.62, 0.5, 2, 0)
        self.assertEqual(seen.tolist(), [False, False, False, False, False, False, True, True])


    def cards(self, quads):
        """Unit cards at the given (z, facing away) pairs, two triangles each."""
        positions, tris = [], []
        for z, away in quads:
            base = len(positions)
            positions += [[-1, -1, z], [1, -1, z], [1, 1, z], [-1, 1, z]]
            for f in ([0, 1, 2], [0, 2, 3]):
                tris.append([base + f[0], base + f[2], base + f[1]] if away else [base + i for i in f])
        positions, tris = np.array(positions, dtype=float), np.array(tris)
        return positions, tris, soup.rays(soup.Soup(positions, tris))

    def test_a_wall_seen_through_its_back_face_keeps_its_front_twin(self):
        positions, tris, intersector = self.cards([(0.0, True), (0.0, False), (-1.0, False)])
        poses = [np.array([0.0, 0.0, 5.0, 0.0, 0.0, -1.0])]
        seen = visible_from_path(positions, tris, np.zeros(len(tris), dtype=bool), intersector, poses, 16, 12, 0.62, 0.5, 2, 0)
        self.assertEqual(seen.tolist(), [False, False, True, True, False, False])

    def test_what_lies_before_the_near_plane_hides_nothing(self):
        positions, tris, intersector = self.cards([(4.8, False), (0.0, False)])
        poses = [np.array([0.0, 0.0, 5.0, 0.0, 0.0, -1.0])]
        seen = visible_from_path(positions, tris, np.zeros(len(tris), dtype=bool), intersector, poses, 16, 12, 0.62, 0.5, 2, 0)
        self.assertEqual(seen.tolist(), [False, False, True, True])


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

    def test_a_flat_fit_moves_the_edge_between_two_face_colours(self):
        from r3d.appearance_simplify import Renderer, optimise

        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_flat_start(directory))
        size = (96, 80)
        matrix = projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 48, 40, 0.62, 0.5)
        truth = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, "cuda", flat=True)
        # The target's colour boundary sits a third of a column to the right of the start's.
        shifted = mesh.points + np.where(np.abs(mesh.points[:, :1]) < 0.4, [[0.15, 0.0, 0.0]], 0.0)
        target = truth(torch.as_tensor(shifted, dtype=torch.float32, device="cuda"),
                       torch.as_tensor(mesh.rgb, dtype=torch.float32, device="cuda"),
                       torch.as_tensor(matrix, dtype=torch.float32, device="cuda")).detach().cpu().numpy()
        points, rgb, history = optimise(mesh, [(matrix, target)], size, steps=150, batch=1, lr_position=0.005,
                                        lr_colour=0.0, laplacian=0.0, report=0)
        self.assertEqual(rgb.shape, mesh.rgb.shape)
        self.assertLess(np.mean(history[-10:]), 0.5 * np.mean(history[:3]), "the colour edge did not move")
        self.assertGreater(points[np.abs(mesh.points[:, 0]) < 1e-9, 0].mean(), 0.05)

    def test_a_flat_renderer_draws_each_triangle_in_its_own_colour(self):
        from r3d.appearance_simplify import Renderer

        positions, tris = grid_mesh(2, 1.0)
        colours = np.stack([[(k + 1) / (len(tris) + 1), 1.0 - k / len(tris), (k % 3) / 2.0] for k in range(len(tris))])
        size = (96, 80)
        matrix = torch.as_tensor(projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 48, 40, 0.62, 0.5), dtype=torch.float32, device="cuda")
        render = Renderer(tris, np.zeros(len(tris), dtype=bool), np.arange(len(positions)), size, "cuda", flat=True)
        points = torch.as_tensor(positions, dtype=torch.float32, device="cuda")
        with torch.no_grad():
            image = render(points, torch.as_tensor(colours, dtype=torch.float32, device="cuda"), matrix).cpu().numpy()
            ids = render.visible_ids(points, matrix).flip(0).cpu().numpy()
        shown = sorted({int(k) for k in ids.ravel() if k >= 0})
        self.assertGreaterEqual(len(shown), 2)
        for k in shown:
            # The triangle's own pixels, away from its edges: a pixel whose four neighbours show the same triangle.
            inner = (ids == k)
            inner[1:-1, 1:-1] &= (ids[:-2, 1:-1] == k) & (ids[2:, 1:-1] == k) & (ids[1:-1, :-2] == k) & (ids[1:-1, 2:] == k)
            inner[0], inner[-1], inner[:, 0], inner[:, -1] = False, False, False, False
            rows, cols = np.nonzero(inner)
            self.assertTrue(len(rows), f"triangle {k} has no interior pixel")
            self.assertTrue(np.allclose(image[rows[0], cols[0]], colours[k], atol=1e-4), f"triangle {k} is not in its own colour")

    def test_a_flat_fit_moves_each_face_colour_toward_its_own_target(self):
        from r3d.appearance_simplify import Renderer, optimise

        positions, tris = grid_mesh(2, 1.0)
        count = len(tris)
        vertex_point = np.arange(len(positions))
        double = np.zeros(count, dtype=bool)
        size = (96, 80)
        matrix = projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 48, 40, 0.62, 0.5)
        wanted = np.stack([[(k % 4) / 3.0, ((k // 4) % 2), 1.0 - k / count] for k in range(count)])
        truth = Renderer(tris, double, vertex_point, size, "cuda", flat=True)
        target = truth(torch.as_tensor(positions, dtype=torch.float32, device="cuda"),
                       torch.as_tensor(wanted, dtype=torch.float32, device="cuda"),
                       torch.as_tensor(matrix, dtype=torch.float32, device="cuda")).detach().cpu().numpy()
        start = np.full((count, 3), 0.5)
        mesh = FitMesh(positions, start, tris, double, 64, vertex_point, True)
        _points, rgb, _history = optimise(mesh, [(matrix, target)], size, steps=400, batch=1, lr_position=0.0, lr_colour=0.05,
                                          laplacian=0.0, report=0)
        with torch.no_grad():
            ids = truth.visible_ids(torch.as_tensor(positions, dtype=torch.float32, device="cuda"),
                                    torch.as_tensor(matrix, dtype=torch.float32, device="cuda")).cpu().numpy()
        coverage_pixels = np.bincount(ids[ids >= 0], minlength=count)
        seen = np.nonzero(coverage_pixels > 50)[0]
        self.assertGreaterEqual(len(seen), 4)
        for k in seen:
            own = np.abs(rgb[k] - wanted[k]).max()
            others = [np.abs(rgb[k] - wanted[j]).max() for j in seen if j != k and np.abs(wanted[j] - wanted[k]).max() > 0.2]
            self.assertLess(own, 0.15, f"face {k} did not reach its target colour")
            self.assertTrue(all(own < other for other in others), f"face {k} is closer to a neighbour's target")
        self.assertGreater(np.abs(rgb[seen] - 0.5).max(), 0.2, "the colours never left the start")

    def test_views_of_both_orientations_train_one_mesh_each_at_its_own_size(self):
        from r3d.appearance_simplify import Renderer, optimise

        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
        moved = torch.as_tensor(mesh[0] * [1.3, 0.8, 1.0], dtype=torch.float32, device="cuda")
        colours = torch.as_tensor(mesh[1][::-1].copy(), dtype=torch.float32, device="cuda")
        views = []
        for width, height in ((24, 20), (20, 24)):
            matrix = projection([0.2, -0.1, 3.0], [0.0, 0.0, -1.0], width, height, 0.62, 0.5)
            truth = Renderer(mesh[2], mesh[3], mesh[5], (2 * width, 2 * height), "cuda")
            target = truth(moved, colours, torch.as_tensor(matrix, dtype=torch.float32, device="cuda")).detach().cpu().numpy()
            self.assertEqual(target.shape[:2], (2 * height, 2 * width))
            views.append((matrix, target))
        _points, _rgb, history = optimise(mesh, views, None, steps=150, batch=2, lr_position=0.01, lr_colour=0.05,
                                          laplacian=0.0, report=0)
        self.assertLess(np.mean(history[-10:]), 0.5 * np.mean(history[:3]))

    def test_the_normal_term_turns_the_surface_toward_the_reference_normals(self):
        from r3d.appearance_simplify import Renderer, normal_error, optimise

        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
        size, eye = (48, 40), np.array([0.0, 0.0, 3.0])
        matrix = projection(eye, [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5)
        reference = np.zeros((40, 48, 3), dtype=np.float32)
        reference[...] = np.array([0.0, 0.6, 0.8], dtype=np.float32)
        render = Renderer(mesh[2], mesh[3], mesh[5], size, "cuda")
        with torch.no_grad():
            colour = render(torch.as_tensor(mesh[0], dtype=torch.float32, device="cuda"),
                            torch.as_tensor(mesh[1], dtype=torch.float32, device="cuda"),
                            torch.as_tensor(matrix, dtype=torch.float32, device="cuda")).cpu().numpy()
        view = (matrix, colour, reference, eye)
        before = normal_error(mesh, [view], size)
        points, _rgb, _history = optimise(mesh, [view], size, steps=200, batch=1, lr_position=0.02, lr_colour=0.0,
                                          laplacian=0.0, report=0, normal_weight=1.0)
        after = normal_error(mesh._replace(points=points), [view], size)
        self.assertLess(after, 0.5 * before)

    def test_coverage_never_prunes_a_visible_triangle(self):
        from r3d.appearance_simplify import coverage

        with tempfile.TemporaryDirectory() as directory:
            front, front_tris = grid_mesh(2, 1.0)
            back = front + [0.0, 0.0, -1.0]
            positions = np.concatenate([front, back])
            tris = np.concatenate([front_tris, front_tris + len(front)])
            write_lit_mesh(directory, "pair", positions, np.full((len(positions), 3), 128.0), tris, np.zeros(len(tris), dtype=int),
                           position_scale=64)
            mesh = start_mesh(pathlib.Path(directory) / "pair.mesh")
        matrix = projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5)
        shown = coverage(mesh, [(matrix, None)], (48, 40))
        pruned = prune(mesh, shown, len(front_tris))
        z = mesh[0][mesh[5]][pruned[2]][..., 2]
        self.assertEqual(len(pruned[2]), len(front_tris))
        self.assertTrue(np.all(np.abs(z) < 1e-6), "a hidden triangle outlived a visible one")


if __name__ == "__main__":
    unittest.main()

"""Checks the appearance fit and what feeds it: its camera casts
reference_render.py's rays, a fitted mesh keeps the start's triangle budget
and its seams closed, pruning keeps what shows most, the cost model grows
with triangles and area, path visibility keeps what a pose sees, and on a
small synthetic scene the image error falls. What draws on the GPU needs
CUDA, PyTorch and nvdiffrast and is skipped without them; the rest needs the
pinned r3d environment (tools/r3d/requirements.txt)."""

import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

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
    import torch
except ImportError:
    torch = None

try:
    import nvdiffrast.torch  # noqa: F401

    GPU = torch is not None and torch.cuda.is_available()
except ImportError:
    GPU = False


def cuda(array):
    """`array` as a float32 CUDA tensor."""
    return torch.as_tensor(array, dtype=torch.float32, device="cuda")


def draw(renderer, points, colours, matrix):
    """What `renderer` draws of `points` in `colours` through `matrix`, as a NumPy image."""
    with torch.no_grad():
        return renderer(cuda(points), cuda(colours), cuda(matrix)).cpu().numpy()


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
class SeamHoldTests(unittest.TestCase):
    def test_optimise_defaults_to_the_shared_seam_margin(self):
        from inspect import signature
        from r3d import appearance_simplify as appearance

        self.assertEqual(signature(appearance.optimise).parameters["seam_margin"].default,
                         appearance.SEAM_MARGIN)

    def mesh(self):
        points = np.array([[0.5, 0.5, 0.1], [4, 0, 1], [0, 4, 1],
                           [0, 0, 0], [2, 0, 0], [0, 2, 0]], dtype=float)
        return FitMesh(points, np.ones_like(points), np.array([[0, 1, 2], [3, 4, 5]]),
                       np.zeros(2), 1, np.arange(len(points)))

    def test_border_point_held_at_start_barycentrics(self):
        from r3d.appearance_simplify import seam_holds

        mesh = self.mesh()
        tolerance = np.zeros(len(mesh.points))
        tolerance[0] = 0.1
        holds = seam_holds(mesh, tolerance)
        np.testing.assert_array_equal(holds.points, [0])
        np.testing.assert_array_equal(holds.corners, [[3, 4, 5]])
        np.testing.assert_allclose(holds.barycentric, [[0.5, 0.25, 0.25]])

    def test_triangle_radius_filters_distant_candidates_without_changing_holds(self):
        from r3d import appearance_simplify as appearance

        mesh = self.mesh()
        tolerance = np.zeros(len(mesh.points))
        tolerance[0] = 0.1
        expected = appearance.seam_holds(mesh, tolerance)
        distant = mesh.points[3:] + [10, 0, 0]
        large = np.array([[-200, -200, 100], [200, -200, 100], [0, 200, 100]])
        points = np.vstack([mesh.points, distant, large])
        mesh = mesh._replace(points=points, tris=np.vstack([mesh.tris, [6, 7, 8], [9, 10, 11]]),
                             vertex_point=np.arange(len(points)))
        tolerance = np.r_[tolerance, np.zeros(len(distant) + len(large))]
        with mock.patch.object(appearance, "closest_point_on_triangles",
                               wraps=appearance.closest_point_on_triangles) as closest:
            actual = appearance.seam_holds(mesh, tolerance)
        np.testing.assert_array_equal(closest.call_args_list[0].args[1], mesh.points[[3, 9]])
        for first, second in zip(expected, actual):
            np.testing.assert_array_equal(first, second)

    def test_outside_tolerance_and_same_part_are_not_held(self):
        from r3d.appearance_simplify import seam_holds

        mesh = self.mesh()
        connected = mesh._replace(tris=np.vstack([mesh.tris, [1, 2, 3]]))
        for candidate, tolerance in ((mesh, 0.05), (connected, 0.25)):
            with self.subTest(connected=candidate is connected):
                holds = seam_holds(candidate, np.full(len(mesh.points), tolerance))
                self.assertEqual(holds.points.shape, (0,))
                self.assertEqual(holds.corners.shape, (0, 3))
                self.assertEqual(holds.barycentric.shape, (0, 3))

    def test_greedy_holds_have_no_chains(self):
        from r3d.appearance_simplify import seam_holds

        mesh = self.mesh()
        plane = np.array([[0, 0, 0], [2, 0, 0], [0, 2, 0]], dtype=float)
        mesh = mesh._replace(points=np.vstack([plane + [0, 0, 0.1], plane]))
        holds = seam_holds(mesh, np.full(len(mesh.points), 0.25))
        np.testing.assert_array_equal(holds.points, [0, 1, 2])
        self.assertFalse(set(holds.points) & set(holds.corners.ravel()))
        repeated = seam_holds(mesh, np.full(len(mesh.points), 0.25))
        for first, second in zip(holds, repeated):
            np.testing.assert_array_equal(first, second)
        tolerance = np.full(len(mesh.points), 0.25)
        tolerance[3:] *= 2
        reverse = seam_holds(mesh, tolerance)
        np.testing.assert_array_equal(reverse.points, [3, 4, 5])

    def test_target_with_a_held_corner_is_skipped(self):
        from r3d.appearance_simplify import seam_holds

        mesh = self.mesh()
        plane = mesh.points[3:]
        points = np.vstack([plane + [0, 0, 0.1], plane, plane + [0, 0, 0.25]])
        mesh = mesh._replace(points=points, tris=np.vstack([mesh.tris, [6, 7, 8]]),
                             vertex_point=np.arange(len(points)))
        tolerance = np.repeat([0.5, 0, 0.25], 3)
        holds = seam_holds(mesh, tolerance)
        np.testing.assert_array_equal(holds.points, [0, 1, 2])
        self.assertFalse(set(holds.points) & set(holds.corners.ravel()))

    def test_welded_interior_point_is_not_a_border(self):
        from r3d.appearance_simplify import seam_holds

        mesh = self.mesh()
        points = np.vstack([mesh.points[0], [[-1, -1, 0.1], [2, -1, 0.1],
                                           [2, 2, 0.1], [-1, 2, 0.1]], mesh.points[3:]])
        tris = np.array([[0, 1, 2], [8, 2, 3], [0, 3, 4], [8, 4, 1], [5, 6, 7]])
        mesh = mesh._replace(points=points, tris=tris, vertex_point=np.r_[np.arange(len(points)), 0])
        tolerance = np.zeros(len(points))
        tolerance[0] = 0.25
        holds = seam_holds(mesh, tolerance)
        self.assertEqual(len(holds.points), 0)

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_hold_seams_is_exact_out_of_place_and_differentiable(self):
        from r3d.appearance_simplify import hold_seams, seam_holds

        mesh = self.mesh()
        holds = seam_holds(mesh, np.array([0.25, 0, 0, 0, 0, 0]))
        points = torch.tensor(mesh.points, requires_grad=True)
        result = hold_seams(points, holds)
        np.testing.assert_array_equal(result.detach()[0], [0.5, 0.5, 0])
        np.testing.assert_array_equal(points.detach(), mesh.points)
        result[0].sum().backward()
        np.testing.assert_array_equal(points.grad[:3], np.zeros((3, 3)))
        np.testing.assert_array_equal(points.grad[3:], np.repeat(holds.barycentric.T, 3, axis=1))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class StretchTests(unittest.TestCase):
    def test_small_part_stretch_is_reported_separately(self):
        from r3d.appearance_simplify import STRETCH_RATIO, stretch_report

        points = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.],
                           [10., 0., 0.], [20., 0., 0.], [10., 10., 0.]])
        tris = np.array([[0, 1, 2], [3, 4, 5]])
        mesh = FitMesh(points, np.ones_like(points), tris, np.zeros(len(tris)), 1, np.arange(len(points)))
        for ratio, expected in ((STRETCH_RATIO - 0.5, 0.0), (STRETCH_RATIO, 0.0), (STRETCH_RATIO + 1, 100.0)):
            with self.subTest(ratio=ratio):
                fitted = points * ratio
                fitted[3:] = points[3:]
                report = stretch_report(mesh, mesh._replace(points=fitted), np.arange(len(tris)))
                self.assertEqual(len(report["parts"]), 2)
                self.assertEqual(report["parts"][0]["stretch"], expected)
                self.assertEqual(report["parts"][1]["stretch"], 0)
                self.assertEqual(report["small"]["triangles"], 1)
                self.assertEqual(report["small"]["stretch"], expected)
                self.assertEqual(report["rest"]["stretch"], 0)
                self.assertEqual(report["worst"]["stretch"][0]["part"], report["parts"][0]["part"])
                self.assertAlmostEqual(report["parts"][0]["mean_start_edge"], (2 + np.sqrt(2)) / 3)
        report = stretch_report(mesh, mesh._replace(points=fitted, tris=tris[1:]), np.array([1]))
        self.assertEqual(len(report["parts"]), 1)
        self.assertEqual(report["parts"][0]["triangles"], 1)
        self.assertEqual(report["parts"][0]["part"], 1)
        self.assertEqual(report["small"]["triangles"], 0)
        self.assertEqual(report["rest"], dict(triangles=1, stretch=0.0, area_growth=0.0, moved=0.0))

    def test_part_size_uses_unique_edges_before_pruning(self):
        from r3d.appearance_simplify import stretch_report

        points = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [4., 0., 0.],
                           [10., 0., 0.], [13., 0., 0.], [10., 3., 0.]])
        tris = np.array([[0, 1, 2], [1, 3, 2], [4, 5, 6]])
        mesh = FitMesh(points, np.ones_like(points), tris, np.zeros(len(tris)), 1, np.arange(len(points)))
        expected = (1 + 1 + np.sqrt(2) + 3 + np.sqrt(17)) / 5
        for kept in (np.arange(len(tris)), np.array([1, 2])):
            with self.subTest(kept=kept):
                report = stretch_report(mesh, mesh._replace(tris=tris[kept]), kept)
                self.assertAlmostEqual(report["parts"][0]["mean_start_edge"], expected)
                self.assertEqual(report["small"], dict(triangles=len(kept) - 1, stretch=0.0, area_growth=0.0, moved=0.0))

    def test_collapsed_start_triangle_counts_as_stretched(self):
        from r3d.appearance_simplify import stretch_report, triangle_stretch

        points = np.zeros((2, 3))
        tris = np.array([[0, 0, 1]])
        mesh = FitMesh(points, np.ones_like(points), tris, np.zeros(len(tris)), 1, np.arange(len(points)))
        fitted = points.copy()
        fitted[1, 0] = 1
        fitted_mesh = mesh._replace(points=fitted)
        self.assertTrue(np.isinf(triangle_stretch(points, fitted, mesh)[0]))
        report = stretch_report(mesh, fitted_mesh, np.array([0]))
        self.assertEqual(report["parts"][0]["stretch"], 100.0)
        self.assertEqual(report["rest"], dict(triangles=1, stretch=100.0, area_growth=0.0, moved=100.0))

    def test_area_growth_detects_opening_without_long_edge_stretch(self):
        from r3d.appearance_simplify import AREA_GROWTH_RATIO, STRETCH_RATIO, stretch_report, triangle_area_growth, triangle_stretch

        points = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1 / (AREA_GROWTH_RATIO * STRETCH_RATIO), 0.]])
        tris = np.array([[2, 0, 1]])
        mesh = FitMesh(points, np.ones_like(points), tris, np.zeros(len(tris)), 1, np.array([1, 2, 0]))
        for ratio, expected in ((AREA_GROWTH_RATIO * 0.99, 0.0), (AREA_GROWTH_RATIO, 0.0),
                                (AREA_GROWTH_RATIO * 1.01, 100.0)):
            with self.subTest(ratio=ratio):
                fitted = points.copy()
                fitted[2, 1] *= ratio
                self.assertAlmostEqual(triangle_area_growth(points, fitted, mesh)[0], ratio)
                self.assertLess(triangle_stretch(points, fitted, mesh)[0], STRETCH_RATIO)
                report = stretch_report(mesh, mesh._replace(points=fitted), np.array([0]))
                self.assertEqual(report["rest"]["area_growth"], expected)
                self.assertEqual(report["parts"][0]["area_growth"], expected)
                self.assertEqual(report["worst"]["area_growth"][0]["area_growth"], expected)
                self.assertEqual(report["rest"]["stretch"], 0.0)

    def test_translation_counts_corner_moves_in_mean_start_edges(self):
        from r3d.appearance_simplify import MOVED_EDGES, stretch_report, triangle_edges, triangle_moved

        points = np.array([[0., 0., 0.], [0.003, 0., 0.], [0., 0.004, 0.]])
        tris = np.array([[0, 1, 2]])
        mesh = FitMesh(points, np.ones_like(points), tris, np.zeros(len(tris)), 1, np.arange(len(points)))
        mean_edge = triangle_edges(points, tris, mesh.vertex_point).mean()
        for ratio, expected in ((MOVED_EDGES * 0.99, 0.0), (MOVED_EDGES, 0.0), (MOVED_EDGES * 1.01, 100.0)):
            with self.subTest(ratio=ratio):
                fitted = points + [0., 0., ratio * mean_edge]
                self.assertAlmostEqual(triangle_moved(points, fitted, mesh)[0], ratio)
                report = stretch_report(mesh, mesh._replace(points=fitted), np.array([0]))
                self.assertEqual(report["rest"], dict(triangles=1, stretch=0.0, area_growth=0.0, moved=expected))
                self.assertEqual(report["parts"][0]["moved"], expected)
                self.assertEqual(report["worst"]["moved"][0]["moved"], expected)

    def test_zero_area_start_triangle_that_opens_counts_as_grown(self):
        from r3d.appearance_simplify import stretch_report, triangle_area_growth, triangle_moved

        points = np.zeros((3, 3))
        tris = np.array([[0, 1, 2]])
        mesh = FitMesh(points, np.ones_like(points), tris, np.zeros(len(tris)), 1, np.arange(len(points)))
        fitted = np.eye(3)
        self.assertTrue(np.isinf(triangle_area_growth(points, fitted, mesh)[0]))
        self.assertTrue(np.isinf(triangle_moved(points, fitted, mesh)[0]))
        report = stretch_report(mesh, mesh._replace(points=fitted), np.array([0]))
        self.assertEqual(report["rest"]["area_growth"], 100.0)
        unchanged = stretch_report(mesh, mesh, np.array([0]))
        self.assertEqual(unchanged["rest"], dict(triangles=1, stretch=0.0, area_growth=0.0, moved=0.0))


@unittest.skipIf(torch is None, "needs PyTorch")
class GatherTests(unittest.TestCase):
    def test_fixed_point_backward_matches_float_and_repeats(self):
        from r3d.appearance_gather import gather
        from r3d.appearance_simplify import deterministic_fit

        for device in (["cpu", "cuda"] if GPU else ["cpu"]):
            for seed in (1, 7, 19):
                with self.subTest(device=device, seed=seed), deterministic_fit():
                    generator = torch.Generator(device=device).manual_seed(seed)
                    index = torch.randint(8, (4096, 3), device=device, generator=generator)
                    incoming = torch.randn((*index.shape, 4), device=device, generator=generator)
                    source = torch.randn((8, 4), device=device, generator=generator, requires_grad=True)
                    expected = torch.autograd.grad(source[index], source, incoming)[0]
                    gradients = [torch.autograd.grad(gather(source, index), source, incoming)[0] for _ in range(5)]
                    torch.testing.assert_close(gradients[0], expected, atol=1e-4, rtol=1e-4)
                    for actual in gradients[1:]:
                        self.assertTrue(torch.equal(actual, gradients[0]))
                    zero = torch.autograd.grad(gather(source, index), source, torch.zeros_like(incoming))[0]
                    self.assertEqual(torch.count_nonzero(zero).item(), 0)
                    empty = torch.autograd.grad(gather(source, index[:0]), source, incoming[:0])[0]
                    self.assertEqual(torch.count_nonzero(empty).item(), 0)

    def test_nonfinite_gradient_propagates_and_restores_deterministic_mode(self):
        from r3d.appearance_gather import gather
        from r3d.appearance_simplify import deterministic_fit

        for value in (float("inf"), float("nan")):
            with self.subTest(value=value), deterministic_fit():
                source = torch.ones((1, 1), requires_grad=True)
                gather(source, torch.tensor([0])).backward(torch.tensor([[value]]))
                self.assertTrue(torch.isnan(source.grad).all())
                self.assertTrue(torch.are_deterministic_algorithms_enabled())


    def test_deterministic_scope_restores_memory_fill_on_exception(self):
        from r3d.appearance_simplify import deterministic_fit

        previous = torch.utils.deterministic.fill_uninitialized_memory
        try:
            for enabled in (True, False):
                torch.utils.deterministic.fill_uninitialized_memory = enabled
                with self.assertRaisesRegex(ValueError, "scope"):
                    with deterministic_fit():
                        self.assertFalse(torch.utils.deterministic.fill_uninitialized_memory)
                        raise ValueError("scope")
                self.assertEqual(torch.utils.deterministic.fill_uninitialized_memory, enabled)
        finally:
            torch.utils.deterministic.fill_uninitialized_memory = previous


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AppearanceMeshTests(unittest.TestCase):
    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_fit_rejects_nonfinite_loss_without_reporting(self):
        from r3d import appearance_simplify as appearance

        points, rgb, tris = grid(1)
        mesh = FitMesh(points, rgb / 255, tris, np.ones(len(tris)), 64, np.arange(len(points)))
        target = np.full((1, 1, 3), np.nan, dtype=np.float32)
        render = mock.Mock(side_effect=lambda points, colours, *args: colours.mean(0).reshape(1, 1, 3))
        with mock.patch.object(appearance, "Renderer", return_value=render):
            with self.assertRaisesRegex(ValueError, "finite"):
                appearance.optimise(mesh, [(np.eye(4), target)], (1, 1), steps=2, batch=1,
                                    lr_position=0, lr_colour=0, laplacian=0, report=0, device="cpu", seam_margin=0)

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_normal_loss_without_angles_handles_empty_masks(self):
        from r3d import appearance_simplify as appearance

        for covered in (False, True):
            with self.subTest(covered=covered):
                normal = torch.tensor([[[1.0, 0, 0], [0, 1.0, 0]]], requires_grad=True)
                reference = torch.tensor([[[0.0, 1.0, 0], [0, 0, 1.0]]])
                both = torch.tensor([[covered, False]])
                with mock.patch.object(appearance, "normal_pairs", return_value=(normal, reference, both)):
                    distance, angles = appearance.normal_l1(None, None, None, None, angles=False)
                    self.assertIsNone(angles)
                    self.assertEqual(distance.item(), 2.0 if covered else 0.0)
                    distance.backward()
                    self.assertTrue(torch.isfinite(normal.grad).all())
                    if not covered:
                        self.assertEqual(torch.count_nonzero(normal.grad).item(), 0)

    def test_surface_recipe_keys_are_required_and_validated(self):
        from r3d.import_settings import SettingsError, load_fit

        recipe = dict(prune=dict(budget=1, coverage_every_ms=1),
                      poses=dict(size=[2, 2], train_every_ms=1, held_out_every_ms=2),
                      optimise=dict(steps=1, batch=1, laplacian=1, normal_weight=0,
                                    surface_weight=10.0, surface_margin=0.25))
        for key in ("surface_weight", "surface_margin"):
            for value in (None, -1, float("nan"), float("inf"), "bad", True):
                with self.subTest(key=key, value=value):
                    options = {group: dict(table) for group, table in recipe.items()}
                    if value is None:
                        del options["optimise"][key]
                    else:
                        options["optimise"][key] = value
                    with self.assertRaises(SettingsError):
                        load_fit(options, SimpleNamespace(triangles=1), "fit")
        fit = load_fit(recipe, SimpleNamespace(triangles=1), "fit")
        self.assertEqual((fit.surface_weight, fit.surface_margin), (10.0, 0.25))

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_surface_optimise_pulls_to_plane_refreshes_and_repeats(self):
        from scipy.spatial import cKDTree
        from r3d import appearance_simplify as appearance

        points, rgb, tris = grid(1)
        mesh = FitMesh(points + [0, 0, 1], rgb / 255, tris, np.ones(len(tris)), 64, np.arange(len(points)))
        source = SimpleNamespace(p=points, tri_v=tris)
        views = [(np.eye(4), np.zeros((1, 1, 3), dtype=np.float32))]
        options = dict(steps=appearance.SURFACE_QUERY_EVERY + 1, batch=1, lr_position=0.01,
                       lr_colour=0, laplacian=0, report=0, device="cpu", source=source,
                       surface_weight=10, surface_margin=0.25, seam_margin=0)
        render = mock.Mock(side_effect=lambda points, colours, *args: colours.mean(0).reshape(1, 1, 3))
        queries = []

        def tree(reference):
            query = mock.Mock(wraps=cKDTree(reference).query)
            queries.append(query)
            return SimpleNamespace(query=query)

        with mock.patch.object(appearance, "Renderer", return_value=render), \
                mock.patch.object(appearance, "SURFACE_REFERENCE_SAMPLES", appearance.SURFACE_SAMPLES_PER_TRIANGLE ** 4), \
                mock.patch("scipy.spatial.cKDTree", side_effect=tree):
            for seed in (1, 7, 19):
                first = appearance.optimise(mesh, views, (1, 1), seed=seed, **options)
                second = appearance.optimise(mesh, views, (1, 1), seed=seed, **options)
                for actual, expected in zip(first, second):
                    self.assertEqual(np.asarray(actual).tobytes(), np.asarray(expected).tobytes())
                self.assertLess(first[0][np.unique(tris), 2].mean(), mesh.points[:, 2].mean())
        for query in queries:
            self.assertEqual(query.call_count, 2)
            self.assertEqual(query.call_args.args[0].shape, (len(tris) * appearance.SURFACE_SAMPLES_PER_TRIANGLE, 3))

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_local_scale_holds_tiny_edges_against_image_pull(self):
        from r3d import appearance_simplify as appearance

        tiny_size = 0.05
        large_ratio = 40
        target_spread = 8
        max_stretch = 2
        tiny = tiny_size * np.array([[-1, 0, 0], [1, 0, 0], [0, 1, 0]])
        points = np.concatenate([tiny, tiny * large_ratio + [large_ratio * tiny_size, 0, 0]])
        tris = np.arange(len(points)).reshape(-1, 3)
        mesh = FitMesh(points, np.full_like(points, 0.5), tris, np.ones(len(tris)), 64, np.arange(len(points)))
        target = (0.5 + target_spread * tiny).astype(np.float32)[None]
        views = [(np.eye(4), target)]
        render = mock.Mock(side_effect=lambda points, colours, *args: (0.5 + points[:len(tiny)])[None])
        edges = point_edges(tris[:1], mesh.vertex_point)
        lengths = np.linalg.norm(points[edges[:, 0]] - points[edges[:, 1]], axis=1)
        with mock.patch.object(appearance, "Renderer", return_value=render):
            for seed in (1, 7, 19):
                with self.subTest(seed=seed):
                    fitted, _, history = appearance.optimise(
                        mesh, views, (len(tiny), 1), steps=60, batch=1, lr_position=0.001,
                        lr_colour=0, laplacian=40, report=0, device="cpu", seed=seed, seam_margin=0)
                    final = np.linalg.norm(fitted[edges[:, 0]] - fitted[edges[:, 1]], axis=1)
                    self.assertLess(history[-1], history[0])
                    np.testing.assert_array_less(final, max_stretch * lengths)

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_position_step_uses_capped_local_start_scale(self):
        from r3d import appearance_simplify as appearance

        large_size = 1.0
        separation = 2 * large_size
        triangle = np.array([[0.0, 0, 0], [1.0, 0, 0], [0.0, 1.0, 0]])
        tiny_count = len(triangle)
        lr_position = 0.001
        # The image term pulls only z, so Adam's first step is its per-component bound.
        render = mock.Mock(side_effect=lambda points, colours, *args: (0.5 + points[:, 2:3]).expand(-1, 3)[None])
        for tiny_size in (0.01, 0.1):
            with self.subTest(tiny_size=tiny_size):
                points = np.concatenate([tiny_size * triangle, large_size * triangle + [separation, 0, 0]])
                tris = np.arange(len(points)).reshape(-1, 3)
                mesh = FitMesh(points, np.full_like(points, 0.5), tris, np.ones(len(tris)), 64, np.arange(len(points)))
                views = [(np.eye(4), np.full((1, len(points), 3), 0.75, dtype=np.float32))]
                start = torch.as_tensor(points, dtype=torch.float32)
                scale = appearance.vertex_scale(start, torch.as_tensor(point_edges(tris, mesh.vertex_point))).numpy()
                step_scale = scale[:, 0] / scale.max()
                diagonal = np.linalg.norm(points.max(axis=0) - points.min(axis=0))
                bound = lr_position * diagonal * step_scale
                tolerance = np.finfo(np.float32).eps * diagonal
                np.testing.assert_allclose(step_scale[tiny_count:], 1)
                with mock.patch.object(appearance, "Renderer", return_value=render):
                    fitted, _, _ = appearance.optimise(
                        mesh, views, (len(points), 1), steps=1, batch=1, lr_position=lr_position,
                        lr_colour=0, laplacian=0, report=0, device="cpu", seam_margin=0)
                displacement = np.linalg.norm(fitted - points, axis=1)
                np.testing.assert_array_less(displacement, bound + tolerance)
                np.testing.assert_array_less(displacement[:tiny_count], bound[tiny_count:])
                self.assertTrue(np.all(displacement > 0))
                np.testing.assert_allclose(displacement[tiny_count:], bound[tiny_count:], atol=tolerance)

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_surface_envelope_and_tangential_pull(self):
        from r3d.appearance_simplify import SURFACE_TANGENTIAL_WEIGHT, surface_loss

        margin = 0.25
        normal = torch.tensor([[0.0, 0.0, 1.0]])
        centre = torch.zeros_like(normal)
        for scale in (1.0, 7.0):
            for offset in (-2 * margin, -margin / 2, margin / 2, 2 * margin):
                with self.subTest(scale=scale, offset=offset):
                    point = torch.tensor([[0.0, 0.0, offset * scale]], requires_grad=True)
                    loss = surface_loss(point, centre, normal, scale, margin)
                    envelope = loss - SURFACE_TANGENTIAL_WEIGHT * offset ** 2
                    self.assertAlmostEqual(envelope.item(), max(0, abs(offset) - margin) ** 2)
                    loss.backward()
                    self.assertGreater(point.grad[0, 2].item() * offset, 0)
                    if abs(offset) < margin:
                        with mock.patch("r3d.appearance_simplify.SURFACE_TANGENTIAL_WEIGHT", 0):
                            envelope = surface_loss(point, centre, normal, scale, margin)
                            self.assertEqual(envelope.item(), 0)
                            self.assertEqual(torch.count_nonzero(torch.autograd.grad(envelope, point)[0]).item(), 0)

    @unittest.skipIf(torch is None, "needs PyTorch")
    def test_surface_sample_scales_match_scalar_rows(self):
        from r3d.appearance_simplify import surface_loss

        scales = torch.tensor([[1.0], [7.0]])
        samples = torch.tensor([[0.5, 0.0, 1.0], [1.0, 0.0, -2.0]])
        centres = torch.zeros_like(samples)
        normals = torch.tensor([[0.0, 0.0, 1.0]]).expand_as(samples)
        margin = 0.25
        rows = [surface_loss(sample[None], centre[None], normal[None], scale.item(), margin)
                for sample, centre, normal, scale in zip(samples, centres, normals, scales)]
        torch.testing.assert_close(surface_loss(samples, centres, normals, scales, margin), torch.stack(rows).mean())

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
            write_mesh(pathlib.Path(directory) / "out", "card", mesh._replace(points=points, rgb=rgb))
            back = read_lit_mesh(pathlib.Path(directory) / "out" / "card.mesh")
        q, colours, _tris, _double, _face = finest_triangles(back)
        written = {(tuple(p), tuple(c)) for p, c in zip(q, colours)}
        wanted = {(tuple(np.rint(p * back.position_scale).astype(int)), tuple(np.rint(c * 255).astype(int)))
                  for p, c in zip(points[mesh[5]], rgb)}
        self.assertEqual(written, wanted)

    def test_a_flat_start_reads_each_face_colour_onto_its_own_triangle(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_flat_start(directory))
        left = mesh.points[mesh.vertex_point][mesh.tris].mean(axis=1)[:, 0] < 0
        self.assertTrue(left.any() and (~left).any())
        # The colours write_flat_start gives each half, within an RGB565 step.
        wanted = np.where(left[:, None], [[200.0, 60.0, 40.0]], [[40.0, 90.0, 200.0]]) / 255.0
        self.assertTrue(np.all(np.abs(mesh.rgb - wanted) <= 8 / 255.0), "a face colour landed on another triangle")

    def test_a_flat_start_carries_a_colour_per_triangle_and_writes_back_flat(self):
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_flat_start(directory))
            self.assertTrue(mesh.flat)
            self.assertEqual(len(mesh.rgb), len(mesh.tris))
            self.assertEqual(len(mesh.points), len(mesh.vertex_point), "a flat start welds by position alone")
            points, rgb = mesh.points + [0.25, 0.0, 0.0], 1.0 - mesh.rgb
            write_mesh(pathlib.Path(directory) / "out", "flat", mesh._replace(points=points, rgb=rgb))
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
            count = write_mesh(pathlib.Path(directory) / "out", "card", mesh._replace(points=points, rgb=rgb))
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
    def test_main_prunes_fitted_geometry_and_colours_before_writing(self):
        from r3d import appearance_simplify as appearance

        with tempfile.TemporaryDirectory() as directory:
            for flat in (False, True):
                with self.subTest(flat=flat):
                    mesh = start_mesh(write_start(directory))._replace(flat=flat)
                    if flat:
                        mesh = mesh._replace(rgb=np.full((len(mesh.tris), 3), 0.5))
                    points, rgb = mesh.points + 1, mesh.rgb / 2
                    fitted = mesh._replace(points=points, rgb=rgb)
                    pruned, _kept = prune(fitted, np.ones(len(mesh.tris)), len(mesh.tris) - 1)
                    calls = mock.Mock()
                    with mock.patch.multiple(appearance,
                            load_scene=mock.Mock(return_value=SimpleNamespace(camera=SimpleNamespace(
                                component=SimpleNamespace(background=0)))),
                            start_mesh=mock.Mock(return_value=mesh),
                            load_views=mock.Mock(return_value=(["training"], (24, 20))),
                            pose_views=mock.Mock(return_value=(["coverage"], (80, 80))),
                            optimise=calls.optimise, coverage=calls.coverage, prune=calls.prune,
                            write_mesh=calls.write_mesh):
                        calls.optimise.return_value = (points, rgb, [1.0])
                        calls.coverage.return_value = np.ones(len(mesh.tris))
                        calls.prune.return_value = (pruned, np.arange(len(pruned.tris)))
                        calls.write_mesh.return_value = len(pruned.tris)
                        appearance.main(["--scene", "scene.toml", "--start", "start.mesh", "--poses", "poses.txt",
                                         "--reference", "reference", "--coverage-poses", "coverage.txt",
                                         "--budget", str(len(pruned.tris)), "--out", directory])
                    self.assertEqual(calls.optimise.call_args.kwargs["seam_margin"], appearance.SEAM_MARGIN)
                    self.assertEqual([call[0] for call in calls.mock_calls],
                                     ["optimise", "coverage", "prune", "write_mesh"])
                    covered, views, size = calls.coverage.call_args.args
                    np.testing.assert_array_equal(covered.points, points)
                    np.testing.assert_array_equal(covered.rgb, rgb)
                    self.assertEqual((views, size), (["coverage"], (80, 80)))
                    written = calls.write_mesh.call_args.args
                    self.assertEqual(len(written), 3)
                    self.assertIs(written[2], pruned)
                    pruned_mesh, shown, budget = calls.prune.call_args.args
                    self.assertIs(pruned_mesh, covered)
                    np.testing.assert_array_equal(shown, calls.coverage.return_value)
                    self.assertEqual(budget, len(pruned.tris))
                    report = json.loads((pathlib.Path(directory) / "stretch.json").read_text())
                    self.assertEqual(sum(row["triangles"] for row in report["parts"]), len(pruned.tris))
                    self.assertEqual(report["rest"]["stretch"], 0)

    def test_invalid_surface_flags_fail_before_loading_a_scene(self):
        from r3d import appearance_simplify as appearance

        for flags in (["--object", "card", "--surface-weight", "-1"], ["--object", "card", "--surface-weight", "nan"],
                      ["--object", "card", "--surface-margin", "-1"], ["--surface-weight", "1"],
                      *(["--seam-margin", value] for value in ("-1", "nan", "inf"))):
            with self.subTest(flags=flags), mock.patch.object(appearance, "load_scene") as scene:
                with self.assertRaises(SystemExit), mock.patch("sys.stderr"):
                    appearance.main(["--scene", "scene.toml", "--start", "start.mesh", "--poses", "poses.txt",
                                     "--reference", "reference", "--out", "out", *flags])
                scene.assert_not_called()

    def test_main_without_a_budget_optimises_and_writes_without_pruning(self):
        from r3d import appearance_simplify as appearance

        source = SimpleNamespace()
        source_for = mock.Mock(return_value=(source, None))
        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
            points, rgb = mesh.points + 1, mesh.rgb / 2
            with mock.patch.multiple(appearance,
                    load_scene=mock.Mock(return_value=SimpleNamespace(camera=SimpleNamespace(
                        component=SimpleNamespace(background=0)))),
                    start_mesh=mock.Mock(return_value=mesh),
                    load_views=mock.Mock(return_value=(["training"], (24, 20))),
                    optimise=mock.DEFAULT, coverage=mock.DEFAULT, prune=mock.DEFAULT,
                    write_mesh=mock.DEFAULT) as calls, mock.patch.dict(sys.modules, {
                        "r3d.reference_render": SimpleNamespace(source_for=source_for)}):
                calls["optimise"].return_value = (points, rgb, [1.0])
                calls["write_mesh"].return_value = len(mesh.tris)
                appearance.main(["--scene", "scene.toml", "--start", "start.mesh", "--poses", "poses.txt",
                                 "--reference", "reference", "--out", directory,
                                 "--object", "card", "--surface-weight", "10", "--surface-margin", "0.25",
                                 "--seam-margin", "0.25"])
            calls["optimise"].assert_called_once()
            self.assertEqual(source_for.call_args.args[1], "card")
            self.assertEqual(source_for.call_args.kwargs, {"lit": False})
            options = calls["optimise"].call_args.kwargs
            self.assertIs(options["source"], source)
            self.assertEqual(options["seam_margin"], 0.25)
            self.assertEqual((options["surface_weight"], options["surface_margin"]), (10, 0.25))
            calls["coverage"].assert_not_called()
            calls["prune"].assert_not_called()
            written = calls["write_mesh"].call_args.args
            self.assertEqual(len(written), 3)
            np.testing.assert_array_equal(written[2].points, points)
            np.testing.assert_array_equal(written[2].rgb, rgb)
            report = json.loads((pathlib.Path(directory) / "stretch.json").read_text())
            self.assertEqual(sum(row["triangles"] for row in report["parts"]), len(mesh.tris))

    def test_pruning_keeps_the_budget_and_what_shows_most(self):
        mesh = FitMesh(np.zeros((9, 3)), np.zeros((9, 3)), np.arange(18).reshape(6, 3) % 9, np.zeros(6), 8, np.arange(9))
        shown = np.array([0, 50, 3, 0, 900, 7])
        pruned, _kept = prune(mesh, shown, 3)
        self.assertEqual(len(pruned[2]), 3)
        kept = {tuple(t) for t in pruned[2]}
        for index in (4, 1, 5):
            self.assertIn(tuple(mesh[2][index]), kept)
        self.assertEqual(len(prune(mesh, shown, 6)[0].tris), 4, "a triangle no view shows was kept")

    def test_a_flat_mesh_keeps_each_colour_with_its_triangle(self):
        colours = np.arange(18, dtype=float).reshape(6, 3) / 17.0
        mesh = FitMesh(np.zeros((9, 3)), colours, np.arange(18).reshape(6, 3) % 9, np.zeros(6), 8, np.arange(9), True)
        pruned, _kept = prune(mesh, np.array([0, 50, 3, 0, 900, 7]), 3)
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
    def test_barycentrics_and_antialias_match_reference(self):
        from r3d.appearance_simplify import Renderer, deterministic_fit

        size = (64, 48)
        points = cuda([[-0.83, -0.72, 0.1], [0.71, -0.61, -0.2], [0.64, 0.81, 0.2], [-0.69, 0.68, 0]])
        tris = np.array([[0, 1, 2], [0, 2, 3]])
        render = Renderer(tris, [True, True], np.arange(4), size, "cuda")
        colours = cuda([[0.8, 0.2, 0.1]] * 4)
        matrix = cuda(projection([0, 0, 2], [0, 0, -1], *size, 0.62, 0.5))
        with deterministic_fit():
            points.requires_grad_()
            clip, rast, keep = render.raster(points, matrix)
            bary = render.barycentrics(clip, rast, keep)
            covered = rast[0, ..., 3] > 0
            torch.testing.assert_close(bary[covered][:, :2], rast[0, ..., :2][covered], atol=2e-5, rtol=2e-5)
            image = render(points, colours, matrix)
            gradient = torch.autograd.grad(image.double().sum(), points)[0]
        from r3d.appearance_parity import reference_image

        with torch.no_grad():
            reference = reference_image(render, points, colours, matrix)
        torch.testing.assert_close(image, reference, atol=2e-4, rtol=2e-4)
        epsilon = 1e-4
        finite = torch.zeros_like(points)
        with deterministic_fit(), torch.no_grad():
            for vertex in range(len(points)):
                for axis in range(points.shape[1]):
                    plus, minus = points.detach().clone(), points.detach().clone()
                    plus[vertex, axis] += epsilon
                    minus[vertex, axis] -= epsilon
                    finite[vertex, axis] = (render(plus, colours, matrix).double().sum() -
                                            render(minus, colours, matrix).double().sum()) / (2 * epsilon)
        # Float32 crossing arithmetic limits central differences at a subpixel displacement.
        torch.testing.assert_close(gradient, finite, atol=0.1, rtol=0.01)
        cosine = torch.nn.functional.cosine_similarity(gradient.flatten(), finite.flatten(), dim=0)
        self.assertGreater(float(cosine), 0.999)

    def test_renderer_backward_is_bitwise_repeatable(self):
        from r3d.appearance_simplify import Renderer, deterministic_fit

        points, rgb, tris = grid(3)
        render = Renderer(tris, np.ones(len(tris)), np.arange(len(points)), (48, 40), "cuda")
        matrix = cuda(projection([0.2, -0.1, 3], [0, 0, -1], 48, 40, 0.62, 0.5))
        results = []
        with deterministic_fit():
            for _ in range(5):
                positions = cuda(points).requires_grad_()
                colours = cuda(rgb / 255).requires_grad_()
                image = render(positions, colours, matrix)
                normal, _ = render.normals(positions, matrix, [0.2, -0.1, 3])
                gradients = torch.autograd.grad(image.square().sum() + normal.sum(), (positions, colours))
                results.append((image.detach(), *gradients))
        for result in results[1:]:
            for actual, expected in zip(result, results[0]):
                self.assertTrue(torch.equal(actual, expected))

    def test_optimise_with_surface_and_seams_is_bitwise_repeatable(self):
        from r3d.appearance_simplify import Renderer, optimise, seam_holds, vertex_scale, SEAM_MARGIN, SURFACE_QUERY_EVERY

        plane = np.array([[-1, -1, 0], [1, -1, 0], [0, 1, 0]], dtype=float)
        points = np.vstack([plane * 0.5 + [0, 0, 0.1], plane])
        tris = np.array([[0, 1, 2], [3, 4, 5]])
        rgb = np.tile([[0.8, 0.2, 0.1], [0.1, 0.4, 0.8]], (len(plane), 1))
        mesh = FitMesh(points, rgb, tris, np.ones(len(tris)), 64, np.arange(len(points)))
        size = (32, 24)
        render = Renderer(tris, mesh.double, mesh.vertex_point, size, "cuda")
        matrices = [projection([x, 0, 3], [0, 0, -1], *size, 0.62, 0.5) for x in (-0.2, 0.2)]
        normal = np.broadcast_to(np.array([0, 0, 1], dtype=np.float32), (size[1], size[0], 3)).copy()
        views = [(matrix, draw(render, points * [1.1, 1, 1], mesh.rgb, matrix), normal, [x, 0, 3])
                 for matrix, x in zip(matrices, (-0.2, 0.2))]
        moved = mesh._replace(points=points * [0.8, 1, 1])
        options = dict(steps=SURFACE_QUERY_EVERY + 1, batch=2, lr_position=0.001, lr_colour=0.01, laplacian=0.1, report=0,
                       surface_weight=10, surface_margin=0.25, source=SimpleNamespace(p=points, tri_v=tris))
        scale = vertex_scale(cuda(moved.points), torch.as_tensor(point_edges(tris, mesh.vertex_point), device="cuda"))
        holds = seam_holds(moved, SEAM_MARGIN * scale.cpu().numpy()[:, 0])
        self.assertGreaterEqual(len(holds.points), len(plane))
        for normal_weight in (0, 1):
            for seam_margin in (0, SEAM_MARGIN):
                with self.subTest(normal_weight=normal_weight, seam_margin=seam_margin):
                    first = optimise(moved, views, size, normal_weight=normal_weight, seam_margin=seam_margin, **options)
                    second = optimise(moved, views, size, normal_weight=normal_weight, seam_margin=seam_margin, **options)
                    for actual, expected in zip(first, second):
                        self.assertEqual(np.asarray(actual).tobytes(), np.asarray(expected).tobytes())
                    with tempfile.TemporaryDirectory() as directory:
                        outputs = [pathlib.Path(directory) / name for name in ("first", "second")]
                        for output, (positions, colours, _history) in zip(outputs, (first, second)):
                            write_mesh(output, "card", mesh._replace(points=positions, rgb=colours))
                        self.assertEqual((outputs[0] / "card.mesh").read_bytes(), (outputs[1] / "card.mesh").read_bytes())

    def test_image_error_falls(self):
        from r3d.appearance_simplify import Renderer, optimise

        with tempfile.TemporaryDirectory() as directory:
            mesh = start_mesh(write_start(directory))
            size = (48, 40)
            matrix = projection([0.2, -0.1, 3.0], [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5)
            truth = Renderer(mesh[2], mesh[3], mesh[5], size, "cuda")
            target = draw(truth, mesh[0] * [1.3, 0.8, 1.0], mesh[1][::-1].copy(), matrix)
            _points, _rgb, history = optimise(mesh, [(matrix, target)], size, steps=150, batch=1, lr_position=0.01,
                                              lr_colour=0.05, laplacian=0.0, report=0, seam_margin=0)
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
        target = draw(truth, shifted, mesh.rgb, matrix)
        points, rgb, history = optimise(mesh, [(matrix, target)], size, steps=150, batch=1, lr_position=0.005,
                                        lr_colour=0.0, laplacian=0.0, report=0, seam_margin=0)
        self.assertEqual(rgb.shape, mesh.rgb.shape)
        self.assertLess(np.mean(history[-10:]), 0.5 * np.mean(history[:3]), "the colour edge did not move")
        self.assertGreater(points[np.abs(mesh.points[:, 0]) < 1e-9, 0].mean(), 0.05)

    def test_a_flat_renderer_draws_each_triangle_in_its_own_colour(self):
        from r3d.appearance_simplify import Renderer

        positions, tris = grid_mesh(2, 1.0)
        colours = np.stack([[(k + 1) / (len(tris) + 1), 1.0 - k / len(tris), (k % 3) / 2.0] for k in range(len(tris))])
        size = (96, 80)
        matrix = projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 48, 40, 0.62, 0.5)
        render = Renderer(tris, np.zeros(len(tris), dtype=bool), np.arange(len(positions)), size, "cuda", flat=True)
        image = draw(render, positions, colours, matrix)
        # Each triangle's centroid, projected by the matrix alone, lands on a pixel of that triangle's colour.
        width, height = size
        for k, tri in enumerate(tris):
            clip = matrix @ np.append(positions[tri].mean(axis=0), 1.0)
            x, y = clip[:2] / clip[3]
            column, row = int((x + 1) / 2 * width), int((1 - y) / 2 * height)
            self.assertTrue(0 <= column < width and 0 <= row < height, f"triangle {k} is off screen")
            self.assertTrue(np.allclose(image[row, column], colours[k], atol=1e-4), f"triangle {k} is not in its own colour")

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
        target = draw(truth, positions, wanted, matrix)
        start = np.full((count, 3), 0.5)
        mesh = FitMesh(positions, start, tris, double, 64, vertex_point, True)
        _points, rgb, _history = optimise(mesh, [(matrix, target)], size, steps=400, batch=1, lr_position=0.0, lr_colour=0.05,
                                          laplacian=0.0, report=0, seam_margin=0)
        with torch.no_grad():
            ids = truth.visible_ids(cuda(positions), cuda(matrix)).cpu().numpy()
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
        views = []
        for width, height in ((24, 20), (20, 24)):
            matrix = projection([0.2, -0.1, 3.0], [0.0, 0.0, -1.0], width, height, 0.62, 0.5)
            truth = Renderer(mesh[2], mesh[3], mesh[5], (2 * width, 2 * height), "cuda")
            target = draw(truth, mesh[0] * [1.3, 0.8, 1.0], mesh[1][::-1].copy(), matrix)
            self.assertEqual(target.shape[:2], (2 * height, 2 * width))
            views.append((matrix, target))
        _points, _rgb, history = optimise(mesh, views, None, steps=150, batch=2, lr_position=0.01, lr_colour=0.05,
                                          laplacian=0.0, report=0, seam_margin=0)
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
        colour = draw(render, mesh[0], mesh[1], matrix)
        view = (matrix, colour, reference, eye)
        before = normal_error(mesh, [view], size)
        points, _rgb, _history = optimise(mesh, [view], size, steps=200, batch=1, lr_position=0.02, lr_colour=0.0,
                                          laplacian=0.0, report=0, normal_weight=1.0, seam_margin=0)
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
        pruned, _kept = prune(mesh, shown, len(front_tris))
        z = mesh[0][mesh[5]][pruned[2]][..., 2]
        self.assertEqual(len(pruned[2]), len(front_tris))
        self.assertTrue(np.all(np.abs(z) < 1e-6), "a hidden triangle outlived a visible one")


    def card(self, z, half=1.0, rgb=0.5):
        """A two-by-two grid card at depth `z` facing the camera, as a FitMesh."""
        positions, tris = grid_mesh(2, half)
        positions[:, 2] = z
        return FitMesh(positions, np.full((len(positions), 3), rgb), tris, np.zeros(len(tris), dtype=int), 1,
                       np.arange(len(positions)))

    def test_copies_drawn_in_front_hide_the_rest_from_the_fit(self):
        from r3d.appearance_simplify import Renderer, optimise

        rest, copy = self.card(0.0), self.card(1.0, half=2.0)
        size = (48, 40)
        matrix = projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5)
        target = draw(Renderer(rest.tris, rest.double, rest.vertex_point, size, "cuda"), rest.points,
                      np.zeros_like(rest.rgb), matrix)
        fits = {copies is None: optimise(rest, [(matrix, target)], size, steps=20, batch=1, lr_position=0.0,
                                         lr_colour=0.05, laplacian=0.0, report=0, seam_margin=0, copies=copies)[1]
                for copies in (None, copy)}
        self.assertLess(fits[True].max(), 0.4, "without copies the rest is fitted to the target")
        np.testing.assert_array_equal(fits[False], rest.rgb, "a rest behind a copy is not fitted")

    def test_hidden_copy_pixels_count_where_a_mesh_stands_in_front_of_a_copy(self):
        from r3d.fit.copies import hidden_copy_pixels
        from r3d.fitted_variant import poses_text

        copy, other = self.card(0.0, half=0.5), self.card(0.0, half=1.0)
        other.points[:, 0] += 3.0
        source = SimpleNamespace(p=np.concatenate([copy.points, other.points]),
                                 tri_v=np.concatenate([copy.tris, other.tris + len(copy.points)]))
        carved = np.arange(len(source.tri_v)) < len(copy.tris)
        meshes = (self.card(-1.0, half=0.5), self.card(1.0, half=0.5), self.card(1.0, half=0.5))
        meshes[2].points[:, 0] += 3.0
        with tempfile.TemporaryDirectory() as directory:
            poses = pathlib.Path(directory) / "poses.txt"
            poses.write_text(poses_text(64, 48, 0.62, 0.5, [[1.5, 0.0, 8.0, 0.0, 0.0, -1.0]]))
            behind, in_front, over_the_rest = hidden_copy_pixels(source, carved, meshes, poses)[:, 0]
        self.assertEqual(behind, 0.0)
        self.assertGreater(in_front, 0.0)
        self.assertEqual(over_the_rest, 0.0, "only pixels where the source shows a copy count")

    def test_distance_is_the_view_axis_distance_where_drawn(self):
        import torch

        from r3d.appearance_simplify import Renderer
        from r3d.fit.gbuffer import distance

        card = self.card(0.0, half=0.5)
        matrix = torch.as_tensor(projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5),
                                 dtype=torch.float32, device="cuda")
        render = Renderer(card.tris, card.double, card.vertex_point, (48, 40), "cuda")
        fitted, shown = distance(render, torch.as_tensor(card.points, dtype=torch.float32, device="cuda"), matrix)
        drawn = shown >= 0
        self.assertTrue(drawn.any() and not drawn.all())
        np.testing.assert_allclose(fitted[drawn].cpu().numpy(), 3.0, rtol=1e-5)
        self.assertTrue(torch.isinf(fitted[~drawn]).all())

    def gbuffer_fit(self, rest, shown, source, carved, copies=None, **weights):
        """The rest fitted on positions alone to one view of `shown`, the mesh the reference shows: its points."""
        from r3d.appearance_simplify import Renderer, optimise

        size = (48, 40)
        matrix = projection([0.0, 0.0, 3.0], [0.0, 0.0, -1.0], 24, 20, 0.62, 0.5)
        target = draw(Renderer(shown.tris, shown.double, shown.vertex_point, size, "cuda"), shown.points, shown.rgb,
                      matrix)
        return optimise(rest, [(matrix, target)], size, steps=200, batch=1, lr_position=0.01, lr_colour=0.0,
                        laplacian=0.0, report=0, seam_margin=0, copies=copies, source=source, carved=carved,
                        **weights)[0]

    def test_the_front_term_pushes_the_rest_behind_the_copy(self):
        copy = self.card(0.0, half=1.0, rgb=0.2)
        source = SimpleNamespace(p=copy.points, tri_v=copy.tris)
        carved = np.ones(len(copy.tris), dtype=bool)
        # The rest takes the copy's colour, so the colour term alone has no reason to move it.
        points = {weight: self.gbuffer_fit(self.card(0.5, half=0.5, rgb=0.2), copy, source, carved, copies=copy,
                                           front_weight=weight)
                  for weight in (0.0, 10.0)}
        # In front means nearer than the copy's distance 3 by more than IN_FRONT of it, z above 0.3.
        self.assertGreater(points[0.0][:, 2].min(), 0.3, "the colour term alone leaves the rest in front")
        self.assertLess(points[10.0][:, 2].max(), 0.3)

    def test_the_depth_term_holds_the_rest_to_the_source_surface(self):
        surface = self.card(0.0, half=1.0)
        source = SimpleNamespace(p=surface.points, tri_v=surface.tris)
        carved = np.zeros(len(surface.tris), dtype=bool)
        # A rest the clear colour shows nowhere, so the colour term alone has no reason to move it.
        rest = self.card(0.5, half=0.5, rgb=0.0)
        points = {weight: self.gbuffer_fit(rest, rest, source, carved, depth_weight=weight) for weight in (0.0, 10.0)}
        np.testing.assert_allclose(points[0.0][:, 2], 0.5, err_msg="the colour term alone leaves the rest off the surface")
        self.assertLess(np.abs(points[10.0][:, 2]).max(), 0.05)

if __name__ == "__main__":
    unittest.main()

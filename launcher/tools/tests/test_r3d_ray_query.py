"""Checks the Mitsuba ray queries against a brute-force ray-triangle test on a random triangle soup, on both the
scalar variant and (where libLLVM exists) the LLVM variant the bake runs on, plus the edges: batching, far
coordinates, multi-hit stepping, the variant guard and the missing-runtime errors."""

import io
import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import mitsuba_reference, ray_query
    from r3d.ray_query import RayQuery
    from tests import soup
except ImportError:
    np = None

from tests.r3d_env import HAVE_MITSUBA  # noqa: E402


def have_llvm():
    if not HAVE_MITSUBA:
        return False
    import drjit as dr

    return "llvm_ad_rgb" in mitsuba_reference.import_mitsuba().variants() and dr.has_backend(dr.JitBackend.LLVM)


from tests.r3d_env import needs_mitsuba  # noqa: E402
needs_llvm = unittest.skipIf(not have_llvm(), "the LLVM variant needs libLLVM")


def brute_force(positions, tris, origin, direction):
    """Every (distance, triangle) a ray crosses, nearest first, by the Moller-Trumbore test."""
    a, b, c = positions[tris[:, 0]], positions[tris[:, 1]], positions[tris[:, 2]]
    edge1, edge2 = b - a, c - a
    p = np.cross(direction, edge2)
    det = np.einsum("ij,ij->i", edge1, p)
    with np.errstate(divide="ignore", invalid="ignore"):
        inverse = 1.0 / det
        offset = origin - a
        u = np.einsum("ij,ij->i", offset, p) * inverse
        q = np.cross(offset, edge1)
        v = np.einsum("ij,j->i", q, direction) * inverse
        t = np.einsum("ij,ij->i", edge2, q) * inverse
    hit = (np.abs(det) > 1e-12) & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0)
    order = np.argsort(t[hit])
    return list(zip(t[hit][order], np.flatnonzero(hit)[order]))


def stack(count, gap, z0=0.0):
    """`count` parallel unit triangles `gap` apart along z, and the ray down their common axis."""
    corners = np.array([[-1.0, -1.0, 0.0], [2.0, -1.0, 0.0], [-1.0, 2.0, 0.0]])
    positions = np.concatenate([corners + [0.0, 0.0, z0 + index * gap] for index in range(count)])
    tris = np.arange(count * 3).reshape(count, 3)
    return positions, tris, np.array([[0.2, 0.2, z0 - 1.0]]), np.array([[0.0, 0.0, 1.0]])


class SoupFixture:
    @classmethod
    def setUpClass(cls):
        rng = np.random.default_rng(7)
        cls.positions = rng.uniform(-1.0, 1.0, (60, 3))
        tris = rng.integers(0, 60, (40, 3))
        distinct = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
        cls.tris = tris[distinct]
        cls.origins = rng.uniform(-1.5, 1.5, (200, 3))
        directions = rng.normal(size=(200, 3))
        cls.directions = directions / np.linalg.norm(directions, axis=1, keepdims=True)
        cls.crossings = [brute_force(cls.positions, cls.tris, o, d) for o, d in zip(cls.origins, cls.directions)]

    def setUp(self):
        self.query = RayQuery(self.positions, self.tris, variant=self.VARIANT)


class QueryChecks(SoupFixture):
    def test_first_hits_are_the_nearest_crossings(self):
        locations, rays, tris = self.query.first_hit(self.origins, self.directions)
        expected = [index for index, found in enumerate(self.crossings) if found]
        self.assertGreater(len(expected), 20)
        self.assertEqual(rays.tolist(), expected)
        self.assertEqual(tris.tolist(), [self.crossings[index][0][1] for index in expected])
        distance = np.linalg.norm(locations - self.origins[rays], axis=1)
        np.testing.assert_allclose(distance, [self.crossings[index][0][0] for index in expected], atol=1e-4)

    def test_blocked_rays_are_the_ones_that_cross_anything(self):
        np.testing.assert_array_equal(self.query.blocked(self.origins, self.directions),
                                      [bool(found) for found in self.crossings])

    def test_every_crossing_along_a_ray_is_reported_nearest_first(self):
        tris, rays, locations = self.query.all_hits(self.origins, self.directions)
        for ray, found in enumerate(self.crossings):
            self.assertEqual(tris[rays == ray].tolist(), [int(tri) for _, tri in found], f"ray {ray}")
            along = np.linalg.norm(locations[rays == ray] - self.origins[ray], axis=1)
            self.assertTrue((np.diff(along) > 0).all(), f"ray {ray} lists its hits out of order")

    def test_each_location_is_at_the_distance_of_its_triangle(self):
        tris, rays, locations = self.query.all_hits(self.origins, self.directions)
        for tri, ray, where in zip(tris, rays, locations):
            distance = dict((int(t), d) for d, t in self.crossings[ray])[int(tri)]
            self.assertAlmostEqual(np.linalg.norm(where - self.origins[ray]), distance, delta=2e-4)

    def test_batches_give_the_unbatched_answers(self):
        whole = (self.query.first_hits(self.origins, self.directions), self.query.blocked(self.origins, self.directions))
        for size in (7, 64, 200):
            with mock.patch.object(ray_query, "BATCH", size):
                hit, distance, tri = self.query.first_hits(self.origins, self.directions)
                blocked = self.query.blocked(self.origins, self.directions)
            np.testing.assert_array_equal(hit, whole[0][0])
            np.testing.assert_allclose(distance[hit], whole[0][1][hit], atol=1e-6)
            np.testing.assert_array_equal(tri[hit], whole[0][2][hit])
            np.testing.assert_array_equal(blocked, whole[1])

    def test_a_scaled_direction_reports_the_same_point(self):
        unit = self.query.first_hit(self.origins, self.directions)
        scaled = self.query.first_hit(self.origins, self.directions * 3.0)
        self.assertEqual(unit[1].tolist(), scaled[1].tolist())
        np.testing.assert_allclose(unit[0], scaled[0], atol=1e-4)

    def test_a_ray_that_misses_everything_returns_nothing(self):
        away, up = np.array([[0.0, 0.0, 10.0]]), np.array([[0.0, 0.0, 1.0]])
        locations, rays, tris = self.query.first_hit(away, up)
        self.assertEqual((len(locations), len(rays), len(tris)), (0, 0, 0))
        self.assertEqual(len(self.query.all_hits(away, up)[0]), 0)
        self.assertEqual(self.query.first_hits(away, up)[0].tolist(), [False])

    def test_no_rays_give_empty_answers(self):
        none = np.zeros((0, 3))
        self.assertEqual(len(self.query.first_hit(none, none)[1]), 0)
        self.assertEqual(len(self.query.blocked(none, none)), 0)
        self.assertEqual(len(self.query.all_hits(none, none)[0]), 0)

    def test_both_sides_of_a_triangle_are_hit(self):
        query = RayQuery(np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]), np.array([[0, 1, 2]]),
                         variant=self.VARIANT)
        for z, dz in ((1.0, -1.0), (-1.0, 1.0)):
            origin, direction = np.array([[0.2, 0.2, z]]), np.array([[0.0, 0.0, dz]])
            self.assertTrue(query.blocked(origin, direction)[0])
            locations, _, tris = query.first_hit(origin, direction)
            self.assertEqual((tris.tolist(), round(float(locations[0][2]), 6)), ([0], 0.0))

    def test_close_surfaces_are_both_reported_where_the_step_is_small(self):
        positions, tris, origin, direction = stack(3, 3e-3)
        found = RayQuery(positions, tris, variant=self.VARIANT).all_hits(origin, direction)[0]
        self.assertEqual(found.tolist(), [0, 1, 2])

    def test_close_surfaces_far_from_the_origin_are_reported_once_each(self):
        positions, tris, origin, direction = stack(3, 3e-3, z0=1400.0)
        found = RayQuery(positions, tris, variant=self.VARIANT).all_hits(origin, direction)[0]
        self.assertEqual(found.tolist(), [0, 1, 2])

    def test_a_ray_is_cut_off_after_the_most_hits_and_says_so(self):
        positions, tris, origin, direction = stack(ray_query.MAX_HITS + 5, 0.01)
        with mock.patch("sys.stderr", new_callable=io.StringIO) as report:
            found = RayQuery(positions, tris, variant=self.VARIANT).all_hits(origin, direction)[0]
        self.assertEqual(found.tolist(), list(range(ray_query.MAX_HITS)))
        self.assertIn("cut off", report.getvalue())


@needs_mitsuba
class ScalarQueryTests(QueryChecks, unittest.TestCase):
    VARIANT = "scalar_rgb"

    def test_the_crossings_survive_far_from_the_origin(self):
        shift = np.array([1400.0, 0.0, 1400.0])
        positions, origins = [(a + shift).astype(np.float32).astype(np.float64) for a in (self.positions, self.origins)]
        far = RayQuery(positions, self.tris, variant=self.VARIANT)
        tris, rays, _ = far.all_hits(origins, self.directions)
        want = sorted((index, int(tri)) for index, (o, d) in enumerate(zip(origins, self.directions))
                      for _, tri in brute_force(positions, self.tris, o, d))
        got = set(zip(rays.tolist(), tris.tolist()))
        self.assertEqual(len(rays), len(got), "a triangle was reported twice")
        # float32 at 1400 resolves 1e-4: a hit on a triangle's very edge may fall either way.
        self.assertLessEqual(len(got ^ set(want)), 0.03 * len(want))


@needs_llvm
class LlvmQueryTests(QueryChecks, unittest.TestCase):
    VARIANT = "llvm_ad_rgb"

    @classmethod
    def tearDownClass(cls):
        mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")


@needs_mitsuba
class RuntimeGuardTests(unittest.TestCase):
    def test_a_query_refuses_to_trace_after_the_variant_changed(self):
        mi = mitsuba_reference.import_mitsuba()
        positions, tris, origin, direction = stack(1, 1.0)
        query = RayQuery(positions, tris, variant="scalar_rgb")
        try:
            mi.set_variant("scalar_spectral")
            for call in (query.blocked, query.first_hits, query.first_hit, query.all_hits):
                with self.assertRaisesRegex(RuntimeError, "scalar_rgb"):
                    call(origin, direction)
        finally:
            mi.set_variant("scalar_rgb")
        self.assertTrue(query.blocked(origin, direction)[0])

    def test_the_llvm_variant_without_libllvm_names_what_is_missing(self):
        import drjit as dr

        positions, tris, _, _ = stack(1, 1.0)
        with mock.patch.object(dr, "has_backend", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "libLLVM"):
                RayQuery(positions, tris, variant="llvm_ad_rgb")
            RayQuery(positions, tris, variant="scalar_rgb")

    def test_without_mitsuba_the_error_names_the_requirements(self):
        positions, tris, _, _ = stack(1, 1.0)
        with mock.patch.object(ray_query, "import_mitsuba", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "requirements.txt"):
                RayQuery(positions, tris)


@unittest.skipIf(np is None, "needs NumPy")
class BoxTests(unittest.TestCase):
    def test_every_face_of_a_box_points_away_from_its_centre_and_all_six_are_covered(self):
        box = soup.box((2.0, 4.0, 6.0))
        a, b, c = (box.vertices[box.faces[:, index]] for index in range(3))
        normal = np.cross(b - a, c - a)
        outward = (normal * (a - box.vertices.mean(axis=0))).sum(axis=1)
        self.assertTrue((outward > 0).all())
        axes = {(int(np.abs(n).argmax()), int(np.sign(n[np.abs(n).argmax()]))) for n in normal}
        self.assertEqual(len(axes), 6)
        self.assertEqual(len(box.faces), 12)


if __name__ == "__main__":
    unittest.main()

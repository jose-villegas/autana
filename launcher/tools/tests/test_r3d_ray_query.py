"""Checks the Mitsuba ray queries against a brute-force ray-triangle test on a random triangle soup, and on a few
cases with known answers."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import mitsuba_reference
    from r3d.ray_query import RayQuery
except ImportError:
    np = None

HAVE_MITSUBA = np is not None and mitsuba_reference.import_mitsuba() is not None


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


@unittest.skipIf(not HAVE_MITSUBA, "the ray queries need Mitsuba")
class RayQueryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")
        rng = np.random.default_rng(7)
        cls.positions = rng.uniform(-1.0, 1.0, (60, 3))
        tris = rng.integers(0, 60, (40, 3))
        distinct = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
        cls.tris = tris[distinct]
        cls.origins = rng.uniform(-1.5, 1.5, (200, 3))
        directions = rng.normal(size=(200, 3))
        cls.directions = directions / np.linalg.norm(directions, axis=1, keepdims=True)
        cls.query = RayQuery(cls.positions, cls.tris)
        cls.crossings = [brute_force(cls.positions, cls.tris, o, d) for o, d in zip(cls.origins, cls.directions)]

    def test_first_hits_are_the_nearest_crossings(self):
        locations, rays, tris = self.query.intersects_location(self.origins, self.directions)
        expected = [index for index, found in enumerate(self.crossings) if found]
        self.assertGreater(len(expected), 20)
        self.assertEqual(rays.tolist(), expected)
        self.assertEqual(tris.tolist(), [self.crossings[index][0][1] for index in expected])
        distance = np.linalg.norm(locations - self.origins[rays], axis=1)
        np.testing.assert_allclose(distance, [self.crossings[index][0][0] for index in expected], atol=1e-4)

    def test_blocked_rays_are_the_ones_that_cross_anything(self):
        np.testing.assert_array_equal(self.query.intersects_any(self.origins, self.directions),
                                      [bool(found) for found in self.crossings])

    def test_every_crossing_along_a_ray_is_reported(self):
        tris, rays = self.query.intersects_id(self.origins, self.directions, multiple_hits=True)
        got = sorted(zip(rays.tolist(), tris.tolist()))
        want = sorted((index, tri) for index, found in enumerate(self.crossings) for _, tri in found)
        self.assertEqual(got, want)

    def test_locations_lie_on_the_ray(self):
        tris, rays, locations = self.query.intersects_id(self.origins, self.directions, return_locations=True)
        along = np.einsum("ij,ij->i", locations - self.origins[rays], self.directions[rays])
        np.testing.assert_allclose(self.origins[rays] + self.directions[rays] * along[:, None], locations, atol=1e-4)

    def test_a_ray_that_misses_everything_returns_nothing(self):
        away, up = np.array([[0.0, 0.0, 10.0]]), np.array([[0.0, 0.0, 1.0]])
        locations, rays, tris = self.query.intersects_location(away, up)
        self.assertEqual((len(locations), len(rays), len(tris)), (0, 0, 0))
        self.assertEqual(len(self.query.intersects_id(away, up)[0]), 0)

    def test_both_sides_of_a_triangle_are_hit(self):
        query = RayQuery(np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]]), np.array([[0, 1, 2]]))
        for z, dz in ((1.0, -1.0), (-1.0, 1.0)):
            self.assertTrue(query.intersects_any(np.array([[0.2, 0.2, z]]), np.array([[0.0, 0.0, dz]]))[0])


if __name__ == "__main__":
    unittest.main()

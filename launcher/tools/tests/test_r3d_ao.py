"""Checks the distance-limited local occlusion the bake can apply to the ambient and indirect light."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    import trimesh
    from trimesh.ray.ray_pyembree import RayMeshIntersector

    from r3d.light import IndirectCache, light, local_occlusion, sky_directions
except ImportError:
    np = None


def corner():
    """A floor at z = 0 (x from -40 to 40) and a wall rising from it at x = 0, both facing the +x, +z side."""
    p = np.array([[-40.0, -40.0, 0.0], [40.0, -40.0, 0.0], [40.0, 40.0, 0.0], [-40.0, 40.0, 0.0],
                  [0.0, -40.0, 0.0], [0.0, 40.0, 0.0], [0.0, 40.0, 40.0], [0.0, -40.0, 40.0]])
    tris = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]])
    return RayMeshIntersector(trimesh.Trimesh(p, tris, process=False))


def ao(distance=10.0, rays=64, strength=1.0, indirect=False):
    return SimpleNamespace(distance=distance, rays=rays, strength=strength, indirect=indirect)


def floor_points(*xs):
    points = np.array([[x, 0.0, 0.0] for x in xs])
    return points, np.tile([0.0, 0.0, 1.0], (len(xs), 1))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class LocalOcclusionTest(unittest.TestCase):
    def factor(self, xs, **settings):
        points, normals = floor_points(*xs)
        return local_occlusion(points, normals, corner(), ao(**settings), 0.01)

    def test_open_floor_is_unoccluded_and_the_wall_foot_is_darkened(self):
        near, far = self.factor([1.0, 30.0])
        self.assertAlmostEqual(far, 1.0, places=9)
        self.assertLess(near, 0.7)

    def test_occlusion_deepens_toward_the_wall_and_stops_at_the_distance(self):
        factors = self.factor([0.5, 2.0, 5.0, 9.0, 12.0])
        self.assertTrue(all(a < b for a, b in zip(factors[:4], factors[1:4])) and factors[3] < 1.0)
        self.assertEqual(factors[4], 1.0)

    def test_a_longer_distance_reaches_more_geometry(self):
        short, long = self.factor([8.0], distance=4.0)[0], self.factor([8.0], distance=20.0)[0]
        self.assertEqual(short, 1.0)
        self.assertLess(long, 1.0)

    def test_a_hit_is_worth_one_minus_its_reach_over_the_distance(self):
        # One ray from the floor up to a ceiling two units over it: it travels 2 / z along the ray's z component.
        p = np.array([[-40.0, -40.0, 2.0], [40.0, -40.0, 2.0], [40.0, 40.0, 2.0], [-40.0, 40.0, 2.0]])
        ceiling = RayMeshIntersector(trimesh.Trimesh(p, np.array([[0, 2, 1], [0, 3, 2]]), process=False))
        reach = 2.0 / sky_directions(1)[0][2]
        for distance in (100.0, 40.0):
            got = local_occlusion(*floor_points(0.0), ceiling, ao(distance=distance, rays=1), 0.0)[0]
            self.assertAlmostEqual(got, reach / distance, places=9)

    def test_strength_scales_the_darkening_linearly(self):
        full, half, none = (self.factor([1.0], strength=s)[0] for s in (1.0, 0.5, 0.0))
        self.assertEqual(none, 1.0)
        self.assertAlmostEqual(1.0 - half, 0.5 * (1.0 - full), places=9)

    def test_the_same_surroundings_give_the_same_factor(self):
        a, b = self.factor([3.0])[0], self.factor([3.0])[0]
        self.assertEqual(a, b)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class OccludedLightTest(unittest.TestCase):
    AMBIENT = {"type": "ambient", "color": [1.0, 1.0, 1.0], "intensity": 0.5}
    SUN = {"type": "directional", "direction": [0.0, 0.0, 1.0], "color": [1.0, 1.0, 1.0], "intensity": 1.0,
           "disc_degrees": 0.0, "rays": 1}

    def lit(self, lights, x, **kwargs):
        points, normals = floor_points(x)
        return light(points, normals, np.array([False]), corner(), lights, 0.01, np.random.default_rng(1), **kwargs)[0]

    def test_without_the_setting_the_ambient_is_the_same_everywhere(self):
        np.testing.assert_allclose(self.lit([self.AMBIENT], 1.0), [0.5] * 3)
        np.testing.assert_allclose(self.lit([self.AMBIENT], 30.0), [0.5] * 3)

    def test_the_ambient_is_darkened_at_the_wall_foot_and_not_far_from_it(self):
        near = self.lit([self.AMBIENT], 1.0, ao=ao())
        far = self.lit([self.AMBIENT], 30.0, ao=ao())
        np.testing.assert_allclose(far, [0.5] * 3)
        self.assertLess(near[0], 0.35)

    def test_a_double_sided_card_against_the_wall_is_lit_by_its_open_side_not_darkened_by_the_wall_behind(self):
        # A card two units from the wall, both sides lit by a sun on the wall's side: its normal is turned to that
        # sun, into the wall, yet it is seen from the open side. The ambient it receives must be the open side's.
        card = np.array([[2.0, -30.0, 0.0], [2.0, 30.0, 0.0], [2.0, 30.0, 40.0], [2.0, -30.0, 40.0]])
        p = np.array([[0.0, -40.0, 0.0], [0.0, 40.0, 0.0], [0.0, 40.0, 40.0], [0.0, -40.0, 40.0]])
        mesh = trimesh.Trimesh(np.concatenate([p, card]), np.array([[0, 1, 2], [0, 2, 3], [4, 5, 6], [4, 6, 7]]), process=False)
        sun = dict(self.SUN, direction=[-1.0, 0.0, 0.5])
        point, normal = np.array([[2.0, 0.0, 10.0]]), np.array([[1.0, 0.0, 0.0]])
        args = (RayMeshIntersector(mesh), [self.AMBIENT, sun], 0.01, np.random.default_rng(1))
        sun_only = light(point, normal, np.array([True]), args[0], [sun], 0.01, args[3], ao=ao())[0]
        both = light(point, normal, np.array([True]), *args, ao=ao())[0]
        self.assertGreater((both - sun_only)[0], 0.5 * 0.85)

    def test_direct_sun_is_not_scaled(self):
        np.testing.assert_allclose(self.lit([self.SUN], 1.0, ao=ao()), self.lit([self.SUN], 1.0))

    def test_the_indirect_light_is_scaled_only_when_asked(self):
        # A cache that returns a constant: one triangle's radiance seen by every gather ray.
        normals = np.array([[0.0, 0.0, 1.0]] * 2 + [[1.0, 0.0, 0.0]] * 2)
        cache = IndirectCache(np.ones((1, 4, 3)), 8, 0.01, normals, np.zeros(4, dtype=bool))
        plain = self.lit([], 1.0, indirect=cache)
        scaled = self.lit([], 1.0, indirect=cache, ao=ao(indirect=True))
        unscaled = self.lit([], 1.0, indirect=cache, ao=ao(indirect=False))
        self.assertGreater(plain[0], 0.0)
        np.testing.assert_allclose(unscaled, plain)
        self.assertLess(scaled[0], plain[0])


if __name__ == "__main__":
    unittest.main()

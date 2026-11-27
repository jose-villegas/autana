"""Point and spot lights in the bake: the inverse-square falloff and its range window over a plane, a box's hard shadow,
a shadow ray that stops at the light, a spot's cone, the probe term, and a probe sun that bakes as before.

The surface bake runs in float32 on Dr.Jit, so its values match the float64 formulas to float32 rounding."""
import math
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
try:
    import numpy as np
    from tests import soup
    from tests.soup import lamp, sun
    from r3d.light import light
    from r3d.ray_query import RayQuery
    from r3d.sh_probe_asset import COEFFICIENTS
    from r3d.sh_probe_bake import basis, direct_term
except ImportError:
    np = None
from tests.r3d_env import needs_llvm  # noqa: E402

OFFSET = 0.01
FLOAT32 = 1e-6
HEIGHT = 1.0
RANGE = 4.0
FAR = [50.0, 50.0, 50.0]


def floor_points(half=3.0, count=13):
    """A grid of points on the plane y = 0, facing up."""
    axis = np.linspace(-half, half, count)
    points = np.array([[x, 0.0, z] for x in axis for z in axis])
    return points, np.tile([0.0, 1.0, 0.0], (len(points), 1))


def expected(points, normals, position, range_):
    """The point light's falloff written out: cosine times the range window over the squared distance, from the ray
    origin a ray offset above the surface."""
    origin = points + normals * OFFSET
    offset = np.asarray(position) - origin
    distance = np.linalg.norm(offset, axis=1)
    cosine = np.maximum(np.sum(normals * offset, axis=1) / distance, 0.0)
    window = np.clip(1 - (distance / range_) ** 4, 0, 1) ** 2
    return cosine * window / distance ** 2


def under(points, half=2.0):
    """The points below a 4 by 4 sheet centred over the origin."""
    return (np.abs(points[:, 0]) < half) & (np.abs(points[:, 2]) < half)


def box_at(centre, extents):
    part = soup.box(extents)
    part.apply_translation(centre)
    return soup.rays(part)


class SolidFrom:
    """A hit filter: triangles from `first` on are solid, the ones before it are looked through."""

    def __init__(self, first):
        self.first = first

    def device(self, query):
        return lambda prim, where, active: active & (prim >= self.first)


def filtered(parts, solid_from=0):
    """The ray queries of `parts` joined, through a hit filter that looks through the triangles before `solid_from`."""
    joined = soup.concatenate(parts)
    return RayQuery(joined.vertices, joined.faces, hit_filter=SolidFrom(solid_from))


def bake(points, normals, lights, intersector):
    return light(points, normals, np.zeros(len(points), bool), intersector, lights, OFFSET)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class PointLightTests(unittest.TestCase):
    def setUp(self):
        self.points, self.normals = floor_points()
        self.open = box_at(FAR, [1.0, 1.0, 1.0])

    def test_a_point_light_over_a_plane_falls_off_by_inverse_square_to_zero_at_its_range(self):
        got = bake(self.points, self.normals, [lamp([0, HEIGHT, 0], RANGE, color=(1, .5, .25), intensity=2)], self.open)
        want = 2 * expected(self.points, self.normals, [0, HEIGHT, 0], RANGE)
        np.testing.assert_allclose(got, want[:, None] * [1, .5, .25], rtol=FLOAT32, atol=0)
        reach = np.linalg.norm(self.points + self.normals * OFFSET - [0, HEIGHT, 0], axis=1)
        self.assertTrue((got[reach >= RANGE] == 0).all())
        self.assertTrue((got[reach < RANGE] > 0).all())

    def test_a_box_casts_a_hard_shadow_of_zero(self):
        # A box between the light and the floor near x = 1.25: the floor behind it, x of 2.5 and on, gets no light.
        box = box_at([1.25, 0.35, 0.0], [0.5, 0.7, 0.5])
        got = bake(self.points, self.normals, [lamp([0, HEIGHT, 0], RANGE)], box)[:, 0]
        want = expected(self.points, self.normals, [0, HEIGHT, 0], RANGE)
        x, z = self.points[:, 0], self.points[:, 2]
        shadow = (x >= 2.5) & (np.abs(z) < 0.2)
        clear = (x < 0.5) | (np.abs(z) > 1.2)
        self.assertTrue(shadow.any())
        self.assertTrue((want[shadow] > 0).all())
        np.testing.assert_array_equal(got[shadow], 0.0)
        np.testing.assert_allclose(got[clear], want[clear], rtol=FLOAT32)

    def test_a_blocker_beyond_the_light_casts_no_shadow(self):
        above = box_at([0.0, HEIGHT + 0.5, 0.0], [4.0, 0.2, 4.0])
        lights = [lamp([0, HEIGHT, 0], RANGE)]
        np.testing.assert_array_equal(bake(self.points, self.normals, lights, above),
                                      bake(self.points, self.normals, lights, self.open))

    def test_through_a_hit_filter_a_blocker_beyond_the_light_casts_no_shadow(self):
        # The filtered path steps through hits; a solid hit past the light must not shadow it.
        slab = soup.box([4.0, 0.2, 4.0])
        slab.apply_translation([0.0, HEIGHT + 0.5, 0.0])
        lights = [lamp([0, HEIGHT, 0], RANGE)]
        np.testing.assert_array_equal(bake(self.points, self.normals, lights, filtered([slab])),
                                      bake(self.points, self.normals, lights, self.open))

    def test_through_a_hit_filter_a_looked_through_sheet_before_the_light_leaves_the_solid_one_beyond_it_unseen(self):
        sheet = soup.box([4.0, 0.02, 4.0])
        sheet.apply_translation([0.0, HEIGHT / 2, 0.0])
        slab = soup.box([4.0, 0.2, 4.0])
        slab.apply_translation([0.0, HEIGHT + 0.5, 0.0])
        lights = [lamp([0, HEIGHT, 0], RANGE)]
        query = filtered([sheet, slab], solid_from=len(sheet.faces))
        np.testing.assert_array_equal(bake(self.points, self.normals, lights, query),
                                      bake(self.points, self.normals, lights, self.open))
        solid_sheet = filtered([sheet, slab])
        lit = bake(self.points, self.normals, lights, self.open)[:, 0] > 0
        np.testing.assert_array_equal(bake(self.points, self.normals, lights, solid_sheet)[lit & under(self.points)], 0.0)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class SpotLightTests(unittest.TestCase):
    def test_a_spot_is_its_point_light_inside_the_cone_zero_outside_and_a_squared_ramp_between(self):
        inner, outer = 20.0, 30.0
        angles = np.array([0.0, 10.0, 22.0, 25.0, 28.0, 31.0, 45.0])
        # Floor points at those angles from the spot's downward axis, seen from their ray origins.
        x = (HEIGHT - OFFSET) * np.tan(np.radians(angles))
        points = np.column_stack([x, np.zeros_like(x), np.zeros_like(x)])
        normals = np.tile([0.0, 1.0, 0.0], (len(points), 1))
        open_ = box_at(FAR, [1.0, 1.0, 1.0])
        spot = bake(points, normals, [lamp([0, HEIGHT, 0], RANGE, cone=(inner, outer))], open_)[:, 0]
        point = bake(points, normals, [lamp([0, HEIGHT, 0], RANGE)], open_)[:, 0]
        ramp = np.clip((np.cos(np.radians(angles)) - math.cos(math.radians(outer)))
                       / (math.cos(math.radians(inner)) - math.cos(math.radians(outer))), 0, 1) ** 2
        np.testing.assert_allclose(spot, point * ramp, rtol=FLOAT32, atol=0)
        self.assertEqual(list(ramp[:2]), [1.0, 1.0])
        self.assertEqual(list(spot[-2:]), [0.0, 0.0])
        self.assertTrue(((ramp[2:5] > 0) & (ramp[2:5] < 1)).all())

    def test_a_spots_rotation_turns_its_cone(self):
        # Turned 90 degrees about Z, the spot's -Y axis points along +X: a wall facing it is lit, the floor is not.
        spot = lamp([0, HEIGHT, 0], RANGE, cone=(20.0, 30.0), rotation=(0, 0, 90))
        np.testing.assert_allclose(spot["axis"], [1.0, 0.0, 0.0], atol=1e-12)
        open_ = box_at(FAR, [1.0, 1.0, 1.0])
        wall = bake(np.array([[2.0, HEIGHT, 0.0]]), np.array([[-1.0, 0.0, 0.0]]), [spot], open_)
        floor = bake(np.array([[0.0, 0.0, 0.0]]), np.array([[0.0, 1.0, 0.0]]), [spot], open_)
        self.assertGreater(wall[0, 0], 0.0)
        self.assertEqual(floor[0, 0], 0.0)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeSunUnchangedTests(unittest.TestCase):
    """The probe sun formula from before point and spot lights, kept here as the reference."""

    def test_the_probe_sun_term_is_bit_for_bit_as_before(self):
        rng = np.random.default_rng(4)
        points = rng.uniform(-2, 2, (50, 3))
        box = box_at([0.0, 1.0, 0.0], [1.0, 0.5, 1.0])
        light_ = sun([0.3, 1.0, 0.2], (1, .7, .4), 1.7)
        unit = np.array(light_["direction"], dtype=float)
        unit /= np.linalg.norm(unit)
        visible = ~box.blocked(points, np.broadcast_to(unit, points.shape).copy())
        weights = basis(unit[None]).reshape(1, COEFFICIENTS, 1)
        before = math.pi * visible[:, None, None] * weights * (np.asarray(light_["color"]) * light_["intensity"])
        np.testing.assert_array_equal(direct_term(box, points, [light_], OFFSET)[0], before)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeTermTests(unittest.TestCase):
    def test_a_probe_sees_the_point_light_from_its_direction_unless_a_box_stands_between(self):
        box = box_at([2.0, HEIGHT, 0.0], [0.5, 0.5, 0.5])
        probes = np.array([[0.0, 0.0, 0.0], [3.0, HEIGHT, 0.0], [0.0, HEIGHT, 5.0]])
        out, traced = direct_term(box, probes, [lamp([0, HEIGHT, 0], RANGE, color=(1, .5, .25), intensity=2)], OFFSET)
        window = (1 - (HEIGHT / RANGE) ** 4) ** 2
        want = math.pi * 2 * window / HEIGHT ** 2 * basis(np.array([[0.0, 1.0, 0.0]]))[0][:, None] * [1, .5, .25]
        np.testing.assert_allclose(out[0], want, rtol=1e-12)
        np.testing.assert_array_equal(out[1], 0.0)  # behind the box
        np.testing.assert_array_equal(out[2], 0.0)  # past the range: no ray traced
        self.assertEqual(traced, 2)


if __name__ == "__main__":
    unittest.main()

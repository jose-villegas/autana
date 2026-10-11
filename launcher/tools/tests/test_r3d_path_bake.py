"""Checks the bounced light of the bake (r3d/path_bake.py) against a wall whose answer is known, and its settings: the
albedo boost, colour, the sky, one- and two-sided surfaces, chunks of points, groups of points and determinism, and
that the scene's indirect intensity scales the bounce where `light()` sums it."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import mitsuba_reference, path_bake, ray_query
    from r3d.geometry import corner_normals
    from r3d.light import bounced_light, light
    from r3d.mitsuba_reference import ALBEDO_CEILING, boosted_albedo
    from r3d.path_bake import PathLight
    from tests import soup
except ImportError:
    np = None

from tests.r3d_env import HAVE_MITSUBA, needs_mitsuba  # noqa: E402


def have_llvm():
    if not HAVE_MITSUBA:
        return False
    import drjit as dr

    return "llvm_ad_rgb" in mitsuba_reference.import_mitsuba().variants() and dr.has_backend(dr.JitBackend.LLVM)


needs_llvm = unittest.skipIf(not have_llvm(), "the LLVM variant needs libLLVM")

BIG = 400.0
SUN = {"type": "directional", "direction": [0.6, 1.0, 0.0], "color": [1.0, 1.0, 1.0], "intensity": 2.0}
SKY = {"type": "sky", "color": [1.0, 1.0, 1.0], "intensity": 1.5, "rays": 16}


def grey(albedo):
    """The Kd that decodes to `albedo`, as the export reads an untextured material."""
    return (albedo ** (1 / 2.2),) * 3


def corridor(albedo=0.5, wall_faces_floor=True, wall_kd=None):
    """A large floor facing +y at y = 0 and a large wall at x = 0 facing +x (or -x). Material 0 is the floor and
    material 1 the wall; `wall_kd` colours the wall."""
    floor = [[0, 0, BIG], [BIG * 2, 0, BIG], [BIG * 2, 0, -BIG], [0, 0, -BIG]]
    wall = [[0, 0, -BIG], [0, 0, BIG], [0, BIG, BIG], [0, BIG, -BIG]]
    if wall_faces_floor:
        wall = wall[::-1]
    corners, tris, tri_m = [], [], []
    for index, quad in enumerate((floor, wall)):
        base = 4 * index
        corners += quad
        tris += [[base, base + 1, base + 2], [base, base + 2, base + 3]]
        tri_m += [index] * 2
    p, tri_v = np.array(corners, dtype=float), np.array(tris)
    source = SimpleNamespace(colors=None, p=p, uv=np.zeros((8, 2)), tri_v=tri_v, tri_t=tri_v, tri_m=np.array(tri_m),
                             names=["floor", "wall"],
                             materials={"floor": {"Kd": grey(albedo)}, "wall": {"Kd": wall_kd or grey(albedo)}},
                             textures=[None, None])
    source.corner_normals = corner_normals(p, tri_v)
    return source


def floor_only():
    source = corridor()
    keep = source.tri_m == 0
    source.tri_v, source.tri_t, source.tri_m = source.tri_v[keep], source.tri_t[keep], source.tri_m[keep]
    source.corner_normals = corner_normals(source.p, source.tri_v)
    return source


def path_light(source=None, double=(), bounces=1, rays=256, boost=1.0, lights=(SUN,)):
    return PathLight(source or corridor(), list(lights), set(double), SimpleNamespace(bounces=bounces, rays=rays), boost)


def bounce_at(path, xs, ray_offset=0.01):
    points = np.array([[x, 0.0, 0.0] for x in xs])
    return path.bounce(points, np.tile([0.0, 1.0, 0.0], (len(xs), 1)), ray_offset)


def bounce_at_the_wall_foot(**kwargs):
    return bounce_at(path_light(**kwargs), [0.5])[0]


@needs_mitsuba
class BounceTests(unittest.TestCase):
    def test_a_floor_beside_a_sunlit_wall_gets_half_the_walls_radiance(self):
        # The wall covers half of the floor point's cosine-weighted hemisphere, and sends back its albedo times the
        # sun's irradiance over pi: intensity * cos(angle to the wall normal) = 2 * 0.6 / |(0.6, 1, 0)|.
        cos_wall = 0.6 / np.linalg.norm([0.6, 1.0, 0.0])
        np.testing.assert_allclose(bounce_at_the_wall_foot(), [0.5 * 0.5 * 2.0 * cos_wall] * 3, rtol=0.08)

    def test_a_plane_alone_bounces_nothing(self):
        np.testing.assert_array_equal(bounce_at_the_wall_foot(source=floor_only()), [0.0, 0.0, 0.0])

    def test_a_coloured_wall_tints_the_bounce_per_channel(self):
        kd = (0.9, 0.5, 0.15)
        got = bounce_at_the_wall_foot(source=corridor(wall_kd=kd))
        wall_albedo = np.array(kd) ** 2.2
        np.testing.assert_allclose(got / got.sum(), wall_albedo / wall_albedo.sum(), rtol=0.08)
        self.assertGreater(got[0], got[1])
        self.assertGreater(got[1], got[2])

    def test_the_albedo_boost_multiplies_the_reflectance_and_holds_it_below_one(self):
        low, high = bounce_at_the_wall_foot(source=corridor(0.2)), bounce_at_the_wall_foot(source=corridor(0.2), boost=2.0)
        np.testing.assert_allclose(high, low * 2.0, rtol=1e-5)
        capped = bounce_at_the_wall_foot(source=corridor(0.8), boost=2.0)
        np.testing.assert_allclose(capped, bounce_at_the_wall_foot(source=corridor(0.8)) * ALBEDO_CEILING / 0.8, rtol=1e-5)
        self.assertLess(ALBEDO_CEILING, 1.0)
        self.assertEqual(boosted_albedo(np.array([0.5, 0.2, 0.995]), 3.0).tolist(), [ALBEDO_CEILING, 0.6000000000000001, 0.995])

    def test_a_boost_of_one_changes_no_byte(self):
        albedo = np.array([[0.0, 0.3, 0.995], [0.5, 1.0, 0.02]])
        self.assertEqual(boosted_albedo(albedo, 1.0).tobytes(), albedo.tobytes())

    def test_a_wall_seen_from_behind_gives_no_light_unless_it_is_double_sided(self):
        np.testing.assert_array_equal(bounce_at_the_wall_foot(source=corridor(wall_faces_floor=False)), [0.0, 0.0, 0.0])

    def test_a_double_sided_wall_bounces_as_much_from_behind_as_from_the_front(self):
        front = bounce_at_the_wall_foot(source=corridor(), double={"wall"})
        behind = bounce_at_the_wall_foot(source=corridor(wall_faces_floor=False), double={"wall"})
        np.testing.assert_allclose(behind, front, rtol=0.05)
        self.assertGreater(front[0], 0.0)

    def test_the_sky_adds_to_the_bounce_through_the_walls_own_view_of_it_and_a_plane_alone_still_bounces_nothing(self):
        np.testing.assert_array_equal(bounce_at_the_wall_foot(source=floor_only(), lights=[SKY]), [0.0, 0.0, 0.0])
        got = bounce_at_the_wall_foot(lights=[SKY])
        # The wall sends back at most its albedo times the sky's radiance over half the floor point's hemisphere.
        self.assertGreater(got[0], 0.0)
        self.assertLess(got[0], 0.5 * 0.5 * 1.5)

    def test_more_bounces_add_light(self):
        one, two = bounce_at_the_wall_foot(bounces=1), bounce_at_the_wall_foot(bounces=2)
        self.assertGreater(two[0], one[0])

    def test_the_same_scene_gives_the_same_bytes(self):
        self.assertEqual(bounce_at_the_wall_foot(bounces=2).tobytes(), bounce_at_the_wall_foot(bounces=2).tobytes())

    def test_ambient_is_left_to_the_bake(self):
        ambient = {"type": "ambient", "color": [1.0, 1.0, 1.0], "intensity": 1.0}
        np.testing.assert_array_equal(bounce_at_the_wall_foot(lights=[ambient]), [0.0, 0.0, 0.0])

    def test_points_split_over_many_chunks_each_get_their_own_answer(self):
        xs = [0.5, 2.0, 8.0, 30.0, 90.0]
        path = path_light(rays=32)
        alone = np.array([bounce_at(path, [x])[0] for x in xs])
        self.assertGreater(alone[0, 0], alone[-1, 0] * 1.1, "the points must differ for the test to mean anything")
        original = path_bake.BATCH
        path_bake.BATCH = 2 * 32
        try:
            split = bounce_at(path, xs)
        finally:
            path_bake.BATCH = original
        np.testing.assert_allclose(split, alone, rtol=1e-6)

    def test_copies_at_one_position_share_one_gather(self):
        path = path_light(rays=32)
        points = np.array([[0.5, 0.0, 0.0], [0.5, 0.0, 0.0], [100.0, 0.0, 0.0]])
        normals = np.array([[0.0, 1.0, 0.0], [-0.6, 0.8, 0.0], [0.0, 1.0, 0.0]])
        got = bounced_light(path, points, normals, 0.01, np.array([0, 0, 1]))
        self.assertEqual(got[0].tolist(), got[1].tolist())
        mean = np.array([[-0.3, 0.9, 0.0]]) / np.linalg.norm([-0.3, 0.9, 0.0])
        np.testing.assert_allclose(got[0], path.bounce(points[:1], mean, 0.01)[0], rtol=1e-12)


class RoofOverhead:
    """Ray queries without a mesh: a roof over the points blocks every ray that heads up."""

    def blocked(self, origins, directions):
        return directions[:, 1] > 0


class Open:
    def blocked(self, origins, directions):
        return np.zeros(len(origins), bool)


class SideBounce:
    """Bounced light that depends on the side asked for: `up` for normals facing +y, `down` for -y."""

    def __init__(self, up=(0.0, 0.0, 0.0), down=(0.0, 0.0, 0.0)):
        self.up, self.down, self.calls = np.array(up), np.array(down), []

    def bounce(self, points, normals, ray_offset):
        self.calls.append(normals)
        return np.where(normals[:, 1:2] > 0, self.up, self.down)


SUN_UP = {"type": "directional", "direction": [0.0, 1.0, 0.0], "color": [1.0, 1.0, 1.0], "intensity": 1.0}


def lit(points, normals, double, intersector, lights, **kwargs):
    count = len(points)
    return light(np.array(points, float), np.array(normals, float), np.array(double), intersector, lights, 0.01, **kwargs)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class LightPlumbingTests(unittest.TestCase):
    """What `light()` hands to the bounce and which side it keeps, without Mitsuba."""

    UP, DOWN = [0.0, 1.0, 0.0], [0.0, -1.0, 0.0]

    def test_the_indirect_intensity_scales_the_bounce_and_nothing_else(self):
        for intensity in (0.0, 0.5, 3.0):
            got = lit([[0, 0, 0]], [self.UP], [False], Open(), [SUN_UP], bounce=SideBounce(up=(1, 1, 1)),
                      bounce_intensity=intensity)
            np.testing.assert_allclose(got, [[1.0 + intensity] * 3])

    def test_a_double_sided_point_in_shade_takes_the_side_with_more_light_and_a_one_sided_one_keeps_its_own(self):
        both = lit([[0, 0, 0]], [self.UP], [True], RoofOverhead(), [SUN_UP], bounce=SideBounce(down=(0.5, 0.5, 0.5)))
        np.testing.assert_allclose(both, [[0.5] * 3])
        one = lit([[0, 0, 0]], [self.UP], [False], RoofOverhead(), [SUN_UP], bounce=SideBounce(down=(0.5, 0.5, 0.5)))
        np.testing.assert_allclose(one, [[0.0] * 3])

    def test_a_double_sided_card_in_the_sun_keeps_its_sunlit_side(self):
        got = lit([[0, 0, 0]], [self.DOWN], [True], Open(), [SUN_UP], bounce=SideBounce(down=(0.25, 0.25, 0.25)))
        np.testing.assert_allclose(got, [[1.0] * 3])

    def test_in_a_mixed_batch_each_row_keeps_its_own_rule(self):
        got = lit([[0, 0, 0]] * 3, [self.UP] * 3, [False, True, False], RoofOverhead(), [SUN_UP],
                  bounce=SideBounce(down=(0.5, 0.5, 0.5)))
        np.testing.assert_allclose(got, [[0.0] * 3, [0.5] * 3, [0.0] * 3])

    def test_a_mixed_batch_with_groups_does_not_trip_over_the_subset(self):
        got = lit([[0, 0, 0]] * 4, [self.UP] * 4, [True, True, True, False], RoofOverhead(), [SUN_UP],
                  bounce=SideBounce(down=(0.5, 0.5, 0.5)), bounce_groups=np.array([0, 1, 1, 2]))
        np.testing.assert_allclose(got, [[0.5] * 3, [0.5] * 3, [0.5] * 3, [0.0] * 3])

    def test_groups_with_gaps_in_their_ids_gather_once_each(self):
        bounce = SideBounce(up=(1, 1, 1))
        got = bounced_light(bounce, np.zeros((4, 3)), np.tile(self.UP, (4, 1)), 0.01, np.array([7, 7, 3, 9]))
        self.assertEqual(len(bounce.calls[0]), 3)
        np.testing.assert_allclose(got, np.ones((4, 3)))

    def test_a_tie_keeps_the_sun_facing_side_and_a_brighter_side_is_kept_whole(self):
        tie = lit([[0, 0, 0]], [self.UP], [True], Open(), [], bounce=SideBounce(up=(0.5, 0, 0), down=(0, 0.25, 0.25)))
        np.testing.assert_allclose(tie, [[0.5, 0.0, 0.0]])
        brighter = lit([[0, 0, 0]], [self.UP], [True], Open(), [], bounce=SideBounce(up=(0.5, 0, 0), down=(0.1, 0.5, 0.5)))
        np.testing.assert_allclose(brighter, [[0.1, 0.5, 0.5]])

    def test_only_double_sided_points_cost_a_second_gather_on_the_opposite_normal(self):
        single = SideBounce(up=(1, 1, 1))
        lit([[0, 0, 0]], [self.UP], [False], Open(), [SUN_UP], bounce=single)
        self.assertEqual(len(single.calls), 1)
        double = SideBounce(up=(1, 1, 1))
        lit([[0, 0, 0]], [self.UP], [True], Open(), [SUN_UP], bounce=double)
        self.assertEqual(len(double.calls), 2)
        np.testing.assert_allclose(double.calls[1], -double.calls[0])

    def test_a_double_sided_point_facing_away_from_the_sun_is_first_gathered_on_its_sun_facing_side(self):
        for double, want in ((True, self.UP), (False, self.DOWN)):
            recorder = SideBounce()
            lit([[0, 0, 0]], [self.DOWN], [double], Open(), [SUN_UP], bounce=recorder)
            self.assertEqual(recorder.calls[0].tolist(), [want])


@needs_mitsuba
@needs_llvm
class LlvmBounceTests(unittest.TestCase):
    def setUp(self):
        ray_query.VARIANT = "llvm_ad_rgb"

    def tearDown(self):
        ray_query.VARIANT = "scalar_rgb"
        mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")

    def test_a_path_light_traces_on_the_variant_it_is_given_whatever_the_bake_variant(self):
        ray_query.VARIANT = "scalar_rgb"
        scalar = bounce_at_the_wall_foot()
        source = corridor()
        path = PathLight(source, [SUN], set(), SimpleNamespace(bounces=1, rays=256), 1.0, "llvm_ad_rgb")
        self.assertEqual((path.mi.variant(), path.scalar), ("llvm_ad_rgb", False))
        np.testing.assert_allclose(bounce_at(path, [0.5])[0], scalar, rtol=1e-4)

    def test_the_wall_answer_holds_on_the_llvm_variant(self):
        ray_query.VARIANT = "scalar_rgb"
        scalar = bounce_at_the_wall_foot()
        mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")
        ray_query.VARIANT = "llvm_ad_rgb"
        np.testing.assert_allclose(bounce_at_the_wall_foot(), scalar, rtol=1e-4)

    def test_colour_and_point_order_survive_the_llvm_output(self):
        kd = (0.9, 0.5, 0.15)
        path = path_light(source=corridor(wall_kd=kd), rays=32)
        xs = [0.5, 8.0, 90.0]
        alone = np.array([bounce_at(path, [x])[0] for x in xs])
        wall_albedo = np.array(kd) ** 2.2
        np.testing.assert_allclose(alone[0] / alone[0].sum(), wall_albedo / wall_albedo.sum(), rtol=0.08)
        original = path_bake.BATCH
        path_bake.BATCH = 2 * 32
        try:
            split = bounce_at(path, xs)
        finally:
            path_bake.BATCH = original
        np.testing.assert_allclose(split, alone, rtol=1e-4)

    def test_the_same_scene_gives_the_same_bytes_on_llvm(self):
        self.assertEqual(bounce_at_the_wall_foot(bounces=2).tobytes(), bounce_at_the_wall_foot(bounces=2).tobytes())


if __name__ == "__main__":
    unittest.main()

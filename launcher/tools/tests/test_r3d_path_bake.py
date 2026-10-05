"""Checks the bounced light of the bake (r3d/path_bake.py) against a wall whose answer is known, and its settings: the
indirect intensity, the albedo boost, one- and two-sided surfaces, groups of points and determinism."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import mitsuba_reference, path_bake, ray_query
    from r3d.geometry import corner_normals
    from r3d.light import bounced_light
    from r3d.mitsuba_reference import ALBEDO_CEILING, boosted_albedo
    from r3d.path_bake import PathLight
    from tests import soup  # noqa: F401  (traces the bake's rays on the scalar variant)
except ImportError:
    np = None

HAVE_MITSUBA = np is not None and mitsuba_reference.import_mitsuba() is not None
needs_mitsuba = unittest.skipIf(not HAVE_MITSUBA, "the bounced light needs Mitsuba")


def have_llvm():
    if not HAVE_MITSUBA:
        return False
    import drjit as dr

    return "llvm_ad_rgb" in mitsuba_reference.import_mitsuba().variants() and dr.has_backend(dr.JitBackend.LLVM)


needs_llvm = unittest.skipIf(not have_llvm(), "the LLVM variant needs libLLVM")

BIG = 400.0
SUN = {"type": "directional", "direction": [0.6, 1.0, 0.0], "color": [1.0, 1.0, 1.0], "intensity": 2.0}


def grey(albedo):
    """The Kd that decodes to `albedo`, as the export reads an untextured material."""
    return (albedo ** (1 / 2.2),) * 3


def corridor(albedo=0.5, wall_faces_floor=True):
    """A large floor facing +y at y = 0 and a large wall at x = 0 facing +x (or -x). Material 0 is the floor and
    material 1 the wall, both of the same albedo."""
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
    source = SimpleNamespace(p=p, uv=np.zeros((8, 2)), tri_v=tri_v, tri_t=tri_v, tri_m=np.array(tri_m),
                             names=["floor", "wall"], materials={"floor": {"Kd": grey(albedo)}, "wall": {"Kd": grey(albedo)}},
                             textures=[None, None])
    source.corner_normals = corner_normals(p, tri_v)
    return source


def bounce_at_the_wall_foot(source=None, double=(), bounces=1, rays=256, intensity=1.0, boost=1.0, lights=(SUN,)):
    path = PathLight(source or corridor(), list(lights), set(double), SimpleNamespace(bounces=bounces, rays=rays),
                     SimpleNamespace(intensity=intensity, albedo_boost=boost))
    return path.bounce(np.array([[0.5, 0.0, 0.0]]), np.array([[0.0, 1.0, 0.0]]), 0.01)[0]


@needs_mitsuba
class BounceTests(unittest.TestCase):
    def test_a_floor_beside_a_sunlit_wall_gets_half_the_walls_radiance(self):
        # The wall covers half of the floor point's cosine-weighted hemisphere, and sends back its albedo times the
        # sun's irradiance over pi: intensity * cos(angle to the wall normal) = 2 * 0.6 / |(0.6, 1, 0)|.
        cos_wall = 0.6 / np.linalg.norm([0.6, 1.0, 0.0])
        got = bounce_at_the_wall_foot()
        np.testing.assert_allclose(got, [0.5 * 0.5 * 2.0 * cos_wall] * 3, rtol=0.08)

    def test_a_plane_alone_bounces_nothing(self):
        source = corridor()
        keep = source.tri_m == 0
        source.tri_v, source.tri_t, source.tri_m = source.tri_v[keep], source.tri_t[keep], source.tri_m[keep]
        source.corner_normals = corner_normals(source.p, source.tri_v)
        np.testing.assert_array_equal(bounce_at_the_wall_foot(source), [0.0, 0.0, 0.0])

    def test_the_intensity_scales_the_bounce_linearly(self):
        base = bounce_at_the_wall_foot()
        for intensity in (0.0, 0.5, 3.0):
            np.testing.assert_allclose(bounce_at_the_wall_foot(intensity=intensity), base * intensity, rtol=1e-9, atol=0.0)

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
        behind = corridor(wall_faces_floor=False)
        np.testing.assert_array_equal(bounce_at_the_wall_foot(behind), [0.0, 0.0, 0.0])
        self.assertGreater(bounce_at_the_wall_foot(corridor(wall_faces_floor=False), double={"wall"})[0], 0.0)

    def test_more_bounces_add_light(self):
        one, two = bounce_at_the_wall_foot(bounces=1), bounce_at_the_wall_foot(bounces=2)
        self.assertGreater(two[0], one[0])

    def test_the_same_scene_gives_the_same_bytes(self):
        self.assertEqual(bounce_at_the_wall_foot(bounces=2).tobytes(), bounce_at_the_wall_foot(bounces=2).tobytes())

    def test_copies_at_one_position_share_one_gather(self):
        path = PathLight(corridor(), [SUN], set(), SimpleNamespace(bounces=1, rays=32),
                         SimpleNamespace(intensity=1.0, albedo_boost=1.0))
        points = np.array([[0.5, 0.0, 0.0], [0.5, 0.0, 0.0], [100.0, 0.0, 0.0]])
        normals = np.array([[0.0, 1.0, 0.0], [-0.6, 0.8, 0.0], [0.0, 1.0, 0.0]])
        got = bounced_light(path, points, normals, 0.01, np.array([0, 0, 1]))
        self.assertEqual(got[0].tolist(), got[1].tolist())
        mean = np.array([[-0.3, 0.9, 0.0]]) / np.linalg.norm([-0.3, 0.9, 0.0])
        np.testing.assert_allclose(got[0], path.bounce(points[:1], mean, 0.01)[0], rtol=1e-12)

    def test_ambient_is_left_to_the_bake(self):
        ambient = {"type": "ambient", "color": [1.0, 1.0, 1.0], "intensity": 1.0}
        np.testing.assert_array_equal(bounce_at_the_wall_foot(lights=[ambient]), [0.0, 0.0, 0.0])


@needs_mitsuba
@needs_llvm
class LlvmBounceTests(unittest.TestCase):
    def test_the_wall_answer_holds_on_the_llvm_variant(self):
        scalar = bounce_at_the_wall_foot()
        ray_query.VARIANT = "llvm_ad_rgb"
        try:
            got = bounce_at_the_wall_foot()
        finally:
            ray_query.VARIANT = "scalar_rgb"
            mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")
        np.testing.assert_allclose(got, scalar, rtol=1e-4)

    def test_batches_give_the_unbatched_answer(self):
        ray_query.VARIANT = "llvm_ad_rgb"
        try:
            whole = bounce_at_the_wall_foot()
            original = path_bake.BATCH
            path_bake.BATCH = 100
            try:
                split = bounce_at_the_wall_foot()
            finally:
                path_bake.BATCH = original
        finally:
            ray_query.VARIANT = "scalar_rgb"
            mitsuba_reference.import_mitsuba().set_variant("scalar_rgb")
        np.testing.assert_allclose(split, whole, rtol=1e-3)


if __name__ == "__main__":
    unittest.main()

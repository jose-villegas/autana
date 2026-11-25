"""Checks the Dr.Jit ray queries and baked light on what a launch may not change: a mixed launch answers each lane as
if it flew alone, a launch split into small chunks gives the same bytes, empty and all-masked launches run, and a hit
filter or surface table built from one source never answers for another."""

import pathlib
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from tests import soup
    from tests.test_r3d_alpha_cutout import CARD_Z, EYE_Z, KEEP_ALPHA, PLANE_Z, SOLID, checker_rays, checker_source
    from tests.test_r3d_reference import LOOK_DOWN, plane_source, square, sun_scene

    from r3d import light, ray_query
    from r3d.geometry import corner_normals
    from r3d.import_settings import LIGHT_MODE_BAKED
    from r3d.mesh_source import with_triangles
    from r3d.mitsuba_reference import import_mitsuba
    from r3d.reference_render import trace
except ImportError:
    np = None

from tests.r3d_env import needs_llvm  # noqa: E402

# A triangle number far enough past any mesh that reading it takes the process down.
FAR_PRIM = 10 ** 8
SKY_RAYS, AO_RAYS, BOUNCE_RAYS = 6, 5, 4


def sky(rays=SKY_RAYS):
    return {"type": "sky", "mode": LIGHT_MODE_BAKED, "color": [0.5, 0.7, 1.0], "intensity": 1.5, "rays": rays}


def ao(distance=6.0, rays=AO_RAYS, strength=0.8, indirect=False):
    return SimpleNamespace(distance=distance, rays=rays, strength=strength, indirect=indirect)


def wall_scene():
    """A floor facing +y at y = 0 and a tall wall box across x = 0, as a ray query."""
    floor = soup.Soup([[-20, 0, -20], [20, 0, -20], [20, 0, 20], [-20, 0, 20]], [[0, 2, 1], [0, 3, 2]])
    wall = soup.box((0.2, 10.0, 20.0))
    wall.apply_translation((0.0, 5.0, 0.0))
    return soup.rays(soup.concatenate([floor, wall]))


def points_and_normals():
    """Lit on the floor, in the wall's shadow, and facing away from the sun, in this order."""
    points = np.array([[5.0, 0.0, 0.0], [-1.0, 0.0, 0.0], [6.0, 0.0, 3.0], [9.0, 0.0, -2.0]])
    up, down = [0.0, 1.0, 0.0], [0.0, -1.0, 0.0]
    return points, np.array([up, up, down, up])


def lit_each_alone(query, points, normals, lights, **settings):
    return np.array([light.light(points[i:i + 1], normals[i:i + 1], [False], query, lights, 0.01, **settings)[0]
                     for i in range(len(points))])


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class MixedLaunchTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.source = checker_source(directory.name)
        self.query = ray_query.RayQuery(self.source.p, self.source.tri_v,
                                        hit_filter=light.AlphaTest(self.source, KEEP_ALPHA))

    def test_a_filtered_launch_of_hits_and_misses_answers_each_ray(self):
        origins, directions = checker_rays()
        far = np.array([[9.0, 0.0, EYE_Z], [-9.0, 0.5, EYE_Z]])
        origins = np.concatenate([origins, far])
        directions = np.concatenate([directions, np.tile([0.0, 0.0, -1.0], (2, 1))])
        hit, distance, tri = self.query.first_hits(origins, directions)
        np.testing.assert_array_equal(hit, [True] * 4 + [False] * 2)
        np.testing.assert_array_equal(tri[:4] < 2, SOLID)
        np.testing.assert_allclose(distance[:4], EYE_Z - np.where(SOLID, CARD_Z, PLANE_Z), atol=1e-6)
        np.testing.assert_array_equal(self.query.blocked(origins, directions), hit)
        firsts = {}
        for tris, alive, _ in self.query.hit_rounds(origins, directions):
            for triangle, ray in zip(tris, alive):
                firsts.setdefault(int(ray), int(triangle))
        self.assertEqual(firsts, dict(enumerate(tri[:4].tolist())))
        where, rays, _ = self.query.first_hit(origins, directions)
        np.testing.assert_array_equal(rays, np.arange(4))
        np.testing.assert_allclose(where[:, 2], np.where(SOLID, CARD_Z, PLANE_Z), atol=1e-6)

    def test_corners_and_barycentrics_read_nothing_for_inactive_lanes(self):
        mi, dr = self.query.mi, self.query.dr
        prim = mi.UInt32([10 ** 6, 10 ** 6, 1])
        where = mi.Point3f([0.0, 0.0, 0.5], [0.0, 0.0, 0.25], [0.0, 0.0, CARD_Z])
        inactive = dr.zeros(mi.Bool, 3)
        corners = self.query.corners(prim, inactive)
        bary = self.query.barycentrics(prim, where, inactive)
        dr.eval(corners, bary)
        self.assertEqual(dr.width(corners[0]), 3)
        mixed = mi.Bool([False, False, True])
        corners = self.query.corners(prim, mixed)
        bary = self.query.barycentrics(prim, where, mixed)
        dr.eval(corners, bary)
        triangle = self.source.p[self.source.tri_v[1]]
        for corner in range(3):
            np.testing.assert_array_equal(np.array(corners[corner], np.float64).reshape(3, -1)[:, 2], triangle[corner])
        weights = np.array([float(np.array(weight).reshape(-1)[2]) for weight in bary])
        np.testing.assert_allclose(weights.sum(), 1.0, atol=1e-6)
        np.testing.assert_allclose(weights @ triangle, [0.5, 0.25, CARD_Z], atol=1e-6)

    def test_a_missed_rays_triangle_number_is_never_read_even_far_past_the_mesh(self):
        # A miss reports a triangle number far outside the mesh. Reading through it can take the process down, so the
        # call runs in its own process, where a crash is a failed test.
        tools = pathlib.Path(__file__).resolve().parents[1]
        script = "\n".join([
            f"import sys; sys.path.insert(0, {str(tools)!r})",
            "from tests import soup",
            "q = soup.rays(soup.box((1.0, 1.0, 1.0)))",
            "mi, dr = q.mi, q.dr",
            f"prim = dr.full(mi.UInt32, {FAR_PRIM}, 3)",
            "off = dr.zeros(mi.Bool, 3)",
            "dr.eval(q.corners(prim, off), q.barycentrics(prim, mi.Point3f(0.0, 0.0, 0.0), off))",
            "print('read nothing')"])
        done = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, check=False)
        self.assertEqual((done.returncode, done.stdout.strip().splitlines()[-1:]), (0, ["read nothing"]), done.stderr)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class LightLaunchTests(unittest.TestCase):
    def setUp(self):
        self.query = wall_scene()
        self.points, self.normals = points_and_normals()
        self.double = [False] * len(self.points)

    def check_each_alone(self, lights, **settings):
        together = light.light(self.points, self.normals, self.double, self.query, lights, 0.01, **settings)
        alone = lit_each_alone(self.query, self.points, self.normals, lights, **settings)
        np.testing.assert_array_equal(together, alone)
        return together

    def test_each_point_of_a_mixed_sun_launch_gets_what_it_gets_alone(self):
        got = self.check_each_alone([soup.sun((1.0, 1.0, 0.0), intensity=2.0)])
        self.assertGreater(got[0, 0], 0.5, "the open floor is lit")
        np.testing.assert_array_equal(got[1:3], 0.0)  # in the wall's shadow, and facing away
        self.assertGreater(got[3, 0], 0.5, "the far floor is lit again")

    def test_each_point_of_a_mixed_sky_and_ao_launch_gets_what_it_gets_alone(self):
        lights = [soup.sun((1.0, 1.0, 0.0)), sky()]
        got = self.check_each_alone(lights, ao=ao())
        self.assertTrue((got > 0).all(axis=1).any())
        self.assertNotEqual(got[0, 0], got[1, 0], "the points must differ for the test to mean anything")
        self.check_each_alone(lights, ao=ao(indirect=True))

    def test_a_launch_of_one_point_s_rays_and_an_odd_one_leave_the_bytes_alone(self):
        source = soup.corridor()
        path = soup.path_light(source, rays=BOUNCE_RAYS)
        scene = soup.rays(soup.Soup(source.p, source.tri_v))
        points, normals = soup.floor_points([0.5, 1.5, 3.0, 8.0, 20.0, 45.0, 90.0])
        lights = [soup.SUN, sky(), {"type": "ambient", "mode": LIGHT_MODE_BAKED, "color": [1, 1, 1],
                                    "intensity": 0.3}]

        def lit():
            return light.light(points, normals, [False] * len(points), scene, lights, 0.01, bounce=path, ao=ao(),
                               bounce_intensity=0.7)

        whole = lit()
        direct = light.light(points, normals, [False] * len(points), scene, lights, 0.01, ao=ao())
        self.assertTrue((whole > direct).all(), "the bounce must add light for the test to cover it")
        self.assertGreater(np.ptp(whole[:, 0]), 0.0)
        # A launch of SKY_RAYS holds one point of each; of 11 rays a chunk holds 1 sky point, 2 AO and bounce points:
        # no chunk edge falls twice where another does.
        for launch in (SKY_RAYS, 11):
            with self.subTest(launch=launch), patch.object(ray_query, "LAUNCH", launch):
                self.assertEqual(lit().tobytes(), whole.tobytes())
        with patch.object(ray_query, "LAUNCH", SKY_RAYS - 1), self.assertRaisesRegex(ValueError, "exceed one launch"):
            lit()


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class TraceLaunchTests(unittest.TestCase):
    def test_a_trace_split_into_tiny_launches_has_the_bytes_of_the_default(self):
        # A shelf over half the view: the sun leaves a strip of floor beside it in shadow.
        cover = [[-2.0, -2.0, 0.5], [0.0, -2.0, 0.5], [0.0, 2.0, 0.5], [-2.0, 2.0, 0.5]]
        source = plane_source(square(), cover)
        job, scene = sun_scene([1.0, 0.0, 1.0])
        job.bake.ao = ao(distance=1.0, rays=3)
        scene.lights.append(sky(4))

        def pictures():
            return [part.tobytes() for part in trace(source, job, scene, LOOK_DOWN, 3, 3, 1.0, 2)]

        whole = pictures()
        linear = trace(source, job, scene, LOOK_DOWN, 3, 3, 1.0, 2)[0]
        self.assertGreater(np.ptp(linear), 0.0, "the pixels must differ for the test to mean anything")
        # 4 rays per point is the sky's, the most of any light here.
        for launch in (5, 4):
            with self.subTest(launch=launch), patch.object(ray_query, "LAUNCH", launch):
                self.assertEqual(pictures(), whole)

    def test_a_trace_launches_at_most_launch_camera_rays_at_a_time(self):
        from r3d import reference_render
        source = plane_source(square())
        job, scene = sun_scene([0.0, 0.0, 1.0])
        widths = []
        drawn = reference_render.device_drawn_hits

        def counted(source, double_materials, origin, direction):
            widths.append(len(origin))
            return drawn(source, double_materials, origin, direction)

        with patch.object(ray_query, "LAUNCH", 5), patch.object(reference_render, "device_drawn_hits", counted):
            trace(source, job, scene, LOOK_DOWN, 3, 3, 1.0, 2)
        self.assertEqual(sum(widths), 3 * 3 * 2 * 2)
        self.assertLessEqual(max(widths), 5)


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class EmptyInputTests(unittest.TestCase):
    def test_no_points_no_hits_and_no_rays_give_empty_answers(self):
        from r3d import reference_render
        query = wall_scene()
        lights = sun_scene([0.0, 1.0, 1.0])[1].lights + [sky()]
        self.assertEqual(light.light(np.zeros((0, 3)), np.zeros((0, 3)), [], query, lights, 0.01, ao=ao()).shape,
                         (0, 3))
        source = plane_source(square())
        self.assertEqual(reference_render.hit_albedo(source, np.zeros(0, int), np.zeros((0, 3))).shape, (0, 3))
        found = reference_render.drawn_hit_rows(source, [], np.zeros((0, 3)), np.zeros((0, 3)))
        self.assertEqual((len(found.rays), found.where.shape), (0, (0, 3)))
        self.assertEqual(len(query.first_hits(np.zeros((0, 3)), np.zeros((0, 3)))[0]), 0)


def table_arrays(source, mi):
    """The surface table's per-triangle material, first-corner UV and the albedo of triangle 0, as NumPy."""
    table = light.surface_table(source, mi)
    one = mi.UInt32(0)
    albedo = table.albedo(one, [mi.Float(1.0), mi.Float(0.0), mi.Float(0.0)], mi.Bool(True))
    return (np.array(table.tri_m, np.int64), np.array(table.uv[0].x, np.float64),
            np.array(albedo, np.float64).reshape(-1))


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class SurfaceTableTests(unittest.TestCase):
    def setUp(self):
        self.mi = import_mitsuba()
        self.mi.set_variant(ray_query.VARIANT)

    def source(self):
        source = plane_source(square(), square(1.0))
        source.uv = np.arange(len(source.p) * 2, dtype=float).reshape(-1, 2) / 16
        source.names, source.textures = ["red", "blue"], [None, None]
        source.materials = {"red": {"Kd": (1.0, 0.0, 0.0)}, "blue": {"Kd": (0.0, 0.0, 1.0)}}
        source.tri_m = np.array([0, 0, 1, 1])
        return source

    def reversed_copy(self, source):
        order = np.arange(len(source.tri_v))[::-1]
        return with_triangles(source, source.tri_v[order], source.tri_t[order], source.tri_m[order],
                              corner_normals=corner_normals(source.p, source.tri_v[order]))

    def expected(self, source):
        red = np.array([1.0, 0.0, 0.0]) ** light.LIGHT_GAMMA
        blue = np.array([0.0, 0.0, 1.0]) ** light.LIGHT_GAMMA
        return (np.asarray(source.tri_m), source.uv[source.tri_t[:, 0], 0],
                (red, blue)[int(source.tri_m[0])])

    def check(self, source):
        tri_m, uv, albedo = table_arrays(source, self.mi)
        want_m, want_uv, want_albedo = self.expected(source)
        np.testing.assert_array_equal(tri_m, want_m)
        np.testing.assert_allclose(uv, want_uv, atol=1e-6)
        np.testing.assert_allclose(albedo, want_albedo, atol=1e-6)
        self.assertIs(light.surface_table(source, self.mi).source, source)

    def test_a_copy_answers_from_its_own_triangles_and_the_original_keeps_its_own(self):
        for copy_first in (False, True):
            with self.subTest(copy_first=copy_first):
                source = self.source()
                original_first = None
                if not copy_first:
                    self.check(source)
                    original_first = light.surface_table(source, self.mi)
                duplicate = self.reversed_copy(source)
                self.check(duplicate)
                self.check(source)
                self.assertFalse(np.array_equal(self.expected(source)[0], self.expected(duplicate)[0]))
                if original_first is not None:
                    self.assertIs(light.surface_table(source, self.mi), original_first)
                self.assertIsNot(light.surface_table(source, self.mi), light.surface_table(duplicate, self.mi))


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class HiddenFilterTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.source = checker_source(directory.name)
        self.origins, self.directions = checker_rays()

    def on_card(self, hit_filter):
        query = ray_query.RayQuery(self.source.p, self.source.tri_v, hit_filter=hit_filter)
        hit, _, tri = query.first_hits(self.origins, self.directions)
        np.testing.assert_array_equal(hit, True)
        return tri < 2

    def test_hidden_triangles_pass_alone_and_beside_the_alpha_test_on_one_source(self):
        # The rays cross the card on triangle 1, but the one at (0.7, 0.3) on triangle 0; the plane is 2 and 3.
        hidden = np.array([False, True, False, False])
        hidden_only = light.AlphaTest(self.source, None, hidden)
        alpha_only = light.AlphaTest(self.source, KEEP_ALPHA)
        both = light.AlphaTest(self.source, KEEP_ALPHA, hidden)
        np.testing.assert_array_equal(self.on_card(hidden_only), [False, True, False, False])
        np.testing.assert_array_equal(self.on_card(alpha_only), SOLID)
        np.testing.assert_array_equal(self.on_card(both), [False] * 4)
        np.testing.assert_array_equal(self.on_card(hidden_only), [False, True, False, False])

    def test_a_filter_with_nothing_to_hide_or_cut_has_no_device_test(self):
        query = ray_query.RayQuery(self.source.p, self.source.tri_v)
        nothing = np.zeros(len(self.source.tri_v), bool)
        self.assertIsNone(light.AlphaTest(self.source, None, nothing).device(query))
        self.assertIsNone(light.AlphaTest(self.source, None).device(query))
        self.assertIsNotNone(light.AlphaTest(self.source, KEEP_ALPHA, nothing).device(query))
        query = ray_query.RayQuery(self.source.p, self.source.tri_v, hit_filter=light.AlphaTest(self.source, None,
                                                                                               nothing))
        np.testing.assert_array_equal(query.first_hits(self.origins, self.directions)[2] < 2, True)


if __name__ == "__main__":
    unittest.main()

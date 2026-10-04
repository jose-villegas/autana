"""Checks the path-traced reference backend against the Embree reference on small fixtures: direct-only parity,
materials, texture orientation and wrap, cameras, seeds and depth, and the energy of a constant environment."""

import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

try:
    import numpy as np
    import trimesh
    from PIL import Image
    from trimesh.ray.ray_pyembree import RayMeshIntersector

    from r3d import mitsuba_reference
    from r3d.geometry import corner_normals
    from r3d.obj import Texture
    from r3d.reference_render import render_linear
except ImportError:
    np = None

HAVE_MITSUBA = np is not None and mitsuba_reference.import_mitsuba() is not None
needs_mitsuba = unittest.skipIf(not HAVE_MITSUBA, "the path-traced backend needs requirements-gpu.txt")

LOOK_DOWN = None if np is None else np.array([0.0, 0.0, 3.0, 0.0, 0.0, -1.0])
LOOK_UP = None if np is None else np.array([0.0, 0.0, -3.0, 0.0, 0.0, 1.0])
CORRIDOR_POSE = None if np is None else np.array([0.0, 0.0, 0.5, 1.0, 0.0, 0.0001])
VARIANT = "scalar_rgb"
NEAR = 0.01


def quad(z, half, x0=0.0, y0=0.0):
    return [[x0 - half, y0 - half, z], [x0 + half, y0 - half, z], [x0 + half, y0 + half, z], [x0 - half, y0 + half, z]]


def make_source(quads, kd=(1.0, 1.0, 1.0), texture=None, materials=None, uv_scale=1.0):
    """A two-triangle quad per entry of `quads`, facing +z with UVs 0..uv_scale over its corners. `materials` gives each
    quad's material index, with `kd` then a list of colours (default: one material)."""
    materials = materials or [0] * len(quads)
    colours = kd if isinstance(kd, list) else [kd]
    corners, uvs, tris, tri_m = [], [], [], []
    for index, corner in enumerate(quads):
        base = 4 * index
        corners += corner
        uvs += [[0.0, 0.0], [uv_scale, 0.0], [uv_scale, uv_scale], [0.0, uv_scale]]
        tris += [[base, base + 1, base + 2], [base, base + 2, base + 3]]
        tri_m += [materials[index]] * 2
    names = [f"material{i}" for i in range(len(colours))]
    source = SimpleNamespace(p=np.array(corners, dtype=float), uv=np.array(uvs), tri_v=np.array(tris), tri_t=np.array(tris),
                             tri_m=np.array(tri_m), names=names, materials={n: {"Kd": c} for n, c in zip(names, colours)},
                             textures=[texture] + [None] * (len(colours) - 1))
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.indirect_cache = None
    source.intersector = RayMeshIntersector(trimesh.Trimesh(source.p, source.tri_v, process=False))
    return source


def sun(direction):
    return {"type": "directional", "direction": direction, "color": [1.0, 1.0, 1.0], "intensity": 1.0, "disc_degrees": 0.0,
            "rays": 1}


def sky(intensity=1.0):
    return {"type": "sky", "color": [1.0, 1.0, 1.0], "intensity": intensity, "rays": 16}


def embree(source, lights, pose=LOOK_DOWN, width=16, height=16, lens=0.5, double=()):
    settings = SimpleNamespace(double_sided=set(double), seed=1)
    job = SimpleNamespace(settings=settings, bake=SimpleNamespace(ray_offset=0.001, ao=None))
    return render_linear(source, job, SimpleNamespace(lights=lights), pose, width, height, lens, 4)


def mitsuba(source, lights, pose=LOOK_DOWN, width=16, height=16, lens=0.5, spp=64, depth=2, seed=1, double=(),
            sky_settings=None, near=NEAR):
    """The path-traced linear image and coverage of a fixture; the source's textures are consumed."""
    path = mitsuba_reference.prepare(source, lights, set(double), sky_settings, VARIANT)
    return path.trace(pose, width, height, lens, near, spp, seed, depth)


def corridor(albedo=0.8):
    """A floor and a ceiling facing each other: light that reaches the floor bounces between them."""
    return make_source([quad(0.0, 8.0), quad(1.0, 8.0)[::-1]], kd=(albedo ** (1 / 2.2),) * 3)


def corridor_path(**options):
    return mitsuba_reference.prepare(corridor(), [sky()], set(), None, VARIANT, **options)


def corridor_trace(path, spp, seed, depth=12):
    return path.trace(CORRIDOR_POSE, 8, 8, 0.3, NEAR, spp, seed, depth)[0]


@needs_mitsuba
class MitsubaDirectParityTest(unittest.TestCase):
    def test_a_shadowed_plane_matches_the_embree_reference_pixel_for_pixel(self):
        # An occluder card above a ground plane, and a sun tilted so its shadow falls inside the view.
        lights = [sun([0.5, 0.2, 0.8])]
        a = embree(make_source([quad(0.0, 4.0), quad(1.0, 0.8, 0.0, 0.0)]), lights)
        b = mitsuba(make_source([quad(0.0, 4.0), quad(1.0, 0.8, 0.0, 0.0)]), lights)[0]
        self.assertGreater(a.max() - a.min(), 0.1, "the fixture needs a shadow to compare")
        self.assertGreaterEqual((np.abs(a - b).max(axis=2) < 0.02).mean(), 0.9)
        self.assertLess(np.abs(a - b).mean(), 0.01)

    def test_the_camera_looks_where_the_embree_reference_looks(self):
        # A card covering only the upper-left of the view: both backends must cover the same pixels.
        lights = [sun([0.0, 0.0, 1.0])]
        covered_a = embree(make_source([quad(0.0, 0.5, -0.6, 0.6)]), lights).max(axis=2) > 0.5
        covered_b = mitsuba(make_source([quad(0.0, 0.5, -0.6, 0.6)]), lights)[0].max(axis=2) > 0.5
        self.assertTrue(covered_a[:4, :4].any() and not covered_a[8:, :].any())
        self.assertGreaterEqual((covered_a == covered_b).mean(), 0.97)

    def test_lens_is_the_half_field_of_view_along_the_shorter_side_in_wide_and_tall_frames(self):
        lights = [sun([0.0, 0.0, 1.0])]
        for width, height in ((24, 8), (8, 24)):
            a = embree(make_source([quad(0.0, 1.0)]), lights, width=width, height=height, lens=0.4)
            b = mitsuba(make_source([quad(0.0, 1.0)]), lights, width=width, height=height, lens=0.4)[0]
            self.assertGreaterEqual(((a.max(axis=2) > 0.5) == (b.max(axis=2) > 0.5)).mean(), 0.97, (width, height))
            self.assertTrue((a.max(axis=2) > 0.5).any() and not (a.max(axis=2) > 0.5).all(), "the card must end inside the frame")

    def test_the_near_plane_clips_what_is_closer(self):
        lights = [sun([0.0, 0.0, 1.0])]
        self.assertEqual(mitsuba(make_source([quad(0.0, 4.0)]), lights, width=4, height=4, near=0.01)[1].min(), 1.0)
        self.assertEqual(mitsuba(make_source([quad(0.0, 4.0)]), lights, width=4, height=4, near=10.0)[1].max(), 0.0)

    def test_pixels_the_mesh_does_not_cover_have_zero_coverage(self):
        _, coverage = mitsuba(make_source([quad(0.0, 0.5)]), [sun([0.0, 0.0, 1.0])], width=8, height=8, spp=16)
        self.assertEqual(coverage[0, 0], 0.0)
        self.assertEqual(coverage[4, 4], 1.0)


@needs_mitsuba
class MitsubaMaterialTest(unittest.TestCase):
    @staticmethod
    def blocks():
        # Four distinct blocks, so a flipped or transposed texture changes the picture. Image row 0 is the top of the
        # texture and v = 1; the repo's sampler reads row (1 - v) * height.
        colours = {(0, 0): (255, 0, 0), (0, 1): (0, 255, 0), (1, 0): (0, 0, 255), (1, 1): (255, 255, 0)}
        pixels = np.zeros((8, 8, 3), dtype=np.uint8)
        for (row, column), colour in colours.items():
            pixels[row * 4 : row * 4 + 4, column * 4 : column * 4 + 4] = colour
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "blocks.png"
            Image.fromarray(pixels).save(path)
            return Texture(path)

    def test_texels_land_where_the_embree_sampler_puts_them(self):
        lights = [sun([0.0, 0.0, 1.0])]
        a = embree(make_source([quad(0.0, 1.0)], texture=self.blocks()), lights, width=8, height=8)
        b = mitsuba(make_source([quad(0.0, 1.0)], texture=self.blocks()), lights, width=8, height=8, lens=0.4)[0]
        centres = [(2, 2), (2, 5), (5, 2), (5, 5)]
        for y, x in centres:
            np.testing.assert_allclose(b[y, x], a[y, x], atol=0.02)
        self.assertEqual(len({tuple(np.round(b[y, x], 2)) for y, x in centres}), 4)

    def test_kd_scales_a_texture_per_channel_as_the_embree_albedo_does(self):
        lights = [sun([0.0, 0.0, 1.0])]
        kd = (0.9, 0.5, 0.2)
        a = embree(make_source([quad(0.0, 1.0)], kd=kd, texture=self.blocks()), lights, width=8, height=8)
        b = mitsuba(make_source([quad(0.0, 1.0)], kd=kd, texture=self.blocks()), lights, width=8, height=8, lens=0.4)[0]
        np.testing.assert_allclose(b[2, 2], a[2, 2], atol=0.02)
        self.assertLess(b[2, 2, 0], 0.95)

    def test_an_untextured_colour_is_decoded_with_power_2_2(self):
        source = make_source([quad(0.0, 1.0)], kd=(0.5, 0.25, 1.0))
        picture = mitsuba(source, [sun([0.0, 0.0, 1.0])], width=4, height=4, lens=0.4)[0]
        np.testing.assert_allclose(picture[2, 2], np.array([0.5, 0.25, 1.0]) ** 2.2, rtol=1e-4)

    def test_texture_coordinates_beyond_one_repeat_the_texture(self):
        lights = [sun([0.0, 0.0, 1.0])]
        a = embree(make_source([quad(0.0, 1.0)], texture=self.blocks(), uv_scale=2.0), lights, width=32, height=32, lens=0.4)
        b = mitsuba(make_source([quad(0.0, 1.0)], texture=self.blocks(), uv_scale=2.0), lights, width=32, height=32, lens=0.4)[0]
        # Away from the blocks' edges, where only the wrap decides the colour.
        flat = np.ones((32, 32), dtype=bool)
        for shift, axis in ((1, 0), (-1, 0), (1, 1), (-1, 1)):
            flat &= np.abs(a - np.roll(a, shift, axis)).max(axis=2) < 1e-6
        flat[:2] = flat[-2:] = False
        flat[:, :2] = flat[:, -2:] = False
        self.assertGreater(flat.sum(), 40)
        self.assertGreaterEqual((np.abs(a - b).max(axis=2)[flat] < 0.05).mean(), 0.95)
        self.assertGreater(len({tuple(np.round(p, 2)) for p in a[flat]}), 3)

    def test_each_triangle_keeps_its_own_materials_colour(self):
        quads = [quad(0.0, 1.0, -1.0), quad(0.0, 1.0, 1.0)]
        kd = [(1.0, 0.0, 0.0), (0.0, 0.0, 1.0)]
        picture = mitsuba(make_source(quads, kd=kd, materials=[0, 1]), [sun([0.0, 0.0, 1.0])], lens=0.8)[0]
        left, right = picture[8, 3], picture[8, 12]
        self.assertGreater(left[0], 0.9)
        self.assertLess(left[2], 0.05)
        self.assertGreater(right[2], 0.9)
        self.assertLess(right[0], 0.05)

    def test_the_export_takes_the_sources_textures(self):
        source = make_source([quad(0.0, 1.0)], texture=self.blocks())
        mitsuba(source, [sun([0.0, 0.0, 1.0])], width=4, height=4)
        self.assertEqual(source.textures, [None])

    def test_a_declared_double_sided_card_matches_embree_and_an_undeclared_one_is_dark_from_behind(self):
        # The sun and the viewer are both behind the card.
        lights = [sun([0.0, 0.0, -1.0])]
        for double, lit in (((), False), (("material0",), True)):
            a = embree(make_source([quad(0.0, 2.0)]), lights, pose=LOOK_UP, width=4, height=4, double=double)
            b = mitsuba(make_source([quad(0.0, 2.0)]), lights, pose=LOOK_UP, width=4, height=4, double=double)[0]
            self.assertEqual(a.max() > 0.5, lit, double)
            np.testing.assert_allclose(b, a, atol=0.02)

    def test_a_card_seen_from_behind_under_a_sun_on_its_front_is_lit_in_embree_only(self):
        # The recorded difference: the Embree reference shades a back-face hit with the front normal, and a double-sided
        # card turns toward the sun. Mitsuba shades the side the ray reached.
        lights = [sun([0.0, 0.0, 1.0])]
        for double in ((), ("material0",)):
            a = embree(make_source([quad(0.0, 2.0)]), lights, pose=LOOK_UP, width=4, height=4, double=double)
            b = mitsuba(make_source([quad(0.0, 2.0)]), lights, pose=LOOK_UP, width=4, height=4, double=double)[0]
            self.assertGreater(a.max(), 0.5)
            self.assertEqual(b.max(), 0.0)


@needs_mitsuba
class MitsubaTransportTest(unittest.TestCase):
    def test_a_plane_under_a_constant_sky_returns_albedo_times_radiance_at_any_depth(self):
        for albedo in (1.0, 0.5):
            for depth in (2, 12):
                source = make_source([quad(0.0, 8.0)], kd=(albedo ** (1 / 2.2),) * 3)
                picture = mitsuba(source, [sky(2.0)], width=4, height=4, lens=0.3, spp=256, depth=depth)[0]
                np.testing.assert_allclose(picture.mean(axis=(0, 1)), [2.0 * albedo] * 3, rtol=0.03)

    def test_the_embree_sky_and_the_path_traced_sky_agree_in_units(self):
        a = embree(make_source([quad(0.0, 8.0)], kd=(0.7,) * 3), [sky(1.5)], width=4, height=4, lens=0.3)
        b = mitsuba(make_source([quad(0.0, 8.0)], kd=(0.7,) * 3), [sky(1.5)], width=4, height=4, lens=0.3, spp=256)[0]
        np.testing.assert_allclose(b.mean(axis=(0, 1)), a.mean(axis=(0, 1)), rtol=0.03)

    def test_a_deeper_cap_adds_interreflection_and_the_override_matches_a_scene_prepared_for_it(self):
        shallow, deep = (corridor_trace(corridor_path(), 512, 1, depth).mean() for depth in (2, 12))
        self.assertGreater(deep, 1.5 * shallow)
        self.assertEqual(corridor_trace(corridor_path(), 512, 1, 12).mean(), deep)

    def test_a_seed_reproduces_and_another_seed_differs(self):
        path = corridor_path()
        a, again, other = (corridor_trace(path, 8, seed) for seed in (1, 1, 2))
        np.testing.assert_array_equal(a, again)
        self.assertGreater(np.abs(a - other).mean(), 0)

    def test_passes_are_independent_so_more_of_them_average_the_noise_down(self):
        # 3 paths in flight per pixel: 67 spp is 22 passes, the last of one path.
        path = corridor_path(pixels_per_pass=8 * 8 * 3)
        few = np.abs(corridor_trace(path, 3, 1) - corridor_trace(path, 3, 2)).mean()
        many = np.abs(corridor_trace(path, 67, 1) - corridor_trace(path, 67, 2)).mean()
        self.assertLess(many, 0.5 * few)

    def test_the_physical_sun_follows_the_engines_y_up_world(self):
        # A ground plane facing +y: a sun straight overhead lights it far more than one on the horizon.
        ground = [[-8.0, 0.0, -8.0], [-8.0, 0.0, 8.0], [8.0, 0.0, 8.0], [8.0, 0.0, -8.0]]
        pose = np.array([0.0, 3.0, 0.0, 0.0, -1.0, 0.05])
        values = [mitsuba(make_source([ground], kd=(0.5,) * 3), [sun(direction)], pose=pose, width=4, height=4, lens=0.3,
                          sky_settings={"turbidity": 3.0, "albedo": 0.3})[0].mean()
                  for direction in ([0.0, 1.0, 0.0], [0.0, 0.05, 1.0])]
        self.assertGreater(values[0], 3.0 * values[1])


@needs_mitsuba
class MitsubaEmitterTest(unittest.TestCase):
    def test_an_ambient_light_has_no_path_traced_meaning_unless_it_is_black(self):
        mi = mitsuba_reference.import_mitsuba()
        with self.assertRaises(ValueError):
            mitsuba_reference.emitters(mi, [{"type": "ambient", "color": [1, 1, 1], "intensity": 0.3}])
        self.assertEqual(mitsuba_reference.emitters(mi, [{"type": "ambient", "color": [1, 1, 1], "intensity": 0.0}]), {})

    def test_a_physical_sky_takes_the_first_directional_light_and_needs_one(self):
        mi = mitsuba_reference.import_mitsuba()
        physical = {"turbidity": 3.0, "albedo": 0.3}
        with self.assertRaises(ValueError):
            mitsuba_reference.emitters(mi, [sky()], physical)
        found = mitsuba_reference.emitters(mi, [sky(), sun([0.0, 1.0, 0.0]), sun([1.0, 0.0, 0.0])], physical)
        self.assertEqual(found["sunsky"]["sun_direction"], [0.0, 1.0, 0.0])

    def test_a_directional_light_is_pi_times_its_colour_travelling_away_from_the_sun(self):
        mi = mitsuba_reference.import_mitsuba()
        found = mitsuba_reference.emitters(mi, [dict(sun([0.0, 0.0, 1.0]), intensity=2.0)])["light_0"]
        self.assertEqual(found["direction"], [-0.0, -0.0, -1.0])
        self.assertAlmostEqual(found["irradiance"]["value"][0], 2.0 * np.pi)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class TextureDtypeTest(unittest.TestCase):
    def test_a_texture_loads_float64_unless_float32_is_asked_for(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "t.png"
            Image.fromarray(np.full((4, 4, 3), 200, dtype=np.uint8)).save(path)
            self.assertEqual(Texture(path).levels[0].dtype, np.float64)
            lean = Texture(path, dtype=np.float32)
            self.assertTrue(all(level.dtype == np.float32 for level in lean.levels))
            np.testing.assert_allclose(lean.levels[0][0, 0, :3], (200 / 255) ** 2.2, rtol=1e-5)


if __name__ == "__main__":
    unittest.main()

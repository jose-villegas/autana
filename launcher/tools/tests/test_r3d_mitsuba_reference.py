"""Checks the path-traced reference backend against the Embree reference on small fixtures: direct-only parity,
texture UV orientation and the energy of a constant environment."""

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

LOOK_DOWN = None if np is None else np.array([0.0, 0.0, 3.0, 0.0, 0.0, -1.0])
VARIANT = "scalar_rgb"


def quad(z, half, x0=0.0, y0=0.0):
    return [[x0 - half, y0 - half, z], [x0 + half, y0 - half, z], [x0 + half, y0 + half, z], [x0 - half, y0 + half, z]]


def make_source(quads, kd=(1.0, 1.0, 1.0), texture=None):
    """One material, one two-triangle quad per entry of `quads`, each facing +z with UVs 0..1 over its corners."""
    corners, uvs, tris = [], [], []
    for index, corner in enumerate(quads):
        base = 4 * index
        corners += corner
        uvs += [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]]
        tris += [[base, base + 1, base + 2], [base, base + 2, base + 3]]
    source = SimpleNamespace(p=np.array(corners, dtype=float), uv=np.array(uvs), tri_v=np.array(tris), tri_t=np.array(tris),
                             tri_m=np.zeros(len(tris), dtype=int), names=["fixture"], materials={"fixture": {"Kd": kd}},
                             textures=[texture])
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.indirect_cache = None
    source.intersector = RayMeshIntersector(trimesh.Trimesh(source.p, source.tri_v, process=False))
    return source


def sun(direction):
    return {"type": "directional", "direction": direction, "color": [1.0, 1.0, 1.0], "intensity": 1.0, "disc_degrees": 0.0,
            "rays": 1}


def sky(intensity=1.0):
    return {"type": "sky", "color": [1.0, 1.0, 1.0], "intensity": intensity, "rays": 16}


def embree(source, lights, pose=LOOK_DOWN, size=16, lens=0.5):
    settings = SimpleNamespace(double_sided=set(), seed=1)
    job = SimpleNamespace(settings=settings, bake=SimpleNamespace(ray_offset=0.001))
    return render_linear(source, job, SimpleNamespace(lights=lights), pose, size, size, lens, 4)


def mitsuba(source, lights, pose=LOOK_DOWN, size=16, lens=0.5, spp=64, depth=2):
    return mitsuba_reference.render(source, lights, set(), pose, size, size, lens, 0.01, spp, depth, 1, variant=VARIANT)[0]


@unittest.skipIf(not HAVE_MITSUBA, "the path-traced backend needs requirements-gpu.txt")
class MitsubaDirectParityTest(unittest.TestCase):
    def test_a_shadowed_plane_matches_the_embree_reference_pixel_for_pixel(self):
        # An occluder card above a ground plane, and a sun tilted so its shadow falls inside the view.
        source = make_source([quad(0.0, 4.0), quad(1.0, 0.8, 0.0, 0.0)])
        lights = [sun([0.5, 0.2, 0.8])]
        a, b = embree(source, lights), mitsuba(source, lights)
        self.assertGreater(a.max() - a.min(), 0.1, "the fixture needs a shadow to compare")
        close = np.abs(a - b).max(axis=2) < 0.02
        self.assertGreaterEqual(close.mean(), 0.9)
        self.assertLess(np.abs(a - b).mean(), 0.01)

    def test_the_camera_looks_where_the_embree_reference_looks(self):
        # A card covering only the upper-left of the view: both backends must cover the same pixels.
        source = make_source([quad(0.0, 0.5, -0.6, 0.6)])
        lights = [sun([0.0, 0.0, 1.0])]
        covered_a = embree(source, lights).max(axis=2) > 0.5
        covered_b = mitsuba(source, lights).max(axis=2) > 0.5
        self.assertTrue(covered_a[:4, :4].any() and not covered_a[8:, :].any())
        self.assertGreaterEqual((covered_a == covered_b).mean(), 0.97)

    def test_pixels_the_mesh_does_not_cover_have_zero_coverage(self):
        source = make_source([quad(0.0, 0.5)])
        _, coverage = mitsuba_reference.render(source, [sun([0.0, 0.0, 1.0])], set(), LOOK_DOWN, 8, 8, 0.5, 0.01, 16, 2, 1,
                                               variant=VARIANT)
        self.assertEqual(coverage[0, 0], 0.0)
        self.assertEqual(coverage[4, 4], 1.0)


@unittest.skipIf(not HAVE_MITSUBA, "the path-traced backend needs requirements-gpu.txt")
class MitsubaTextureTest(unittest.TestCase):
    def test_texels_land_where_the_embree_sampler_puts_them(self):
        # Four distinct blocks, so a flipped or transposed texture changes the picture. Image row 0 is the top of the
        # texture and v = 1; the repo's sampler reads row (1 - v) * height.
        blocks = {(0, 0): (255, 0, 0), (0, 1): (0, 255, 0), (1, 0): (0, 0, 255), (1, 1): (255, 255, 0)}
        pixels = np.zeros((8, 8, 3), dtype=np.uint8)
        for (row, column), colour in blocks.items():
            pixels[row * 4 : row * 4 + 4, column * 4 : column * 4 + 4] = colour
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "blocks.png"
            Image.fromarray(pixels).save(path)
            texture = Texture(path)
        source = make_source([quad(0.0, 1.0)], texture=texture)
        lights = [sun([0.0, 0.0, 1.0])]
        a, b = embree(source, lights, size=8), mitsuba(source, lights, size=8, lens=0.4)
        for y, x in ((2, 2), (2, 5), (5, 2), (5, 5)):
            np.testing.assert_allclose(b[y, x], a[y, x], atol=0.02)
        centres = {tuple(np.round(b[y, x], 2)) for y, x in ((2, 2), (2, 5), (5, 2), (5, 5))}
        self.assertEqual(len(centres), 4)

    def test_kd_scales_a_texture_and_decodes_an_untextured_colour_with_power_2_2(self):
        source = make_source([quad(0.0, 1.0)], kd=(0.5, 0.25, 1.0))
        lights = [sun([0.0, 0.0, 1.0])]
        picture = mitsuba(source, lights, size=4, lens=0.4)
        np.testing.assert_allclose(picture[2, 2], np.array([0.5, 0.25, 1.0]) ** 2.2, rtol=1e-4)


@unittest.skipIf(not HAVE_MITSUBA, "the path-traced backend needs requirements-gpu.txt")
class MitsubaEnergyTest(unittest.TestCase):
    def test_a_plane_under_a_constant_sky_returns_albedo_times_radiance_at_any_depth(self):
        for albedo in (1.0, 0.5):
            kd = (albedo ** (1 / 2.2),) * 3
            source = make_source([quad(0.0, 8.0)], kd=kd)
            for depth in (2, 12):
                picture = mitsuba(source, [sky(2.0)], size=4, lens=0.3, spp=256, depth=depth)
                np.testing.assert_allclose(picture.mean(axis=(0, 1)), [2.0 * albedo] * 3, rtol=0.03)

    def test_the_embree_sky_and_the_path_traced_sky_agree_in_units(self):
        source = make_source([quad(0.0, 8.0)], kd=(0.7,) * 3)
        a = embree(source, [sky(1.5)], size=4, lens=0.3)
        b = mitsuba(source, [sky(1.5)], size=4, lens=0.3, spp=256)
        np.testing.assert_allclose(b.mean(axis=(0, 1)), a.mean(axis=(0, 1)), rtol=0.03)

    def test_the_physical_sun_follows_the_engines_y_up_world(self):
        # A ground plane facing +y: a sun straight overhead lights it far more than one on the horizon.
        ground = [[-8.0, 0.0, -8.0], [-8.0, 0.0, 8.0], [8.0, 0.0, 8.0], [8.0, 0.0, -8.0]]
        source = make_source([ground], kd=(0.5,) * 3)
        pose = np.array([0.0, 3.0, 0.0, 0.0, -1.0, 0.05])
        values = []
        for direction in ([0.0, 1.0, 0.0], [0.0, 0.05, 1.0]):
            picture = mitsuba_reference.render(source, [sun(direction)], set(), pose, 4, 4, 0.3, 0.01, 64, 2, 1,
                                               sky={"turbidity": 3.0, "albedo": 0.3}, variant=VARIANT)[0]
            values.append(picture.mean())
        self.assertGreater(values[0], 3.0 * values[1])

    def test_an_ambient_light_has_no_path_traced_meaning(self):
        with self.assertRaises(ValueError):
            mitsuba_reference.emitters(mitsuba_reference.import_mitsuba(), [{"type": "ambient", "color": [1, 1, 1], "intensity": 0.3}])


if __name__ == "__main__":
    unittest.main()

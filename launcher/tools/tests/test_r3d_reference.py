"""Checks the source-reference renderer on a one-triangle lit mesh: its light, its pixels and its normal buffer."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

try:
    import numpy as np
    import trimesh
    from trimesh.ray.ray_pyembree import RayMeshIntersector

    from r3d.geometry import corner_normals
    from r3d.reference_render import device_picture, render_linear, trace
    from render_compare import expand_565
except ImportError:
    np = None


def plane_source(corners):
    """A one-material quad of two triangles facing +z, white, unlit by anything but the scene."""
    source = SimpleNamespace(
        p=np.array(corners), uv=np.zeros((4, 2)), tri_v=np.array([[0, 1, 2], [0, 2, 3]]), tri_t=np.array([[0, 1, 2], [0, 2, 3]]),
        tri_m=np.array([0, 0]), names=["plane"], materials={"plane": {"Kd": (1.0, 1.0, 1.0)}}, textures=[None],
    )
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.indirect_cache = None
    source.intersector = RayMeshIntersector(trimesh.Trimesh(source.p, source.tri_v, process=False))
    return source


def sun_scene(direction):
    settings = SimpleNamespace(double_sided=set(), light=SimpleNamespace(ray_offset=0.01), seed=1)
    scene = SimpleNamespace(lights=[{"type": "directional", "direction": direction, "color": [1.0, 1.0, 1.0], "intensity": 1.0,
                                     "disc_degrees": 0.0, "rays": 1}])
    return settings, scene


LOOK_DOWN = None if np is None else np.array([0.0, 0.0, 1.0, 0.0, 0.0, -1.0])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ReferenceRenderTest(unittest.TestCase):
    def test_an_unshadowed_plane_is_exact_lambert(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[1.0, 1.0, 1.0]]], atol=1e-12, rtol=0)

    def test_a_tilted_sun_scales_the_value_by_the_cosine_to_the_normal(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.8, 0.0, 0.6]), LOOK_DOWN, 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[0.6, 0.6, 0.6]]], atol=1e-12, rtol=0)

    def test_subpixels_land_in_their_own_pixel_and_average_to_its_coverage(self):
        # The quad covers -0.5 < x < 0 and y > 0.5 of a view spanning -1..1: of the 2 x 2 pixels only the
        # top-left is reached, by its upper-right subpixel, one of its four.
        source = plane_source([[-0.5, 0.5, 0.0], [0.0, 0.5, 0.0], [0.0, 2.0, 0.0], [-0.5, 2.0, 0.0]])
        picture = render_linear(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(picture[..., 0], [[0.25, 0.0], [0.0, 0.0]], atol=1e-12, rtol=0)

    def test_the_normal_buffer_faces_the_eye_and_is_zero_where_rays_miss(self):
        # Wound to face away from the eye at z = 1: the buffer still turns its normal toward the eye.
        source = plane_source([[-0.5, 0.5, 0.0], [-0.5, 2.0, 0.0], [0.0, 2.0, 0.0], [0.0, 0.5, 0.0]])
        _linear, _covered, normal = trace(source, *sun_scene([0.0, 0.0, 1.0]), LOOK_DOWN, 2, 2, 1.0, 2)
        np.testing.assert_allclose(normal[0, 0], [0.0, 0.0, 1.0], atol=1e-12, rtol=0)
        np.testing.assert_allclose(normal[1], 0.0, atol=1e-12, rtol=0)

    def test_a_pose_at_empty_sky_is_the_scene_background_as_the_device_shows_it(self):
        source = plane_source([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [2.0, 2.0, 0.0], [-2.0, 2.0, 0.0]])
        away = np.array([0.0, 0.0, 1.0, 0.0, 0.0, 1.0])
        linear, covered, _normal = trace(source, *sun_scene([0.0, 0.0, 1.0]), away, 3, 3, 1.0, 2)
        picture = device_picture(linear, covered, 0.35, 0x9CC0E6)
        shown = expand_565(np.full((3, 3, 3), (0x9C, 0xC0, 0xE6), dtype=np.uint8))
        np.testing.assert_array_equal(picture, shown)

    def test_a_half_covered_pixel_blends_the_mesh_with_the_background(self):
        linear, covered = np.full((1, 1, 3), 1.0), np.full((1, 1), 0.5)
        picture = device_picture(linear, covered, 0.0, 0x000000)
        np.testing.assert_array_equal(picture, expand_565(np.full((1, 1, 3), 128, dtype=np.uint8)))


if __name__ == "__main__":
    unittest.main()

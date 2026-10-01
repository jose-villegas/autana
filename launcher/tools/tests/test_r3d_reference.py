"""Checks the source-reference renderer on a one-triangle lit mesh."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    import trimesh
    from trimesh.ray.ray_pyembree import RayMeshIntersector

    from r3d.geometry import corner_normals
    from r3d.reference_render import render_linear
except ImportError:
    np = None


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ReferenceRenderTest(unittest.TestCase):
    def test_an_unshadowed_plane_is_exact_lambert(self):
        source = SimpleNamespace(
            p=np.array([[-2.0, -2.0, 0.0], [2.0, -2.0, 0.0], [0.0, 2.0, 0.0]]),
            uv=np.zeros((3, 2)), tri_v=np.array([[0, 1, 2]]), tri_t=np.array([[0, 1, 2]]),
            tri_m=np.array([0]), names=["plane"], materials={"plane": {"Kd": (1.0, 1.0, 1.0)}}, textures=[None],
        )
        source.corner_normals = corner_normals(source.p, source.tri_v)
        source.intersector = RayMeshIntersector(trimesh.Trimesh(source.p, source.tri_v, process=False))
        settings = SimpleNamespace(double_sided=set(), light=SimpleNamespace(ray_offset=0.01), seed=1)
        scene = SimpleNamespace(lights=[{"type": "directional", "direction": [0.0, 0.0, 1.0],
                                         "color": [1.0, 1.0, 1.0], "intensity": 1.0, "disc_degrees": 0.0, "rays": 1}])
        picture = render_linear(source, settings, scene, np.array([0.0, 0.0, 1.0, 0.0, 0.0, -1.0]), 1, 1, 0.1, 1)
        np.testing.assert_allclose(picture, [[[1.0, 1.0, 1.0]]], atol=1e-12, rtol=0)


if __name__ == "__main__":
    unittest.main()

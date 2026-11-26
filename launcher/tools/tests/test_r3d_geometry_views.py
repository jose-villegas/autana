"""Geometry-only probe selection and its fit boundary."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import numpy as np

from r3d import appearance_simplify
from r3d.fit import geometry_views
from r3d.reference_set import PROBE_SEED
from r3d.views import probes
from tests.r3d_env import needs_llvm
from tests.test_r3d_views_probes import closed_box


try:
    import torch
except ImportError:
    torch = None


@needs_llvm
@unittest.skipIf(torch is None, "the reference buffers need PyTorch")
class SelectionTests(unittest.TestCase):
    def test_visible_copies_independent_poses_and_determinism(self):
        from r3d import ray_query

        self.addCleanup(setattr, ray_query, "VARIANT", ray_query.VARIANT)
        ray_query.VARIANT = "llvm_ad_rgb"
        box, box_tris = closed_box(0.5)
        room, room_tris = closed_box(3.0)
        source = SimpleNamespace(p=np.concatenate([box, room]),
                                 tri_v=np.concatenate([box_tris, room_tris[:, ::-1] + len(box)]), textures=[None])
        carved = np.arange(len(source.tri_v)) < len(box_tris)
        region, near, spacing = (np.full(3, -2.0), np.full(3, 2.0)), 0.05, 4.0
        ordinary = probes.select(source.p, source.tri_v, region, near, spacing, PROBE_SEED, select_size=8)
        # One hidden room triangle selects the shared CPU ray route without a CUDA rasteriser.
        source.camera_hidden = np.zeros(len(source.tri_v), dtype=bool)
        source.camera_hidden[-1] = True
        first = geometry_views.select(source, carved, region, near, spacing, device="cpu")
        second = geometry_views.select(source, carved, region, near, spacing, device="cpu")
        empty = geometry_views.select(source, np.zeros_like(carved), region, near, spacing, device="cpu")
        source.camera_hidden |= carved
        hidden = geometry_views.select(source, carved, region, near, spacing, device="cpu")
        self.assertGreater(len(first.poses), 0)
        self.assertLessEqual(len(first.poses), geometry_views.FACE_COUNT_MAX)
        np.testing.assert_array_equal(first.poses, second.poses)
        self.assertTrue(all(mask.any().item() for mask in first.buffers.copy))
        for pose in first.poses:
            self.assertFalse(any(np.array_equal(pose, other) for other in ordinary.training + ordinary.held_out))
        self.assertEqual(len(empty.poses), 0)
        self.assertEqual(len(empty.buffers.copy), 0)
        self.assertEqual(len(hidden.poses), 0, "camera-hidden copies contributed geometry views")


class WiringTests(unittest.TestCase):
    def test_only_probe_fit_passes_geometry_views_to_optimise(self):
        mesh = appearance_simplify.FitMesh(np.zeros((3, 3)), np.zeros((3, 3)), np.array([[0, 1, 2]]),
                                         np.ones(1), 1, np.arange(3))
        source = SimpleNamespace(object_names=["copy"], tri_o=np.array([0]), tri_m=np.array([0]), names=["mat"],
                                 textures=[None])
        camera = SimpleNamespace(background=0, region="region", near_z=0.1)
        selected = object()
        for shaping in (None, SimpleNamespace(spacing=2.0)):
            job = SimpleNamespace(settings=SimpleNamespace(meshes=[], fit=SimpleNamespace(probes=shaping)))
            with self.subTest(probes=shaping), mock.patch.multiple(appearance_simplify,
                    load_scene=mock.DEFAULT, start_mesh=mock.DEFAULT, placed_copies=mock.DEFAULT,
                    load_views=mock.DEFAULT, match_meshes=mock.DEFAULT, optimise=mock.DEFAULT) as patches, \
                    mock.patch("r3d.reference_render.source_for", return_value=(source, job)), \
                    mock.patch.object(geometry_views, "select", return_value=selected) as select:
                patches["load_scene"].return_value = SimpleNamespace(camera=SimpleNamespace(component=camera))
                patches["start_mesh"].return_value = mesh
                patches["placed_copies"].return_value = mesh
                patches["load_views"].return_value = ([], (8, 8))
                patches["match_meshes"].return_value = [0]
                patches["optimise"].side_effect = RuntimeError("fit boundary")
                with self.assertRaisesRegex(RuntimeError, "fit boundary"):
                    appearance_simplify.main(["--scene", "scene", "--start", "mesh.bundle", "--poses", "poses",
                                              "--reference", "ref", "--out", "out", "--copies", "copies",
                                              "--copy-poses", "held", "--object", "object"])
                kwargs = patches["optimise"].call_args.kwargs
                if shaping is None:
                    select.assert_not_called()
                    self.assertNotIn("geometry_views", kwargs)
                else:
                    self.assertIs(kwargs["geometry_views"], selected)
                    self.assertEqual(select.call_args.args[2:], (camera.region, camera.near_z, shaping.spacing))


if __name__ == "__main__":
    unittest.main()

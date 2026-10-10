"""Checks that a model exported to FBX and converted back is the model: the
same glTF skinned mesh, posed in each of its clips, and the same static mesh.
The pair is frozen in data/: exported_from_glb.glb, an export of the capybara,
and exported_from_glb.fbx, which Blender wrote from it (gltf/glb_to_fbx.py, the
command in its header; write both again together, never one alone); vertices
are matched by position, because the FBX splits a vertex wherever
its normal changes."""

import pathlib
import sys
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf import gltf_read  # noqa: E402
from r3d import gltf_mesh, gltf_skin  # noqa: E402

DATA = pathlib.Path(__file__).resolve().parent / "data"
GLB = DATA / "exported_from_glb.glb"
FBX = DATA / "exported_from_glb.fbx"

FPS = 30
# Blender resamples the clips onto whole frames and the FBX holds Euler angles,
# so a pose differs by millimetres (the pair's worst is under 6 mm on a 0.9 m
# animal); a unit, axis or weight error is centimetres to metres.
POSE_TOLERANCE = 0.01
# A static mesh is the bind pose, carried through no resampling.
STATIC_TOLERANCE = 1e-4
CLIP_TIMES = (0.0, 0.25, 0.5, 0.77, 1.0)  # fractions of a clip's length
CLIP_PREFIX_SEPARATOR = "|"  # Blender names a clip "<armature>|<action>" in an FBX


def worst_gap(points, others):
    """The largest distance from a point to the nearest one of the other set,
    either way round."""
    a, b = np.array(points), np.array(others)
    distance = np.linalg.norm(a[:, None] - b[None], axis=2)
    return max(distance.min(axis=1).max(), distance.min(axis=0).max())


def clip_names(asset):
    return {name.split(CLIP_PREFIX_SEPARATOR)[-1]: name for name in asset.animations}


class RoundTripTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.glb = gltf_skin.SkinnedAsset(*gltf_read.load_asset(GLB))
        cls.fbx = gltf_skin.SkinnedAsset(*gltf_read.load_asset(FBX))
        cls.glb_clips, cls.fbx_clips = clip_names(cls.glb), clip_names(cls.fbx)

    def test_the_clips_survive_with_their_lengths(self):
        self.assertEqual(set(self.glb_clips), set(self.fbx_clips))
        for clip in self.glb_clips:
            self.assertAlmostEqual(self.glb.duration(self.glb_clips[clip]),
                                   self.fbx.duration(self.fbx_clips[clip]), delta=1 / FPS, msg=clip)

    def test_every_clip_poses_the_mesh_alike(self):
        for clip in self.glb_clips:
            length = self.glb.duration(self.glb_clips[clip])
            for fraction in CLIP_TIMES:
                with self.subTest(clip=clip, at=fraction):
                    a, _ = self.glb.skin(self.glb.sample(self.glb_clips[clip], fraction * length))
                    b, _ = self.fbx.skin(self.fbx.sample(self.fbx_clips[clip], fraction * length))
                    self.assertLess(worst_gap(a, b), POSE_TOLERANCE)

    def test_every_joint_follows_the_same_path(self):
        index_a = {n["name"]: i for i, n in enumerate(self.glb.document["nodes"])}
        index_b = {n["name"]: i for i, n in enumerate(self.fbx.document["nodes"])}
        joints = [self.glb.document["nodes"][j]["name"] for j in self.glb.joints]
        self.assertEqual(set(joints), {self.fbx.document["nodes"][j]["name"] for j in self.fbx.joints})
        for clip in self.glb_clips:
            length = self.glb.duration(self.glb_clips[clip])
            for fraction in CLIP_TIMES:
                world_a = self.glb.world_matrices(self.glb.sample(self.glb_clips[clip], fraction * length))
                world_b = self.fbx.world_matrices(self.fbx.sample(self.fbx_clips[clip], fraction * length))
                for joint in joints:
                    here, there = world_a[index_a[joint]], world_b[index_b[joint]]
                    gap = max(abs(here[i] - there[i]) for i in (3, 7, 11))  # the matrix's translation
                    self.assertLess(gap, POSE_TOLERANCE, f"{clip} {joint} at {fraction}")

    def test_static_import_matches(self):
        a, b = gltf_mesh.load_gltf_mesh(GLB), gltf_mesh.load_gltf_mesh(FBX)
        self.assertEqual(len(a.tri_v), len(b.tri_v))
        self.assertLess(worst_gap(a.positions, b.positions), STATIC_TOLERANCE)


if __name__ == "__main__":
    unittest.main()

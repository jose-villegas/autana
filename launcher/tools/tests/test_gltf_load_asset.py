"""Checks gltf_read.load_asset: a .glb is read as it is, an .fbx is converted
once and read from the cache, and the tools that take a scene file take both.
The FBX is data/skinned_probe.fbx (see test_fbx_to_glb.py)."""

import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset  # noqa: E402
from fbx import fbx_to_glb  # noqa: E402
from gltf import gltf_read  # noqa: E402
from r3d import gltf_mesh  # noqa: E402

PROBE = pathlib.Path(__file__).resolve().parent / "data" / "skinned_probe.fbx"
BEND_ANIMATION = "rig|bend"
BAR_TRIANGLES = 48


class LoadAssetTests(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        patch = mock.patch.object(fbx_to_glb, "CACHE", self.directory / "cache")
        patch.start()
        self.addCleanup(patch.stop)

    def copy_probe(self, name="probe.fbx", extra=b""):
        path = self.directory / name
        path.write_bytes(PROBE.read_bytes() + extra)
        return path

    def test_a_glb_is_read_as_it_is(self):
        path = self.directory / "probe.glb"
        path.write_bytes(fbx_to_glb.convert(PROBE))
        self.assertEqual(gltf_read.load_asset(path), gltf_read.load_glb(path))

    def test_an_fbx_reads_as_its_conversion(self):
        document, binary = gltf_read.load_asset(self.copy_probe())
        self.assertEqual((document, binary), gltf_read.parse_glb(fbx_to_glb.convert(PROBE)))

    def test_the_suffix_may_be_in_any_case(self):
        document, _ = gltf_read.load_asset(self.copy_probe("PROBE.FBX"))
        self.assertTrue(document["meshes"])

    def test_an_fbx_is_converted_once_per_content(self):
        path = self.copy_probe(extra=b"\0" * 7)  # trailing bytes: a file of its own
        gltf_read.load_asset(path)
        with mock.patch.object(fbx_to_glb, "convert", side_effect=AssertionError("converted again")):
            gltf_read.load_asset(path)
            gltf_read.load_asset(self.copy_probe("moved.fbx", extra=b"\0" * 7))  # same bytes, other name
        with mock.patch.object(fbx_to_glb, "convert", wraps=fbx_to_glb.convert) as convert:
            gltf_read.load_asset(self.copy_probe("other.fbx", extra=b"\0" * 9))
        convert.assert_called_once()

    def test_a_scene_tool_reads_an_fbx(self):
        mesh = gltf_mesh.load_gltf_mesh(self.copy_probe())
        self.assertEqual(len(mesh.tri_v), BAR_TRIANGLES)

    def test_an_anim_toml_may_name_an_fbx(self):
        self.copy_probe()
        clip = self.directory / "bend.anim.toml"
        clip.write_text('source = "probe.fbx"\nanimation = "%s"\n' % BEND_ANIMATION)
        tracks, duration_ms = tracks_asset.decode(tracks_asset.bake(clip))
        self.assertTrue(tracks)
        self.assertGreater(duration_ms, 0)

    def test_an_anim_toml_refuses_other_sources(self):
        clip = self.directory / "bend.anim.toml"
        clip.write_text('source = "probe.obj"\nanimation = "x"\n')
        with self.assertRaisesRegex(tracks_asset.TracksError, r"\.glb, \.fbx or \.keys\.toml"):
            tracks_asset.load_source(clip)


if __name__ == "__main__":
    unittest.main()

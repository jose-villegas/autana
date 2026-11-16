"""The host programs built on scene/scene_host.py's sources: the skin figures' engine skinning (skin_frame.EngineSkin)
builds and poses a rig, and the playback figures' probe builds. The doc-image refresh is their only other caller, so
a scene source that needs a new file, header or app-heap entry fails here first."""

import pathlib
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim.playback.playback_figures import PROBE_SOURCES  # noqa: E402
from build.find_cc import host_cc  # noqa: E402
from docs.figures import build_probe  # noqa: E402
from r3d.skin.skin_frame import EngineSkin  # noqa: E402
from scene import scene_host  # noqa: E402
from skin_probe import POSITIONS, POSITION_SCALE, rig_glb  # noqa: E402


@unittest.skipUnless(host_cc(), "needs a C compiler")
class SceneHostPrograms(unittest.TestCase):
    def test_engine_skin_poses_every_vertex_of_a_rig(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            model = work / "rig.glb"
            model.write_bytes(rig_glb())
            positions = EngineSkin(model, POSITION_SCALE, work).positions("move", 500, 4)
        self.assertEqual(len(positions), len(POSITIONS))

    def test_playback_probe_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            build_probe(host_cc(), PROBE_SOURCES, directory, scene_host.FLAGS)  # raises when it does not


if __name__ == "__main__":
    unittest.main()

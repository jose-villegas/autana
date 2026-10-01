"""Checks render_scene.sh builds a substituted source in place of the one a
scene lists, which is how a scratch mesh is rendered without touching the
tracked one."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

RENDER = pathlib.Path(__file__).resolve().parents[1] / "render"
FIXTURE = RENDER / "tests" / "frame_watch_fixture.c"

SCENE = """
scene_name=substitution_fixture
scene_sources="main/gfx/gfx.c main/util/tune.c tools/render/tests/frame_watch_fixture.c"
scene_renders=""
scene_pin=0
. "{render_scene}"
render_scene_build -o "{out}" --build-only {options}
"""


@unittest.skipIf(shutil.which("sh") is None or shutil.which("gcc") is None, "needs a POSIX shell and gcc")
class SubstituteTest(unittest.TestCase):
    def build(self, root, source):
        """The build's completed process, substituting a renamed fixture for `source` when given."""
        renamed = root / "renamed.c"
        renamed.write_text(FIXTURE.read_text().replace('"frame_watch_fixture"', '"substituted_fixture"'))
        options = f"--substitute {source}={renamed.as_posix()}" if source else ""
        script = SCENE.format(render_scene=(RENDER / "render_scene.sh").as_posix(), out=(root / "out").as_posix(),
                              options=options)
        return subprocess.run(["sh", "-c", script, str(RENDER / "tests" / "x.sh")], capture_output=True, text=True)

    def built_name(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.assertEqual(self.build(root, source).returncode, 0)
            binary = next((root / "out").glob("*_render*"))
            done = subprocess.run([str(binary), "-o", str(root / "x.bmp")], capture_output=True, text=True)
            return done.stderr

    def test_a_listed_source_is_built_from_its_substitute(self):
        self.assertIn("RENDER substituted_fixture", self.built_name("tools/render/tests/frame_watch_fixture.c"))

    def test_without_one_the_listed_source_is_built(self):
        self.assertIn("RENDER frame_watch_fixture", self.built_name(None))

    def test_a_substitute_that_matches_no_listed_source_fails_the_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            done = self.build(pathlib.Path(tmp), "tools/render/tests/not_a_source.c")
        self.assertEqual(done.returncode, 1)
        self.assertIn("matches no source", done.stderr)


if __name__ == "__main__":
    unittest.main()

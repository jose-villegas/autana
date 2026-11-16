"""scene_edit: placements read from a scene, and changed ones written back with nothing else in the file touched."""

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from scene import scene_edit  # noqa: E402
from test_r3d_import import HEAD, NO_LEVELS, camera, renderer, sun_object, write_import, write_scene  # noqa: E402

SUN_LINE = "rotation = [40.0, 30.0, 0.0]   # toward the front: above and to the right\n"
BOX_POSITION = "position = [1.0, 2.0, 3.0]  # on the table\n"
SHELF_POSITION = "position = [\n    1.0,  # along the wall\n    2.0,\n    3.0,\n]\n"


def scene_text():
    """A baked ground, a sun whose rotation carries a comment, a camera, and renderers that are not baked: a box
    with a commented position, a lid under it, a shelf whose position spans lines, and a skinned pet."""
    sun = sun_object().replace("rotation = [0.0, 0.0, 0.0]\n", SUN_LINE)
    box = renderer(name="box", transform=BOX_POSITION)
    lid = renderer(name="lid", transform='parent = "box"\n')
    shelf = renderer(name="shelf", transform=SHELF_POSITION)
    pet = renderer("pet.import.toml", name="pet").replace("mesh_renderer", "skinned_renderer")
    return ("# A scene with placements to edit.\n" + renderer(name="ground", extra="bake = true\n") + "\n" + sun
            + "\n" + camera(region=False) + "\n" + box + "\n" + lid + "\n" + shelf + "\n" + pet)


def run(*argv):
    """scene_edit's exit code, stdout and stderr for `argv`."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = scene_edit.main([str(arg) for arg in argv])
    return code, out.getvalue(), err.getvalue()


class SceneEditTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.folder = pathlib.Path(directory.name)
        write_import(self.folder)
        write_import(self.folder, "pet.import.toml", output='[output]\nname = "pet"\n', body=NO_LEVELS)
        self.scene = write_scene(self.folder, scene_text(), HEAD)
        self.before = self.scene.read_bytes()

    def lines(self):
        return self.scene.read_text().splitlines(keepends=True)

    def assert_refused(self, *assignments, why):
        """`set` with `assignments` exits 1 naming `why`, and leaves the folder as it was."""
        listing = sorted(self.folder.iterdir())
        code, _, err = run("set", self.scene, *(part for text in assignments for part in ("--set", text)))
        self.assertEqual(code, 1)
        self.assertRegex(err, why)
        self.assertEqual(self.scene.read_bytes(), self.before)
        self.assertEqual(sorted(self.folder.iterdir()), listing)

    def test_read_lists_every_object_as_authored_with_defaults(self):
        code, out, _ = run("read", self.scene)
        self.assertEqual(code, 0)
        objects = {item["name"]: item for item in json.loads(out)["objects"]}
        self.assertEqual(list(objects), ["ground", "sun", "camera", "box", "lid", "shelf", "pet"])
        self.assertEqual(objects["box"], dict(name="box", parent=None, kind="mesh_renderer", baked=False,
                                              position=[1.0, 2.0, 3.0], rotation=[0.0] * 3, scale=[1.0] * 3))
        self.assertEqual(objects["lid"]["parent"], "box")
        self.assertTrue(objects["ground"]["baked"])
        self.assertEqual((objects["sun"]["kind"], objects["sun"]["rotation"]), ("light", [40.0, 30.0, 0.0]))

    def test_an_existing_value_changes_in_place_and_keeps_its_comment(self):
        before = self.lines()
        self.assertEqual(run("set", self.scene, "--set", "box.position=0.5,-2,4.25")[0], 0)
        after = self.lines()
        changed = [(old, new) for old, new in zip(before, after) if old != new]
        self.assertEqual(len(after), len(before))
        self.assertEqual(changed, [(BOX_POSITION, "position = [0.5, -2.0, 4.25]  # on the table\n")])

    def test_a_value_over_several_lines_keeps_its_layout(self):
        self.assertEqual(run("set", self.scene, "--set", "shelf.position=4,5,6")[0], 0)
        self.assertIn(SHELF_POSITION.replace("1.0", "4.0").replace("2.0", "5.0").replace("3.0", "6.0"), self.scene.read_text())

    def test_a_skinned_renderer_is_placed(self):
        self.assertEqual(run("set", self.scene, "--set", "pet.position=0.5,0,-0.25", "--set", "pet.scale=2,2,2")[0], 0)
        pet = {item["name"]: item for item in json.loads(run("read", self.scene)[1])["objects"]}["pet"]
        self.assertEqual((pet["kind"], pet["baked"]), ("skinned_renderer", False))
        self.assertEqual((pet["position"], pet["scale"]), ([0.5, 0.0, -0.25], [2.0, 2.0, 2.0]))

    def test_the_last_of_two_values_for_one_field_stays(self):
        self.assertEqual(run("set", self.scene, "--set", "lid.position=1,1,1", "--set", "lid.position=2,2,2")[0], 0)
        text = self.scene.read_text()
        self.assertIn("position = [2.0, 2.0, 2.0]" + chr(10), text)
        self.assertNotIn("position = [1.0, 1.0, 1.0]", text)
        self.assertEqual(text.count("position = [2.0, 2.0, 2.0]"), 1)

    def test_a_missing_value_is_one_added_line_in_its_object(self):
        before = self.lines()
        self.assertEqual(run("set", self.scene, "--set", "lid.scale=1.5,1.5,2")[0], 0)
        after = self.lines()
        added = after.index("scale = [1.5, 1.5, 2.0]\n")
        self.assertEqual(after[:added] + after[added + 1:], before)
        self.assertEqual(after[added - 1], 'parent = "box"\n')

    def test_added_values_take_the_scene_formats_order(self):
        self.assertEqual(run("set", self.scene, "--set", "lid.scale=2,2,2", "--set", "lid.position=0,1,0")[0], 0)
        after = self.lines()
        lid = after.index('name = "lid"\n')
        self.assertEqual(after[lid + 1:lid + 4], ['parent = "box"\n', "position = [0.0, 1.0, 0.0]\n",
                                                  "scale = [2.0, 2.0, 2.0]\n"])

    def test_several_values_in_one_call(self):
        self.assertEqual(run("set", self.scene, "--set", "box.rotation=0,90,0", "--set", "lid.position=0,1,0")[0], 0)
        objects = {item["name"]: item for item in json.loads(run("read", self.scene)[1])["objects"]}
        self.assertEqual(objects["box"]["rotation"], [0.0, 90.0, 0.0])
        self.assertEqual(objects["lid"]["position"], [0.0, 1.0, 0.0])

    def test_windows_line_ends_stay(self):
        self.scene.write_bytes(self.before.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertEqual(run("set", self.scene, "--set", "lid.position=0,1,0")[0], 0)
        text = self.scene.read_bytes()
        self.assertEqual(text.count(b"\n"), text.count(b"\r\n"))
        self.assertIn(b"position = [0.0, 1.0, 0.0]\r\n", text)

    def test_a_baked_renderer_is_refused(self):
        self.assert_refused("ground.position=1,0,0", why="'ground' is baked")

    def test_a_light_and_a_camera_are_refused(self):
        self.assert_refused("sun.rotation=10,30,0", why="'sun' is a light")
        self.assert_refused("camera.position=0,1,0", why="'camera' is a camera")

    def test_one_refusal_writes_none_of_the_call(self):
        self.assert_refused("box.position=9,9,9", "ground.scale=2,2,2", why="'ground' is baked")

    def test_a_result_the_scene_rules_refuse_is_not_written(self):
        self.assert_refused("box.scale=0,1,1", why="scale must be positive")

    def test_a_check_that_cannot_read_the_result_puts_the_file_back(self):
        load = scene_edit.import_settings.load_scene
        with mock.patch.object(scene_edit.import_settings, "load_scene", side_effect=[load(self.scene), OSError("held")]):
            self.assert_refused("box.position=9,9,9", why="held")

    def test_unknown_objects_fields_and_values_are_refused(self):
        self.assert_refused("chair.position=0,0,0", why="no object 'chair'")
        self.assert_refused("box.colour=0,0,0", why="FIELD is one of position, rotation, scale")
        self.assert_refused("box.position=0,0", why="3 numbers")
        self.assert_refused("box.position=0,0,0,0", why="3 numbers")
        self.assert_refused("box.position=0,x,0", why="could not convert")
        self.assert_refused("box=0,0,0", why="expected NAME.FIELD=X,Y,Z")

    def test_only_a_scene_file_is_written(self):
        mesh = self.folder / "mesh.import.toml"
        before = mesh.read_bytes()
        code, _, err = run("set", mesh, "--set", "mesh.position=0,0,0")
        self.assertEqual((code, mesh.read_bytes()), (1, before))
        self.assertIn("only a .scene.toml is written", err)
        backup = self.folder / "scene.scene.toml.bak"
        backup.write_bytes(self.before)
        self.assertEqual(run("set", backup, "--set", "box.position=0,0,0")[0], 1)
        self.assertEqual(backup.read_bytes(), self.before)

    def test_a_missing_or_invalid_scene_is_refused_without_a_traceback(self):
        absent = self.folder / "absent.scene.toml"
        for command in (["read", absent], ["set", absent, "--set", "box.position=0,0,0"]):
            code, out, err = run(*command)
            self.assertEqual((code, out), (1, ""))
            self.assertIn("absent.scene.toml", err)
        self.scene.write_bytes(self.before.replace(b'name = "lid"', b'name = "box"'))
        code, out, err = run("read", self.scene)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("names must be unique", err)


if __name__ == "__main__":
    unittest.main()

"""scene_edit: placements read from a scene, and changed ones written back with nothing else in the file touched."""

import collections
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import tomlkit
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from scene import scene_edit  # noqa: E402
from test_r3d_import import (  # noqa: E402
    FIT, HEAD, NO_LEVELS, PATH, SIMPLIFY, VARIANT, VARIANT_OUTPUT, camera, renderer, sun_object, tree_scenes,
    write_import, write_scene)
from tests.r3d_env import needs_llvm  # noqa: E402

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



def keys_json(*keys):
    """read-keys' JSON for `keys`, each (t, eye, look_at)."""
    return [dict(t=t, eye=eye, look_at=look_at) for t, eye, look_at in keys]

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
                                              locked=None, position=[1.0, 2.0, 3.0], rotation=[0.0] * 3,
                                              scale=[1.0] * 3))
        self.assertEqual(objects["lid"]["parent"], "box")
        self.assertTrue(objects["ground"]["baked"])
        self.assertEqual((objects["sun"]["kind"], objects["sun"]["rotation"]), ("light", [40.0, 30.0, 0.0]))

    def test_read_says_why_set_would_refuse_each_object(self):
        _, out, _ = run("read", self.scene)
        locked = {item["name"]: item["locked"] for item in json.loads(out)["objects"]}
        self.assertEqual([name for name, why in locked.items() if why is None], ["box", "lid", "shelf", "pet"])
        self.assertRegex(locked["ground"], "baked")
        self.assertRegex(locked["sun"], "light")
        self.assertRegex(locked["camera"], "camera")
        # What read says is what set does: each locked object is refused for the reason read gave.
        for name, why in locked.items():
            if why is not None:
                code, _, err = run("set", self.scene, "--set", f"{name}.position=1,2,3")
                self.assertEqual(code, 1)
                self.assertIn(why, err)

    def test_read_writes_its_json_to_a_file_when_asked(self):
        out_path = self.folder / "objects.json"
        code, out, _ = run("read", self.scene, "-o", out_path)
        self.assertEqual((code, out), (0, ""))
        self.assertEqual(json.loads(out_path.read_text(encoding="utf-8")), json.loads(run("read", self.scene)[1]))

    def test_read_into_a_missing_folder_fails_saying_where(self):
        out_path = self.folder / "missing" / "objects.json"
        code, out, err = run("read", self.scene, "-o", out_path)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("objects.json", err)
        self.assertNotIn("Traceback", err)

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


WALK_KEYS = """# A walk round the box.
node = "camera"
animation = "walk"

[[keys]]
t = 0
eye = [0, 1, 5]
look_at = [0, 1, 0]

# Round the corner.
[[keys]]
t = 2.50
eye = [5, 1, 0]   # beside the box
look_at = [0, 1, 0]

[[keys]]
t = 5
eye = [0, 1, -5]
look_at = [0, 1, 0]
"""
WALK_ANIM = 'source = "walk.keys.toml"\nanimation = "walk"\n'
# The walk's keys as set-keys spells them, unchanged.
WALK = ("0:0:0,1,5:0,1,0", "1:2.5:5,1,0:0,1,0", "2:5:0,1,-5:0,1,0")


def camera_object(name, clip=None):
    """A camera named `name` with no region, flying `clip` when given."""
    return camera(path=clip is not None, region=False, name=name, clip=clip)


class KeysEditTests(unittest.TestCase):
    """A scene whose first camera walks a keys path that a baked renderer's visibility keeps, a second camera on a
    .glb clip, and a third on none."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.folder = pathlib.Path(directory.name)
        write_import(self.folder)
        self.keys = self.folder / "walk.keys.toml"
        self.keys.write_text(WALK_KEYS, newline="\n")
        (self.folder / "walk.anim.toml").write_text(WALK_ANIM)
        objects = (renderer(extra="bake = true\n" + PATH) + sun_object() + camera_object("walker", "walk.anim.toml")
                   + camera_object("flyer", "fly.anim.toml") + camera_object("still"))
        self.scene = write_scene(self.folder, objects, HEAD)
        self.before = self.keys.read_bytes()

    def set_keys(self, *keys):
        return run("set-keys", self.keys, *(part for key in keys for part in ("--key", key)))

    def assert_refused(self, *keys, why):
        """`set-keys` with `keys` exits 1 naming `why`, and leaves the folder as it was."""
        listing = sorted(self.folder.iterdir())
        code, _, err = self.set_keys(*keys)
        self.assertEqual(code, 1)
        self.assertRegex(err, why)
        self.assertEqual(self.keys.read_bytes(), self.before)
        self.assertEqual(sorted(self.folder.iterdir()), listing)

    def read_objects(self):
        code, out, err = run("read", self.scene)
        self.assertEqual(code, 0, err)
        return {item["name"]: item for item in json.loads(out)["objects"]}

    def test_read_gives_each_camera_its_path_and_whether_bakes_read_it(self):
        objects = self.read_objects()
        walker, flyer = objects["walker"]["path"], objects["flyer"]["path"]
        self.assertEqual(pathlib.Path(walker["keys"]), self.keys.resolve())
        self.assertEqual(pathlib.Path(walker["anim"]), (self.folder / "walk.anim.toml").resolve())
        self.assertTrue(walker["feeds_bakes"])
        self.assertEqual((flyer["keys"], flyer["feeds_bakes"]), (None, False))
        self.assertIsNone(objects["still"]["path"])
        self.assertNotIn("path", objects["mesh"])

    def test_a_camera_path_no_bake_reads_feeds_no_bakes(self):
        self.scene.write_text(self.scene.read_text().replace(PATH, ""))
        self.assertFalse(self.read_objects()["walker"]["path"]["feeds_bakes"])

    def test_read_keys_lists_the_keys_as_authored(self):
        code, out, _ = run("read-keys", self.keys)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), dict(node="camera", animation="walk", keys=keys_json(
            (0, [0, 1, 5], [0, 1, 0]), (2.5, [5, 1, 0], [0, 1, 0]), (5, [0, 1, -5], [0, 1, 0]))))

    def test_read_keys_writes_its_json_to_a_file_when_asked(self):
        out = self.folder / "keys.json"
        self.assertEqual(run("read-keys", self.keys, "-o", out)[:2], (0, ""))
        self.assertEqual(json.loads(out.read_text())["animation"], "walk")

    def test_the_same_keys_leave_every_byte(self):
        self.assertEqual(self.set_keys(*WALK)[0], 0)
        self.assertEqual(self.keys.read_bytes(), self.before)

    def test_a_moved_eye_changes_only_its_number(self):
        code, _, err = self.set_keys(WALK[0], "1:2.5:6,1,0:0,1,0", WALK[2])
        self.assertEqual(code, 0, err)
        self.assertEqual(self.keys.read_text(), WALK_KEYS.replace("eye = [5, 1, 0]", "eye = [6, 1, 0]"))

    def test_a_fractional_value_is_written_in_its_fewest_digits(self):
        self.assertEqual(self.set_keys(WALK[0], "1:2.25:5,1,0.5:0,1,0", WALK[2])[0], 0)
        text = self.keys.read_text()
        self.assertIn("t = 2.25\n", text)
        self.assertIn("eye = [5, 1, 0.5]   # beside the box\n", text)

    def test_an_added_key_is_a_table_of_its_own_in_place(self):
        self.assertEqual(self.set_keys(WALK[0], "new:1:3,1,3:0,1,0", WALK[1], WALK[2])[0], 0)
        added = "[[keys]]\nt = 1\neye = [3, 1, 3]\nlook_at = [0, 1, 0]\n\n"
        self.assertEqual(self.keys.read_text(), WALK_KEYS.replace("# Round the corner.", added + "# Round the corner."))

    def test_a_key_added_last_follows_the_last(self):
        self.assertEqual(self.set_keys(*WALK, "new:7.5:-5,1,0:0,1,0")[0], 0)
        self.assertEqual(self.keys.read_text(),
                         WALK_KEYS + "\n[[keys]]\nt = 7.5\neye = [-5, 1, 0]\nlook_at = [0, 1, 0]\n")

    def test_a_deleted_last_key_takes_only_its_own_table(self):
        self.assertEqual(self.set_keys(WALK[0], WALK[1])[0], 0)
        self.assertEqual(self.keys.read_text(), WALK_KEYS.split("\n[[keys]]\nt = 5\n")[0])

    def test_deleting_a_key_keeps_the_comment_above_the_next(self):
        self.assertEqual(self.set_keys("1:0:5,1,0:0,1,0", WALK[2])[0], 0)
        self.assertEqual(self.keys.read_text(), WALK_KEYS.replace(
            "[[keys]]\nt = 0\neye = [0, 1, 5]\nlook_at = [0, 1, 0]\n\n", "").replace("t = 2.50", "t = 0"))

    def test_windows_line_ends_stay(self):
        self.keys.write_bytes(self.before.replace(b"\n", b"\r\n"))
        self.assertEqual(self.set_keys(WALK[0], "new:1:3,1,3:0,1,0", "1:2.5:6,1,0:0,1,0", WALK[2])[0], 0)
        text = self.keys.read_bytes()
        self.assertEqual(text.count(b"\n"), text.count(b"\r\n"))
        self.assertIn(b"eye = [3, 1, 3]\r\n", text)
        self.assertIn(b"eye = [6, 1, 0]   # beside the box\r\n", text)

    def test_keys_file_refusals_are_not_written(self):
        self.assert_refused("0:1:0,1,5:0,1,0", WALK[1], why="times must start at zero")
        self.assert_refused(WALK[0], "1:0:5,1,0:0,1,0", why="increase strictly")
        self.assert_refused(WALK[0], why="at least two keys")
        self.assert_refused("0:0:0,1,5:0,1,5", WALK[1], why="look_at must differ from eye")

    def test_malformed_keys_are_refused(self):
        self.assert_refused("0:0:0,1,5", WALK[1], why="expected FROM:T:EX,EY,EZ:LX,LY,LZ")
        self.assert_refused("0:0:0,1:0,1,0", WALK[1], why="3 numbers")
        self.assert_refused("0:x:0,1,5:0,1,0", WALK[1], why="could not convert")
        self.assert_refused(WALK[0], "9:2.5:5,1,0:0,1,0", why="no key 9")
        self.assert_refused(WALK[0], "0:2.5:5,1,0:0,1,0", why="key 0 is named twice")
        self.assert_refused(WALK[0], "one:2.5:5,1,0:0,1,0", why="FROM is a key's index in the file, or new")
        self.assert_refused("0:0:0,1,inf:0,1,0", WALK[1], why="finite")

    def test_only_a_keys_file_is_written(self):
        anim = self.folder / "walk.anim.toml"
        code, _, err = run("set-keys", anim, "--key", WALK[0], "--key", WALK[1])
        self.assertEqual((code, anim.read_text()), (1, WALK_ANIM))
        self.assertIn("only a .keys.toml is written", err)
        self.assertEqual(run("read-keys", anim)[0], 1)

    def test_a_missing_or_invalid_keys_file_is_refused_without_a_traceback(self):
        absent = self.folder / "absent.keys.toml"
        for command in (["read-keys", absent], ["set-keys", absent, "--key", WALK[0], "--key", WALK[1]]):
            code, out, err = run(*command)
            self.assertEqual((code, out), (1, ""))
            self.assertIn("absent.keys.toml", err)
        self.keys.write_text(WALK_KEYS.replace("t = 2.50", "t = 0"), newline="\n")
        code, out, err = run("read-keys", self.keys)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("increase strictly", err)


    def test_a_fit_feeds_bakes_with_probes_or_without(self):
        """A fit takes reference views along the path whenever there is one, probes or not."""
        for body, fit in ((SIMPLIFY + VARIANT, FIT),
                          (SIMPLIFY + "probes = { face = 64 }\n" + VARIANT, FIT.replace("budget = 8\n", ""))):
            with self.subTest(probes="probes" in body):
                write_import(self.folder, output=VARIANT_OUTPUT, body=body)
                fitted = renderer(extra='bake = true\nvariant = "mesh"\n' + fit)
                self.scene.write_text(HEAD + fitted + sun_object() + camera_object("walker", "walk.anim.toml"))
                self.assertTrue(self.read_objects()["walker"]["path"]["feeds_bakes"])

    def test_only_the_scene_camera_path_feeds_bakes(self):
        self.scene.write_text(self.scene.read_text().replace('"fly.anim.toml"', '"walk.anim.toml"'))
        objects = self.read_objects()
        self.assertEqual(objects["flyer"]["path"]["keys"], objects["walker"]["path"]["keys"])
        self.assertEqual((objects["walker"]["path"]["feeds_bakes"], objects["flyer"]["path"]["feeds_bakes"]),
                         (True, False))

    def test_a_clip_file_that_does_not_read_is_refused_without_a_traceback(self):
        (self.folder / "walk.anim.toml").write_text('source = "walk.keys.toml"\n')
        code, out, err = run("read", self.scene)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("walk.anim.toml", err)

    def test_a_deleted_middle_key_takes_the_comment_above_it(self):
        self.assertEqual(self.set_keys(WALK[0], "2:5:0,1,-5:0,1,0")[0], 0)
        self.assertEqual(self.keys.read_text(), WALK_KEYS.replace(
            "# Round the corner.\n[[keys]]\nt = 2.50\neye = [5, 1, 0]   # beside the box\nlook_at = [0, 1, 0]\n\n", ""))

    def test_comments_after_the_last_key_and_on_a_header_stay(self):
        text = WALK_KEYS.replace("[[keys]]\nt = 5", "[[keys]]  # the far side\nt = 5") + "\n# The end.\n"
        self.keys.write_text(text, newline="\n")
        self.assertEqual(self.set_keys(WALK[0], WALK[1], "2:5:0,1,-6:0,1,0", "new:6:1,1,1:0,1,0")[0], 0)
        self.assertEqual(self.keys.read_text(), text.replace("eye = [0, 1, -5]", "eye = [0, 1, -6]").replace(
            "\n# The end.\n", "\n[[keys]]\nt = 6\neye = [1, 1, 1]\nlook_at = [0, 1, 0]\n\n# The end.\n"))

    def test_kept_keys_out_of_the_files_order_carry_their_own_tables(self):
        self.assert_refused(WALK[0], "2:5:0,1,-5:0,1,0", "1:2.5:5,1,0:0,1,0", why="increase strictly")
        self.assertEqual(self.set_keys(WALK[0], "2:2.5:0,1,-5:0,1,0", "1:5:5,1,0:0,1,0")[0], 0)
        self.assertEqual(self.keys.read_text(), WALK_KEYS.split("\n# Round the corner.\n")[0]
                         + "\n[[keys]]\nt = 2.5\neye = [0, 1, -5]\nlook_at = [0, 1, 0]\n"
                         + "\n# Round the corner.\n[[keys]]\nt = 5\neye = [5, 1, 0]   # beside the box\nlook_at = [0, 1, 0]\n")

    def test_one_call_adds_deletes_and_edits(self):
        self.assertEqual(self.set_keys("0:0:0,2,5:0,1,0", "new:1:3,1,3:0,1,0", "2:5:0,1,-5:1,1,0")[0], 0)
        self.assertEqual(json.loads(run("read-keys", self.keys)[1])["keys"], keys_json(
            (0, [0, 2, 5], [0, 1, 0]), (1, [3, 1, 3], [0, 1, 0]), (5, [0, 1, -5], [1, 1, 0])))
        self.assertNotIn("Round the corner", self.keys.read_text())

    def test_a_changed_time_keeps_its_line_comment(self):
        self.keys.write_text(WALK_KEYS.replace("t = 2.50", "t = 2.50  # halfway"), newline="\n")
        self.assertEqual(self.set_keys(WALK[0], "1:3:5,1,0:0,1,-1", WALK[2])[0], 0)
        text = self.keys.read_text()
        self.assertIn("t = 3  # halfway\n", text)
        self.assertIn("look_at = [0, 1, -1]\n", text)

    def test_a_time_spelled_otherwise_but_equal_is_not_rewritten(self):
        self.assertEqual(self.set_keys(WALK[0], "1:2.500:5,1,0:0,1,0", WALK[2])[0], 0)
        self.assertEqual(self.keys.read_bytes(), self.before)

    def test_a_key_added_last_to_a_file_with_no_final_line_end(self):
        self.keys.write_text(WALK_KEYS.rstrip("\n"), newline="\n")
        self.assertEqual(self.set_keys(*WALK, "new:7.5:-5,1,0:0,1,0")[0], 0)
        self.assertEqual(self.keys.read_text(),
                         WALK_KEYS + "\n[[keys]]\nt = 7.5\neye = [-5, 1, 0]\nlook_at = [0, 1, 0]\n")

    def test_keys_written_as_an_inline_array_are_refused(self):
        self.keys.write_text('node = "camera"\nanimation = "walk"\nkeys = [\n'
                             '    { t = 0, eye = [0, 1, 5], look_at = [0, 1, 0] },\n'
                             '    { t = 5, eye = [0, 1, -5], look_at = [0, 1, 0] },\n]\n', newline="\n")
        self.before = self.keys.read_bytes()
        self.assert_refused("0:0:0,1,4:0,1,0", "1:5:0,1,-5:0,1,0", why=r"\[\[keys\]\] tables")

    def test_odd_numbers_and_froms_are_refused(self):
        for key, why in (("0:0:0,1,nan:0,1,0", "finite"), ("0:0:0,,5:0,1,0", "could not convert"),
                         ("²:0:0,1,5:0,1,0", "FROM is"), ("-1:0:0,1,5:0,1,0", "FROM is"), ("", "expected")):
            with self.subTest(key=key):
                self.assert_refused(key, WALK[1], why=why)

    def test_a_keys_file_named_in_capitals_is_read_and_written(self):
        capitals = self.keys.rename(self.folder / "walk.KEYS.TOML")
        self.assertEqual(run("read-keys", capitals)[0], 0)
        code, _, err = run("set-keys", capitals, "--key", WALK[0], "--key", "1:2.5:6,1,0:0,1,0", "--key", WALK[2])
        self.assertEqual(code, 0, err)
        self.assertIn("eye = [6, 1, 0]", capitals.read_text())

    def test_a_quoted_keys_header_is_a_table_too(self):
        for quote in ('"', "'"):
            with self.subTest(quote=quote):
                self.keys.write_text(WALK_KEYS.replace("[[keys]]\nt = 5", f"[[{quote}keys{quote}]]\nt = 5"),
                                     newline="\n")
                self.assertEqual(self.set_keys(WALK[0], WALK[1], "2:5:0,1,-6:0,1,0")[0], 0)
                self.assertIn(f"[[{quote}keys{quote}]]\nt = 5\neye = [0, 1, -6]\n", self.keys.read_text())

    def test_a_header_inside_a_multi_line_string_is_refused(self):
        self.keys.write_text(WALK_KEYS.replace('animation = "walk"', 'animation = """walk\n[[keys]]\n"""'),
                             newline="\n")
        self.before = self.keys.read_bytes()
        self.assert_refused("0:0:0,1,4:0,1,0", WALK[1], WALK[2], why=r"\[\[keys\]\] tables")

    def test_a_spaced_keys_header_is_a_table_too(self):
        self.keys.write_text(WALK_KEYS.replace("[[keys]]\nt = 5", "[[ keys ]]\nt = 5"), newline="\n")
        self.assertEqual(self.set_keys(WALK[0], WALK[1], "2:5:0,1,-6:0,1,0")[0], 0)
        self.assertIn("[[ keys ]]\nt = 5\neye = [0, 1, -6]\n", self.keys.read_text())


class RecordedPoses:
    """bake.Poses as stage_keys asks it, recording which cameras' paths a bake samples."""

    def __init__(self):
        self.cameras = []

    def digest(self, camera, every_ms, size, tools, unavailable=None):
        self.cameras.append(camera)
        return "poses"

    def reason(self, unavailable):
        return ""


class FeedsBakesTreeTests(unittest.TestCase):
    @needs_llvm
    def test_feeds_bakes_is_true_exactly_where_a_bake_samples_the_path(self):
        """On every scene in the tree whose camera has a path, read's feeds_bakes says whether any of its bakes'
        keys samples that path (bake.stage_keys asking Poses for it): the rule stays the bake's."""
        from bake import bake

        checked = 0
        for path in tree_scenes():
            scene = scene_edit.import_settings.load_scene(path)
            if scene.camera is None or scene.camera.component.path is None:
                continue
            with self.subTest(scene=path.name):
                poses = RecordedPoses()
                for job in scene.renderers:
                    bake.stage_keys(job, scene, collections.defaultdict(str), None, poses)
                sampled = any(camera is scene.camera.component for camera in poses.cameras)
                self.assertEqual(scene_edit.path_baked(scene), sampled)
                checked += sampled
        self.assertGreater(checked, 0, "no scene in the tree has a path a bake reads")


if __name__ == "__main__":
    unittest.main()

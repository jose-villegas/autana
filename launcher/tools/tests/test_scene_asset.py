"""The scene entry (r3d/scene_asset.py): what it writes reads back as the
scene file says, what it refuses to read, and the pack build_pack makes of a
scene. The firmware's reader of the same bytes is suite_scene.c."""

import pathlib
import struct
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import tracks_asset  # noqa: E402
from anim_probe import probe_entry, probe_glb  # noqa: E402
from asset.asset_pack import parse_pack  # noqa: E402
from r3d import build_pack, scene_asset  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402
from r3d.mesh_asset import TYPE as LIT_MESH  # noqa: E402
from r3d.scene_asset import CAMERA, HEADER, NAME, RENDERER, TRANSFORM, SceneError  # noqa: E402
from test_r3d_import import HEAD, renderer, sun_object, write_import  # noqa: E402

CLIP = 'source = "probe.glb"\nanimation = "clip"\n'


def lens(path=None, extra=""):
    """A camera object; `path` is the .anim.toml it flies, relative to the scene."""
    flight = f'path = {{ animation = "{path}", node = "lamp" }}\n' if path else ""
    return ('[[objects]]\nname = "camera"\n[objects.camera]\nhalf_fov_short_tan = 0.6\nnear_z = 1.5\n'
            + extra + flight)


def single(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


class SceneFiles(unittest.TestCase):
    """Two imports with their meshes, and a clip in clips/, in a scratch folder."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        for mesh in ("a", "b"):
            write_import(self.root, f"{mesh}.import.toml", output=f'[output]\ndirectory = "."\nname = "{mesh}"\n')
            (self.root / f"{mesh}.mesh").write_bytes(mesh.encode())
        (self.root / "clips").mkdir()
        (self.root / "clips" / "probe.glb").write_bytes(probe_glb())
        (self.root / "clips" / "fly.anim.toml").write_text(CLIP)

    def tearDown(self):
        self.temp.cleanup()

    def scene(self, objects, head="", name="hall.scene.toml"):
        path = self.root / name
        path.write_text(head + objects)
        return path

    def entry(self, objects, head=""):
        return scene_asset.bake(self.scene(objects, head))


class SceneEntryTests(SceneFiles):
    def test_multiple_cameras_and_their_clips_share_the_scene_pack(self):
        (self.root / "clips" / "tour.anim.toml").write_text(CLIP)
        second = lens("clips/tour.anim.toml").replace('name = "camera"', 'name = "tour"')
        path = self.scene(renderer("a.import.toml") + renderer("b.import.toml") + lens("clips/fly.anim.toml") + second)
        loaded = load_scene(path)
        self.assertEqual(loaded.camera.name, "camera")
        decoded = scene_asset.decode(scene_asset.bake(path))
        self.assertEqual([c["clip"] for c in decoded["cameras"]], ["fly", "tour"])
        self.assertEqual([decoded["entities"][c["entity"]]["name"] for c in decoded["cameras"]], ["camera", "tour"])
        packs = build_pack.pack_bytes([self.root], tree=True)
        self.assertEqual(list(packs), ["hall"])
        self.assertEqual(set(parse_pack(packs["hall"])), {"hall", "a", "b", "fly", "tour"})

    def test_a_region_on_a_secondary_camera_is_refused(self):
        second = lens(extra='region = { min = [-1, -1, -1], max = [1, 1, 1] }\n').replace('name = "camera"', 'name = "tour"')
        with self.assertRaisesRegex(SettingsError, "first camera"):
            load_scene(self.scene(renderer("a.import.toml") + lens() + second))

    def test_entities_renderers_and_the_camera_read_back_in_file_order(self):
        # The first entity is the only one that moved, so an array reversed or shifted is caught.
        moved = renderer("a.import.toml", transform="position = [1.0, 2.0, 3.0]\nscale = [2.0, 2.0, 0.5]\n")
        flying = lens("clips/fly.anim.toml", "background = 0x336699\n")
        scene = scene_asset.decode(self.entry(moved + renderer("b.import.toml") + flying))
        self.assertEqual([e["name"] for e in scene["entities"]], ["a", "b", "camera"])
        self.assertEqual(scene["entities"][0]["matrix"], [[2.0, 0.0, 0.0], [0.0, 2.0, 0.0], [0.0, 0.0, 0.5]])
        self.assertEqual(scene["entities"][0]["position"], [1.0, 2.0, 3.0])
        self.assertEqual([e["position"] for e in scene["entities"][1:]], [[0.0, 0.0, 0.0]] * 2)
        self.assertEqual(scene["renderers"], [{"entity": 0, "mesh": "a"}, {"entity": 1, "mesh": "b"}])
        self.assertEqual(scene["cameras"], [{"entity": 2, "half_fov_short_tan": single(0.6), "near_z": 1.5,
                                             "clear_rgb": 0x336699, "clip": "fly", "node": "lamp"}])

    def test_a_camera_without_a_path_has_no_clip_and_clears_to_black(self):
        scene = scene_asset.decode(self.entry(renderer("a.import.toml") + lens()))
        self.assertEqual((scene["cameras"][0]["clip"], scene["cameras"][0]["node"]), ("", ""))
        self.assertEqual(scene["cameras"][0]["clear_rgb"], 0)

    def test_a_placement_is_baked_as_rotation_times_scale(self):
        quarter = "rotation = [0.0, 90.0, 0.0]\nscale = [1.0, 2.0, 3.0]\n"
        matrix = scene_asset.decode(self.entry(renderer("a.import.toml", transform=quarter)))["entities"][0]["matrix"]
        for row, want in zip(matrix, ((0.0, 0.0, 3.0), (0.0, 2.0, 0.0), (-1.0, 0.0, 0.0))):
            self.assertTrue(all(abs(a - b) < 1e-6 for a, b in zip(row, want)), matrix)

    def test_a_light_has_no_entity_and_the_indices_step_over_it(self):
        objects = (renderer("a.import.toml", extra="bake = true\n") + sun_object()
                   + renderer("b.import.toml", extra="bake = true\n") + lens())
        scene = scene_asset.decode(self.entry(objects, HEAD))
        self.assertEqual([e["name"] for e in scene["entities"]], ["a", "b", "camera"])
        self.assertEqual(scene["renderers"], [{"entity": 0, "mesh": "hall.a"}, {"entity": 1, "mesh": "hall.b"}])

    def test_the_entry_holds_only_what_the_device_reads(self):
        objects = renderer("a.import.toml", extra="bake = true\n") + sun_object() + lens()
        entry = self.entry(objects, HEAD)
        size = HEADER.size + 2 * (NAME.size + TRANSFORM.size) + RENDERER.size + CAMERA.size
        self.assertEqual(len(entry), size)
        self.assertNotIn(b"sun", entry)

    def test_the_same_scene_bakes_the_same_bytes(self):
        objects = renderer("a.import.toml") + lens("clips/fly.anim.toml")
        self.assertEqual(self.entry(objects), self.entry(objects))

    def test_a_camera_path_names_an_existing_anim_file_and_a_short_node(self):
        for path, pattern in (("clips/gone.anim.toml", "is not an .anim.toml file"),
                              ("clips/probe.glb", "is not an .anim.toml file")):
            with self.subTest(path=path), self.assertRaisesRegex(SettingsError, pattern):
                load_scene(self.scene(renderer("a.import.toml") + lens(path)))
        long_node = lens("clips/fly.anim.toml").replace('node = "lamp"', 'node = "' + "n" * 20 + '"')
        with self.assertRaisesRegex(SettingsError, "track names exceed"):
            load_scene(self.scene(renderer("a.import.toml") + long_node))
        with self.assertRaisesRegex(SettingsError, "letters, digits and _"):
            load_scene(self.scene(renderer("a.import.toml") + lens("clips/fly.anim.toml").replace('"lamp"', '"no-good"')))

    def test_a_clip_id_must_fit_a_pack_name(self):
        for stem, refused in (("c" * 31, False), ("c" * 32, True)):
            (self.root / "clips" / f"{stem}.anim.toml").write_text(CLIP)
            path = self.scene(renderer("a.import.toml") + lens(f"clips/{stem}.anim.toml"))
            with self.subTest(length=len(stem)):
                if refused:
                    with self.assertRaisesRegex(SettingsError, "exceeds the pack's 31-byte limit"):
                        load_scene(path)
                else:
                    self.assertEqual(load_scene(path).camera.component.path.clip, stem)

    def test_a_name_too_long_for_its_field_is_refused_by_the_writer_itself(self):
        scene = load_scene(self.scene(renderer("a.import.toml")))
        scene.renderers[0].asset_name = "m" * 32
        with self.assertRaisesRegex(SceneError, "31-byte field"):
            scene_asset.encode(scene)

    def test_an_object_name_must_fit_the_entry(self):
        with self.assertRaisesRegex(SettingsError, "letters, digits and _"):
            load_scene(self.scene(renderer("a.import.toml", name="not a name")))
        with self.assertRaisesRegex(SettingsError, "at most 31"):
            load_scene(self.scene(renderer("a.import.toml", name="n" * 32)))


class SceneEntryRefusalTests(unittest.TestCase):
    """decode() refuses what scene_asset_open() refuses: each edit below keeps the size."""

    def entry(self):
        entities = NAME.pack(b"camera") + NAME.pack(b"quad")
        transforms = TRANSFORM.pack(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 10) + TRANSFORM.pack(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)
        renderers = RENDERER.pack(1, 0, b"quad")
        cameras = CAMERA.pack(0, 0, 1.0, 1.0, 0x336699, b"fly", b"camera")
        names_at = HEADER.size
        transforms_at = names_at + len(entities)
        renderers_at = transforms_at + len(transforms)
        cameras_at = renderers_at + len(renderers)
        return bytearray(HEADER.pack(scene_asset.VERSION, 2, 1, 1, names_at, transforms_at, renderers_at, cameras_at)
                         + entities + transforms + renderers + cameras)

    def refuses(self, edit, pattern):
        entry = self.entry()
        edit(entry)
        with self.assertRaisesRegex(SceneError, pattern):
            scene_asset.decode(bytes(entry))

    def test_the_fixture_reads(self):
        scene = scene_asset.decode(bytes(self.entry()))
        self.assertEqual(scene["cameras"][0]["clip"], "fly")

    def test_a_short_entry_or_another_version_is_refused(self):
        with self.assertRaisesRegex(SceneError, "shorter"):
            scene_asset.decode(bytes(self.entry()[:HEADER.size - 1]))
        self.refuses(lambda e: struct.pack_into("<H", e, 0, 2), "version 2")

    def test_a_section_leaving_the_entry_or_misaligned_is_refused(self):
        for field, value in ((0, 9999), (4, 2), (8, len(self.entry())), (12, HEADER.size - 4)):
            with self.subTest(field=field):
                self.refuses(lambda e: struct.pack_into("<I", e, 8 + field, value), "leave the entry or are misaligned")

    def test_a_name_without_its_nul_or_with_bytes_after_it_is_refused(self):
        names = HEADER.size
        self.refuses(lambda e: e.__setitem__(slice(names, names + 32), b"x" * 32), "NUL")
        self.refuses(lambda e: e.__setitem__(names + 20, ord("x")), "NUL")

    def test_a_renderer_naming_no_entity_or_with_padding_in_use_is_refused(self):
        at = HEADER.size + 2 * (NAME.size + TRANSFORM.size)
        self.refuses(lambda e: struct.pack_into("<H", e, at, 2), "out of range")
        self.refuses(lambda e: struct.pack_into("<H", e, at + 2, 1), "out of range")

    def test_a_camera_with_a_bad_lens_or_a_clip_without_a_node_is_refused(self):
        at = HEADER.size + 2 * (NAME.size + TRANSFORM.size) + RENDERER.size
        self.refuses(lambda e: struct.pack_into("<f", e, at + 4, 0.0), "does not allow")
        self.refuses(lambda e: struct.pack_into("<f", e, at + 8, float("nan")), "does not allow")
        self.refuses(lambda e: struct.pack_into("<I", e, at + 12, 0x1000000), "does not allow")
        self.refuses(lambda e: e.__setitem__(slice(at + 48, at + 80), bytes(32)), "does not allow")
        self.refuses(lambda e: e.__setitem__(slice(at + 16, at + 48), bytes(32)), "does not allow")
        self.refuses(lambda e: struct.pack_into("<H", e, at, 2), "does not allow")
        self.refuses(lambda e: struct.pack_into("<H", e, at + 2, 1), "does not allow")


class ScenePackTests(SceneFiles):
    """build_pack: a scene's pack holds its entry, its meshes and its camera's clip."""

    def test_the_pack_holds_the_entry_every_mesh_and_the_clip_which_makes_no_pack_of_its_own(self):
        path = self.scene(renderer("a.import.toml") + renderer("b.import.toml") + lens("clips/fly.anim.toml"))
        packs = {name: parse_pack(pack) for name, pack in build_pack.pack_bytes([self.root], tree=True).items()}
        self.assertEqual(sorted(packs), ["hall"])
        hall = packs["hall"]
        self.assertEqual(hall["hall"], (scene_asset.TYPE, scene_asset.bake(path)))
        self.assertEqual({name: kind for name, (kind, _) in hall.items()},
                         {"hall": scene_asset.TYPE, "a": LIT_MESH, "b": LIT_MESH, "fly": tracks_asset.TYPE})
        self.assertEqual(hall["fly"][1], probe_entry())

    def test_replace_takes_only_a_mesh(self):
        self.scene(renderer("a.import.toml") + lens("clips/fly.anim.toml"))
        (self.root / "scratch.mesh").write_bytes(b"9")
        packs = build_pack.pack_bytes([self.root], [f"a={self.root / 'scratch.mesh'}"], tree=True)
        self.assertEqual(parse_pack(packs["hall"])["a"], (LIT_MESH, b"9"))
        for entry in ("hall", "fly"):
            with self.subTest(entry=entry), self.assertRaisesRegex(SettingsError, "no such mesh"):
                build_pack.pack_bytes([self.root], [f"{entry}={self.root / 'scratch.mesh'}"], tree=True)

    def test_a_scene_and_its_clip_cannot_share_an_id_and_the_refusal_names_both_files(self):
        (self.root / "clips" / "hall.anim.toml").write_text(CLIP)
        self.scene(renderer("a.import.toml") + lens("clips/hall.anim.toml"))
        with self.assertRaisesRegex(SettingsError, r"hall\.scene\.toml and .*hall\.anim\.toml both make entry 'hall'"):
            build_pack.pack_files([self.root])

    def test_two_scenes_flying_one_clip_are_refused_until_shared_packs_exist(self):
        flying = renderer("a.import.toml", name="x") + lens("clips/fly.anim.toml")
        self.scene(flying, name="one.scene.toml")
        self.scene(flying.replace('mesh = "a.import.toml"', 'mesh = "b.import.toml"'), name="two.scene.toml")
        with self.assertRaisesRegex(SettingsError, "'fly' is named by packs"):
            build_pack.pack_files([self.root])


if __name__ == "__main__":
    unittest.main()

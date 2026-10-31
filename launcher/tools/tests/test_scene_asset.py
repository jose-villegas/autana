"""The scene entry (r3d/scene_asset.py): what it writes reads back as the
scene file says, what it refuses to read, and the pack build_pack makes of a
scene. The firmware's reader of the same bytes is suite_scene.c."""

import pathlib
import math
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "tests"))

from anim import tracks_asset  # noqa: E402
from anim_probe import probe_entry, probe_glb  # noqa: E402
from asset.asset_pack import NAME_BYTES, parse_pack  # noqa: E402
from r3d import build_pack, scene_asset  # noqa: E402
from asset.engine_frame import to_engine  # noqa: E402
from build.name_hash import NONE as HASH_NONE, name_hash  # noqa: E402
from test_engine_frame import mesh_entry  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402
from r3d.mesh_asset import TYPE as LIT_MESH  # noqa: E402
from r3d.scene_asset import CAMERA, HEADER, NAME, RENDERER, TRANSFORM, SceneError  # noqa: E402
from unittest import mock
from skin_probe import rig
from test_r3d_import import HEAD, renderer, sun_object, write_import  # noqa: E402

CLIP = 'source = "probe.glb"\nanimation = "clip"\n'


def lens(path=None, extra=""):
    """A camera object; `path` is the .anim.toml it flies, relative to the scene."""
    flight = f'path = {{ animation = "{path}", node = "camera" }}\n' if path else ""
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
            write_import(self.root, f"{mesh}.import.toml", output=f'[output]\nname = "{mesh}"\n')
            (self.root / f"{mesh}.mesh").write_bytes(mesh_entry())
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
    def test_header_size_mismatch_raises_scene_error(self):
        owner_size = HEADER.size + 1
        with self.assertRaises(SceneError) as raised:
            scene_asset.check_header(owner_size)
        for value in (str(HEADER.size), str(owner_size), str(scene_asset.OWNER)):
            self.assertIn(value, str(raised.exception))

    def test_host_scene_with_a_mesh_encodes_a_renderer_row(self):
        from scene import scene_host
        entry = scene_host.one_entity_scene("mesh")
        self.assertGreater(len(entry), len(scene_host.one_entity_scene()))

    def test_skinned_renderer_forwards_cache_and_offline(self):
        import_file = write_import(self.root, "rig.import.toml", source='[source]\npath = "rig.glb"\ncredit = "test"\n')
        objects = renderer("rig.import.toml").replace("mesh_renderer", "skinned_renderer")
        scene = load_scene(self.scene(objects))
        document, binary = rig()
        cache = self.root / "cache"
        with mock.patch("r3d.skin_pack.load_rig", return_value=(import_file, document, binary, 5)) as load:
            scene_asset.encode(scene, cache, True)
            load.assert_called_once_with(scene.renderers[0].settings, cache, True)

    def test_multiple_cameras_and_their_clips_share_the_scene_pack(self):
        (self.root / "clips" / "tour.anim.toml").write_text(CLIP)
        second = lens("clips/tour.anim.toml").replace('name = "camera"', 'name = "tour"')
        path = self.scene(renderer("a.import.toml") + renderer("b.import.toml") + lens("clips/fly.anim.toml") + second)
        loaded = load_scene(path)
        self.assertEqual(loaded.camera.name, "camera")
        decoded = scene_asset.decode(scene_asset.bake(path))
        self.assertEqual(decoded["clips"], ["fly", "tour"])
        self.assertEqual(decoded["animators"], [dict(entity=entity, clip_count=1, first_clip=first,
                        default_clip=0, wrap=scene_asset.WRAPS["loop"]) for entity, first in ((2, 0), (3, 1))])
        self.assertEqual([decoded["entities"][c["entity"]]["name"] for c in decoded["cameras"]], ["camera", "tour"])
        packs = build_pack.pack_bytes([self.root])
        self.assertEqual(list(packs), ["hall"])
        self.assertEqual(set(parse_pack(packs["hall"])), {"hall", "a", "b", "fly", "tour"})

    def test_skinned_renderer_and_animator_are_authored(self):
        import_file = write_import(self.root, "rig.import.toml", source='[source]\npath = "rig.glb"\ncredit = "test"\n')
        objects = (renderer("rig.import.toml").replace("mesh_renderer", "skinned_renderer")
                   + '[objects.animator]\nclips = ["move"]\ndefault_clip = "move"\nwrap = "clamp"\n' + sun_object().replace('color = [1.0, 1.0, 1.0]', 'color = [0.25, 0.5, 0.75]')
                   .replace('intensity = 1.0', 'intensity = 2.5'))
        document, binary = rig()
        with mock.patch("r3d.skin_pack.load_rig", return_value=(import_file, document, binary, 5)):
            entry = self.entry(objects, 'tonemap_white = 0.3\n[ambient]\ncolor = [0.4, 0.5, 0.6]\nintensity = 0.2\n')
            decoded = scene_asset.decode(entry)
        self.assertEqual(decoded["renderers"], [])
        self.assertEqual(decoded["skinned_renderers"][0], dict(entity=0, mesh="hall.rig", skin="hall.rig.skin", skeleton="armature"))
        self.assertEqual(decoded["animators"][0]["wrap"], scene_asset.WRAPS["clamp"])
        self.assertEqual(decoded["clips"], ["move"])
        self.assertEqual(decoded["lights"][0], dict(entity=1, kind=scene_asset.DIRECTIONAL,
                         color=[0.25, 0.5, 0.75], intensity=2.5))
        self.assertEqual(decoded["settings"], dict(ambient_color=[single(v) for v in (0.4, 0.5, 0.6)],
                         ambient_intensity=single(0.2), tonemap_white=single(0.3)))
        with self.assertRaisesRegex(SettingsError, "takes no bake"):
            load_scene(self.scene(objects.replace('mesh = "rig.import.toml"', 'mesh = "rig.import.toml"\nbake = false')))
        for value in ("default_clip = \"missing\"", "wrap = \"unknown\""):
            setting = "default_clip" if value.startswith("default") else "wrap"
            bad = objects.replace('default_clip = "move"' if setting == "default_clip" else 'wrap = "clamp"', value)
            with self.assertRaisesRegex(SettingsError, setting):
                load_scene(self.scene(bad))

    def test_import_refusals_name_the_rule(self):
        import_file = write_import(self.root, "rig.import.toml",
            source='[source]\npath = "rig.blend"\ncredit = "test"\nclips = ["idle", "walk"]\n')
        skin = renderer("rig.import.toml").replace("mesh_renderer", "skinned_renderer")
        cases = [(skin + sun_object().replace('color = [1.0, 1.0, 1.0]', 'color = [1.1, 0.5, 0.5]'), '', 'color'),
                 (skin + sun_object().replace('intensity = 1.0', 'intensity = -1.0'), '', 'intensity'),
                 (skin, 'tonemap_white = -1.0\n', 'tonemap_white'),
                 (skin, '[sky]\ncolor = [1.0, 1.0, 1.0]\nintensity = 1.0\nrays = 1\n', 'only baked renderers'),
                 (renderer("a.import.toml") + '[objects.animator]\nclips = ["idle"]\n', '', 'requires a skinned_renderer'),
                 (skin + '[objects.animator]\nclips = ["missing"]\n', '', 'unique import source.clips'),
                 (skin + '[objects.animator]\nclips = ["idle", "idle"]\n', '', 'unique import source.clips')]
        for objects, head, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(SettingsError, message):
                load_scene(self.scene(objects, head))
        import_file.write_text(import_file.read_text().replace('["idle", "walk"]', '["idle", "idle"]'))
        with self.assertRaisesRegex(SettingsError, "duplicate clip names"):
            load_scene(self.scene(skin))

    def test_camera_node_without_tracks_and_shared_clip_different_nodes_are_refused(self):
        path = self.root / "clips/fly.anim.toml"
        with self.assertRaisesRegex(tracks_asset.TracksError, "has no property tracks"):
            tracks_asset.bake(path, "absent")
        second = lens("clips/fly.anim.toml").replace('name = "camera"', 'name = "other"').replace('node = "camera"', 'node = "other"')
        self.scene(renderer("a.import.toml") + lens("clips/fly.anim.toml") + second)
        with self.assertRaisesRegex(SettingsError, "selects different nodes"):
            build_pack.pack_bytes([self.root])

    def test_skin_import_modes_and_named_refusal(self):
        from r3d.import_settings import load_import_settings
        for mode in ("direct", "renormalised", "table"):
            path = write_import(self.root, body=f'[skin]\nlighting = "{mode}"\n')
            self.assertEqual(load_import_settings(path).skin_lighting, mode)
        path = write_import(self.root, body='[skin]\nlighting = "unknown"\n')
        with self.assertRaisesRegex(SettingsError, "mesh.import.toml.*direct, renormalised, table"):
            load_import_settings(path)

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
        self.assertEqual(scene["renderers"], [{"entity": 0, "mesh": "a", "colour_set": scene_asset.COLOUR_SET_OWN}, {"entity": 1, "mesh": "b", "colour_set": scene_asset.COLOUR_SET_OWN}])
        self.assertEqual(scene["cameras"], [{"entity": 2, "half_fov_short_tan": single(0.6), "near_z": 1.5,
                                             "clear_rgb": 0x336699}])

    def test_authored_colour_set_is_encoded_and_checks_its_range(self):
        scene = scene_asset.decode(self.entry(renderer("a.import.toml", extra="colour_set = 7\n")))
        self.assertEqual(scene["renderers"][0]["colour_set"], 7)
        own = scene_asset.decode(self.entry(renderer("a.import.toml", extra=f"colour_set = {scene_asset.COLOUR_SET_OWN}\n")))
        self.assertEqual(own["renderers"][0]["colour_set"], scene_asset.COLOUR_SET_OWN)
        for value in ("-1", str(scene_asset.COLOUR_SET_OWN + 1), "true", "1.5", '\"own\"'):
            with self.subTest(value=value), self.assertRaises(SettingsError):
                load_scene(self.scene(renderer("a.import.toml", extra=f"colour_set = {value}\n")))

    def test_a_camera_without_a_path_has_no_clip_and_clears_to_black(self):
        scene = scene_asset.decode(self.entry(renderer("a.import.toml") + lens()))
        self.assertEqual(scene["animators"], [])
        self.assertEqual(scene["settings"], dict(ambient_color=list(scene_asset.DEFAULT_COLOR),
                         ambient_intensity=scene_asset.AMBIENT_NONE, tonemap_white=scene_asset.TONEMAP_NONE))
        self.assertEqual(scene["cameras"][0]["clear_rgb"], 0)

    def test_a_placement_is_baked_as_rotation_times_scale(self):
        quarter = "rotation = [0.0, 90.0, 0.0]\nscale = [1.0, 2.0, 3.0]\n"
        matrix = scene_asset.decode(self.entry(renderer("a.import.toml", transform=quarter)))["entities"][0]["matrix"]
        for row, want in zip(matrix, ((0.0, 0.0, 3.0), (0.0, 2.0, 0.0), (-1.0, 0.0, 0.0))):
            self.assertTrue(all(abs(a - b) < 1e-6 for a, b in zip(row, want)), matrix)

    def test_a_light_is_an_entity_with_its_authored_values(self):
        objects = (renderer("a.import.toml", extra="bake = true\n") + sun_object()
                   + renderer("b.import.toml", extra="bake = true\n") + lens())
        scene = scene_asset.decode(self.entry(objects, HEAD))
        self.assertEqual([e["name"] for e in scene["entities"]], ["a", "sun", "b", "camera"])
        self.assertEqual(scene["renderers"], [{"entity": 0, "mesh": "hall.a", "colour_set": scene_asset.COLOUR_SET_OWN}, {"entity": 2, "mesh": "hall.b", "colour_set": scene_asset.COLOUR_SET_OWN}])

    def test_the_entry_holds_only_what_the_device_reads(self):
        objects = renderer("a.import.toml", extra="bake = true\n") + sun_object() + lens()
        entry = self.entry(objects, HEAD)
        parents = -(-3 * scene_asset.PARENT.size // scene_asset.ALIGN) * scene_asset.ALIGN
        size = (HEADER.size + 3 * (NAME.size + TRANSFORM.size) + RENDERER.size + CAMERA.size + scene_asset.LIGHT.size
                + scene_asset.SETTINGS.size + parents)
        self.assertEqual(len(entry), size)
        self.assertIn(b"sun", entry)

    def test_the_same_scene_bakes_the_same_bytes(self):
        objects = renderer("a.import.toml") + lens("clips/fly.anim.toml")
        self.assertEqual(self.entry(objects), self.entry(objects))

    def test_a_camera_path_names_an_existing_anim_file_and_a_valid_node(self):
        for path, pattern in (("clips/gone.anim.toml", "is not an .anim.toml file"),
                              ("clips/probe.glb", "is not an .anim.toml file")):
            with self.subTest(path=path), self.assertRaisesRegex(SettingsError, pattern):
                load_scene(self.scene(renderer("a.import.toml") + lens(path)))
        node = "n" * (NAME_BYTES - 1)
        long_node = lens("clips/fly.anim.toml").replace('node = "camera"', 'node = "' + node + '"')
        scene = load_scene(self.scene(renderer("a.import.toml") + long_node))
        self.assertEqual(scene.camera.component.path.node, node)
        with self.assertRaisesRegex(SettingsError, "at most"):
            load_scene(self.scene(renderer("a.import.toml") +
                       long_node.replace(node, "n" * NAME_BYTES)))
        with self.assertRaisesRegex(SettingsError, "letters, digits and _"):
            load_scene(self.scene(renderer("a.import.toml") + lens("clips/fly.anim.toml").replace('node = "camera"', 'node = "no-good"')))

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

    def test_each_name_row_stores_its_name_hash(self):
        entry = self.entry(renderer("a.import.toml") + renderer("b.import.toml") + lens())
        names_at = HEADER.unpack_from(entry)[1 + len(scene_asset.PARTS)]
        for index, name in enumerate(("a", "b", "camera")):
            hashed, raw = NAME.unpack_from(entry, names_at + index * NAME.size)
            self.assertEqual((hashed, raw.rstrip(b"\0")), (name_hash(name), name.encode()))
        self.assertEqual([(e["name"], e["name_hash"]) for e in scene_asset.decode(entry)["entities"]],
                         [(name, name_hash(name)) for name in ("a", "b", "camera")])

    def test_names_that_share_a_hash_or_hash_to_none_are_refused(self):
        self.assertEqual(name_hash("costarring"), name_hash("liquid"))
        objects = renderer("a.import.toml", name="costarring") + renderer("b.import.toml", name="liquid")
        with self.assertRaisesRegex(SceneError, "'costarring' and 'liquid' share a name hash"):
            self.entry(objects)
        self.assertEqual(name_hash("akhnp9x"), HASH_NONE)
        with self.assertRaisesRegex(SceneError, "'akhnp9x' hashes to NONE"):
            self.entry(renderer("a.import.toml", name="akhnp9x"))

    def test_an_object_name_must_fit_the_entry(self):
        with self.assertRaisesRegex(SettingsError, "letters, digits and _"):
            load_scene(self.scene(renderer("a.import.toml", name="not a name")))
        with self.assertRaisesRegex(SettingsError, "at most 31"):
            load_scene(self.scene(renderer("a.import.toml", name="n" * 32)))


def authored_entry():
    parts = [
        NAME.pack(name_hash("first"), b"first") + NAME.pack(name_hash("second"), b"second"),
        TRANSFORM.pack(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0) * 2,
        RENDERER.pack(0, 0, b"mesh"),
        CAMERA.pack(1, 0, 0.5, 0.1, 0),
        b"".join(scene_asset.LIGHT.pack(e, scene_asset.DIRECTIONAL, 0, 0.8, 0.7, 0.6, 1.5) for e in range(2)),
        b"".join(scene_asset.SKINNED_RENDERER.pack(e, 0, b"mesh", b"mesh.skin", b"rig") for e in range(2)),
        b"".join(scene_asset.ANIMATOR.pack(e, 1, e, 0, scene_asset.WRAPS["loop"], 0) for e in range(2)),
        scene_asset.CLIP.pack(b"idle") + scene_asset.CLIP.pack(b"walk"),
        scene_asset.SETTINGS.pack(0.1, 0.2, 0.3, 0.4, 0.5),
        scene_asset.PARENT.pack(scene_asset.PARENT_ROOT) * 2]
    counts = [len(part) // row.size for part, row in zip(parts, scene_asset.ROWS)]
    offsets, at = [], HEADER.size
    for part in parts:
        offsets.append(at)
        at += len(part)
    return bytearray(HEADER.pack(scene_asset.VERSION, *counts, *offsets) + b"".join(parts))


class SceneEntryRefusalTests(unittest.TestCase):
    def test_authored_rows_round_trip(self):
        decoded = scene_asset.decode(authored_entry())
        self.assertEqual(decoded["clips"], ["idle", "walk"])
        self.assertEqual(decoded["skinned_renderers"][1], dict(entity=1, mesh="mesh", skin="mesh.skin", skeleton="rig"))
        self.assertEqual(decoded["animators"][1], dict(entity=1, clip_count=1, first_clip=1, default_clip=0,
                                                      wrap=scene_asset.WRAPS["loop"]))
        self.assertAlmostEqual(decoded["settings"]["tonemap_white"], 0.5)

    def test_a_stored_name_hash_must_be_the_names_and_not_none(self):
        names_at = HEADER.unpack_from(authored_entry())[1 + len(scene_asset.PARTS)]
        for what, at, value in (("another hash", names_at, struct.pack("<I", name_hash("first") ^ 1)),
                                ("another name", names_at + struct.calcsize("<I"), b"girst"),
                                ("none", names_at + NAME.size, NAME.pack(HASH_NONE, b"akhnp9x"))):
            entry = authored_entry()
            entry[at:at + len(value)] = value
            with self.subTest(what), self.assertRaisesRegex(SceneError, "stored name hash"):
                scene_asset.decode(entry)

    def test_v1_and_truncation_are_refused(self):
        entry = authored_entry()
        struct.pack_into("<H", entry, 0, 1)
        with self.assertRaisesRegex(SceneError, "version 1"):
            scene_asset.decode(entry)
        with self.assertRaises(SceneError):
            scene_asset.decode(authored_entry()[:HEADER.size - 1])

    def test_every_authored_refusal(self):
        original = authored_entry()
        offsets = HEADER.unpack_from(original)[1 + len(scene_asset.PARTS):]
        cases = [("lights", 2, "B", 0), ("lights", 2, "B", 2), ("lights", 2, "B", 255), ("lights", 3, "B", 1),
                 ("animators", 4, "<I", 2), ("animators", 4, "<I", (1 << 32) - 1), ("animators", 8, "<H", 1),
                 ("animators", 10, "B", 2), ("animators", 10, "B", 255), ("animators", 11, "B", 1),
                 ("skinned_renderers", 2, "<H", 1), ("cameras", 2, "<H", 1)]
        for part in ("lights", "animators", "skinned_renderers"):
            row = scene_asset.ROWS[scene_asset.PARTS.index(part)]
            cases.append((part, row.size, "<H", 0))
            cases.append((part, 0, "<H", 2))
        for value in (-0.1, 1.1, math.nan, math.inf):
            for component in range(3):
                cases.append(("lights", 4 + component * 4, "<f", value))
                cases.append(("settings", component * 4, "<f", value))
        for value in (-1, math.nan, math.inf):
            cases.extend((part, offset, "<f", value) for part, offset in
                         (("lights", 16), ("settings", 12), ("settings", 16)))
        for part, offset, fmt, value in cases:
            message = (part + ": entity" if fmt == "<H" and offset in (0, scene_asset.ROWS[scene_asset.PARTS.index(part)].size)
                       else "animator: invalid" if part == "animators"
                       else "light: invalid" if part == "lights"
                       else "settings: invalid" if part == "settings" else part + ": padding")
            with self.subTest(part=part, offset=offset, value=value), self.assertRaisesRegex(SceneError, message):
                entry = bytearray(original)
                struct.pack_into(fmt, entry, offsets[scene_asset.PARTS.index(part)] + offset, value)
                scene_asset.decode(entry)
        entry = bytearray(original)
        animator_at = offsets[scene_asset.PARTS.index("animators")]
        struct.pack_into("<H", entry, animator_at + 8, scene_asset.DEFAULT_CLIP_NONE)
        self.assertEqual(scene_asset.decode(entry)["animators"][0]["default_clip"], scene_asset.DEFAULT_CLIP_NONE)


    def test_counts_and_skin_ids_are_strict(self):
        original = authored_entry()
        for part, counts in (("transforms", (1, 3)), ("parents", (1, 3)), ("settings", (0, 2))):
            for count in counts:
                entry = bytearray(original)
                struct.pack_into("<H", entry, 2 + 2 * scene_asset.PARTS.index(part), count)
                with self.subTest(part=part, count=count), self.assertRaisesRegex(SceneError, "must match names"):
                    scene_asset.decode(entry)
        parents = HEADER.unpack_from(original)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("parents")]
        for row, parent, message in ((0, 0, "not listed before it"), (0, 1, "not listed before it"),
                                     (1, 1, "not listed before it"), (1, 0, "skinned_renderers: entity 1 has a parent")):
            entry = bytearray(original)
            struct.pack_into("<H", entry, parents + row * scene_asset.PARENT.size, parent)
            with self.subTest(row=row, parent=parent), self.assertRaisesRegex(SceneError, message):
                scene_asset.decode(entry)
        entry = bytearray(original)
        entry[2 * (1 + len(scene_asset.PARTS))] = 1
        with self.assertRaisesRegex(SceneError, "header's padding"):
            scene_asset.decode(entry)
        skin = HEADER.unpack_from(original)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("skinned_renderers")]
        for field, name in enumerate(("mesh", "skin", "skeleton")):
            for raw in (b"x" * NAME_BYTES, bytes(NAME_BYTES)):
                entry = bytearray(original)
                at = skin + 4 + field * NAME_BYTES
                entry[at:at + NAME_BYTES] = raw
                with self.subTest(name=name, raw=raw), self.assertRaisesRegex(SceneError, name + " id is not NUL"):
                    scene_asset.decode(entry)


class SceneParentTests(unittest.TestCase):
    """An object's `parent` names an entity listed before it; cameras, lights and skins stay roots."""

    @staticmethod
    def objects(*rows):
        return [SimpleNamespace(name=name, kind=kind, parent=parent, position=(0, 0, 0),
                                matrix=((1, 0, 0), (0, 1, 0), (0, 0, 1))) for name, kind, parent in rows]

    def test_a_parent_listed_first_round_trips(self):
        objects = self.objects(("variant", "empty", None), ("copy", "mesh_renderer", "variant"))
        entry = scene_asset.encode(SimpleNamespace(objects=objects, renderers=[]))
        self.assertEqual([e["parent"] for e in scene_asset.decode(entry)["entities"]], [None, 0])

    @staticmethod
    def placed(name, kind, parent, matrix, position=(0, 0, 0)):
        return SimpleNamespace(name=name, kind=kind, parent=parent, position=position, matrix=matrix)

    def test_a_parents_uneven_scale_folds_into_its_children(self):
        objects = [self.placed("variant", "empty", None, ((2, 0, 0), (0, 1, 0), (0, 0, 1))),
                   self.placed("copy", "mesh_renderer", "variant", ((1, 0, 0), (0, 0, -1), (0, 1, 0)), (1, 1, 0))]
        entities = scene_asset.decode(scene_asset.encode(SimpleNamespace(objects=objects, renderers=[])))["entities"]
        self.assertEqual(entities[0]["matrix"], [[1, 0, 0], [0, 1, 0], [0, 0, 1]])
        self.assertEqual(entities[1]["matrix"], [[2, 0, 0], [0, 0, -1], [0, 1, 0]])
        self.assertEqual(entities[1]["position"], [2, 1, 0])

    def test_an_uneven_parent_that_draws_or_would_shear_is_refused(self):
        turned = math.sqrt(0.5)
        stretched = ((2, 0, 0), (0, 1, 0), (0, 0, 1))
        for parent_kind, child_matrix, message in (
                ("mesh_renderer", ((1, 0, 0), (0, 1, 0), (0, 0, 1)), "a parent that draws"),
                ("empty", ((turned, -turned, 0), (turned, turned, 0), (0, 0, 1)), "would shear")):
            objects = [self.placed("variant", parent_kind, None, stretched),
                       self.placed("copy", "empty", "variant", child_matrix)]
            renderers = [SimpleNamespace(object=objects[0], asset_name="m")] if parent_kind == "mesh_renderer" else []
            with self.subTest(message=message), self.assertRaisesRegex(SceneError, message):
                scene_asset.encode(SimpleNamespace(objects=objects, renderers=renderers))

    def test_the_reader_refuses_an_uneven_or_mirrored_parent(self):
        objects = self.objects(("variant", "empty", None), ("copy", "empty", "variant"))
        original = scene_asset.encode(SimpleNamespace(objects=objects, renderers=[]))
        transforms = HEADER.unpack_from(original)[1 + len(scene_asset.PARTS) + scene_asset.PARTS.index("transforms")]
        for x_scale in (2.0, -1.0):
            entry = bytearray(original)
            struct.pack_into("<f", entry, transforms, x_scale)
            with self.subTest(x_scale=x_scale), self.assertRaisesRegex(SceneError, "scale is not even or mirrors"):
                scene_asset.decode(entry)

    def test_a_mirrored_child_round_trips_and_a_mirrored_parent_camera_or_light_is_refused(self):
        mirror = ((-1, 0, 0), (0, 1, 0), (0, 0, 1))
        objects = [self.placed("variant", "empty", None, ((1, 0, 0), (0, 1, 0), (0, 0, 1))),
                   self.placed("copy", "mesh_renderer", "variant", mirror)]
        entities = scene_asset.decode(scene_asset.encode(SimpleNamespace(objects=objects, renderers=[])))["entities"]
        self.assertEqual(entities[1]["matrix"], [list(row) for row in mirror])
        for objects, message in (
                ([self.placed("variant", "empty", None, mirror), self.placed("copy", "empty", "variant", mirror)],
                 "a parent cannot mirror"),
                ([self.placed("eye", "camera", None, mirror)], "a camera cannot mirror"),
                ([self.placed("sun", "light", None, mirror)], "a light cannot mirror")):
            with self.subTest(message=message), self.assertRaisesRegex(SceneError, message):
                scene_asset.encode(SimpleNamespace(objects=objects, renderers=[]))

    def test_the_padding_after_an_odd_count_of_parents_is_zero(self):
        entry = bytearray(scene_asset.encode(SimpleNamespace(objects=self.objects(("one", "empty", None)), renderers=[])))
        self.assertEqual(len(entry) % scene_asset.ALIGN, 0)
        entry[-1] = 1
        with self.assertRaisesRegex(SceneError, "padding after the parents"):
            scene_asset.decode(entry)

    def test_a_parent_after_its_child_or_on_a_camera_is_refused(self):
        for rows, message in (((("copy", "empty", "variant"), ("variant", "empty", None)), "listed before it"),
                              ((("variant", "empty", None), ("sun", "light", "variant")), "a light has no parent"),
                              ((("copy", "empty", "copy"),), "listed before it"),
                              ((("copy", "empty", "gone"),), "listed before it"),
                              ((("variant", "empty", None), ("eye", "camera", "variant")), "a camera has no parent")):
            with self.subTest(rows=rows), self.assertRaisesRegex(SceneError, message):
                scene_asset.encode(SimpleNamespace(objects=self.objects(*rows), renderers=[]))


class ScenePackTests(SceneFiles):
    """build_pack: a scene's pack holds its entry, its meshes and its camera's clip."""

    def test_the_pack_holds_the_entry_every_mesh_and_the_clip_which_makes_no_pack_of_its_own(self):
        path = self.scene(renderer("a.import.toml") + renderer("b.import.toml") + lens("clips/fly.anim.toml"))
        packs = {name: parse_pack(pack) for name, pack in build_pack.pack_bytes([self.root]).items()}
        self.assertEqual(sorted(packs), ["hall"])
        hall = packs["hall"]
        self.assertEqual(hall["hall"], (scene_asset.TYPE, to_engine(scene_asset.TYPE, scene_asset.bake(path))))
        self.assertEqual({name: kind for name, (kind, _) in hall.items()},
                         {"hall": scene_asset.TYPE, "a": LIT_MESH, "b": LIT_MESH, "fly": tracks_asset.TYPE})
        self.assertEqual(hall["fly"][1], to_engine(tracks_asset.TYPE, tracks_asset.bake(self.root / "clips/fly.anim.toml", "camera")))

    def test_replace_takes_only_a_mesh(self):
        self.scene(renderer("a.import.toml") + lens("clips/fly.anim.toml"))
        (self.root / "scratch.mesh").write_bytes(mesh_entry((9, 3, -7)))
        packs = build_pack.pack_bytes([self.root], [f"a={self.root / 'scratch.mesh'}"])
        self.assertEqual(parse_pack(packs["hall"])["a"], (LIT_MESH, to_engine(LIT_MESH, mesh_entry((9, 3, -7)))))
        for entry in ("hall", "fly"):
            with self.subTest(entry=entry), self.assertRaisesRegex(SettingsError, "no such mesh"):
                build_pack.pack_bytes([self.root], [f"{entry}={self.root / 'scratch.mesh'}"])

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

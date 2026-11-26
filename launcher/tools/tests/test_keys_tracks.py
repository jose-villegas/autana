"""Track-form keys baked to TRCK and sampled by the firmware's track sampler."""
import hashlib
import math
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_camera_keys import TOOLS, keys_file, clip_of, has_compiler, track_host, tracks_asset
from anim import scene_fields
from bake import bake
from gltf import gltf_read


DEMO_GLBS = {
    "bistro/street.keys.toml": "8d83bf46ad9e2de73596315a35f868d25ec98a015a51e73810fd03b493e0e93c",
    "butterfly/swing.keys.toml": "4306d088112b474d7085b9a114e647857a033fcc4834ee6037f6db8b1580978e",
    "sponza/lion.keys.toml": "268d4e4fed63adb0872d4ce975b90f6cebef5cd087d23dadb9df2683de201f3e",
    "sponza/tour.keys.toml": "c0212a918d291fc4a85e148228c3bb5f153525b790543e0e4a5ebec3c832fb17",
    "stress/culled.keys.toml": "f4d573b5f159a103d80010593b662dd853ffd53684680d981edae31bd4c2f8ac",
    "stress/near.keys.toml": "ec04853e10848426e1b09b71182614327dddc2ca1909575bc785b8f6344a63a3",
    "stress/orbit.keys.toml": "60981138e6df1a35cf5644a60a81d277d2e16456baf164b2b3ee6e1c0e758156",
    "stress_ring/sweep.keys.toml": "3e26e6fa8b1b5ce6da9601a440613972ad2c479d0a0fdd28722dd3fb25995b3b",
}

INTERPOLATIONS = (("step", False), ("linear", False), ("cubic", False), ("cubic", True))


def component(root, tag, declarations="ANIM_FIELD(test_t, amount, ANIM_VALUE_VEC2)"):
    (root / f"{tag}.h").write_text(f"#define COMPONENT_{tag} ASSET_TYPE({', '.join(map(repr, tag))})\n")
    path = root / f"{tag}.c"
    path.write_text(f'''#include "{tag}.h"
static const anim_field_t FIELDS[] = {{ {declarations} }};
const anim_component_fields_t COMPONENT = {{ COMPONENT_{tag}, FIELDS, sizeof FIELDS / sizeof FIELDS[0] }};
''')
    return path


def decoded_curve(field, interp, keys, **extra):
    track = dict(object="prop", field=field, interp=interp, keys=keys, **extra)
    doc, binary = gltf_read.parse_glb(keys_file.build(dict(animation="path", tracks=[track])))
    entry = tracks_asset.encode(*tracks_asset.clip_tracks(doc, binary, doc["animations"][0]))
    tracks, _ = tracks_asset.decode(entry)
    return tracks[0]


class KeysTracksTestCase(unittest.TestCase):
    """Each test gets its own scratch folder as `self.root`."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)

    def assertClose(self, got, want, rel=1e-6, abs_tol=1e-12):
        """Each component of `got` within `rel` of `want`, or within `abs_tol` of it."""
        got, want = list(got), list(want)
        self.assertEqual(len(got), len(want), (got, want))
        for a, b in zip(got, want):
            self.assertTrue(math.isclose(a, b, rel_tol=rel, abs_tol=abs_tol), (got, want))


class TrackKeysBakeTests(KeysTracksTestCase):
    def test_demo_camera_glbs_are_byte_identical(self):
        demo = TOOLS.parent / "demo"
        actual = {p.relative_to(demo).as_posix(): hashlib.sha256(keys_file.load_glb_bytes(p)).hexdigest()
                  for p in demo.rglob("*.keys.toml")}
        self.assertEqual(actual, DEMO_GLBS)

    def test_track_keys_round_trip(self):
        example = TOOLS.parent / "test/anim_examples/path.keys.toml"
        clip = clip_of(self.root, example.read_text(encoding="utf-8"))
        doc, binary = gltf_read.load_asset(self.root / "path.keys.toml")
        self.assertEqual([node["name"] for node in doc["nodes"]], ["prop", "camera"])
        channels = gltf_read.read_animation(doc, binary, doc["animations"][0])
        self.assertEqual([channel["path"] for channel in channels], ["translation", "scale", "pointer"])
        self.assertEqual(channels[-1]["pointer"], "/nodes/1/extensions/AUTANA_scene_fields/CAMR/half_fov_short_tan")
        self.assertIn("AUTANA_scene_fields", doc["extensionsUsed"])
        self.assertEqual(doc["nodes"][1]["extensions"]["AUTANA_scene_fields"]["CAMR"]["half_fov_short_tan"], 0.25)
        tracks, duration = tracks_asset.decode(tracks_asset.bake(clip))
        self.assertEqual(duration, 3000)
        self.assertEqual([(t["name"], t["interpolation"]) for t in tracks], [
            ("prop:TRNS.position", "CUBICSPLINE"), ("prop:TRNS.scale", "STEP"),
            ("camera:CAMR.half_fov_short_tan", "LINEAR")])
        self.assertEqual(tracks[0]["times"], [0, 1, 3])
        self.assertEqual([v[0] for v in tracks[0]["values"]], [3, 0, 3, 2, 3, 2, 1.5, 6, 1.5])
        self.assertEqual([v[1] for v in tracks[0]["values"]], [0, 2, 0] * 3)
        self.assertEqual(tracks[1]["values"], [(1, 1, 1), (2, 3, 4)])
        self.assertEqual(tracks[2]["values"], [(0.25,), (0.75,)])

    @unittest.skipUnless(has_compiler(), "needs sh and a C compiler")
    def test_track_offsets_commute_with_host_sampling(self):
        h = math.sqrt(0.5)

        def sample(interp, explicit, with_offset):
            text = 'animation = "path"\n'
            for field, vectors, offset in (
                ("position", [[1, 2, 3], [3, -1, 6], [0, 4, 1]],
                 f'position = [7, 11, 13], rotation = [0, 0, {h}, {h}]'),
                ("rotation", [[0, 0, 0, 1], [-h, 0, 0, -h], [0, h, 0, h]],
                 f'position = [1, 2, 3], rotation = [0, {h}, 0, {h}]'),
            ):
                text += f'\n[[tracks]]\nobject = "blue"\nfield = "{field}"\ninterp = "{interp}"\n'
                if with_offset:
                    text += f"offset = {{ {offset} }}\n"
                keys = []
                for time, vector in zip((0, 1, 3), vectors):
                    tangents = ""
                    if explicit:
                        sign = -1 if field == "rotation" and time == 1 else 1
                        tangents = f", in = {[sign * 0.25] * len(vector)}, out = {[sign * -0.5] * len(vector)}"
                    keys.append(f"{{ t = {time}, v = {vector}{tangents} }}")
                text += "keys = [" + ", ".join(keys) + "]\n"
            clip = clip_of(self.root, text)
            return [line.split() for line in
                    track_host.sample(clip, ["--every", 23, "--until", 3013, "--clamp"]).splitlines()]

        for interp, explicit in INTERPOLATIONS:
            with self.subTest(interp=interp, explicit=explicit):
                plain, baked = sample(interp, explicit, False), sample(interp, explicit, True)
                self.assertEqual(len(baked), len(plain))
                self.assertGreaterEqual(len(plain), 200)
                for (time, name, *value), (baked_time, baked_name, *got) in zip(plain, baked):
                    self.assertEqual((time, name), (baked_time, baked_name))
                    if name.endswith(".position"):
                        x, y, z = map(float, value)
                        want = [7 - y, 11 + x, 13 + z]
                    else:
                        x, y, z, w = map(float, value)
                        want = [h * (x + z), h * (y + w), h * (z - x), h * (w - y)]
                    self.assertClose(map(float, got), want, rel=4e-7, abs_tol=1e-6)

    def test_invalid_track_keys_name_the_track_and_key(self):
        for change, label, key, message in (
            ({"interp": "smooth"}, "blue.position", 0, "interp"),
            ({"interp": []}, "blue.position", 0, "interp"),
            ({"object": ""}, ".position", 0, "object"),
            ({"field": "missing"}, "blue.missing", 0, "field"),
            ({"keys": []}, "blue.position", 0, "keys"),
            ({"keys": [1]}, "blue.position", 0, "key"),
            ({"extra": "unexpected"}, "blue.position", 0, "unexpected"),
            ({"keys": [{"t": 1, "v": [0, 0, 0]}]}, "blue.position", 0, "zero"),
            ({"keys": [{"t": 0, "v": [0, 0, 0]}, {"t": 0, "v": [0, 0, 0]}]}, "blue.position", 1, "strictly"),
            ({"keys": [{"t": 0, "v": [0, 0, 0]}, {"t": 1e-50, "v": [0, 0, 0]}]}, "blue.position", 1, "strictly"),
            ({"keys": [{"t": float("nan"), "v": [0, 0, 0]}]}, "blue.position", 0, "finite"),
            ({"keys": [{"t": 0, "v": [0, 0]}]}, "blue.position", 0, "3"),
            ({"keys": [{"t": 0, "v": [0, True, 0]}]}, "blue.position", 0, "finite"),
            ({"keys": [{"t": 0, "v": [0, float("inf"), 0]}]}, "blue.position", 0, "finite"),
            ({"keys": [{"t": 0, "v": [0, 1e100, 0]}]}, "blue.position", 0, "float32"),
            ({"field": "rotation", "keys": [{"t": 0, "v": [0, 0, 0, 2]}]}, "blue.rotation", 0, "unit"),
            ({"field": "scale", "offset": {}}, "blue.scale", 0, "offset"),
            ({"offset": []}, "blue.position", 0, "offset"),
            ({"offset": {"rotation": [0, 0, 0, 0]}}, "blue.position", 0, "unit"),
            ({"offset": {"position": [0, 0]}}, "blue.position", 0, "3"),
            ({"keys": [{"t": 0, "v": [0, 0, 0], "in": [0, 0, 0]}]}, "blue.position", 0, "both"),
            ({"keys": [{"t": 0, "v": [0, 0, 0], "in": [0, 0, 0], "out": [0, 0, 0]}]}, "blue.position", 0, "cubic"),
            ({"interp": "cubic", "keys": [{"t": 0, "v": [0, 0, 0]},
                                         {"t": 1, "v": [1, 0, 0], "in": [0, 0, 0], "out": [0, 0, 0]}]},
             "blue.position", 1, "every key"),
            ({"interp": "cubic", "keys": [{"t": 0, "v": [0, 0, 0], "in": [0, 0], "out": [0, 0, 0]}]},
             "blue.position", 0, "3"),
        ):
            track = dict(object="blue", field="position", interp="linear", keys=[{"t": 0, "v": [0, 0, 0]}])
            track.update(change)
            with self.subTest(change=change), \
                    self.assertRaisesRegex(ValueError, rf"{re.escape(label)}.*key {key}.*{message}"):
                keys_file.build(dict(animation="path", tracks=[track]))

    def test_invalid_track_keys_tables(self):
        track = dict(object="blue", field="position", interp="linear", keys=[{"t": 0, "v": [0, 0, 0]}])
        for values, message in (
            (dict(animation="path", tracks=[track], node="blue"), "expected animation and tracks"),
            (dict(animation="", tracks=[track]), "animation"),
            (dict(animation="path", tracks=[]), "tracks"),
            (dict(animation="path", tracks=[track, dict(track)]), r"blue.position.*key 0.*duplicate"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                keys_file.build(values)


class SceneFieldTests(KeysTracksTestCase):
    def test_track_field_discovery(self):
        main = TOOLS.parent / "main"
        header = (main / "anim/anim_track.h").read_text()
        names = re.findall(r"^    (ANIM_VALUE_\w+),", header, re.M)
        self.assertEqual(scene_fields.VALUE_TYPES, dict(zip(names, range(len(names)))))
        self.assertEqual(scene_fields.WIDTHS,
                         tuple(map(int, re.search(r"ANIM_VALUE_WIDTHS\s+\{([^}]+)", header)[1].split(","))))
        headers = "\n".join(path.read_text(encoding="utf-8") for path in main.rglob("*.h"))
        tags = {name: int.from_bytes("".join(re.findall(r"'(.)'", value)).encode(), "little")
                for name, value in re.findall(r"#define (ANIM_COMPONENT_\w+)\s+ASSET_TYPE\(([^)]+)\)", headers)}
        field_pattern = r"ANIM_FIELD(_STEP)?\(\w+,\s*(\w+),\s*(ANIM_VALUE_\w+)\)"
        field_lists = dict(re.findall(r"^#define[ \t]+(\w+)[ \t]+([^\n]+)", headers.replace("\\" + "\n", " "), re.M))
        expected = {}
        for path in main.rglob("*.c"):
            source = path.read_text(encoding="utf-8")
            for tag, table in re.findall(r"anim_component_fields_t\s+\w+\s*=\s*\{\s*(ANIM_COMPONENT_\w+),\s*(\w+),",
                                         source):
                body = re.search(rf"anim_field_t\s+{table}\[\]\s*=\s*\{{([^}}]+)\}}", source)[1].strip()
                entries = re.findall(field_pattern, field_lists.get(body, body))
                self.assertTrue(entries, table)
                expected.update({(tags[tag], field): (names.index(kind), bool(step))
                                 for step, field, kind in entries})
        self.assertTrue(expected)
        self.assertEqual(scene_fields.fields(), expected)

    def test_discovery_reads_a_field_list_macro(self):
        path = component(self.root, "TEST", "TEST_FIELD_LIST")
        header = self.root / "TEST.h"
        header.write_text(header.read_text() + "#define TEST_FIELD_LIST \\\n"
                          "    ANIM_FIELD(test_t, amount, ANIM_VALUE_VEC2), \\\n"
                          "    ANIM_FIELD_STEP(test_t, enabled, ANIM_VALUE_FLOAT)\n")
        tag = int.from_bytes(b"TEST", "little")
        self.assertEqual(scene_fields.discover(self.root), {
            (tag, "amount"): (scene_fields.VALUE_VEC2, False),
            (tag, "enabled"): (scene_fields.VALUE_FLOAT, True),
        })
        path.write_text(path.read_text().replace("TEST_FIELD_LIST", "MISSING_FIELD_LIST"))
        with self.assertRaisesRegex(ValueError, "unsupported animation field declaration"):
            scene_fields.discover(self.root)

    def test_fields_resolve_by_component_and_pointer_node(self):
        component(self.root, "TEST", """ANIM_FIELD(test_t, amount, ANIM_VALUE_VEC2),
        ANIM_FIELD(test_t, other, ANIM_VALUE_FLOAT)""")
        component(self.root, "NEXT", "ANIM_FIELD(test_t, amount, ANIM_VALUE_FLOAT)")
        custom = scene_fields.discover(self.root)
        real = scene_fields.fields()
        self.assertEqual(len(custom), 3)
        first = 'animation = "path"\n'
        tracks = '''
[[tracks]]
object = "prop"
field = "position"
interp = "step"
keys = [{t = 0, v = [0, 0, 0]}]

[[tracks]]
object = "prop"
field = "TEST.amount"
interp = "cubic"
keys = [{t = 0, v = [1, 2], in = [3, 4], out = [5, 6]}]

[[tracks]]
object = "prop"
field = "other"
interp = "step"
keys = [{t = 0, v = 7}]

[[tracks]]
object = "prop"
field = "NEXT.amount"
interp = "linear"
keys = [{t = 0, v = 8}]
'''
        with mock.patch.object(scene_fields, "fields", lambda: real | custom):
            with self.assertRaisesRegex(tracks_asset.TracksError, r'amount.*field = "TEST\.amount"'):
                tracks_asset.bake(clip_of(self.root, first + tracks.replace('"TEST.amount"', '"amount"')))
            clip = clip_of(self.root, first + tracks)
            doc, binary = gltf_read.load_asset(self.root / "path.keys.toml")
            self.assertEqual(len(doc["nodes"]), 1)
            self.assertEqual(set(doc["extensionsUsed"]), {"AUTANA_scene_fields", "KHR_animation_pointer"})
            self.assertEqual(doc["nodes"][0]["extensions"]["AUTANA_scene_fields"], {
                "TEST": {"amount": [1, 2], "other": 7}, "NEXT": {"amount": 8}})
            channels = gltf_read.read_animation(doc, binary, doc["animations"][0])
            self.assertEqual(channels[0]["node"], 0)
            self.assertTrue(all(c["pointer"].startswith("/nodes/0/") for c in channels[1:]))
            rows, _ = tracks_asset.decode(tracks_asset.bake(clip))
            self.assertEqual([r["name"] for r in rows],
                             ["prop:TRNS.position", "prop:TEST.amount", "prop:TEST.other", "prop:NEXT.amount"])
            self.assertEqual(rows[1]["values"], [(3, 4), (1, 2), (5, 6)])
            for bad in ("NOPE/amount", "TEST/missing"):
                with self.subTest(bad=bad), \
                        self.assertRaisesRegex(tracks_asset.TracksError, "unknown scene component or field"):
                    tracks_asset.channel_binding(
                        doc, dict(channels[1], pointer="/nodes/0/extensions/AUTANA_scene_fields/" + bad))

    def test_encode_refuses_a_field_on_the_wrong_component(self):
        track = decoded_curve("position", "linear", [dict(t=0, v=[1, 2, 3])])
        track["component"] = scene_fields.CAMERA
        with self.assertRaisesRegex(tracks_asset.TracksError, "unknown component or field"):
            tracks_asset.encode([track], 0)

    def test_yfov_requires_its_camera_field(self):
        doc = {"nodes": [{"name": "camera", "camera": 0}], "cameras": [{}]}
        channel = {"pointer": "/cameras/0/perspective/yfov"}
        component(self.root, "CAMR", "ANIM_FIELD(test_t, renamed_fov, ANIM_VALUE_FLOAT)")
        with mock.patch.object(scene_fields, "fields", lambda: scene_fields.discover(self.root)), \
                self.assertRaisesRegex(tracks_asset.TracksError, tracks_asset.YFOV_FIELD):
            tracks_asset.channel_binding(doc, channel)

    def test_colour_rotation_field_round_trips_and_samples_linearly(self):
        component(self.root, "TEST", "ANIM_FIELD(test_t, rotation, ANIM_VALUE_COLOUR)")
        fields = scene_fields.fields() | scene_fields.discover(self.root)
        keys = '''
animation = "path"
[[tracks]]
object = "prop"
field = "TEST.rotation"
interp = "linear"
keys = [{t = 0, v = [0, 0.5, 1]}, {t = 2, v = [0.5, 1, 0]}]
'''
        with mock.patch.object(scene_fields, "fields", lambda: fields):
            clip = clip_of(self.root, keys)
            doc, binary = gltf_read.load_asset(self.root / "path.keys.toml")
            channel, = gltf_read.read_animation(doc, binary, doc["animations"][0])
            rows, _ = tracks_asset.decode(tracks_asset.bake(clip))
            self.assertEqual(rows[0]["type"], scene_fields.VALUE_COLOUR)
            self.assertEqual(rows[0]["values"], [(0, 0.5, 1), (0.5, 1, 0)])
            with self.assertRaisesRegex(ValueError, "needs its binding type"):
                gltf_read.is_rotation(channel)
            self.assertEqual(tracks_asset.sample(rows[0], 1), (0.25, 0.75, 0.5))
            clip_of(self.root, keys.replace("[0.5, 1, 0]", "[0.5, 1]"))
            with self.assertRaisesRegex(tracks_asset.TracksError, r"prop.TEST.rotation key 1.*3 components"):
                tracks_asset.bake(clip)

    def test_discovery_refuses_unreadable_declarations(self):
        for before, after, message in (
            ("ANIM_VALUE_VEC2", "ANIM_VALUE_BAD", "unknown animation value type"),
            ("{ COMPONENT_TEST,", "{ MISSING_TAG,", "cannot resolve.*MISSING_TAG"),
            ("COMPONENT_TEST, FIELDS,", "COMPONENT_TEST, MISSING_TABLE,", "cannot resolve.*MISSING_TABLE"),
            ("{ COMPONENT_TEST,", "{ .component = COMPONENT_TEST,", "component.*initializer"),
            ("test_t, amount,", "test_t, nested.amount,", "field.*declaration"),
            ("ANIM_VALUE_VEC2)", "ANIM_VALUE_VEC2), ANIM_FIELD(test_t, amount, ANIM_VALUE_VEC2)", "duplicate.*amount"),
        ):
            path = component(self.root, "TEST")
            path.write_text(path.read_text().replace(before, after))
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                scene_fields.discover(self.root)

    def test_discovery_ignores_comments_and_refuses_conflicting_tags(self):
        component(self.root, "TEST", """ANIM_FIELD(test_t, amount, ANIM_VALUE_VEC2),
        ANIM_FIELD_STEP(test_t, enabled, ANIM_VALUE_FLOAT)
        // ANIM_FIELD(test_t, amount, ANIM_VALUE_BAD)
        /* ANIM_FIELD(test_t, amount, ANIM_VALUE_BAD) */""")
        tag = int.from_bytes(b"TEST", "little")
        self.assertEqual(scene_fields.discover(self.root), {
            (tag, "amount"): (scene_fields.VALUE_VEC2, False),
            (tag, "enabled"): (scene_fields.VALUE_FLOAT, True),
        })
        (self.root / "other.h").write_text("#define COMPONENT_TEST ASSET_TYPE('N', 'E', 'X', 'T')\n")
        with self.assertRaisesRegex(ValueError, "conflicting definitions of COMPONENT_TEST"):
            scene_fields.discover(self.root)

    def test_discovered_fields_are_bake_inputs(self):
        path = component(self.root, "TEST")
        module = TOOLS / "anim/scene_fields.py"

        def inputs():
            scene_fields.fields.cache_clear()
            return bake.native_inputs([module])

        self.addCleanup(scene_fields.fields.cache_clear)
        with mock.patch.object(scene_fields, "MAIN", self.root):
            before = inputs()
            (self.root / "unrelated.c").write_text("const int value = 42;\n")
            path.write_text(path.read_text() + "\nconst int unrelated = 7;\n")
            self.assertEqual(inputs(), before)
            path.write_text(path.read_text().replace("ANIM_VALUE_VEC2", "ANIM_VALUE_FLOAT"))
            self.assertNotEqual(inputs(), before)

    def test_import_does_not_scan_scene_fields(self):
        script = """
from pathlib import Path
original = Path.rglob

def rglob(path, *args, **kwargs):
    assert path.name != 'main', 'field discovery ran at import'
    return original(path, *args, **kwargs)

Path.rglob = rglob
from anim import tracks_asset
"""
        result = subprocess.run([sys.executable, "-c", script], cwd=TOOLS, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


class DecodedCurveTests(KeysTracksTestCase):
    def test_baked_rotation_sign_alignment(self):
        h = math.sqrt(0.5)
        raw = [(0, 0, 0, 1), (-h, 0, 0, -h), (-1, 0, 0, 0), (0, 1, 0, 0)]
        times = [0, 1, 3, 4]
        incoming, outgoing = (1, 2, 3, 4), (5, 6, 7, 8)
        expected = [raw[0], (h, 0, 0, h), (1, 0, 0, 0), raw[3]]
        for interp, explicit in INTERPOLATIONS:
            with self.subTest(interp=interp, explicit=explicit):
                keys = [dict(t=t, v=q) for t, q in zip(times, raw)]
                if explicit:
                    for key in keys:
                        key.update({"in": incoming, "out": outgoing})
                rows = decoded_curve("rotation", interp, keys)["values"]
                values = rows[1::3] if interp == "cubic" else rows
                self.assertEqual(len(values), len(expected))
                for got, want in zip(values, expected):
                    self.assertClose(got, want)
                dots = [sum(x * y for x, y in zip(a, b)) for a, b in zip(values, values[1:])]
                self.assertClose(dots[:2], [h, h])
                self.assertEqual(dots[-1], 0)
                if explicit:
                    signs = (1, -1, -1, 1)
                    self.assertEqual(rows[::3], [tuple(s * x for x in incoming) for s in signs])
                    self.assertEqual(rows[2::3], [tuple(s * x for x in outgoing) for s in signs])
                elif interp == "cubic":
                    for i, (before, after) in enumerate(((0, 1), (0, 2), (1, 3), (2, 3))):
                        tangent = [(b - a) / (times[after] - times[before])
                                   for a, b in zip(expected[before], expected[after])]
                        self.assertClose(rows[3 * i], tangent)
                        self.assertClose(rows[3 * i + 2], tangent)

    def test_quaternion_unit_tolerance(self):
        identity = (0, 0, 0, 1)
        for name, norm, accepted in (
            ("inside", 1 + 0.999 * keys_file.QUAT_UNIT_TOLERANCE, True),
            ("outside", 1 + 1.001 * keys_file.QUAT_UNIT_TOLERANCE, False),
            ("near-zero", 1e-10, False),
        ):
            for offset in (False, True):
                with self.subTest(norm=name, offset=offset):
                    q = (0, 0, 0, norm)
                    keys = [dict(t=0, v=identity), dict(t=1, v=identity if offset else q)]
                    extra = {"offset": {"rotation": q}} if offset else {}
                    if not accepted:
                        with self.assertRaisesRegex(ValueError, rf"key {0 if offset else 1}.*unit quaternion"):
                            decoded_curve("rotation", "linear", keys, **extra)
                        continue
                    rows = decoded_curve("rotation", "linear", keys, **extra)["values"]
                    if offset:
                        self.assertEqual(rows, [identity])
                    else:
                        self.assertClose([math.hypot(*rows[1])], [norm])

    def test_one_key_cubic_auto_is_constant(self):
        track = decoded_curve("position", "cubic", [dict(t=0, v=[1, 2, 3])])
        self.assertEqual(track["values"], [(0, 0, 0), (1, 2, 3), (0, 0, 0)])
        for time in (-1, 0, 1):
            self.assertEqual(tuple(tracks_asset.sample(track, time)), (1, 2, 3))

    def test_cubic_errors_name_later_key(self):
        for key, message in (
            (dict(v=[1, 2, 3]), "each key"),
            (dict(t="1", v=[1, 2, 3]), "finite"),
            (dict(t=float("inf"), v=[1, 2, 3]), "finite"),
            (dict(t=1e100, v=[1, 2, 3]), "float32"),
            (dict(t=0, v=[1, 2, 3]), "strictly"),
            (dict(t=1, v=[1, 2]), "3 components"),
            (dict(t=1, v=[1, 2, True]), "finite"),
            (dict(t=1, v=[1, 2, 1e100]), "float32"),
            ({"t": 1, "v": [1, 2, 3], "in": [0, 0, 0]}, "both"),
            ({"t": 1, "v": [1, 2, 3], "in": [0, 0], "out": [0, 0, 0]}, "3 components"),
        ):
            first = dict(t=0, v=[0, 0, 0])
            if "out" in key:
                first.update({"in": [0, 0, 0], "out": [0, 0, 0]})
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, rf"prop.position key 1.*{message}"):
                decoded_curve("position", "cubic", [first, key])

    def test_cubic_offset_overflow_names_key_not_row(self):
        keys = [{"t": t, "v": [x, 0, 0], "in": [0, 0, 0], "out": [0, 0, 0]}
                for t, x in enumerate((0, 2e38))]
        with self.assertRaisesRegex(ValueError, r"prop.position key 1.*baked.*float32"):
            decoded_curve("position", "cubic", keys, offset={"position": [2e38, 0, 0]})

    def test_cubic_position_offset_defaults_and_tangents(self):
        h = math.sqrt(0.5)
        keys = [{"t": 0, "v": [1, 2, 3], "in": [1, 0, 0], "out": [0, 1, 0]}]
        rotated = decoded_curve("position", "cubic", keys, offset={"rotation": [0, 0, h, h]})
        translated = decoded_curve("position", "cubic", keys, offset={"position": [4, 5, 6]})
        self.assertClose(rotated["values"][0], (0, 1, 0))
        self.assertClose(rotated["values"][1], (-2, 1, 3))
        self.assertClose(rotated["values"][2], (-1, 0, 0))
        self.assertEqual(translated["values"], [(1, 0, 0), (5, 7, 9), (0, 1, 0)])
        with self.assertRaisesRegex(ValueError, r"prop.near_z key 0.*offset"):
            decoded_curve("near_z", "step", [dict(t=0, v=1)], offset={})


if __name__ == "__main__":
    unittest.main()

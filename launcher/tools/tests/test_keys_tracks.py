"""Track-form keys baked to TRCK and sampled by the firmware's track sampler."""
import hashlib
import math
import re

import pytest

from test_camera_keys import TOOLS, camera_keys, clip_of, has_compiler, track_host, tracks_asset
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


def test_demo_camera_glbs_are_byte_identical():
    demo = TOOLS.parent / "demo"
    actual = {p.relative_to(demo).as_posix(): hashlib.sha256(camera_keys.load_glb_bytes(p)).hexdigest()
              for p in demo.rglob("*.keys.toml")}
    assert actual == DEMO_GLBS


def test_track_keys_round_trip(tmp_path):
    clip = clip_of(tmp_path, '''animation = "path"

[[tracks]]
object = "blue"
field = "position"
interp = "cubic"
keys = [{t = 0, v = [0, 0, 0]}, {t = 1, v = [3, 0, 0]}, {t = 3, v = [6, 0, 0]}]

[[tracks]]
object = "blue"
field = "scale"
interp = "step"
keys = [{t = 0, v = [1, 1, 1]}, {t = 2, v = [2, 3, 4]}]

[[tracks]]
object = "camera"
field = "half_fov_short_tan"
interp = "linear"
keys = [{t = 0, v = 0.25}, {t = 3, v = 0.75}]
''')
    doc, binary = gltf_read.load_asset(tmp_path / "path.keys.toml")
    assert [node["name"] for node in doc["nodes"]] == ["blue", "camera"]
    channels = gltf_read.read_animation(doc, binary, doc["animations"][0])
    assert [channel["path"] for channel in channels] == ["translation", "scale", "pointer"]
    pointer = channels[-1]["pointer"]
    assert pointer == "/nodes/1/extensions/AUTANA_scene_fields/CAMR/half_fov_short_tan"
    assert "AUTANA_scene_fields" in doc["extensionsUsed"]
    assert doc["nodes"][1]["extensions"]["AUTANA_scene_fields"]["CAMR"]["half_fov_short_tan"] == 0.25
    tracks, duration = tracks_asset.decode(tracks_asset.bake(clip))
    assert duration == 3000
    assert [(t["name"], t["interpolation"]) for t in tracks] == [
        ("blue:TRNS.position", "CUBICSPLINE"), ("blue:TRNS.scale", "STEP"),
        ("camera:CAMR.half_fov_short_tan", "LINEAR")]
    assert tracks[0]["times"] == [0, 1, 3]
    assert [v[0] for v in tracks[0]["values"]] == [3, 0, 3, 2, 3, 2, 1.5, 6, 1.5]
    assert tracks[1]["values"] == [(1, 1, 1), (2, 3, 4)]
    assert tracks[2]["values"] == [(0.25,), (0.75,)]


@pytest.mark.parametrize("interp, explicit", [("step", False), ("linear", False), ("cubic", False), ("cubic", True)])
@pytest.mark.skipif(not has_compiler(), reason="needs sh and a C compiler")
def test_track_offsets_commute_with_host_sampling(tmp_path, interp, explicit):
    h = math.sqrt(0.5)

    def sample(with_offset):
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
        clip = clip_of(tmp_path, text)
        if not with_offset and explicit:
            tracks, _ = tracks_asset.decode(tracks_asset.bake(clip))
            turn = tracks[1]
            assert turn["values"][3] == (0.25,) * 4
            assert turn["values"][4] == pytest.approx((h, 0, 0, h))
            assert turn["values"][5] == (-0.5,) * 4
        return [line.split() for line in
                track_host.sample(clip, ["--every", 23, "--until", 3013, "--clamp"]).splitlines()]

    plain, baked = sample(False), sample(True)
    assert len(baked) == len(plain) >= 200
    for (time, name, *value), (baked_time, baked_name, *got) in zip(plain, baked):
        assert (time, name) == (baked_time, baked_name)
        if name.endswith(".position"):
            x, y, z = map(float, value)
            want = [7 - y, 11 + x, 13 + z]
        else:
            x, y, z, w = map(float, value)
            want = [h * (x + z), h * (y + w), h * (z - x), h * (w - y)]
        assert list(map(float, got)) == pytest.approx(want, rel=4e-7, abs=1e-6)


@pytest.mark.parametrize("change, label, key, message", [
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
])
def test_invalid_track_keys_name_the_track_and_key(change, label, key, message):
    track = dict(object="blue", field="position", interp="linear", keys=[{"t": 0, "v": [0, 0, 0]}])
    track.update(change)
    with pytest.raises(ValueError, match=rf"{re.escape(label)}.*key {key}.*{message}"):
        camera_keys.build(dict(animation="path", tracks=[track]))


def test_invalid_track_keys_tables():
    track = dict(object="blue", field="position", interp="linear", keys=[{"t": 0, "v": [0, 0, 0]}])
    for values, message in (
        (dict(animation="path", tracks=[track], node="blue"), "expected animation and tracks"),
        (dict(animation="", tracks=[track]), "animation"),
        (dict(animation="path", tracks=[]), "tracks"),
        (dict(animation="path", tracks=[track, dict(track)]), r"blue.position.*key 0.*duplicate"),
    ):
        with pytest.raises(ValueError, match=message):
            camera_keys.build(values)


def test_track_field_discovery(tmp_path):
    from anim import scene_fields

    fields = scene_fields.discover()
    assert fields["half_fov_short_tan"] == (tracks_asset.CAMERA, tracks_asset.VALUE_FLOAT)
    assert fields["near_z"] == (tracks_asset.CAMERA, tracks_asset.VALUE_FLOAT)
    assert fields["position"] == (tracks_asset.TRANSFORM, tracks_asset.VALUE_VEC3)

    def component(tag):
        (tmp_path / f"{tag}.h").write_text(f"#define COMPONENT_{tag} ASSET_TYPE({', '.join(map(repr, tag))})\n")
        (tmp_path / f"{tag}.c").write_text(f'''#include "{tag}.h"
static const anim_field_t FIELDS[] = {{ ANIM_FIELD(test_t, amount, ANIM_VALUE_VEC2) }};
const anim_component_fields_t COMPONENT = {{ COMPONENT_{tag}, FIELDS, sizeof FIELDS / sizeof FIELDS[0] }};
''')

    component("TEST")
    assert scene_fields.discover(tmp_path) == {"amount": (int.from_bytes(b"TEST", "little"), tracks_asset.VALUE_VEC2)}
    component("NEXT")
    with pytest.raises(ValueError, match=r"amount(?=.*NEXT)(?=.*TEST)"):
        scene_fields.discover(tmp_path)

"""Checks r3d's glTF skin reader and preview on a strip bent by two bones,
built here rather than loaded, in a layout no generator of ours writes:
interleaved positions, 16-bit joints, normalized 8-bit weights, no normals.
The preview's checks are skipped where Pillow is not installed."""

import json
import math
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import gltf_skin  # noqa: E402

try:
    from r3d import gltf_preview
except ImportError:
    gltf_preview = None

XS = (0.0, 0.5, 1.0, 1.5, 2.0)
HALF_WIDTH = 0.1
ELBOW = 1.0


def strip_glb():
    """A strip along +X, bone 1 from the origin, bone 2 from x = 1 bending
    90 degrees about +Z over one second; the column at the elbow is shared."""
    positions, joints, weights = [], [], []
    for x in XS:
        for y in (-HALF_WIDTH, HALF_WIDTH):
            positions.append((x, y, 0.0))
            if x < ELBOW:
                joints.append((0, 1, 0, 0))
                weights.append((255, 0, 0, 0))
            elif x == ELBOW:
                joints.append((0, 1, 0, 0))
                weights.append((128, 127, 0, 0))
            else:
                joints.append((1, 0, 0, 0))
                weights.append((255, 0, 0, 0))
    indices = []
    for column in range(len(XS) - 1):
        a, b, c, d = 2 * column, 2 * column + 1, 2 * column + 2, 2 * column + 3
        indices += [a, c, d, a, d, b]

    blob = bytearray()
    views, accessors = [], []

    def view(data, stride=None):
        while len(blob) % 4:
            blob.append(0)
        entry = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if stride:
            entry["byteStride"] = stride
        blob.extend(data)
        views.append(entry)
        return len(views) - 1

    def accessor(view_index, component, kind, count, **extra):
        accessors.append(dict(bufferView=view_index, componentType=component, type=kind, count=count, **extra))
        return len(accessors) - 1

    interleaved = b"".join(struct.pack("<3f4x", *p) for p in positions)
    lows = [min(p[k] for p in positions) for k in range(3)]
    highs = [max(p[k] for p in positions) for k in range(3)]
    position = accessor(view(interleaved, stride=16), 5126, "VEC3", len(positions), min=lows, max=highs)
    joint = accessor(view(b"".join(struct.pack("<4H", *j) for j in joints)), 5123, "VEC4", len(joints))
    weight = accessor(
        view(bytes(w for ws in weights for w in ws)), 5121, "VEC4", len(weights), normalized=True
    )
    index = accessor(view(struct.pack("<%dH" % len(indices), *indices)), 5123, "SCALAR", len(indices))
    inverse_binds = [
        (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1),
        (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -ELBOW, 0, 0, 1),
    ]
    ibm = accessor(view(b"".join(struct.pack("<16f", *m) for m in inverse_binds)), 5126, "MAT4", 2)
    times = accessor(view(struct.pack("<2f", 0.0, 1.0)), 5126, "SCALAR", 2, min=[0.0], max=[1.0])
    s = math.sin(math.pi / 4.0)
    turn = accessor(view(struct.pack("<8f", 0, 0, 0, 1, 0, 0, s, s)), 5126, "VEC4", 2)
    document = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 2]}],
        "nodes": [
            {"name": "upper", "children": [1]},
            {"name": "lower", "translation": [ELBOW, 0.0, 0.0]},
            {"mesh": 0, "skin": 0},
        ],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": position, "JOINTS_0": joint, "WEIGHTS_0": weight},
            "indices": index,
        }]}],
        "skins": [{"joints": [0, 1], "inverseBindMatrices": ibm}],
        "animations": [
            {"name": "bend", "samplers": [{"input": times, "output": turn}],
             "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]},
            {"name": "snap", "samplers": [{"input": times, "output": turn, "interpolation": "STEP"}],
             "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]},
        ],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    text = json.dumps(document).encode("utf-8")
    text += b" " * (-len(text) % 4)
    blob.extend(b"\0" * (-len(blob) % 4))
    length = 12 + 8 + len(text) + 8 + len(blob)
    return (struct.pack("<III", 0x46546C67, 2, length) + struct.pack("<II", len(text), 0x4E4F534A)
            + text + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def bent(x, y, angle):
    """Where a bone-2 point lands when bone 2 turns by `angle` about +Z."""
    dx = x - ELBOW
    return (ELBOW + dx * math.cos(angle) - y * math.sin(angle), dx * math.sin(angle) + y * math.cos(angle), 0.0)


class GltfSkinTest(unittest.TestCase):
    def setUp(self):
        self.asset = gltf_skin.SkinnedAsset(*gltf_skin.parse_glb(strip_glb()))

    def assert_points(self, actual, expected):
        for a, e in zip(actual, expected):
            for p, q in zip(a, e):
                self.assertAlmostEqual(p, q, places=5)

    def test_reads_strided_positions_and_normalized_weights(self):
        self.assert_points(self.asset.positions, [(x, y, 0.0) for x in XS for y in (-HALF_WIDTH, HALF_WIDTH)])
        self.assertEqual(len(self.asset.triangles), 2 * (len(XS) - 1))
        self.assertIsNone(self.asset.normals)
        for weights in self.asset.weights:
            self.assertAlmostEqual(sum(weights), 1.0, places=6)

    def test_rest_pose_skins_to_the_bind_positions(self):
        positions, normals = self.asset.skin(self.asset.sample(None, 0.0))
        self.assert_points(positions, self.asset.positions)
        self.assertIsNone(normals)

    def test_linear_rotation_slerps_between_keys(self):
        for time, angle in ((1.0, math.pi / 2.0), (0.5, math.pi / 4.0)):
            positions, _ = self.asset.skin(self.asset.sample("bend", time))
            for (x, y, _), p in zip(self.asset.positions, positions):
                if x > ELBOW:
                    self.assert_points([p], [bent(x, y, angle)])
                elif x < ELBOW:
                    self.assert_points([p], [(x, y, 0.0)])

    def test_the_shared_column_blends_both_bones(self):
        positions, _ = self.asset.skin(self.asset.sample("bend", 1.0))
        share = 127.0 / 255.0
        for (x, y, _), p in zip(self.asset.positions, positions):
            if x == ELBOW:
                moved = bent(x, y, math.pi / 2.0)
                expected = tuple((1.0 - share) * a + share * b for a, b in zip((x, y, 0.0), moved))
                self.assert_points([p], [expected])

    def test_step_holds_the_earlier_key(self):
        positions, _ = self.asset.skin(self.asset.sample("snap", 0.9))
        self.assert_points(positions, self.asset.positions)


@unittest.skipIf(gltf_preview is None, "Pillow is not installed")
class GltfPreviewTest(unittest.TestCase):
    def setUp(self):
        self.asset = gltf_skin.SkinnedAsset(*gltf_skin.parse_glb(strip_glb()))

    def test_sheet_tiles_four_views_of_the_requested_size(self):
        sheet = gltf_preview.bind_sheet(self.asset, (48, 40))
        self.assertEqual(sheet.size, (96, 80))

    def test_animation_frames_show_the_motion(self):
        frames = gltf_preview.animation_frames(self.asset, "bend", (48, 40), "top", 4)
        self.assertEqual(len(frames), 4)
        self.assertTrue(all(f.size == (48, 40) for f in frames))
        self.assertNotEqual(frames[0].tobytes(), frames[2].tobytes())


if __name__ == "__main__":
    unittest.main()

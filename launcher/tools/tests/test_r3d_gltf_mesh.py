"""Checks the static read of a binary glTF's meshes and its import as a mesh source."""

import json
import pathlib
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.gltf_mesh import DEFAULT_MATERIAL, load_gltf_mesh
from r3d.import_settings import SettingsError, load_import_settings

try:
    import numpy as np

    from r3d import mesh_import
    from r3d.lit_mesh import read_lit_mesh
    from r3d.mitsuba_reference import source_meshes
except ImportError:
    np = None

FLOAT, USHORT = 5126, 5123
USHORT_MAX = 65535
QUAD = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)]
QUAD_INDICES = [0, 1, 2, 0, 2, 3]


def encode_glb(nodes, meshes, materials=(), skins=()):
    """A .glb of `meshes`: each a list of primitives {positions, indices, optional colors (linear RGB, written as
    normalized RGBA ushort), material, mode}."""
    binary, views, accessors = bytearray(), [], []

    def add(values, component, kind, fmt, normalized=False):
        while len(binary) % 4:
            binary.append(0)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(values) * struct.calcsize("<" + fmt)})
        for value in values:
            binary.extend(struct.pack("<" + fmt, *(value if isinstance(value, tuple) else (value,))))
        accessors.append({"bufferView": len(views) - 1, "componentType": component, "count": len(values), "type": kind,
                          **({"normalized": True} if normalized else {})})
        return len(accessors) - 1

    gltf_meshes = []
    for primitives in meshes:
        out = []
        for primitive in primitives:
            attributes = {"POSITION": add(primitive["positions"], FLOAT, "VEC3", "fff")}
            if "colors" in primitive:
                rgba = [tuple(round(c * USHORT_MAX) for c in colour) + (USHORT_MAX,) for colour in primitive["colors"]]
                attributes["COLOR_0"] = add(rgba, USHORT, "VEC4", "HHHH", normalized=True)
            entry = {"attributes": attributes, "indices": add(primitive["indices"], USHORT, "SCALAR", "H")}
            for key in ("material", "mode"):
                if key in primitive:
                    entry[key] = primitive[key]
            out.append(entry)
        gltf_meshes.append({"primitives": out})
    document = {"asset": {"version": "2.0"}, "nodes": nodes, "meshes": gltf_meshes, "materials": list(materials),
                "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(binary)}]}
    if skins:
        document["skins"] = list(skins)
    text = json.dumps(document).encode()
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    chunks = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(binary), 0x004E4942) + binary
    return struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks


def write_glb(directory, nodes, meshes, **parts):
    path = pathlib.Path(directory) / "m.glb"
    path.write_bytes(encode_glb(nodes, meshes, **parts))
    return path


class GltfMeshReadTests(unittest.TestCase):
    def read(self, nodes, meshes, **parts):
        with tempfile.TemporaryDirectory() as directory:
            return load_gltf_mesh(write_glb(directory, nodes, meshes, **parts))

    def test_a_node_places_its_mesh_through_its_parents(self):
        nodes = [{"children": [1], "translation": [10.0, 0.0, 0.0]}, {"mesh": 0, "scale": [2.0, 2.0, 2.0]}]
        mesh = self.read(nodes, [[{"positions": QUAD, "indices": QUAD_INDICES}]])
        self.assertEqual(mesh.positions, [(10.0 + 2 * x, 2 * y, 2 * z) for x, y, z in QUAD])
        self.assertEqual(mesh.tri_v, [(0, 1, 2), (0, 2, 3)])

    def test_a_skinned_node_keeps_its_bind_pose_and_ignores_its_own_transform(self):
        nodes = [{"mesh": 0, "skin": 0, "translation": [5.0, 5.0, 5.0]}, {"name": "bone"}]
        mesh = self.read(nodes, [[{"positions": QUAD, "indices": QUAD_INDICES}]], skins=[{"joints": [1]}])
        self.assertEqual(mesh.positions, QUAD)

    def test_a_vertex_colour_is_its_paint_times_its_materials_base_colour(self):
        paint = [(1.0, 0.5, 0.0), (0.0, 1.0, 1.0), (1.0, 1.0, 1.0), (0.5, 0.5, 0.5)]
        tint = [0.5, 1.0, 0.25, 1.0]
        mesh = self.read([{"mesh": 0}], [[{"positions": QUAD, "indices": QUAD_INDICES, "colors": paint, "material": 0}]],
                         materials=[{"name": "fur", "pbrMetallicRoughness": {"baseColorFactor": tint}}])
        for got, want in zip(mesh.colors, paint):
            for g, w, t in zip(got, want, tint):
                self.assertAlmostEqual(g, w * t, delta=1.0 / USHORT_MAX)
        self.assertEqual(mesh.names, ["fur"])

    def test_an_unpainted_unmaterialled_primitive_is_white_and_named_default(self):
        mesh = self.read([{"mesh": 0}], [[{"positions": QUAD, "indices": QUAD_INDICES}]],
                         materials=[{"pbrMetallicRoughness": {}}])
        self.assertEqual(mesh.colors, [(1.0, 1.0, 1.0)] * 4)
        self.assertEqual(mesh.names, ["material_0", DEFAULT_MATERIAL])
        self.assertEqual(mesh.tri_m, [1, 1])

    def test_primitives_of_two_nodes_append_with_their_own_materials(self):
        nodes = [{"mesh": 0}, {"mesh": 1, "translation": [0.0, 0.0, 3.0]}]
        meshes = [[{"positions": QUAD, "indices": QUAD_INDICES, "material": 1}],
                  [{"positions": QUAD, "indices": QUAD_INDICES, "material": 0}]]
        mesh = self.read(nodes, meshes, materials=[{"name": "a"}, {"name": "b"}])
        self.assertEqual(mesh.tri_v[2:], [(4, 5, 6), (4, 6, 7)])
        self.assertEqual(mesh.tri_m, [1, 1, 0, 0])

    def test_a_primitive_that_is_not_a_triangle_list_is_refused(self):
        lines = 1
        with self.assertRaisesRegex(ValueError, "not triangles"):
            self.read([{"mesh": 0}], [[{"positions": QUAD, "indices": QUAD_INDICES, "mode": lines}]])

    def test_a_file_without_triangles_is_refused(self):
        with self.assertRaisesRegex(ValueError, "no mesh holds a triangle"):
            self.read([{"name": "empty"}], [])


class GltfImportSettingsTests(unittest.TestCase):
    def settings(self, directory, source):
        path = pathlib.Path(directory) / "m.import.toml"
        path.write_text(f'[source]\npath = "{source}"\ncredit = "c"\n[output]\ndirectory = "."\nname = "m"\n')
        return load_import_settings(path)

    def test_a_glb_source_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.settings(directory, "m.glb").source["path"].suffix, ".glb")

    def test_another_extension_names_every_supported_one(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SettingsError, r"supported: \.obj, \.glb"):
                self.settings(directory, "m.fbx")


@unittest.skipIf(np is None, "the r3d environment is not installed")
class GltfImportTests(unittest.TestCase):
    def test_an_albedo_import_bakes_each_vertex_colour(self):
        red, blue = (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)
        far = [(x, y, z + 5.0) for x, y, z in QUAD]
        meshes = [[{"positions": QUAD, "indices": QUAD_INDICES, "colors": [red] * 4},
                   {"positions": far, "indices": QUAD_INDICES, "colors": [blue] * 4}]]
        with tempfile.TemporaryDirectory() as directory:
            write_glb(directory, [{"mesh": 0}], meshes)
            path = pathlib.Path(directory) / "m.import.toml"
            path.write_text('[source]\npath = "m.glb"\ncredit = "c"\n[output]\ndirectory = "."\nname = "m"\n')
            self.assertEqual(mesh_import.main([str(path)]), 0)
            mesh = read_lit_mesh(pathlib.Path(directory) / "m.mesh")
        self.assertEqual(len(mesh.tris), 4)
        near = mesh.pos[:, 2] < mesh.pos[:, 2].max() / 2
        self.assertTrue((mesh.rgb[near] == (255, 0, 0)).all(), mesh.rgb)
        self.assertTrue((mesh.rgb[~near] == (0, 0, 255)).all(), mesh.rgb)

    def test_the_mitsuba_export_refuses_vertex_colours_it_would_drop(self):
        source = SimpleNamespace(colors=np.ones((4, 3)), names=["m"])
        with self.assertRaisesRegex(ValueError, "no vertex colours"):
            source_meshes(None, source, set())


if __name__ == "__main__":
    unittest.main()

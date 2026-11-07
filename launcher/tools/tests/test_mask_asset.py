"""LMSK layout, atlas bounds and mesh sibling validation."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import mask_asset as mask, mesh_asset, mesh_bundle


class MaskAssetTests(unittest.TestCase):
    def test_round_trip_and_bounds(self):
        scale = 1 << mask.UV_FRACTION_BITS
        uvs = [(0, 0), (mask.MASK_WIDTH * scale - 1, scale - 1)]
        rows = [(1 << mask.MASK_WIDTH) - 1]
        blob = mask.encode(1, uvs, rows)
        self.assertEqual(mask.decode(blob, 3), (1, uvs, rows))
        self.assertEqual(mask.name_for_mesh("cards"), "cards" + mask.NAME_SUFFIX)
        for bad in (blob[:-1], blob + b"x", blob[:mask.HEADER.size - 1],
                    mask.HEADER.pack(mask.VERSION + 1, mask.MASK_WIDTH, 1, 1) + blob[mask.HEADER.size:],
                    mask.HEADER.pack(mask.VERSION, mask.MASK_WIDTH - 1, 1, 1) + blob[mask.HEADER.size:]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                mask.decode(bad, 3)
        for uv in ((mask.MASK_WIDTH * scale, 0), (0, scale)):
            with self.subTest(uv=uv), self.assertRaisesRegex(ValueError, "UV"):
                mask.encode(0, [uv], rows)
        self.assertEqual(mask.decode(mask.encode(0, [], []), 0), (0, [], []))

    def test_bundle_round_trip_and_refusals(self):
        import numpy as np
        from r3d.lit_mesh import bake_lit_mesh
        mesh = bake_lit_mesh(np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]]), None,
                             np.array([[0, 1, 2]]), np.array([0]))
        mesh.masked = [True]
        mesh.mask_vertex_first = 0
        mesh.mask_uvs = [(0, 0)] * len(mesh.pos)
        mesh.mask_rows = [1]
        entries = mesh_bundle.mesh_entries("cards", mesh)
        read = mesh_bundle.read_mesh(entries, "cards")
        self.assertEqual(read.masked, mesh.masked)
        self.assertEqual(read.mask_uvs, mesh.mask_uvs)
        self.assertEqual(read.mask_rows, mesh.mask_rows)
        self.assertEqual(mesh_bundle.decode(mesh_bundle.encode(entries, []))[0], sorted(entries))
        with self.assertRaises(ValueError):
            mesh_bundle.decode(mesh_bundle.encode(entries[:1], []))
        mesh.masked = [False]
        from asset.asset_pack import NAME_BYTES
        from r3d import lod_asset
        long_name = "m" * (NAME_BYTES - len(lod_asset.NAME_SUFFIX) - 1)
        uvs = mesh.mask_uvs
        mesh.mask_uvs = None
        long_entries = mesh_bundle.mesh_entries(long_name, mesh)
        mesh_bundle.decode(mesh_bundle.encode(long_entries, []))
        self.assertIsNone(mesh_bundle.read_mesh(long_entries, long_name).mask_uvs)
        mesh.mask_uvs = uvs
        with self.assertRaises(ValueError):
            mesh_bundle.mesh_entries("cards", mesh)

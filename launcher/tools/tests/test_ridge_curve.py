"""Curve source validation, frozen values and engine pack integration."""

import hashlib
import pathlib
import struct
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from asset import table_asset
from gfx import ridge_curve
from r3d import build_pack
from asset.asset_pack import parse_pack, parse_directory, build_directory
from test_gfx_image_asset import png

class CurveTests(unittest.TestCase):
    def test_source_paths_and_unknown_keys_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder) / "curve.curve.toml"
            for source in ("../line.png", "a/b.png", "C:line.png", "line.jpg"):
                root.write_text(f'source = "{source}"')
                with self.subTest(source=source), self.assertRaises(ValueError):
                    ridge_curve.bake(root)
            root.write_text('source = "line.png"\nquarter_turns = 0')
            with self.assertRaises(ValueError):
                ridge_curve.bake(root)

    def test_missing_line_repeated_crossing_and_jump_are_rejected(self):
        with self.assertRaises(ValueError):
            ridge_curve.column_height([1, 0, 1], 0)
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder) / "curve.curve.toml"
            root.write_text('source = "line.png"')
            source = root.parent / "line.png"
            for width, height, ink in (
                    (1, 1, lambda x, y: False),
                    (ridge_curve.VIEW_W, ridge_curve.VIEW_H, lambda x, y: False),
                    (ridge_curve.VIEW_W, ridge_curve.VIEW_H,
                     lambda x, y: y == ridge_curve.VIEW_H // 2 + (ridge_curve.MAX_NEIGHBOUR_STEP_PX + 1) * (x % 2))):
                rows = [[(255, 255, 255, 255) if ink(x, y) else (0, 0, 0, 255)
                         for x in range(width)] for y in range(height)]
                source.write_bytes(png(width, height, rows))
                with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                    ridge_curve.bake(root)

    def test_curve_values_are_pinned(self):
        root = TOOLS.parent / "main/engine/ridge/ridge.curve.toml"
        rows, frac = table_asset.decode(ridge_curve.bake(root))
        values = rows["y"]
        self.assertEqual((len(values), frac), (ridge_curve.VIEW_W, ridge_curve.Q_SHIFT))
        checksum = hashlib.sha256(struct.pack(f"<{len(values)}h", *values)).hexdigest()
        self.assertEqual(checksum, "8dfde1945d3c5f6940a446dba2496cf4afa0b6895c28e018d95da556bff19d89")

    def test_engine_pack_and_partition_hold_the_curve(self):
        packs = build_pack.pack_bytes([TOOLS.parent / "main/engine"])
        kind, data = parse_pack(packs["engine"])["ridge"]
        self.assertEqual(kind, table_asset.TYPE)
        self.assertEqual(table_asset.decode(data)[1], ridge_curve.Q_SHIFT)
        self.assertIn("engine", parse_directory(build_directory(sorted(packs.items()))))


if __name__ == "__main__":
    unittest.main()

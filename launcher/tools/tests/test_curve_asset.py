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
from gfx import curve_asset
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
                    curve_asset.bake(root)
            root.write_text('source = "line.png"\nquarter_turns = 0')
            with self.assertRaises(ValueError):
                curve_asset.bake(root)

    def test_missing_line_repeated_crossing_and_jump_are_rejected(self):
        with self.assertRaises(ValueError):
            curve_asset.column_height([1, 0, 1], 0)
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder) / "curve.curve.toml"
            root.write_text('source = "line.png"')
            source = root.parent / "line.png"
            for width, height, ink in (
                    (1, 1, lambda x, y: False),
                    (curve_asset.VIEW_W, curve_asset.VIEW_H, lambda x, y: False),
                    (curve_asset.VIEW_W, curve_asset.VIEW_H,
                     lambda x, y: y == curve_asset.VIEW_H // 2 + (curve_asset.MAX_NEIGHBOUR_STEP_PX + 1) * (x % 2))):
                rows = [[(255, 255, 255, 255) if ink(x, y) else (0, 0, 0, 255)
                         for x in range(width)] for y in range(height)]
                source.write_bytes(png(width, height, rows))
                with self.subTest(width=width, height=height), self.assertRaises(ValueError):
                    curve_asset.bake(root)

    def test_neighbour_step_limit(self):
        from render_diff import read_png
        for step in (curve_asset.MAX_NEIGHBOUR_STEP_PX, curve_asset.MAX_NEIGHBOUR_STEP_PX + 1):
            rows = [[(255, 255, 255, 255) if y == curve_asset.VIEW_H // 2 + step * (x % 2)
                     else (0, 0, 0, 255) for x in range(curve_asset.VIEW_W)]
                    for y in range(curve_asset.VIEW_H)]
            image = read_png(png(curve_asset.VIEW_W, curve_asset.VIEW_H, rows))
            if step == curve_asset.MAX_NEIGHBOUR_STEP_PX:
                self.assertEqual(len(curve_asset.heights(image)), curve_asset.VIEW_W)
            else:
                with self.assertRaisesRegex(ValueError, "jumps"):
                    curve_asset.heights(image)

    def test_faint_column_is_rejected(self):
        from render_diff import read_png
        peak = 254
        faint = peak // curve_asset.PEAK_DIVISOR - 1
        rows = [[((faint,) * 3 + (255,)) if x == 0 else (peak, peak, peak, 255)
                 for x in range(curve_asset.VIEW_W)] if y == curve_asset.VIEW_H // 2
                else [(0, 0, 0, 255)] * curve_asset.VIEW_W for y in range(curve_asset.VIEW_H)]
        with self.assertRaisesRegex(ValueError, "column 0 has no line"):
            curve_asset.heights(read_png(png(curve_asset.VIEW_W, curve_asset.VIEW_H, rows)))

    def test_longest_row_name(self):
        name = "x" * (table_asset.NAME_SIZE - 1)
        rows = {name: [0]}
        self.assertEqual(table_asset.decode(table_asset.encode(rows, 0))[0], rows)

    def test_bad_curve_entry_names_its_root(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder) / "bad.curve.toml"
            root.write_text('source = "line.png"' + chr(10) + 'quarter_turns = 0')
            with self.assertRaisesRegex(build_pack.SettingsError, str(root).replace("\\", "\\\\")):
                build_pack.pack_entry("bad", root)

    def test_curve_values_are_pinned(self):
        root = TOOLS.parent / "main/engine/ridge/ridge.curve.toml"
        rows, frac = table_asset.decode(curve_asset.bake(root))
        values = rows["y"]
        self.assertEqual((len(values), frac), (curve_asset.VIEW_W, curve_asset.Q_SHIFT))
        checksum = hashlib.sha256(struct.pack(f"<{len(values)}h", *values)).hexdigest()
        self.assertEqual(checksum, "8dfde1945d3c5f6940a446dba2496cf4afa0b6895c28e018d95da556bff19d89")

    def test_engine_pack_and_partition_hold_the_curve(self):
        packs = build_pack.pack_bytes([TOOLS.parent / "main/engine"])
        kind, data = parse_pack(packs["engine"])["ridge"]
        self.assertEqual(kind, table_asset.TYPE)
        self.assertEqual(table_asset.decode(data)[1], curve_asset.Q_SHIFT)
        self.assertIn("engine", parse_directory(build_directory(sorted(packs.items()))))


if __name__ == "__main__":
    unittest.main()

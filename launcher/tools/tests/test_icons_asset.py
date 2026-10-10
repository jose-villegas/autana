"""Icons (gfx/icons_asset.py): each icon of a set reads back as drawn from
its own one-bit image entry, what the baker refuses, what the image reader
refuses of a one-bit entry, and build_pack putting every root under a
NAME.pack.toml folder into pack NAME. The firmware reads the same bytes in
suite_gfx_image.c and the icon suites."""

import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from asset.asset_pack import parse_pack  # noqa: E402
from gfx import icons_asset, image_asset  # noqa: E402
from gfx.icons_asset import IconsError  # noqa: E402
from gfx.image_asset import HEADER, ImageError  # noqa: E402
from r3d import build_pack  # noqa: E402

# Two 3x2 cells side by side, ink "X": a plus-ish mark and a bar.
ATLAS = ["X.X...",
         ".X.XXX"]
SET = '''atlas = "marks.png"
cell_size = [3, 2]

[[icon]]
name = "cross"
at = [0, 0]

[[icon]]
name = "bar"
at = [1, 0]
'''
SQUARE_SVG = '<svg viewBox="0 0 4 4"><path d="M1 1h2v2H1z"/></svg>'


def png(rows, grey=128):
    """An 8-bit greyscale PNG of `rows`, "X" black and anything else white,
    or `grey` where given."""
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    raw = b"".join(b"\0" + bytes(0 if c == "X" else (grey if c == "g" else 255) for c in row) for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def drawn(rows):
    return [[c == "X" for c in row] for row in rows]


def read_back(path):
    """{name: rows of bools} of every icon of the set at `path`, through its entry."""
    return {name: image_asset.decode(icons_asset.bake(path, name))[2] for name in icons_asset.names(path)}


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text, atlas=ATLAS):
        (self.root / "marks.png").write_bytes(png(atlas))
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path


class IconsAssetTests(Fixture):
    def test_cells_read_back_as_drawn_in_set_order(self):
        icons = read_back(self.write("marks.icons.toml", SET))
        self.assertEqual(list(icons), ["cross", "bar"])
        self.assertEqual(icons["cross"], drawn(["X.X", ".X."]))
        self.assertEqual(icons["bar"], drawn(["...", "XXX"]))

    def test_an_icon_is_a_one_bit_image_whose_rows_are_whole_bytes_msb_first(self):
        data = icons_asset.bake(self.write("marks.icons.toml", SET), "cross")
        self.assertEqual(HEADER.unpack_from(data), (image_asset.VERSION, image_asset.FORMAT_MONO1, 3, 2, 8, HEADER.size))
        self.assertEqual(data[HEADER.size:], bytes([0b10100000, 0b01000000]))

    def test_an_svg_is_its_rectangles(self):
        (self.root / "svg").mkdir()
        (self.root / "svg" / "square.svg").write_text(SQUARE_SVG)
        path = self.write("marks.icons.toml", SET + '\n[[icon]]\nname = "square"\nsvg = "svg/square.svg"\n'
                          'upstream = "square"\ncommit = "0123"\n')
        self.assertEqual(read_back(path)["square"], drawn(["....", ".XX.", ".XX.", "...."]))

    def test_what_it_refuses_to_bake(self):
        cases = {
            "neither black": (SET, ["X.X...", ".XgXXX"]),
            "no icon names it": (SET.split("[[icon]]\nname = \"bar\"")[0], ATLAS),
            "an empty cell": (SET, ["X.X...", ".X...."]),
            "named twice": (SET.replace('"bar"', '"cross"'), ATLAS),
            "lower_snake_case": (SET.replace('"bar"', '"Bar"'), ATLAS),
            "outside the atlas": (SET.replace("at = [1, 0]", "at = [2, 0]"), ATLAS),
            "not whole 3x2 cells": (SET, [row + "." for row in ATLAS]),
            "unknown keys": ("scale = 2\n" + SET, ATLAS),
            "unknown icon keys": (SET + "scale = 2\n", ATLAS),
        }
        for message, (text, atlas) in cases.items():
            with self.subTest(message), self.assertRaisesRegex(IconsError, message):
                read_back(self.write("marks.icons.toml", text, atlas))

    def test_an_svg_needs_its_provenance_and_rectangles(self):
        (self.root / "svg").mkdir()
        for svg, extra, message in (
                (SQUARE_SVG, "", "upstream icon and the commit"),
                ('<svg viewBox="0 0 4 4"><path d="M1 1c1 1 2 2 3 3z"/></svg>', 'upstream = "a"\ncommit = "b"\n',
                 "curves and other commands"),
                ('<svg viewBox="0 0 4 4"><path d="M1.5 1h2v2H1z"/></svg>', 'upstream = "a"\ncommit = "b"\n',
                 "non-integer")):
            (self.root / "svg" / "s.svg").write_text(svg)
            path = self.write("marks.icons.toml", SET + '\n[[icon]]\nname = "s"\nsvg = "svg/s.svg"\n' + extra)
            with self.subTest(message), self.assertRaisesRegex(IconsError, message):
                read_back(path)

    def test_a_one_bit_entry_the_firmware_refuses_is_refused_here(self):
        good = icons_asset.bake(self.write("marks.icons.toml", SET), "cross")
        stride_at = 8
        for name, data in (("stride not whole bytes", good[:stride_at] + struct.pack("<I", 4) + good[stride_at + 4:]),
                           ("truncated", good[:-1])):
            with self.subTest(name), self.assertRaises(ImageError):
                image_asset.decode(data)


class FolderPackTests(Fixture):
    def test_every_root_under_a_pack_toml_is_one_pack_named_after_it(self):
        self.write("ui/ui.pack.toml", "# a folder pack\n")
        self.write("ui/a/marks.icons.toml", SET.replace('"marks.png"', '"../../marks.png"'))
        self.write("ui/b/c/other.icons.toml", SET.replace('"marks.png"', '"../../../marks.png"')
                   .replace('"cross"', '"plus"').replace('"bar"', '"rule"'))
        self.write("alone.icons.toml", SET.replace('"cross"', '"x"').replace('"bar"', '"y"'))
        packs = {name: parse_pack(data) for name, data in build_pack.pack_bytes([self.root]).items()}
        self.assertEqual(sorted(packs), ["alone", "ui"])
        self.assertEqual(sorted(packs["ui"]), ["bar", "cross", "plus", "rule"])
        self.assertEqual(packs["ui"]["cross"][0], image_asset.TYPE)

    def test_a_root_named_on_its_own_still_joins_its_folder_pack(self):
        self.write("ui/ui.pack.toml", "")
        path = self.write("ui/marks.icons.toml", SET.replace('"marks.png"', '"../marks.png"'))
        self.assertEqual(sorted(build_pack.pack_bytes([path])), ["ui"])

    def test_folder_packs_do_not_nest(self):
        self.write("ui/ui.pack.toml", "")
        self.write("ui/inner/inner.pack.toml", "")
        self.write("ui/inner/marks.icons.toml", SET.replace('"marks.png"', '"../../marks.png"'))
        with self.assertRaisesRegex(build_pack.SettingsError, "do not nest"):
            build_pack.pack_bytes([self.root])

    def test_a_pack_toml_holds_no_settings_yet(self):
        self.write("ui/ui.pack.toml", "entries = []\n")
        self.write("ui/marks.icons.toml", SET.replace('"marks.png"', '"../marks.png"'))
        with self.assertRaisesRegex(build_pack.SettingsError, "unknown keys"):
            build_pack.pack_bytes([self.root])

    def test_one_id_twice_in_a_folder_pack_is_refused(self):
        self.write("ui/ui.pack.toml", "")
        self.write("ui/a/marks.icons.toml", SET.replace('"marks.png"', '"../../marks.png"'))
        self.write("ui/b/marks.icons.toml", SET.replace('"marks.png"', '"../../marks.png"'))
        with self.assertRaisesRegex(build_pack.SettingsError, "ids are unique within a pack"):
            build_pack.pack_bytes([self.root])

    def test_a_folder_pack_and_a_root_of_one_name_are_refused(self):
        self.write("ui/ui.pack.toml", "")
        self.write("ui/marks.icons.toml", SET.replace('"marks.png"', '"../marks.png"'))
        self.write("ui.icons.toml", SET.replace('"cross"', '"x"').replace('"bar"', '"y"'))
        with self.assertRaisesRegex(build_pack.SettingsError, "pack named 'ui'"):
            build_pack.pack_bytes([self.root])

    def test_the_tree_packs_the_system_icons_in_the_engine_pack_and_every_icon_reads_back(self):
        packs = build_pack.pack_files([build_pack.DEFAULT_SEARCH])
        self.assertIn("check", packs["engine"])
        for name, entries in packs.items():
            for entry, source in entries.items():
                if source.name.endswith(icons_asset.SUFFIX):
                    width, height, rows = image_asset.decode(icons_asset.bake(source, entry))
                    self.assertTrue(any(any(row) for row in rows), f"{name}/{entry}")


if __name__ == "__main__":
    unittest.main()

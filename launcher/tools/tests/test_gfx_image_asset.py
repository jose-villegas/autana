"""The image entry (gfx/image_asset.py): what it writes reads back as baked,
what it refuses to write or read, the colour and the turn it bakes, and
build_pack finding every .image.toml. The firmware's reader of the same bytes
is suite_gfx_image.c."""

import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from asset.asset_pack import parse_pack  # noqa: E402
from gfx import image_asset  # noqa: E402
from gfx.image_asset import HEADER, ImageError  # noqa: E402
from r3d import build_pack  # noqa: E402

AT_FORMAT, AT_WIDTH, AT_STRIDE, AT_PIXELS = 2, 4, 8, 12


def png(width, height, rgba_rows):
    """An 8-bit RGBA PNG of `rgba_rows`, each a list of (r, g, b, a)."""
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in rgba_rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def edited(entry, offset, fmt, value):
    out = bytearray(entry)
    struct.pack_into(fmt, out, offset, value)
    return bytes(out)


class RoundTripTests(unittest.TestCase):
    def test_pixels_read_back_as_written_row_by_row(self):
        pixels = list(range(0x100, 0x106))
        self.assertEqual(image_asset.decode(image_asset.encode(3, 2, pixels)), (3, 2, pixels))

    def test_the_rows_follow_the_header_4_aligned_and_end_the_entry(self):
        entry = image_asset.encode(3, 2, [0] * 6)
        _, fmt, width, height, stride, offset = HEADER.unpack_from(entry)
        self.assertEqual((fmt, width, height, stride, offset), (image_asset.FORMAT_RGB565, 3, 2, 3, HEADER.size))
        self.assertEqual(offset % image_asset.PIXEL_ALIGN, 0)
        self.assertEqual(len(entry), HEADER.size + 2 * 6)

    def test_a_wider_stride_skips_the_padding_between_rows(self):
        entry = edited(image_asset.encode(2, 2, [1, 2, 3, 4]), AT_STRIDE, "<I", 3)
        entry = edited(entry, AT_WIDTH, "<H", 2) + b"\0\0"
        self.assertEqual(image_asset.decode(entry), (2, 2, [1, 2, 4, 0]))


class RefusalTests(unittest.TestCase):
    def test_writing_a_size_the_pixels_do_not_fill_is_refused(self):
        for width, height, count in ((2, 2, 3), (0, 1, 0), (1 << 16, 1, 1 << 16)):
            with self.assertRaises(ImageError):
                image_asset.encode(width, height, [0] * count)

    def test_a_pixel_that_is_not_16_bits_is_refused(self):
        with self.assertRaises(ImageError):
            image_asset.encode(1, 1, [0x10000])

    def test_each_header_value_the_firmware_refuses_is_refused_here(self):
        entry = image_asset.encode(2, 2, [0] * 4)
        for offset, fmt, value in ((0, "<H", 2), (AT_FORMAT, "<H", 0), (AT_FORMAT, "<H", 2), (AT_WIDTH, "<H", 0),
                                   (AT_STRIDE, "<I", 1), (AT_PIXELS, "<I", 12), (AT_PIXELS, "<I", 18),
                                   (AT_STRIDE, "<I", 0xFFFFFFFF)):
            with self.subTest(offset=offset, value=value), self.assertRaises(ImageError):
                image_asset.decode(edited(entry, offset, fmt, value))

    def test_rows_one_byte_short_or_a_short_header_are_refused(self):
        entry = image_asset.encode(2, 2, [0] * 4)
        for short in (entry[:-1], entry[:HEADER.size - 1]):
            with self.assertRaises(ImageError):
                image_asset.decode(short)


class BakeTests(unittest.TestCase):
    def bake(self, toml, rows=None, name="pic"):
        folder = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        rows = rows or [[(255, 0, 0, 255), (0, 255, 0, 255)], [(0, 0, 255, 255), (255, 255, 255, 255)]]
        (folder / "pic.png").write_bytes(png(len(rows[0]), len(rows), rows))
        root = folder / f"{name}.image.toml"
        root.write_text(toml)
        return root

    def test_colours_are_the_panel_s_byte_swapped_rgb565(self):
        width, height, pixels = image_asset.decode(image_asset.bake(self.bake('source = "pic.png"\n')))
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(pixels, [0x00F8, 0xE007, 0x1F00, 0xFFFF])

    def test_a_quarter_turn_is_clockwise(self):
        root = self.bake('source = "pic.png"\nquarter_turns = 1\n')
        self.assertEqual(image_asset.decode(image_asset.bake(root))[2], [0x1F00, 0x00F8, 0xFFFF, 0xE007])

    def test_a_turn_maps_the_source_corners_as_the_panel_holds_a_landscape_view(self):
        width, height = 3, 2
        pixels = list(range(width * height))
        out_w, out_h, turned = image_asset.turn(width, height, pixels, 1)
        self.assertEqual((out_w, out_h), (height, width))
        for (vx, vy) in ((0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)):
            panel_x, panel_y = out_w - 1 - vy, vx
            self.assertEqual(turned[panel_y * out_w + panel_x], pixels[vy * width + vx])

    def test_four_quarter_turns_are_none(self):
        pixels = list(range(6))
        self.assertEqual(image_asset.turn(3, 2, pixels, 4), (3, 2, pixels))

    def test_a_source_with_any_transparency_is_refused(self):
        rows = [[(1, 2, 3, 255), (1, 2, 3, 254)]]
        with self.assertRaisesRegex(ImageError, "not opaque"):
            image_asset.bake(self.bake('source = "pic.png"\n', rows))

    def test_a_malformed_image_toml_is_refused(self):
        for text in ('source = "../pic.png"\n', 'source = "pic.bmp"\n', 'source = "pic.png"\nquarter_turns = 4\n',
                     'source = "pic.png"\nquarter_turns = true\n', 'source = "pic.png"\nscale = 2\n', ""):
            with self.subTest(text=text), self.assertRaises(ImageError):
                image_asset.bake(self.bake(text))

    def test_build_pack_makes_a_pack_named_after_the_image_toml(self):
        root = self.bake('source = "pic.png"\n', name="sky")
        packs = build_pack.pack_bytes([root.parent])
        self.assertEqual(list(packs), ["sky"])
        self.assertEqual(list(parse_pack(packs["sky"])), ["sky"])


if __name__ == "__main__":
    unittest.main()

"""The image entry (gfx/image_asset.py): what it writes reads back as baked,
what it refuses to write or read, the colour and the turn it bakes, and
build_pack finding every .image.toml. The firmware's reader of the same bytes
is suite_draw_image.c."""

import pathlib
import struct
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "device"))

from asset.asset_pack import parse_pack  # noqa: E402
from gfx import image_asset  # noqa: E402
from gfx.image_asset import HEADER, ImageError  # noqa: E402
from r3d import build_pack  # noqa: E402
from screenshot import PNG_RGBA, encode_png  # noqa: E402

AT_FORMAT, AT_WIDTH, AT_STRIDE, AT_PIXELS = 2, 4, 8, 12


def png(width, height, rgba_rows):
    """An 8-bit RGBA PNG of `rgba_rows`, each a list of (r, g, b, a)."""
    return encode_png(width, height, (bytes(v for px in row for v in px) for row in rgba_rows), PNG_RGBA)


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


class AlphaTests(unittest.TestCase):
    PIXELS = [0x100, 0x101, 0x102, 0x103, 0x104, 0x105]
    ALPHA = [0, 128, 255, 1, 254, 77]

    def test_colour_and_alpha_read_back_as_written(self):
        entry = image_asset.encode(3, 2, self.PIXELS, self.ALPHA)
        self.assertEqual(image_asset.decode_alpha(entry), (3, 2, self.PIXELS, self.ALPHA))

    def test_colour_rows_then_alpha_rows_each_on_a_4_aligned_stride(self):
        entry = image_asset.encode(3, 2, self.PIXELS, self.ALPHA)
        _, fmt, width, height, stride, offset = HEADER.unpack_from(entry)
        self.assertEqual((fmt, width, height), (image_asset.FORMAT_RGB565A8, 3, 2))
        self.assertEqual(stride, image_asset.RGB565A8_STRIDE_STEP)
        alpha_at = offset + 2 * stride * height
        self.assertEqual(alpha_at % image_asset.PIXEL_ALIGN, 0)
        self.assertEqual(list(entry[alpha_at + stride:alpha_at + stride + width]), self.ALPHA[width:])
        self.assertEqual(len(entry), alpha_at + stride * height)

    def test_an_opaque_entry_has_no_alpha(self):
        self.assertIsNone(image_asset.decode_alpha(image_asset.encode(3, 2, self.PIXELS))[3])

    def test_alpha_the_wrong_length_or_past_a_byte_is_refused(self):
        for alpha in (self.ALPHA[:-1], [256] + self.ALPHA[1:]):
            with self.subTest(alpha=alpha), self.assertRaises(ImageError):
                image_asset.encode(3, 2, self.PIXELS, alpha)

    def test_a_stride_off_its_step_or_an_alpha_plane_one_byte_short_is_refused(self):
        entry = image_asset.encode(3, 2, self.PIXELS, self.ALPHA)
        for bad in (edited(entry, AT_STRIDE, "<I", image_asset.RGB565A8_STRIDE_STEP + 1), entry[:-2]):
            with self.assertRaises(ImageError):
                image_asset.decode_alpha(bad)


class SpriteTests(unittest.TestCase):
    """A 5 x 4 picture with a one-pixel border: its centre is 3 x 2."""
    WIDTH, HEIGHT, BORDER = 5, 4, (1, 1, 1, 1)
    CENTRE = [(x, y) for y in range(1, 3) for x in range(1, 4)]

    def picture(self, centre_colour=7, centre_alpha=128, odd_one=None):
        pixels, alpha = [], []
        for y in range(self.HEIGHT):
            for x in range(self.WIDTH):
                inside = (x, y) in self.CENTRE
                pixels.append(centre_colour if inside else 0x1000 + x + y * self.WIDTH)
                alpha.append(centre_alpha if inside else 255)
        if odd_one is not None:
            pixels[odd_one] += 1
        return image_asset.encode(self.WIDTH, self.HEIGHT, pixels, alpha)

    def test_the_border_and_picture_read_back(self):
        image = self.picture()
        border, centre, decoded = image_asset.decode_sprite(image_asset.encode_sprite(self.BORDER, image))
        self.assertEqual(border, self.BORDER)
        self.assertEqual(decoded, image_asset.decode_alpha(image))

    def test_the_picture_follows_the_sprite_header_4_aligned(self):
        image = self.picture()
        entry = image_asset.encode_sprite(self.BORDER, image)
        self.assertEqual(image_asset.SPRITE_HEADER.size % image_asset.PIXEL_ALIGN, 0)
        self.assertEqual(entry[image_asset.SPRITE_HEADER.size:], image)

    def test_a_centre_of_one_colour_at_one_alpha_is_flat(self):
        entry = image_asset.encode_sprite(self.BORDER, self.picture())
        self.assertEqual(image_asset.decode_sprite(entry)[1], (7, 128))

    def test_a_centre_with_one_other_pixel_is_not_flat(self):
        centre_pixel = 1 + 1 * self.WIDTH
        entry = image_asset.encode_sprite(self.BORDER, self.picture(odd_one=centre_pixel))
        self.assertIsNone(image_asset.decode_sprite(entry)[1])

    def test_a_clear_centre_is_flat_whatever_its_colours(self):
        centre_pixel = 1 + 1 * self.WIDTH
        entry = image_asset.encode_sprite(self.BORDER, self.picture(centre_alpha=0, odd_one=centre_pixel))
        self.assertEqual(image_asset.decode_sprite(entry)[1], (0, 0))

    def test_a_border_leaving_no_centre_column_or_row_is_refused(self):
        for border in ((2, 1, 3, 1), (1, 2, 1, 2), (256, 0, 0, 0)):
            with self.subTest(border=border), self.assertRaises(ImageError):
                image_asset.encode_sprite(border, self.picture())

    def test_a_one_bit_picture_is_not_a_sprite(self):
        with self.assertRaises(ImageError):
            image_asset.encode_sprite((0, 0, 0, 0), image_asset.encode_mono([[True, False]]))

    def test_each_header_value_the_firmware_refuses_is_refused_here(self):
        entry = image_asset.encode_sprite(self.BORDER, self.picture())
        for offset, fmt, value in ((0, "<H", 2), (6, "<B", 2), (10, "<H", 1), (2, "<B", 4)):
            with self.subTest(offset=offset, value=value), self.assertRaises(ImageError):
                image_asset.decode_sprite(edited(entry, offset, fmt, value))
        with self.assertRaises(ImageError):
            image_asset.decode_sprite(entry[:image_asset.SPRITE_HEADER.size - 1])

    def test_a_border_turns_with_the_picture(self):
        self.assertEqual(image_asset.turn_border((1, 2, 3, 4), 1), (4, 1, 2, 3))
        self.assertEqual(image_asset.turn_border((1, 2, 3, 4), 4), (1, 2, 3, 4))


class BakeTests(unittest.TestCase):
    def bake(self, toml, rows=None, name="pic"):
        folder = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        rows = rows or [[(255, 0, 0, 255), (0, 255, 0, 255)], [(0, 0, 255, 255), (255, 255, 255, 255)]]
        (folder / "pic.png").write_bytes(png(len(rows[0]), len(rows), rows))
        root = folder / f"{name}.image.toml"
        root.write_text(toml)
        return root

    def test_colours_are_the_panel_s_byte_swapped_rgb565(self):
        width, height, pixels = image_asset.decode(image_asset.bake(self.bake('source = "pic.png"\n'))[1])
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(pixels, [0x00F8, 0xE007, 0x1F00, 0xFFFF])

    def test_a_quarter_turn_is_clockwise(self):
        root = self.bake('source = "pic.png"\nquarter_turns = 1\n')
        self.assertEqual(image_asset.decode(image_asset.bake(root)[1])[2], [0x1F00, 0x00F8, 0xFFFF, 0xE007])

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

    def test_a_source_with_any_transparency_keeps_its_alpha(self):
        rows = [[(1, 2, 3, 255), (1, 2, 3, 254)]]
        kind, entry = image_asset.bake(self.bake('source = "pic.png"\n', rows))
        self.assertEqual(kind, image_asset.TYPE)
        self.assertEqual(HEADER.unpack_from(entry)[1], image_asset.FORMAT_RGB565A8)
        self.assertEqual(image_asset.decode_alpha(entry)[3], [255, 254])

    def test_an_opaque_source_stays_rgb565(self):
        kind, entry = image_asset.bake(self.bake('source = "pic.png"\n'))
        self.assertEqual((kind, HEADER.unpack_from(entry)[1]), (image_asset.TYPE, image_asset.FORMAT_RGB565))

    def test_alpha_turns_with_the_colour(self):
        rows = [[(255, 0, 0, 10), (0, 255, 0, 20)], [(0, 0, 255, 30), (255, 255, 255, 40)]]
        entry = image_asset.bake(self.bake('source = "pic.png"\nquarter_turns = 1\n', rows))[1]
        _, _, pixels, alpha = image_asset.decode_alpha(entry)
        self.assertEqual(pixels, [0x1F00, 0x00F8, 0xFFFF, 0xE007])
        self.assertEqual(alpha, [30, 10, 40, 20])

    def test_a_border_makes_a_sprite_its_border_turned_with_the_source(self):
        rows = [[(9, 9, 9, 255)] * 3 for _ in range(4)]
        kind, entry = image_asset.bake(self.bake('source = "pic.png"\nquarter_turns = 1\nborder = [1, 0, 0, 2]\n',
                                                 rows))
        border, centre, (width, height, _, _) = image_asset.decode_sprite(entry)
        self.assertEqual((kind, border, (width, height)), (image_asset.SPRITE_TYPE, (2, 1, 0, 0), (4, 3)))
        self.assertIsNotNone(centre)

    def test_the_test_tile_is_a_translucent_sprite_with_a_flat_centre(self):
        root = TOOLS.parent / "main/draw/sprite_test/tile.image.toml"
        border, centre, (width, height, _, alpha) = image_asset.decode_sprite(image_asset.bake(root)[1])
        self.assertEqual(border, (16, 16, 16, 16))
        self.assertEqual((width, height), (48, 48))
        self.assertEqual(alpha[0], 0)
        self.assertEqual(centre[1], 128)

    def test_a_malformed_image_toml_is_refused(self):
        for text in ('source = "../pic.png"\n', 'source = "pic.bmp"\n', 'source = "pic.png"\nquarter_turns = 4\n',
                     'source = "pic.png"\nquarter_turns = true\n', 'source = "pic.png"\nscale = 2\n', "",
                     'source = "pic.png"\nborder = [1, 1, 1]\n', 'source = "pic.png"\nborder = [1, 1, 1, -1]\n',
                     'source = "pic.png"\nborder = [1, 0, 1, 0]\n'):
            with self.subTest(text=text), self.assertRaises(ImageError):
                image_asset.bake(self.bake(text))

    def test_build_pack_makes_a_pack_named_after_the_image_toml(self):
        root = self.bake('source = "pic.png"\n', name="sky")
        packs = build_pack.pack_bytes([root.parent])
        self.assertEqual(list(packs), ["sky"])
        self.assertEqual(list(parse_pack(packs["sky"])), ["sky"])

    def test_build_pack_holds_a_sprite_under_its_own_name(self):
        root = self.bake('source = "pic.png"\nborder = [0, 0, 0, 0]\n', name="tile")
        entries = parse_pack(build_pack.pack_bytes([root.parent])["tile"])
        self.assertEqual(entries["tile"][0], image_asset.SPRITE_TYPE)


if __name__ == "__main__":
    unittest.main()

"""The directory writer's page layout and reader checks."""

import pathlib
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from asset import asset_directory
from asset.asset_directory import build_directory, parse_directory
from asset.asset_pack import PackError, NAME_BYTES, build_pack as make_pack, parse_pack
from device.panel_size import read_define

LIT_MESH = b"LMSH"


class DirectoryTests(unittest.TestCase):
    def image(self):
        return build_directory([("one", make_pack([("a", LIT_MESH, b"1")])), ("two", make_pack([("b", LIT_MESH, b"22" * 3000)]))])

    @staticmethod
    def row(index):
        return asset_directory.DIRECTORY_HEADER.size + asset_directory.DIRECTORY_ROW.size * index

    def reseal(self, image):
        """`image` with its directory CRC-32 recomputed after an edit to a row."""
        count = struct.unpack_from("<I", image, 12)[0]
        struct.pack_into("<I", image, 4, zlib.crc32(bytes(image[8:self.row(count)])))
        return bytes(image)

    def test_layout_constants_come_from_the_reader_header(self):
        header = (pathlib.Path(__file__).resolve().parents[2] / "main/asset/asset_directory.h").read_text()
        self.assertEqual(asset_directory.MMU_PAGE_SIZE, read_define(header, "ASSET_MMU_PAGE_SIZE"))
        self.assertEqual(asset_directory.DIRECTORY_VERSION, read_define(header, "ASSET_DIRECTORY_VERSION"))

    def test_each_pack_parses_back_on_its_own_page(self):
        image = self.image()
        packs = parse_directory(image)
        self.assertEqual(list(packs), ["one", "two"])
        self.assertEqual(parse_pack(packs["one"]), {"a": (LIT_MESH, b"1")})
        self.assertEqual(parse_pack(packs["two"]), {"b": (LIT_MESH, b"22" * 3000)})
        for index in range(2):
            _, offset, _ = asset_directory.DIRECTORY_ROW.unpack_from(image, self.row(index))
            self.assertEqual(offset % asset_directory.MMU_PAGE_SIZE, 0)

    def test_a_sector_aligned_pack_off_an_mmu_page_is_refused(self):
        image = bytearray(self.image())
        offset = asset_directory.MMU_PAGE_SIZE + asset_directory.MMU_PAGE_SIZE // 2
        image.extend(bytes(max(0, offset + 1 - len(image))))
        struct.pack_into("<II", image, self.row(0) + NAME_BYTES, offset, 1)
        struct.pack_into("<II", image, self.row(1) + NAME_BYTES, offset + asset_directory.MMU_PAGE_SIZE, 0)
        image.extend(bytes(asset_directory.MMU_PAGE_SIZE))
        with self.assertRaisesRegex(PackError, "page"):
            parse_directory(self.reseal(image))

    def test_the_same_packs_make_the_same_image(self):
        self.assertEqual(self.image(), self.image())

    def test_a_pack_name_that_is_empty_too_long_or_repeated_is_refused(self):
        pack = make_pack([])
        for packs in ([("", pack)], [("x" * 32, pack)], [("a", pack), ("a", pack)]):
            with self.assertRaises(PackError):
                build_directory(packs)

    def test_a_bad_magic_version_or_checksum_is_refused(self):
        for at, value, pattern in ((0, b"NOPE", "magic"), (8, struct.pack("<I", asset_directory.DIRECTORY_VERSION - 1), "version"), (20, b"x", "CRC")):
            image = bytearray(self.image())
            image[at:at + len(value)] = value
            with self.assertRaisesRegex(PackError, pattern):
                parse_directory(bytes(image))

    def test_a_row_out_of_range_misaligned_or_repeated_is_refused_even_with_a_good_checksum(self):
        base = self.image()
        cases = ((self.row(1) + 32, asset_directory.MMU_PAGE_SIZE, "overlaps"),                     # over the first pack
                 (self.row(1) + 36, len(base), "outside"),                      # ends past the image
                 (self.row(1) + 32, asset_directory.MMU_PAGE_SIZE * 2 + 16, "page"),
                 (self.row(0) + 32, 0, "outside"))                              # over the directory itself
        for at, value, pattern in cases:
            image = bytearray(base)
            struct.pack_into("<I", image, at, value)
            with self.assertRaisesRegex(PackError, pattern):
                parse_directory(self.reseal(image))
        repeated = bytearray(base)
        repeated[self.row(1):self.row(1) + 3] = b"one"
        with self.assertRaisesRegex(PackError, "repeated"):
            parse_directory(self.reseal(repeated))
        repeated[self.row(1):self.row(1) + 32] = b"n" * 32
        with self.assertRaisesRegex(PackError, "fills"):
            parse_directory(self.reseal(repeated))


if __name__ == "__main__":
    unittest.main()

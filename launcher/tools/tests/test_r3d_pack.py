"""The asset pack writer (r3d/asset_pack.py), the pack builder and the committed
pack: what the firmware's asset_pack.c reads. The C side has its own suite,
suite_asset_pack.c; this one proves what the tools write is what that reads."""

import pathlib
import re
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import asset_pack, build_pack  # noqa: E402
from r3d.asset_pack import PackError, build_pack as make_pack, parse_pack  # noqa: E402
from r3d.import_settings import SettingsError  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
PACK = REPO / "launcher" / "assets" / "assets.bin"
LIT_MESH = 1
SOURCE = ('[source]\nurl = "https://example.invalid/m.zip"\nsha256 = "00"\npath = "m.obj"\ncache = "m"\n'
          'credit = "A model."\n')


def seal(pack):
    """`pack` with its CRC-32 recomputed, for a test that edits a field the CRC covers."""
    body = bytearray(pack)
    struct.pack_into("<I", body, 16, asset_pack.zlib.crc32(bytes(body[asset_pack.HEADER.size:])))
    return bytes(body)


class RoundTripTests(unittest.TestCase):
    def test_what_is_built_parses_back_with_its_names_types_and_bytes(self):
        pack = make_pack([("one", LIT_MESH, b"abc"), ("two", 9, bytes(range(40)), 64)])
        entries = parse_pack(pack)
        self.assertEqual(entries, {"one": (LIT_MESH, b"abc"), "two": (9, bytes(range(40)))})

    def test_each_entry_starts_on_its_alignment(self):
        pack = make_pack([("a", 1, b"x"), ("b", 1, b"yy", 64), ("c", 1, b"z", 16)])
        for index in range(3):
            _, _, offset, _, align = asset_pack.ENTRY.unpack_from(pack, asset_pack.HEADER.size + asset_pack.ENTRY.size * index)
            self.assertEqual(offset % align, 0)

    def test_the_same_entries_make_the_same_bytes(self):
        entries = [("a", 1, b"x"), ("b", 1, b"yy")]
        self.assertEqual(make_pack(entries), make_pack(entries))

    def test_a_name_that_is_empty_too_long_or_repeated_is_refused(self):
        for entries in ([("", 1, b"")], [("x" * 32, 1, b"")], [("a", 1, b""), ("a", 1, b"")]):
            with self.assertRaises(PackError):
                make_pack(entries)

    def test_an_alignment_that_is_not_a_power_of_two_is_refused(self):
        with self.assertRaises(PackError):
            make_pack([("a", 1, b"x", 12)])


class RejectionTests(unittest.TestCase):
    def pack(self):
        return make_pack([("a", LIT_MESH, b"0123456789abcdef")])

    def test_one_changed_byte_fails_the_checksum(self):
        pack = bytearray(self.pack())
        pack[-1] ^= 1
        with self.assertRaisesRegex(PackError, "CRC"):
            parse_pack(bytes(pack))

    def test_another_version_is_refused(self):
        pack = bytearray(self.pack())
        struct.pack_into("<I", pack, 4, asset_pack.VERSION + 1)
        with self.assertRaisesRegex(PackError, "version"):
            parse_pack(bytes(pack))

    def test_a_wrong_magic_or_a_short_file_is_refused(self):
        pack = bytearray(self.pack())
        pack[0:4] = b"NOPE"
        with self.assertRaisesRegex(PackError, "magic"):
            parse_pack(bytes(pack))
        with self.assertRaisesRegex(PackError, "header"):
            parse_pack(b"APAK")

    def test_an_entry_leaving_the_pack_or_misaligned_is_refused_even_with_a_good_checksum(self):
        base = self.pack()
        far = bytearray(base)
        struct.pack_into("<I", far, asset_pack.HEADER.size + 40, 10_000)
        with self.assertRaisesRegex(PackError, "outside"):
            parse_pack(seal(bytes(far)))
        crooked = bytearray(base)
        struct.pack_into("<I", crooked, asset_pack.HEADER.size + 36, 98)
        with self.assertRaisesRegex(PackError, "outside"):
            parse_pack(seal(bytes(crooked)))


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def import_file(directory, name, mesh):
    return write(pathlib.Path(directory) / name, SOURCE + f'[output]\ndirectory = "."\nname = "{mesh}"\n')


class BuilderTests(unittest.TestCase):
    def test_the_pack_holds_every_mesh_the_files_name_by_its_name(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for folder, mesh, data in (("a", "one", b"1"), ("a2", "two", b"22"), ("b", "three", b"333")):
                import_file(root / folder, f"{mesh}.import.toml", mesh)
                (root / folder / f"{mesh}.mesh").write_bytes(data)
            entries = parse_pack(build_pack.pack_bytes([root]))
        self.assertEqual({name: data for name, (_, data) in entries.items()}, {"one": b"1", "two": b"22", "three": b"333"})

    def test_a_mesh_never_baked_names_the_command_to_run(self):
        with tempfile.TemporaryDirectory() as directory:
            import_file(directory, "a.import.toml", "one")
            with self.assertRaisesRegex(SettingsError, "mesh_import.py"):
                build_pack.pack_bytes([directory])

    def test_two_import_files_writing_one_name_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            import_file(root / "a", "a.import.toml", "same")
            import_file(root / "b", "b.import.toml", "same")
            (root / "a" / "same.mesh").write_bytes(b"1")
            (root / "b" / "same.mesh").write_bytes(b"2")
            with self.assertRaisesRegex(SettingsError, "same"):
                build_pack.pack_bytes([root])

    def test_check_tells_a_stale_pack_from_a_current_one(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            import_file(root, "a.import.toml", "one")
            (root / "one.mesh").write_bytes(b"1")
            out = root / "assets.bin"
            self.assertEqual(build_pack.main([str(root), "-o", str(out)]), 0)
            self.assertEqual(build_pack.main([str(root), "-o", str(out), "--check"]), 0)
            (root / "one.mesh").write_bytes(b"2")
            self.assertEqual(build_pack.main([str(root), "-o", str(out), "--check"]), 1)


class CommittedTreeTests(unittest.TestCase):
    def test_the_committed_pack_is_what_the_committed_meshes_make(self):
        self.assertEqual(PACK.read_bytes(), build_pack.pack_bytes([build_pack.DEFAULT_SEARCH]))

    def test_every_mesh_a_scene_table_names_is_in_the_committed_pack(self):
        names = parse_pack(PACK.read_bytes())
        tables = sorted((REPO / "launcher" / "main").rglob("*_scene_generated.c"))
        self.assertTrue(tables, "no scene table found: the tree test would pass for nothing")
        for table in tables:
            ids = re.findall(r'^\s*\{"([^"]+)", &\w+\},$', table.read_text(), re.M)
            self.assertTrue(ids, f"{table.name} names no mesh")
            for mesh in ids:
                self.assertIn(mesh, names, f"{table.name} names mesh {mesh!r}, which the pack lacks")
                self.assertEqual(names[mesh][0], LIT_MESH)


if __name__ == "__main__":
    unittest.main()

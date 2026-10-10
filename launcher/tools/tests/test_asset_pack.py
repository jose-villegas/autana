"""The asset pack and pack directory writer (asset/asset_pack.py), the
pack builder and the meshes in the tree: what the firmware's asset_pack.c
and asset_directory.c read. The C side has its own suite, suite_asset_pack.c;
this one proves what the tools write is what that reads."""

import contextlib
import io
import os
import pathlib
import struct
import sys
import tempfile
import tomllib
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset  # noqa: E402
from asset import asset_pack  # noqa: E402
from asset.asset_pack import PackError, build_directory, build_pack as make_pack, parse_directory, parse_pack  # noqa: E402
from r3d import build_pack, scene_asset  # noqa: E402
from r3d.import_settings import SettingsError  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
LIT_MESH = b"LMSH"
SOURCE = '[source]\npath = "m.obj"\ncredit = "c"\n'


def seal(pack):
    """`pack` with its CRC-32 recomputed, for a test that edits a field the CRC covers."""
    body = bytearray(pack)
    struct.pack_into("<I", body, 16, asset_pack.zlib.crc32(bytes(body[asset_pack.HEADER.size:])))
    return bytes(body)


class RoundTripTests(unittest.TestCase):
    def test_what_is_built_parses_back_with_its_names_types_and_bytes(self):
        pack = make_pack([("one", LIT_MESH, b"abc"), ("two", b"TEST", bytes(range(40)), 64)])
        entries = parse_pack(pack)
        self.assertEqual(entries, {"one": (LIT_MESH, b"abc"), "two": (b"TEST", bytes(range(40)))})

    def test_each_entry_starts_on_its_alignment(self):
        pack = make_pack([("a", LIT_MESH, b"x"), ("b", LIT_MESH, b"yy", 64), ("c", LIT_MESH, b"z", 16)])
        for index in range(3):
            _, _, offset, _, align = asset_pack.ENTRY.unpack_from(pack, asset_pack.HEADER.size + asset_pack.ENTRY.size * index)
            self.assertEqual(offset % align, 0)

    def test_the_same_entries_make_the_same_bytes(self):
        entries = [("a", LIT_MESH, b"x"), ("b", LIT_MESH, b"yy")]
        self.assertEqual(make_pack(entries), make_pack(entries))

    def test_a_name_that_is_empty_too_long_or_repeated_is_refused(self):
        for entries in ([("", LIT_MESH, b"")], [("x" * 32, 1, b"")], [("a", LIT_MESH, b""), ("a", LIT_MESH, b"")]):
            with self.assertRaises(PackError):
                make_pack(entries)

    def test_a_type_that_is_not_four_bytes_is_refused(self):
        for kind in (b"LMS", b"LMSHX", 1):
            with self.assertRaises(PackError):
                make_pack([("a", kind, b"x")])

    def test_an_alignment_that_is_not_a_power_of_two_is_refused(self):
        with self.assertRaises(PackError):
            make_pack([("a", LIT_MESH, b"x", 12)])


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

    def test_a_header_with_reserved_bytes_in_use_is_refused(self):
        pack = bytearray(self.pack())
        pack[24] = 1
        with self.assertRaisesRegex(PackError, "reserved"):
            parse_pack(seal(bytes(pack)))

    def test_an_entry_inside_the_table_is_refused(self):
        pack = bytearray(self.pack())
        struct.pack_into("<I", pack, asset_pack.HEADER.size + 36, 64)
        with self.assertRaisesRegex(PackError, "outside"):
            parse_pack(seal(bytes(pack)))

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


class DirectoryTests(unittest.TestCase):
    def image(self):
        return build_directory([("one", make_pack([("a", LIT_MESH, b"1")])), ("two", make_pack([("b", LIT_MESH, b"22" * 3000)]))])

    @staticmethod
    def row(index):
        return asset_pack.DIRECTORY_HEADER.size + asset_pack.DIRECTORY_ROW.size * index

    def reseal(self, image):
        """`image` with its directory CRC-32 recomputed after an edit to a row."""
        count = struct.unpack_from("<I", image, 12)[0]
        struct.pack_into("<I", image, 4, asset_pack.zlib.crc32(bytes(image[8:self.row(count)])))
        return bytes(image)

    def test_each_pack_parses_back_on_its_own_sector(self):
        image = self.image()
        packs = parse_directory(image)
        self.assertEqual(list(packs), ["one", "two"])
        self.assertEqual(parse_pack(packs["one"]), {"a": (LIT_MESH, b"1")})
        self.assertEqual(parse_pack(packs["two"]), {"b": (LIT_MESH, b"22" * 3000)})
        for index in range(2):
            _, offset, _ = asset_pack.DIRECTORY_ROW.unpack_from(image, self.row(index))
            self.assertEqual(offset % asset_pack.SECTOR, 0)

    def test_the_same_packs_make_the_same_image(self):
        self.assertEqual(self.image(), self.image())

    def test_a_pack_name_that_is_empty_too_long_or_repeated_is_refused(self):
        pack = make_pack([])
        for packs in ([("", pack)], [("x" * 32, pack)], [("a", pack), ("a", pack)]):
            with self.assertRaises(PackError):
                build_directory(packs)

    def test_a_bad_magic_version_or_checksum_is_refused(self):
        for at, value, pattern in ((0, b"NOPE", "magic"), (8, struct.pack("<I", 2), "version"), (20, b"x", "CRC")):
            image = bytearray(self.image())
            image[at:at + len(value)] = value
            with self.assertRaisesRegex(PackError, pattern):
                parse_directory(bytes(image))

    def test_a_row_out_of_range_misaligned_or_repeated_is_refused_even_with_a_good_checksum(self):
        base = self.image()
        cases = ((self.row(1) + 32, asset_pack.SECTOR, "overlaps"),                     # over the first pack
                 (self.row(1) + 36, len(base), "outside"),                      # ends past the image
                 (self.row(1) + 32, asset_pack.SECTOR * 2 + 16, "sector"),
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


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def import_file(directory, name, mesh):
    return write(pathlib.Path(directory) / name, SOURCE + f'[output]\nname = "{mesh}"\n')


def variants_file(directory, name, *variants):
    rows = "".join(f'[[variants]]\nname = "{variant}"\n' for variant in variants)
    return write(pathlib.Path(directory) / name, SOURCE + '[output]\n' + rows)


def scene_file(directory, name, *placed):
    """A scene placing each (import file, variant) once."""
    objects = "".join(f'[[objects]]\nname = "o{index}"\n[objects.mesh_renderer]\nmesh = "{mesh}"\nvariant = "{variant}"\n'
                      for index, (mesh, variant) in enumerate(placed))
    return write(pathlib.Path(directory) / name, objects)


def contents(packs):
    return {root: {name: data for name, (_, data) in parse_pack(pack).items()} for root, pack in packs.items()}


class BuilderTests(unittest.TestCase):
    def test_each_free_import_is_a_pack_named_after_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for folder, mesh, data in (("a", "one", b"1"), ("a2", "two", b"22"), ("b", "three", b"333")):
                import_file(root / folder, f"{mesh}.import.toml", mesh)
                (root / folder / f"{mesh}.mesh").write_bytes(data)
            packs = build_pack.pack_bytes([root])
        self.assertEqual(contents(packs), {"one": {"one": b"1"}, "two": {"two": b"22"}, "three": {"three": b"333"}})

    def test_a_scene_is_a_pack_of_its_entry_and_every_mesh_it_places_and_its_imports_make_none(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            variants_file(root, "chair.import.toml", "chair")
            variants_file(root, "table.import.toml", "table")
            variants_file(root, "free.import.toml", "lamp")
            scene_file(root, "room.scene.toml", ("chair.import.toml", "chair"), ("table.import.toml", "table"))
            for mesh in ("chair", "table", "lamp"):
                (root / f"{mesh}.mesh").write_bytes(mesh.encode())
            packs = build_pack.pack_bytes([root])
            room = scene_asset.bake(root / "room.scene.toml")
        self.assertEqual(contents(packs), {"room": {"room": room, "chair": b"chair", "table": b"table"},
                                           "free": {"lamp": b"lamp"}})

    def test_a_mesh_two_roots_name_is_refused_naming_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            variants_file(root, "kit.import.toml", "chair")
            scene_file(root, "room.scene.toml", ("kit.import.toml", "chair"))
            scene_file(root, "hall.scene.toml", ("kit.import.toml", "chair"))
            (root / "chair.mesh").write_bytes(b"c")
            with self.assertRaisesRegex(SettingsError, "'chair' is named by packs"):
                build_pack.pack_bytes([root])

    def test_two_roots_with_one_name_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            import_file(root / "a", "same.import.toml", "one")
            import_file(root / "b", "same.import.toml", "two")
            with self.assertRaisesRegex(SettingsError, "pack named 'same'"):
                build_pack.pack_bytes([root])

    def test_a_replaced_mesh_comes_from_its_own_file_and_must_exist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            import_file(root / "a", "a.import.toml", "one")
            (root / "a" / "one.mesh").write_bytes(b"1")
            (root / "scratch.mesh").write_bytes(b"9")
            packs = build_pack.pack_bytes([root / "a"], [f"one={root / 'scratch.mesh'}"])
            self.assertEqual(contents(packs), {"a": {"one": b"9"}})
            with self.assertRaisesRegex(SettingsError, "no such mesh"):
                build_pack.pack_bytes([root / "a"], ["other=x.mesh"])

    def test_a_mesh_never_baked_names_the_command_to_run(self):
        with tempfile.TemporaryDirectory() as directory:
            import_file(directory, "a.import.toml", "one")
            with self.assertRaisesRegex(SettingsError, "mesh_import.py"):
                build_pack.pack_bytes([directory])

    def test_the_command_writes_each_pack_and_the_image_and_drops_stale_packs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for mesh in ("one", "two"):
                import_file(root / "src", f"{mesh}.import.toml", mesh)
                (root / "src" / f"{mesh}.mesh").write_bytes(mesh.encode())
            out = root / "out"
            out.mkdir()
            (out / "gone.apak").write_bytes(b"old")
            with contextlib.redirect_stdout(io.StringIO()):
                build_pack.main(["-o", str(out), "--image", str(root / "assets.bin"), str(root / "src")])
            self.assertEqual(sorted(p.name for p in out.iterdir()), ["one.apak", "two.apak"])
            image = parse_directory((root / "assets.bin").read_bytes())
            self.assertEqual({name: (out / f"{name}.apak").read_bytes() for name in image}, image)
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                build_pack.main(["--pack-of", "two", str(root / "src")])
            self.assertEqual(printed.getvalue().strip(), "two")

    def test_unchanged_packs_and_image_keep_timestamps_and_changed_bytes_are_written(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            out, image = root / "packs", root / "assets.bin"
            packs = {"one": make_pack([("a", LIT_MESH, b"one")]),
                     "two": make_pack([("b", LIT_MESH, b"two")])}
            build_pack.write_packs(out, packs, image)
            files = [out / "one.apak", out / "two.apak", image]
            for path in files:
                os.utime(path, ns=(1_000_000_000, 1_000_000_000))
            before = [(path.stat().st_mtime_ns, path.stat().st_ctime_ns) for path in files]
            build_pack.write_packs(out, packs, image)
            self.assertEqual([(path.stat().st_mtime_ns, path.stat().st_ctime_ns) for path in files], before)
            packs["one"] = make_pack([("a", LIT_MESH, b"changed")])
            build_pack.write_packs(out, packs, image)
            self.assertEqual(files[0].read_bytes(), packs["one"])
            self.assertNotEqual(files[0].stat().st_mtime_ns, before[0][0])
            self.assertEqual((files[1].stat().st_mtime_ns, files[1].stat().st_ctime_ns), before[1])
            self.assertEqual(parse_directory(image.read_bytes()), packs)
            self.assertNotEqual(image.stat().st_mtime_ns, before[2][0])


class DemoAssetTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.main = self.root / "launcher/main"
        self.main.mkdir(parents=True)
        self.demo = self.root / "launcher/demo/sample"
        import_file(self.demo, "sample.import.toml", "one")
        (self.demo / "one.mesh").write_bytes(b"demo")
        patch = mock.patch.object(build_pack, "DEMO", self.demo.parent)
        patch.start()
        self.addCleanup(patch.stop)

    def test_manifest_pulls_in_named_demo_packs(self):
        write(self.main / "apps/example/demo_assets.toml", 'demo = ["sample"]')
        self.assertEqual(contents(build_pack.pack_bytes([self.main])), {"sample": {"one": b"demo"}})

    def test_unknown_demo_names_its_manifest_and_name(self):
        manifest = write(self.main / "apps/example/demo_assets.toml", 'demo = ["missing"]')
        with self.assertRaises(SettingsError) as caught:
            build_pack.pack_bytes([self.main])
        self.assertIn(str(manifest), str(caught.exception))
        self.assertIn("missing", str(caught.exception))

    def test_demo_is_not_packed_without_a_manifest(self):
        self.assertEqual(build_pack.pack_bytes([self.main]), {})

    def test_repeated_demo_and_overlapping_search_folders_are_searched_once(self):
        for app in ("a", "b"):
            write(self.main / app / "demo_assets.toml", 'demo = ["sample", "sample"]')
        self.assertEqual(contents(build_pack.pack_bytes([self.main, self.main / "a", self.demo])),
                         {"sample": {"one": b"demo"}})

    def test_only_the_named_demo_is_packed_and_removing_the_manifest_drops_it(self):
        other = self.demo.parent / "other"
        import_file(other, "other.import.toml", "two")
        (other / "two.mesh").write_bytes(b"other")
        manifest = write(self.main / "demo_assets.toml", 'demo = ["sample"]')
        self.assertEqual(contents(build_pack.pack_bytes([self.main])), {"sample": {"one": b"demo"}})
        manifest.unlink()
        self.assertEqual(build_pack.pack_bytes([self.main]), {})

    def test_malformed_manifests_name_the_file(self):
        for text, pattern in (('demo = [', 'Invalid value'), ('', 'is required'),
                              ('demo = []\nextra = 1', 'not a known setting'),
                              ('demo = "sample"', 'array of strings'),
                              ('demo = [1]', 'non-empty string')):
            with self.subTest(text=text):
                manifest = write(self.main / "demo_assets.toml", text)
                with self.assertRaisesRegex(SettingsError, pattern) as caught:
                    build_pack.pack_bytes([self.main])
                self.assertIn(str(manifest), str(caught.exception))

    def test_non_plain_names_are_rejected_before_folder_lookup(self):
        for name in ("..", ".", "x/source", "x\\source"):
            with self.subTest(name=name):
                (self.demo.parent / name).mkdir(parents=True, exist_ok=True)
                manifest = write(self.main / "demo_assets.toml", f"demo = ['{name}']")
                with self.assertRaises(SettingsError) as caught:
                    build_pack.pack_bytes([self.main])
                self.assertIn(str(manifest), str(caught.exception))
                self.assertIn("must be letters", str(caught.exception))

    def test_relative_and_absolute_searches_produce_one_pack(self):
        relative = pathlib.Path(os.path.relpath(self.demo))
        self.assertEqual(contents(build_pack.pack_bytes([relative, self.demo.resolve()])),
                         {"sample": {"one": b"demo"}})

    def test_a_demo_manifest_is_refused_naming_its_file(self):
        write(self.main / "demo_assets.toml", 'demo = ["sample"]')
        manifest = write(self.demo / "demo_assets.toml", 'demo = ["sample"]')
        with self.assertRaisesRegex(SettingsError, "only an app names demo assets") as caught:
            build_pack.pack_bytes([self.main])
        self.assertIn(str(manifest), str(caught.exception))

    def test_a_manifest_nested_in_a_demo_is_refused_naming_its_file(self):
        write(self.main / "demo_assets.toml", 'demo = ["sample"]')
        manifest = write(self.demo / "nested" / "demo_assets.toml", 'demo = ["sample"]')
        with self.assertRaisesRegex(SettingsError, "only an app names demo assets") as caught:
            build_pack.pack_bytes([self.main])
        self.assertIn(str(manifest), str(caught.exception))

    def test_a_manifest_can_select_two_different_demos(self):
        other = self.demo.parent / "other"
        import_file(other, "other.import.toml", "two")
        (other / "two.mesh").write_bytes(b"other")
        write(self.main / "demo_assets.toml", 'demo = ["sample", "other"]')
        self.assertEqual(contents(build_pack.pack_bytes([self.main])),
                         {"sample": {"one": b"demo"}, "other": {"two": b"other"}})


class TreeTests(unittest.TestCase):
    def test_each_tree_manifest_selects_its_demo_root_packs(self):
        packs = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH])
        manifests = list(build_pack.DEFAULT_SEARCH.rglob("demo_assets.toml"))
        self.assertTrue(manifests)
        for manifest in manifests:
            for name in tomllib.loads(manifest.read_text())["demo"]:
                demo = build_pack.DEMO / name
                roots = build_pack.pack_bytes([demo])
                self.assertTrue(roots, str(demo))
                for root, data in roots.items():
                    with self.subTest(manifest=manifest, root=root):
                        self.assertEqual(packs[root], data)

    def test_the_packs_in_the_tree_pack_and_parse(self):
        packs = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH])
        self.assertTrue(packs)
        parse_directory(build_directory(sorted(packs.items())))
        kinds = {kind for pack in packs.values() for kind, _ in parse_pack(pack).values()}
        self.assertIn(LIT_MESH, kinds)

    def test_each_scene_s_pack_holds_its_entry_every_mesh_it_names_and_its_camera_s_tracks(self):
        packs = build_pack.pack_bytes([build_pack.DEFAULT_SEARCH])
        scenes = [path for path in build_pack.input_files([build_pack.DEFAULT_SEARCH]) if path.name.endswith(build_pack.SCENE)]
        self.assertTrue(scenes, "no scene file found: the tree test would pass for nothing")
        for path in scenes:
            name = scene_asset.scene_id(path)
            self.assertIn(name, packs, f"{path.name} makes no pack")
            entries = parse_pack(packs[name])
            self.assertEqual(entries[name][0], scene_asset.TYPE)
            scene = scene_asset.decode(entries[name][1])
            self.assertTrue(scene["renderers"], f"{path.name} names no mesh")
            for renderer in scene["renderers"]:
                mesh = renderer["mesh"]
                self.assertIn(mesh, entries, f"{path.name} names mesh {mesh!r}, which its pack does not hold")
                self.assertEqual(entries[mesh][0], LIT_MESH)
            for camera in scene["cameras"]:
                if not camera["clip"]:
                    continue
                kind, clip = entries[camera["clip"]]
                self.assertEqual(kind, tracks_asset.TYPE)
                tracks = {track["name"] for track in tracks_asset.decode(clip)[0]}
                for part in ("translation", "rotation"):
                    self.assertIn(f"{camera['node']}/{part}", tracks, f"{path.name}: clip {camera['clip']!r}")


if __name__ == "__main__":
    unittest.main()

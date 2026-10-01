"""`autana flash assets`: the asset pack's own flash. assets_image.py checks the
pack and lays out an image; the rest is the ordinary snapshot-and-write path,
run here against the fake writer, so no board is touched."""

import isolation  # noqa: F401  (first: keeps the suite out of real records)
import contextlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from argparse import Namespace
from datetime import datetime
from pathlib import Path
from unittest import mock

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
import assets_image  # noqa: E402
import autana_config  # noqa: E402
import device  # noqa: E402
import device_lock  # noqa: E402
import fake_flash  # noqa: E402

BOARD = "90:70:69:FE:A3:08"
PARTITIONS = ("# Name,   Type, SubType, Offset,  Size, Flags\n"
              "nvs,      data, nvs,     ,         0x6000,\n"
              "factory,  app,  factory, ,         8M,\n"
              "assets,   data, 0x40,    0x810000, 0x7F0000,\n")


def pack(entries=0, body=b""):
    """A well-formed pack: a header (magic, version, count, size, CRC-32 of the rest) and `body`."""
    return struct.pack("<4sIIII12x", b"APAK", 1, entries, 32 + len(body), zlib.crc32(body)) + body


def project(root, partitions=PARTITIONS, data=None):
    """A worktree that can flash assets: the real assets_image.py, the fake flash script, a table and a pack."""
    root = Path(root)
    fake_flash.worktree(root)
    shutil.copy(DEVICE / "assets_image.py", root / device.ASSETS_SCRIPT)
    (root / "launcher" / "assets").mkdir(parents=True)
    (root / "launcher" / "partitions.csv").write_text(partitions, encoding="utf-8")
    (root / "launcher" / "assets" / "assets.bin").write_bytes(pack() if data is None else data)
    return root


class AssetsImageTests(unittest.TestCase):
    def image(self, **options):
        with tempfile.TemporaryDirectory() as directory:
            root = project(Path(directory) / "engine", **options)
            out = Path(directory) / "image"
            code = subprocess.run([sys.executable, str(root / device.ASSETS_SCRIPT), "--project", str(root),
                                   "--out", str(out)], capture_output=True, text=True)
            files = {path.name: path.read_bytes() for path in out.glob("*")} if out.is_dir() else {}
        return code, files

    def test_the_image_writes_the_pack_at_the_partitions_offset(self):
        code, files = self.image()
        self.assertEqual(code.returncode, 0, code.stderr)
        self.assertEqual(files["assets.bin"], pack())
        self.assertIn(b"0x810000 assets.bin", files["flash_args"])
        self.assertEqual(files["build_id.txt"], b"assets-%08x\n" % zlib.crc32(b""))

    def test_a_pack_whose_checksum_is_wrong_is_not_flashed(self):
        damaged = bytearray(pack(0, b"abcd"))
        damaged[-1] ^= 1
        code, files = self.image(data=bytes(damaged))
        self.assertEqual(code.returncode, 1)
        self.assertIn("checksum", code.stderr)
        self.assertEqual(files, {})

    def test_what_is_not_a_pack_is_not_flashed(self):
        code, _ = self.image(data=b"x" * 64)
        self.assertEqual(code.returncode, 1)
        self.assertIn("not an asset pack", code.stderr)

    def test_a_pack_larger_than_the_partition_is_not_flashed(self):
        code, _ = self.image(partitions=PARTITIONS.replace("0x7F0000", "0x10"))
        self.assertEqual(code.returncode, 1)
        self.assertIn("partition", code.stderr)

    def test_a_table_without_an_explicit_offset_or_the_partition_is_refused(self):
        for table, message in ((PARTITIONS.replace("0x810000", ""), "explicit offset"),
                               (PARTITIONS.replace("assets,", "other,"), "no partition named"),
                               (PARTITIONS.replace("0x810000", "0x810010"), "aligned")):
            code, _ = self.image(partitions=table)
            self.assertEqual(code.returncode, 1, table)
            self.assertIn(message, code.stderr)

    def test_the_repository_table_names_an_aligned_partition_inside_the_flash_after_the_app(self):
        table = DEVICE.parents[1] / "launcher" / "partitions.csv"
        offset, size = assets_image.partition_offset(table)
        self.assertGreaterEqual(offset, 0x10000 + 8 * 1024 * 1024)
        self.assertLessEqual(offset + size, 16 * 1024 * 1024)
        self.assertEqual(offset % 0x10000, 0)


class FlashAssetsTests(unittest.TestCase):
    def flash(self, directory, run):
        worktree = project(Path(directory) / "engine")
        args = Namespace(owner="agent", purpose="flash", wait=0, variant="assets", worktree=str(worktree), out=None)
        store = mock.Mock()
        store.root = Path(os.environ["_AUTANA_DEVICE_LOCK_ROOT"])
        store.acquire.return_value = device_lock.Held({"token": "sekrit", "acquired_at": 1000.0})
        output = io.StringIO()
        with mock.patch.object(device, "records_root", return_value=Path(directory) / "records"), \
             mock.patch.object(device, "now", return_value=datetime(2026, 10, 1, 12, 0, 0)), \
             mock.patch.object(device, "run_to_end", run), \
             mock.patch.object(device, "find_board", return_value=device.Board(BOARD, BOARD)), \
             mock.patch.object(device, "reset"), \
             mock.patch.object(device, "open_serial", return_value=mock.MagicMock()), \
             mock.patch.object(device, "git_commit", return_value="deadbeef"), \
             contextlib.redirect_stdout(output):
            with device.build_image(args, BOARD) as built:
                device.write_image(built, store, BOARD)
        return output.getvalue()

    def test_the_pack_is_checked_snapshotted_and_written_under_the_lock_with_no_firmware_build(self):
        written = {}

        def run(command, lost=None, **options):
            if Path(command[1]).name == "assets_image.py":
                return subprocess.run(command, check=True, stdout=options["stdout"], stderr=subprocess.STDOUT)
            self.assertEqual(Path(command[1]).name, "flash_image.sh")
            image = Path(command[-1])
            written.update({path.name: path.read_bytes() for path in image.glob("*")})
            written["token"] = options["env"][autana_config.TOKEN_ENV]
            return None

        with tempfile.TemporaryDirectory() as directory:
            output = self.flash(directory, run)
        self.assertIn("flashed BUILD_ID=assets-", output)
        self.assertEqual(written["assets.bin"], pack())
        self.assertIn(b"0x810000 assets.bin", written["flash_args"])
        self.assertEqual(written["token"], "sekrit")

    def test_a_bad_pack_stops_before_the_lock_is_taken(self):
        calls = []

        def run(command, lost=None, **options):
            calls.append(Path(command[1]).name)
            if Path(command[1]).name == "assets_image.py":
                return subprocess.run(command, check=True, stdout=options["stdout"], stderr=subprocess.STDOUT)
            return None

        with tempfile.TemporaryDirectory() as directory:
            worktree = project(Path(directory) / "engine", data=b"not a pack, not even close to one.....")
            args = Namespace(owner="agent", purpose="flash", wait=0, variant="assets", worktree=str(worktree), out=None)
            with mock.patch.object(device, "records_root", return_value=Path(directory) / "records"), \
                 mock.patch.object(device, "run_to_end", run), \
                 mock.patch.object(device, "git_commit", return_value="deadbeef"), \
                 contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "assets_image.py failed"):
                    device.build_image(args, BOARD)
        self.assertEqual(calls, ["assets_image.py"])


if __name__ == "__main__":
    unittest.main()

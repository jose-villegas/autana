#!/usr/bin/env python3
"""Checks where patched_qemu.py looks for and puts the patched QEMU."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import patched_qemu


def fake_idf(root, versions):
    tools = root / "tools"
    tools.mkdir(parents=True)
    (tools / "tools.json").write_text(json.dumps({"tools": [
        {"name": "riscv32-esp-elf", "versions": [{"name": "unrelated", "status": "recommended"}]},
        {"name": patched_qemu.TOOL, "versions": versions},
    ]}), encoding="utf-8")
    return root


class PatchedQemuTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.patches = self.root / "patches"
        self.patches.mkdir()
        (self.patches / "0001-a.patch").write_text("first\n", encoding="utf-8")
        for name, value in (("PATCHES", self.patches), ("INSTALL_ROOT", self.root / "install")):
            patcher = mock.patch.object(patched_qemu, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.idf = fake_idf(self.root / "idf", [
            {"name": "esp_develop_9.0.0_20240606", "status": "supported"},
            {"name": "esp_develop_9.2.2_20260417", "status": "recommended"},
        ])

    def test_the_version_is_the_one_esp_idf_recommends(self):
        self.assertEqual("esp_develop_9.2.2_20260417", patched_qemu.idf_qemu_version(self.idf))

    def test_the_install_directory_names_the_version_and_the_patch_set(self):
        directory = patched_qemu.install_dir(self.idf)

        self.assertEqual(self.root / "install", directory.parent)
        self.assertEqual("esp_develop_9.2.2_20260417-" + patched_qemu.patch_hash(), directory.name)
        self.assertEqual(patched_qemu.PATCH_HASH_CHARS, len(patched_qemu.patch_hash()))

    def test_editing_adding_or_renaming_a_patch_moves_the_install(self):
        seen = {patched_qemu.patch_hash()}
        (self.patches / "0001-a.patch").write_text("first, edited\n", encoding="utf-8")
        seen.add(patched_qemu.patch_hash())
        (self.patches / "0002-b.patch").write_text("second\n", encoding="utf-8")
        seen.add(patched_qemu.patch_hash())
        (self.patches / "0002-b.patch").rename(self.patches / "0003-b.patch")
        seen.add(patched_qemu.patch_hash())

        self.assertEqual(4, len(seen))

    def test_nothing_is_installed_until_the_binary_exists(self):
        self.assertIsNone(patched_qemu.installed(self.idf))

        binary = patched_qemu.binary(patched_qemu.install_dir(self.idf))
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"")

        self.assertEqual(binary, patched_qemu.installed(self.idf))

    def test_a_build_for_another_version_is_not_used(self):
        other = fake_idf(self.root / "other", [{"name": "esp_develop_9.3.0_20270101", "status": "recommended"}])
        binary = patched_qemu.binary(patched_qemu.install_dir(self.idf))
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"")

        self.assertIsNone(patched_qemu.installed(other))


if __name__ == "__main__":
    unittest.main()

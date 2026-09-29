#!/usr/bin/env python3
"""Checks that the host build ID names the ELF it writes."""

import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path


LAUNCHER = Path(__file__).resolve().parent.parent
WRITER = LAUNCHER / "tools" / "build" / "write_build_id.sh"
SDKCONFIG_DEFAULTS = LAUNCHER / "sdkconfig.defaults"


class BuildIdTest(unittest.TestCase):
    def test_runtime_elf_hash_has_build_id_length(self):
        settings = SDKCONFIG_DEFAULTS.read_text(encoding="ascii")

        self.assertIn("CONFIG_APP_RETRIEVE_LEN_ELF_SHA=13", settings)

    def test_writes_elf_hash_and_variant(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            elf = build / "launcher.elf"
            elf.write_bytes(b"test ELF contents\0")

            subprocess.run(["bash", str(WRITER), str(build), "diag"], check=True)

            expected = hashlib.sha256(elf.read_bytes()).hexdigest()[:12] + "-diag"
            self.assertEqual(expected, (build / "build_id.txt").read_text(encoding="ascii").strip())


if __name__ == "__main__":
    unittest.main()

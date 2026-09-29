#!/usr/bin/env python3
"""Checks the build-ID CMake generator without an ESP-IDF build."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path


LAUNCHER = Path(__file__).resolve().parent.parent
GENERATOR = LAUNCHER / "main" / "build_id.cmake"
ID = re.compile(r"^\d{8}T\d{6}-[0-9a-f]{8}-(release|dev|diag)$")


def generate(variant):
    with tempfile.TemporaryDirectory() as temp:
        build = Path(temp) / "build"
        generated = build / "generated"
        subprocess.run(
            [
                "cmake",
                f"-DBUILD_DIR={build}",
                f"-DGENERATED_DIR={generated}",
                f"-DVARIANT={variant}",
                "-P",
                str(GENERATOR),
            ],
            check=True,
        )
        return (build / "build_id.txt").read_text(encoding="ascii").strip(), (
            generated / "build_id_generated.h"
        ).read_text(encoding="ascii")


class BuildIdGeneratorTest(unittest.TestCase):
    def test_generates_readable_variant_id_and_short_id(self):
        build_id, header = generate("diag")

        self.assertRegex(build_id, ID)
        self.assertTrue(build_id.endswith("-diag"))
        self.assertRegex(header, r'#define BUILD_ID_SHORT "\d{6}-[0-9a-f]{4}"')

    def test_generates_a_new_id_each_time(self):
        first, _ = generate("dev")
        second, _ = generate("dev")

        self.assertNotEqual(first, second)


if __name__ == "__main__":
    unittest.main()

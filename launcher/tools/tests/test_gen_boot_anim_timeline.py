"""Checks the timeline generator against the shipped input and output."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


LAUNCHER = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = LAUNCHER / "tools" / "gen_boot_anim_timeline.py"
TIMELINE = LAUNCHER / "main" / "boot" / "boot_anim_timeline.json"
HEADER = LAUNCHER / "main" / "boot" / "boot_anim_timeline.h"


class TimelineGeneratorTests(unittest.TestCase):
    def generate(self, change=None):
        config = json.loads(TIMELINE.read_text(encoding="utf-8"))
        if change is not None:
            change(config["timing"])
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "timeline.json"
            source.write_text(json.dumps(config), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(GENERATOR), str(source)],
                cwd=LAUNCHER, capture_output=True, text=True, check=False)

    def test_unknown_timing_key_fails_and_names_key(self):
        result = self.generate(lambda timing: timing.update(title_font=1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown timing key(s): title_font", result.stderr)

    def test_zero_scale_fails(self):
        result = self.generate(lambda timing: timing.update(title_scale=0))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("title_scale must be at least 1", result.stderr)

    def test_one_scale_passes(self):
        result = self.generate(lambda timing: timing.update(title_scale=1))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_two_scale_warns(self):
        result = self.generate(lambda timing: timing.update(title_scale=2))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("the 8x8 bitmap needs about 5x to read as a title", result.stderr)

    def test_three_scale_does_not_warn(self):
        result = self.generate(lambda timing: timing.update(title_scale=3))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("title_scale", result.stderr)

    def test_missing_scale_defaults_to_five(self):
        result = self.generate(lambda timing: timing.pop("title_scale"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define BOOT_ANIM_TITLE_SCALE 5", result.stdout)

    def test_checked_in_header_matches_generator(self):
        result = subprocess.run(
            [sys.executable, str(GENERATOR), str(TIMELINE)],
            cwd=LAUNCHER, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(HEADER.read_bytes().replace(b"\r\n", b"\n"),
                         result.stdout.replace(b"\r\n", b"\n"))


if __name__ == "__main__":
    unittest.main()

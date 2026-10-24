"""apply.py: `autana tune --json` output written into a tree's TUNE(...) lines."""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import apply  # noqa: E402

SOURCE = (
    "#include \"tune.h\"\n"
    "TUNE(ridge, trail, 12, 0, 40);\n"
    "TUNE_HOT(ridge, theme_rgb, 0x38D6E8, 0, 0xFFFFFF);\n"
    "TUNE(ridge, speed, RIDGE_SPEED_DEFAULT, 0, 99);\n"
)


class ApplyTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.source = self.root / "launcher" / "main" / "shell" / "ridge.c"
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(SOURCE.encode("utf-8"))

    def run_apply(self, values):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            changed = apply.apply(values, self.root)
        return changed, out.getvalue()

    def test_a_changed_value_is_written_and_reported(self):
        changed, out = self.run_apply({"ridge.trail": 20})
        self.assertEqual(changed, 1)
        self.assertIn("TUNE(ridge, trail, 20, 0, 40);", self.source.read_text())
        self.assertIn("ridge.trail: 12 -> 20   (launcher/main/shell/ridge.c)", out)

    def test_a_colour_is_written_as_hex(self):
        self.run_apply({"ridge.theme_rgb": 0x102030})
        self.assertIn("TUNE_HOT(ridge, theme_rgb, 0x102030, 0, 0xFFFFFF);", self.source.read_text())

    def test_a_value_the_source_already_has_leaves_the_file_alone(self):
        changed, out = self.run_apply({"ridge.trail": 12, "ridge.theme_rgb": 0x38D6E8})
        self.assertEqual(changed, 0)
        self.assertEqual(self.source.read_text(), SOURCE)
        self.assertIn("the source already has the board's values", out)

    def test_a_macro_becomes_the_number_it_stood_for(self):
        self.run_apply({"ridge.speed": 7})
        self.assertIn("TUNE(ridge, speed, 7, 0, 99);", self.source.read_text())

    def test_an_undeclared_name_is_skipped(self):
        changed, out = self.run_apply({"lab.view": 3})
        self.assertEqual(changed, 0)
        self.assertIn("lab.view: not declared in this tree - skipped", out)

    def test_main_reads_autana_tune_json_from_stdin(self):
        dump = json.dumps({"tunables": [{"name": "ridge.trail", "value": 30, "min": 0, "max": 40, "default": 12}]})
        with mock.patch.object(apply, "ROOT", self.root), mock.patch("sys.stdin", io.StringIO(dump)), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(apply.main([]), 0)
        self.assertIn("TUNE(ridge, trail, 30, 0, 40);", self.source.read_text())

    def test_main_refuses_anything_but_tune_json(self):
        with mock.patch("sys.stdin", io.StringIO("ridge.trail=30")), self.assertRaises(SystemExit) as stop:
            apply.main([])
        self.assertIn("autana tune --json", str(stop.exception.code))


if __name__ == "__main__":
    unittest.main()

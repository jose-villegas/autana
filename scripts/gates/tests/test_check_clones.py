"""Regression tests against the pinned clone engine and its ratchet."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
path = Path(os.environ.get("CLONE_GATE_SCRIPT", Path(__file__).resolve().parents[1] / "check_clones.py"))
spec = importlib.util.spec_from_file_location("check_clones", path)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

BLOCK = """int alpha(int input) {
    int result = 0;
    for (int index = 0; index < input; index++) {
        if (index % 3 == 0) {
            result += index * 2;
        } else {
            result -= index / 4;
        }
    }
    while (result < input * input) {
        result = result * 3 + input;
        if (result > input * 10) {
            break;
        }
    }
    return result > 5 ? result : input;
}
"""


class CloneTests(unittest.TestCase):
    def scan(self, files, minimum=30):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, text in files.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            return gate.scan(root, minimum)

    def test_renamed_copy(self):
        renamed = BLOCK.replace("alpha", "beta").replace("input", "value").replace("result", "total").replace("index", "step").replace("3", "7")
        pairs = self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": renamed}, 80)
        self.assertTrue(pairs)
        self.assertGreaterEqual(pairs[0]["tokens"], 80)

    def test_header_and_cpp_are_compared(self):
        for suffix in ("h", "cpp", "hpp"):
            with self.subTest(suffix=suffix):
                self.assertTrue(self.scan({"launcher/main/a.c": BLOCK,
                                           f"launcher/main/b.{suffix}": BLOCK}))

    def test_python_renamed_copy(self):
        a = "def first(value):\n    total = 0\n    for index in range(value):\n        if index > 3:\n            total += index * 2\n        else:\n            total -= index\n    return total\n"
        b = a.replace("first", "second").replace("value", "limit").replace("total", "sum_value").replace("3", "8")
        self.assertTrue(self.scan({"scripts/a.py": a, "scripts/b.py": b}))

    def test_below_minimum(self):
        self.assertEqual(self.scan({"launcher/main/a.c": "int a(int x) {\n int y = x + 2;\n return y;\n}\n",
                                    "launcher/main/b.c": "int a(int x) {\n int y = x + 2;\n return y;\n}\n"}, 80), [])

    def test_comments_and_whitespace(self):
        changed = BLOCK.replace("    ", "\t").replace("return", "/* constraint */ return")
        self.assertTrue(self.scan({"launcher/main/a.c": BLOCK, "launcher/main/b.c": changed}))

    def test_one_line_clone(self):
        block = " ".join(BLOCK.splitlines())
        self.assertTrue(self.scan({"launcher/main/a.c": block,
                                   "launcher/main/b.c": block}, 80))

    def test_within_file(self):
        self.assertTrue(self.scan({"launcher/main/a.c": BLOCK + "\n" + BLOCK}))

    def test_skip_rules(self):
        files = {"launcher/main/a.c": BLOCK, "scripts/tool.py": "pass",
                 "launcher/components/x.c": BLOCK, "launcher/test/framework/x.h": BLOCK,
                 "editor/tests/fixtures/data.py": "pass", "launcher/main/b.h":
                 "/* GENERATED FILE - do not edit. */\n" + BLOCK}
        self.assertEqual(self.scan(files), [])

    def test_untracked_file_excluded(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "scripts").mkdir()
            (root / "scripts/a.py").write_text("pass", encoding="utf-8")
            with mock.patch.object(gate, "tracked_files", return_value=()):
                self.assertEqual(gate.source_files(root), [])

    def test_ratchet_rise(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.ratchet(3, 2), 1)

    def test_ratchet_fall(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(gate.ratchet(1, 2), 0)
        self.assertIn("Lower", output.getvalue())

    def test_changed_filters_report_not_count(self):
        pair = {"firstFile": {"name": "a.c", "start": 1, "end": 12},
                "secondFile": {"name": "b.c", "start": 1, "end": 12}, "tokens": 90}
        with tempfile.TemporaryDirectory() as folder:
            baseline = Path(folder) / "baseline.txt"
            baseline.write_text("0", encoding="utf-8")
            output = io.StringIO()
            with mock.patch.object(gate, "scan", return_value=[pair]), mock.patch.object(gate, "BASELINE", baseline), \
                    mock.patch.object(gate.subprocess, "run", return_value=mock.Mock(stdout="unrelated.py\n")), \
                    contextlib.redirect_stdout(output):
                self.assertEqual(gate.main(["--changed", "main"]), 1)
            self.assertNotIn("a.c:1-12", output.getvalue())
            self.assertIn("1 clone pairs", output.getvalue())

    def test_changed_checks_both_sides(self):
        pair = {"firstFile": {"name": "a"}, "secondFile": {"name": "b"}}
        self.assertTrue(gate.touching(pair, {"a"}))
        self.assertTrue(gate.touching(pair, {"b"}))
        self.assertFalse(gate.touching(pair, {"c"}))


if __name__ == "__main__":
    unittest.main()

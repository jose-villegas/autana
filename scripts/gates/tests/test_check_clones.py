"""Regression tests against the pinned clone engine and its new-pair gate."""
import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
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


READY = shutil.which("node") is not None and gate.ENGINE.is_file()
SKIP_REASON = "Install Node.js and run npm ci --prefix scripts/gates"


@unittest.skipUnless(READY or os.environ.get("CI"), SKIP_REASON)
class CloneTests(unittest.TestCase):
    def scan(self, files, minimum=30):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, text in files.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            gate.git(root, "init")
            gate.git(root, "add", ".")
            gate.git(root, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                     "commit", "-m", "test: source")
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

    def test_whole_gate_committed_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git(*args):
                return gate.git(root, *args).decode().strip()
            def commit():
                git("add", ".")
                git("commit", "-m", "test: source")
            def check(expected, message):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = gate.main([], root=root)
                self.assertEqual(result, expected, output.getvalue())
                self.assertIn(message, output.getvalue())
            git("init", "-b", "main")
            git("config", "user.email", "test@example.invalid")
            git("config", "user.name", "Test")
            original = root / "launcher/main/a.c"
            original.parent.mkdir(parents=True)
            original.write_text(BLOCK, encoding="utf-8")
            commit()
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            git("checkout", "-b", "feature/test")
            copy = original.with_name("b.c")
            copy.write_text(BLOCK, encoding="utf-8")
            commit()
            check(1, "FAIL: 1 new clone pairs")
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            git("mv", "launcher/main/a.c", "launcher/main/renamed.c")
            commit()
            check(0, "PASS: no new clone pairs (1 checked HEAD pairs, 1 base pairs)")
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            copy.write_text("\n\n" + BLOCK, encoding="utf-8")
            commit()
            check(0, "PASS: no new clone pairs (1 checked HEAD pairs, 1 base pairs)")
            original.write_text(BLOCK, encoding="utf-8")
            self.assertEqual(len(gate.scan(root, 80)), 1)

    def pair(self, first="a.c", second="b.c", fragment=BLOCK):
        return {"firstFile": {"name": first, "start": 1, "end": 12},
                "secondFile": {"name": second, "start": 1, "end": 12},
                "fragment": fragment, "format": "c", "tokens": 90}

    def test_added_clone_fails_when_another_is_removed(self):
        base = [self.pair()]
        head = [self.pair("c.c", "d.c")]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(gate.check_pairs(head, base), 1)
        self.assertIn("c.c:1-12 ~ d.c:1-12", output.getvalue())

    def test_literal_only_table_rows_are_ignored(self):
        rows = "    {0xAB, 1.5e-2f, 42UL, 'x', \"identifier\"}, // label\n" * 12
        pairs = self.scan({"launcher/main/a.c": rows,
                           "launcher/main/b.c": rows}, 80)
        self.assertEqual(pairs, [])

    def test_a_run_test_list_is_ignored_but_a_copy_beside_it_is_not(self):
        listing = "}\n\nvoid\nsuite_a(void) {\n" + "".join(f"    RUN_TEST(test_{i});\n" for i in range(24)) + "}\n"
        self.assertEqual(gate.filter_pairs([self.pair(fragment=listing)]), [])
        copied = self.pair(fragment=BLOCK + listing)
        self.assertEqual(gate.filter_pairs([copied]), [copied])

    def test_self_match_is_ignored(self):
        self.assertEqual(gate.filter_pairs([self.pair("a.c", "a.c")]), [])

    def test_untouched_existing_clone_passes(self):
        pair = self.pair()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs([pair], [pair]), 0)

    def test_key_ignores_locations_file_order_and_whitespace(self):
        base = self.pair()
        head = self.pair("b.c", "a.c", BLOCK.replace("    ", "\t"))
        head["firstFile"]["start"] = 20
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs([head], [base]), 0)

    def test_same_file_distinct_ranges_are_kept(self):
        pair = self.pair("a.c", "a.c")
        pair["secondFile"]["start"] = 20
        self.assertEqual(gate.filter_pairs([pair]), [pair])

    def test_fragment_edit_is_new(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.check_pairs([self.pair(fragment=BLOCK.replace("alpha", "beta"))],
                                             [self.pair()]), 1)

    def test_committed_scan_ignores_worktree_edits(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            def git(*args):
                return subprocess.run(["git", *args], cwd=root, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-b", "main")
            git("config", "user.email", "test@example.invalid")
            git("config", "user.name", "Test")
            for name in ("a.c", "b.c"):
                target = root / "launcher/main" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(BLOCK, encoding="utf-8")
            git("add", ".")
            git("commit", "-m", "test: base")
            base = git("rev-parse", "HEAD")
            git("update-ref", "refs/remotes/origin/main", base)
            git("checkout", "-b", "feature/test")
            (root / "launcher/main/a.c").write_text("int single;", encoding="utf-8")
            git("add", ".")
            git("commit", "-m", "test: remove clone")
            self.assertEqual(gate.comparison_base(root), base)
            self.assertTrue(gate.scan(root, 80, revision=base))
            self.assertTrue(gate.scan(root, 80, names=["launcher/main/a.c", "launcher/main/b.c",
                                                      "launcher/main/new.c"], revision=base))
            self.assertEqual(gate.scan(root, 80, names=["launcher/main/a.c"], revision=base), [])
            self.assertEqual(gate.scan(root, 80, revision="HEAD"), [])
            (root / "launcher/main/a.c").write_text(BLOCK, encoding="utf-8")
            self.assertEqual(gate.scan(root, 80, revision="HEAD"), [])
            git("update-ref", "refs/remotes/origin/main", git("rev-parse", "HEAD"))
            self.assertEqual(gate.comparison_base(root), base)

    def test_unrelated_branch_change_skips_base_scan(self):
        pair = self.pair()
        def git(root, *args):
            if args[0] == "diff":
                return b"M\0docs/notes.md\0"
            return b"head\n"
        with mock.patch.object(gate, "git", side_effect=git), \
                mock.patch.object(gate, "comparison_base", return_value="base"), \
                mock.patch.object(gate, "scan", return_value=[pair]) as scan, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gate.main([]), 0)
        scan.assert_called_once_with(gate.ROOT, 80, revision="head")

    def test_missing_node_message(self):
        with mock.patch.object(gate.shutil, "which", return_value=None):
            self.assertIn("Node.js is missing", gate.dependency_problem())

    def test_missing_engine_message(self):
        with mock.patch.object(gate, "ENGINE", Path("missing-engine.js")):
            self.assertIn("npm ci --prefix scripts/gates", gate.dependency_problem())


if __name__ == "__main__":
    unittest.main()

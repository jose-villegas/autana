"""The generated-body gate discovers documents, checks hashes without rendering,
and runs each named check command once."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from check_doc_generated import check
from generated_blocks import replace_block


class GateTests(unittest.TestCase):
    def test_discovers_app_readme_and_rejects_hand_edit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "launcher/main/apps/example/tools/README.md"
            path.parent.mkdir(parents=True)
            path.write_text("<!-- generated: scores -->\n<!-- /generated: scores -->\n", encoding="utf-8")
            self.assertTrue(check(root))
            replace_block(path, "scores", "| 2 |\n")
            self.assertEqual(check(root), [])
            path.write_text(path.read_text().replace("| 2 |", "| 3 |"), encoding="utf-8")
            self.assertEqual(len(check(root)), 1)

    def test_a_named_check_command_judges_its_block(self):
        # gen.py passes while ok.txt exists; one run covers both blocks.
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "gen.py").write_text("import pathlib, sys\n"
                                         "open('runs.txt', 'a').write('run\\n')\n"
                                         "sys.exit(0 if pathlib.Path('ok.txt').exists() else 1)\n")
            for name in ("first", "second"):
                (root / f"{name}.md").write_text(f"<!-- generated: {name} check: python gen.py --check -->\n"
                                                 f"| 2 |\n<!-- /generated: {name} -->\n", encoding="utf-8")
            (root / "ok.txt").write_text("")
            self.assertEqual(check(root), [])
            self.assertEqual((root / "runs.txt").read_text(), "run\n")
            (root / "ok.txt").unlink()
            self.assertEqual(len(check(root)), 2)

    def test_every_distinct_command_is_judged_with_the_gate_s_own_python(self):
        # Each distinct command runs once, under the gate's own interpreter.
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "pass.py").write_text("")
            (root / "fail.py").write_text("raise SystemExit('stale')\n")
            for name, script in (("first", "pass.py"), ("second", "fail.py"), ("third", "pass.py")):
                (root / f"{name}.md").write_text(f"<!-- generated: {name} check: python3 {script} -->\n"
                                                 f"| 2 |\n<!-- /generated: {name} -->\n", encoding="utf-8")
            with mock.patch("subprocess.run", wraps=subprocess.run) as run:
                errors = check(root)
            scripts = [call.args[0] for call in run.call_args_list if call.args[0][-1].endswith(".py")]
            self.assertEqual(scripts, [[sys.executable, "pass.py"], [sys.executable, "fail.py"]])
            self.assertEqual(len(errors), 1, errors)
            self.assertTrue(errors[0].startswith("second.md#second:"), errors)
            self.assertIn("stale", errors[0])

    def test_a_command_that_cannot_run_is_reported_against_its_block(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name, command in (("quote", 'python "gen.py'), ("program", "true"), ("missing", "python absent.py")):
                (root / f"{name}.md").write_text(f"<!-- generated: {name} check: {command} -->\n"
                                                 f"| 2 |\n<!-- /generated: {name} -->\n", encoding="utf-8")
            errors = check(root)
            self.assertEqual(sorted(error.split(":")[0] for error in errors),
                             ["missing.md#missing", "program.md#program", "quote.md#quote"], errors)
            self.assertIn("names no interpreter", next(error for error in errors if error.startswith("program")))
            real = subprocess.run
            def no_sh(argv, **kwargs):
                if argv[0] == "sh":
                    raise FileNotFoundError("sh: not found")
                return real(argv, **kwargs)
            (root / "program.md").write_text("<!-- generated: program check: sh gen.sh -->\n"
                                             "| 2 |\n<!-- /generated: program -->\n", encoding="utf-8")
            with mock.patch("subprocess.run", side_effect=no_sh):
                errors = check(root)
            self.assertIn("program.md#program: `sh gen.sh` failed: sh: not found", errors)

    def test_a_block_with_neither_digest_nor_command_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "README.md").write_text("<!-- generated: scores -->\n| hand |\n<!-- /generated: scores -->\n",
                                            encoding="utf-8")
            self.assertEqual(len(check(root)), 1)

    def test_broken_boundary_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "README.md").write_text("<!-- generated: scores -->\n", encoding="utf-8")
            self.assertEqual(len(check(root)), 1)

    def test_names_are_unique_across_documents(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name in ("first.md", "second.md"):
                path = root / name
                path.write_text("<!-- generated: scores -->\n<!-- /generated: scores -->\n", encoding="utf-8")
                replace_block(path, "scores", "table\n")
            self.assertEqual(len(check(root)), 1)

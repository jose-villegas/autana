"""The generated-body gate discovers documents, checks hashes without rendering,
and runs each named check command once."""
import pathlib
import sys
import tempfile
import unittest

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

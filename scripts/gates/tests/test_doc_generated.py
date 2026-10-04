"""The generated-body gate discovers documents and checks hashes without rendering."""
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

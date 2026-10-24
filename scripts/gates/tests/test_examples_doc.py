"""Example blocks discover tracked source across areas and nested folders."""
import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "launcher/tools/gen"))
from examples_doc import CHECK, main, write_examples


class ExampleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        self.source("tools/ui_examples/screens/check_box.c")
        self.source("test/input_examples/touch.c")
        self.document("ui-check-box-example")
        self.document("input-touch-example")

    def source(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"/* whole example */\r\nint value;\r\n")
        self.track(name)

    def document(self, name, command=None):
        path = self.root / f"{name}.md"
        stamp = f" check: {command}" if command else ""
        path.write_bytes(f"<!-- generated: {name}{stamp} -->\r\n<!-- /generated: {name} -->\r\n".encode())
        self.track(path.name)

    def track(self, name):
        subprocess.run(["git", "-C", str(self.root), "add", name], check=True)

    def run_tool(self, check=False):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            changed = write_examples(self.root, check)
        return changed, output.getvalue()

    def test_clean_pass_and_whole_source_with_crlf_preserved(self):
        self.assertTrue(self.run_tool()[0])
        self.assertFalse(self.run_tool(True)[0])
        raw = (self.root / "ui-check-box-example.md").read_bytes()
        self.assertIn(b"```c\r\n/* whole example */\r\nint value;\r\n```", raw)
        self.assertNotIn(b"\n", raw.replace(b"\r\n", b""))

    def test_stale_body_names_document_without_writing(self):
        self.run_tool()
        path = self.root / "input-touch-example.md"
        path.write_bytes(path.read_bytes().replace(b"int value;", b"int edited;"))
        before = path.read_bytes()
        changed, output = self.run_tool(True)
        self.assertTrue(changed)
        self.assertIn(path.name, output)
        self.assertEqual(path.read_bytes(), before)

    def test_source_without_owner(self):
        self.source("test/input_examples/gravity.c")
        with self.assertRaisesRegex(ValueError, "input_examples/gravity.c"):
            self.run_tool(True)

    def test_owner_without_source(self):
        self.document("orphan", CHECK)
        with self.assertRaisesRegex(ValueError, "orphan.md"):
            self.run_tool(True)

    def test_duplicate_normalized_stems(self):
        self.source("tools/ui_examples/check-box.c")
        with self.assertRaisesRegex(ValueError, "ui-check-box-example"):
            self.run_tool(True)

    def test_written_marker_names_check(self):
        self.run_tool()
        self.assertIn(f"check: {CHECK}", (self.root / "input-touch-example.md").read_text())

    def test_other_generator_example_is_ignored(self):
        self.source("tools/skin_examples/README.md")
        self.document("skin-weight-example", "python other.py --check")
        self.run_tool()
        self.assertFalse(self.run_tool(True)[0])

    def test_non_c_files_need_no_owner_or_embedding(self):
        self.source("tools/other_examples/README.md")
        self.source("tools/other_examples/api.h")
        self.run_tool()
        text = (self.root / "input-touch-example.md").read_text()
        self.assertNotIn("api.h", text)
        self.assertFalse(self.run_tool(True)[0])

    def test_area_underscores(self):
        self.source("tools/foo_bar_examples/x.c")
        self.document("foo-bar-x-example")
        self.run_tool()
        self.assertIn("```c", (self.root / "foo-bar-x-example.md").read_text())

    def test_same_stem_in_different_areas(self):
        self.source("tools/ui_examples/touch.c")
        self.document("ui-touch-example")
        self.run_tool()
        self.assertFalse(self.run_tool(True)[0])

    def test_duplicate_at_different_depths(self):
        self.source("tools/ui_examples/deeper/check_box.c")
        with self.assertRaisesRegex(ValueError, "multiple files"):
            self.run_tool(True)

    def test_main_exit_codes(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, main(["--check"], root=self.root))
            self.assertEqual(0, main([], root=self.root))
            self.assertEqual(0, main(["--check"], root=self.root))

    def test_main_value_error_exit_code(self):
        self.document("orphan", CHECK)
        with contextlib.redirect_stderr(io.StringIO()) as error:
            with self.assertRaises(SystemExit) as result:
                main(["--check"], root=self.root)
        self.assertEqual(2, result.exception.code)
        self.assertIn("orphan", error.getvalue())

    def test_untracked_source_is_ignored(self):
        (self.root / "test/input_examples/untracked.c").write_text("ignored")
        self.run_tool()
        self.assertFalse(self.run_tool(True)[0])


if __name__ == "__main__":
    unittest.main()

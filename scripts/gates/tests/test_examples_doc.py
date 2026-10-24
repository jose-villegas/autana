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

    def check_fence(self, text, expected):
        path = self.root / "extra.md"
        path.write_text(text, encoding="utf-8")
        self.track(path.name)
        self.run_tool()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = main(["--check"], root=self.root)
        self.assertEqual(expected, result)
        return output.getvalue()

    def test_unmarked_c_fences_fail_with_location_and_remedies(self):
        for language in ("c", "h", "cpp"):
            with self.subTest(language=language):
                output = self.check_fence(f"Text\n```{language}\nint x;\n```\n", 1)
                self.assertIn("extra.md:2", output)
                for remedy in ("example file", "declaration block", "```c fragment"):
                    self.assertIn(remedy, output)

    def test_fragment_word_marks_a_fence(self):
        self.check_fence("```c fragment\nint x;\n```\n", 0)
        self.check_fence("```c notfragment\nint x;\n```\n", 1)

    def test_generated_fence_passes(self):
        self.check_fence("<!-- generated: other -->\n```cpp\nint x;\n```\n"
                         "<!-- /generated: other -->\n", 0)

    def test_other_languages_and_nested_fence_text_are_ignored(self):
        self.check_fence("````text\n```c\nint x;\n```\n````\n"
                         "```python\nx = 1\n```\n", 0)

    def test_tilde_and_long_fences(self):
        self.check_fence("~~~~h\nint x;\n~~~~\n", 1)
        self.check_fence("~~~~h fragment\nint x;\n~~~~~\n", 0)

    def test_indented_fence_in_a_list(self):
        self.check_fence("1. Step\n\n    ```c\n    int x;\n    ```\n", 1)
        self.check_fence("- ```cpp fragment\n  int x;\n  ```\n", 0)
        self.check_fence("1. ~~~cpp\n   int x;\n   ~~~\n", 1)

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

    def header(self, name, text):
        path = self.root / "launcher" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        self.track(path.relative_to(self.root).as_posix())

    def test_declaration_found_without_doc_comment(self):
        self.header("api.h", "/* Call the API. */\nvoid touch_fsm_update(int value);\n")
        self.document("declaration-touch-fsm-update")
        self.run_tool()
        body = (self.root / "declaration-touch-fsm-update.md").read_text()
        self.assertIn("```c\nvoid touch_fsm_update(int value);\n```", body)
        self.assertNotIn("Call the API", body)
        self.assertFalse(self.run_tool(True)[0])

    def test_declaration_missing_names_function(self):
        self.document("declaration-touch-fsm-update", CHECK)
        with self.assertRaisesRegex(ValueError, "touch_fsm_update"):
            self.run_tool(True)

    def test_declaration_ambiguous_names_function_and_headers(self):
        for name in ("a.h", "b.h"):
            self.header(name, "void touch_fsm_update(int value);\n")
        self.document("declaration-touch-fsm-update")
        with self.assertRaisesRegex(ValueError, "touch_fsm_update.*a.h.*b.h"):
            self.run_tool(True)

    def test_declaration_multiline_is_copied_whole(self):
        declaration = "const char*\nlookup_name(int value,\n            bool required);"
        self.header("api.h", "#pragma once\n/* Lookup. */\n" + declaration + "\n")
        self.document("declaration-lookup-name")
        self.run_tool()
        self.assertIn("```c\n" + declaration + "\n```",
                      (self.root / "declaration-lookup-name.md").read_text())

    def test_declaration_stale_check_does_not_write(self):
        self.header("api.h", "void touch_fsm_update(int value);\n")
        self.document("declaration-touch-fsm-update")
        self.run_tool()
        self.header("api.h", "void touch_fsm_update(bool value);\n")
        before = (self.root / "declaration-touch-fsm-update.md").read_bytes()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, main(["--check"], root=self.root))
        self.assertEqual(before, (self.root / "declaration-touch-fsm-update.md").read_bytes())
        self.run_tool()
        self.assertFalse(self.run_tool(True)[0])

    def assert_touch_declaration(self, text):
        self.header("api.h", text)
        self.document("declaration-touch-fsm-update")
        self.run_tool()
        body = (self.root / "declaration-touch-fsm-update.md").read_text()
        self.assertIn("```c\nvoid touch_fsm_update(int value);\n```", body)
        self.assertFalse(self.run_tool(True)[0])

    def test_declaration_ignores_tracked_headers_outside_launcher(self):
        self.header("../outside.h", "void touch_fsm_update(bool value);\n")
        self.assert_touch_declaration("void touch_fsm_update(int value);\n")

    def test_declaration_ignores_commented_prototypes(self):
        self.assert_touch_declaration("// void touch_fsm_update(bool value);\n"
                                      "/*\nvoid touch_fsm_update(char value);\n*/\n"
                                      "void touch_fsm_update(int value);\n")

    def test_declaration_ignores_preprocessor_directives(self):
        self.header("api.h", "#define CALL void touch_fsm_update(int value);\n"
                    "#if touch_fsm_update(int value);\n"
                    "#define MULTILINE \\\nvoid touch_fsm_update(int value);\n#endif\n")
        self.document("declaration-touch-fsm-update", CHECK)
        with self.assertRaisesRegex(ValueError, "touch_fsm_update"):
            self.run_tool(True)

    def test_declaration_ignores_definitions(self):
        self.header("api.h", "static inline void touch_fsm_update(int value) { }\n")
        self.document("declaration-touch-fsm-update", CHECK)
        with self.assertRaisesRegex(ValueError, "touch_fsm_update"):
            self.run_tool(True)

    def test_declaration_requires_exact_function_name(self):
        self.header("api.h", "void touch_fsm_update_x(int value);\n"
                    "void xtouch_fsm_update(int value);\n")
        self.document("declaration-touch-fsm-update", CHECK)
        with self.assertRaisesRegex(ValueError, "touch_fsm_update"):
            self.run_tool(True)

    def test_declaration_repeated_in_one_header_is_unambiguous(self):
        self.assert_touch_declaration("#ifdef OPTION\nvoid touch_fsm_update(int value);\n"
                                      "#else\nvoid touch_fsm_update(int value);\n#endif\n")

    def test_declaration_strips_leading_whitespace(self):
        self.assert_touch_declaration("\n    void touch_fsm_update(int value);\n")

    def test_declaration_ignores_untracked_headers(self):
        (self.root / "launcher").mkdir()
        (self.root / "launcher/untracked.h").write_text("void touch_fsm_update(int value);\n")
        self.document("declaration-touch-fsm-update", CHECK)
        with self.assertRaisesRegex(ValueError, "touch_fsm_update"):
            self.run_tool(True)

    def test_declaration_area_example_is_copied_from_example_file(self):
        self.source("test/declaration_examples/x.c")
        self.document("declaration-x-example")
        self.run_tool()
        body = (self.root / "declaration-x-example.md").read_text()
        self.assertIn("/* whole example */", body)
        self.assertFalse(self.run_tool(True)[0])

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

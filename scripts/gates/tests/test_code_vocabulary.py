"""Regression tests for scripts/gates/code_vocabulary.py."""
import pathlib
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import code_vocabulary  # noqa: E402


class NamesRequireADefinitionTest(unittest.TestCase):
    def write(self, root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def test_a_name_only_inside_a_comment_is_not_defined(self):
        # gfx_default_font() and cover_count() both leaked into the "defined"
        # set this way: a *different* comment citing a dead name was itself
        # enough to make code_vocabulary.names() think the name was real.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* replaces the old gfx_default_font() call */\n"
                      "void gfx_init(void) {}\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertNotIn("gfx_default_font", functions)
        self.assertIn("gfx_init", functions)

    def test_a_name_only_inside_a_string_literal_is_not_defined(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/apps/sand/tests/suite_sand.c",
                      'TEST_ASSERT_TRUE_MESSAGE(x, "the case cover_count() '
                      'could never fire");\n'
                      "void real_function(void) {}\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertNotIn("cover_count", functions)
        self.assertIn("real_function", functions)

    def test_a_real_declaration_is_still_found(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.h",
                      "/* draws a rectangle */\n"
                      "void gfx_fill_rect(int x, int y, int w, int h);\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertIn("gfx_fill_rect", functions)

    def test_a_real_call_site_is_still_found(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "void gfx_init(void) {\n"
                      "    gfx_reset_palette();\n"
                      "}\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertIn("gfx_reset_palette", functions)

    def test_a_python_apostrophe_comment_does_not_eat_the_next_def(self):
        # The C blanker was applied to .py files too: "# don't" has no `#`
        # case, so the apostrophe in "don't" opened a bogus string that
        # swallowed everything up to the next real quote - including
        # real_one()'s own definition - as blanked-out "string content".
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/example.py",
                      "# don't do this\n"
                      "def real_one():\n"
                      "    pass\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertIn("real_one", functions)

    def test_python_floor_division_does_not_blank_the_rest_of_the_line(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/example.py",
                      "def real_two():\n"
                      "    return a // real_two_helper()\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertIn("real_two", functions)
        self.assertIn("real_two_helper", functions)

    def test_a_python_docstring_does_not_hide_the_function_after_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/example.py",
                      '"""Module docstring."""\n'
                      "def real_three():\n"
                      "    pass\n")
            functions, _ = code_vocabulary.names(str(root))
        self.assertIn("real_three", functions)


if __name__ == "__main__":
    unittest.main()

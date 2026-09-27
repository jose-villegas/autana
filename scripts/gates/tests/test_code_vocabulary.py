"""Regression tests for scripts/gates/code_vocabulary.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
        # enough to make code_vocabulary.vocabulary() think the name was real.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "/* replaces the old gfx_default_font() call */\n"
                      "void gfx_init(void) {}\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertNotIn("gfx_default_font", vocab.functions)
        self.assertIn("gfx_init", vocab.functions)

    def test_a_name_only_inside_a_string_literal_is_not_defined(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/apps/sand/tests/suite_sand.c",
                      'TEST_ASSERT_TRUE_MESSAGE(x, "the case cover_count() '
                      'could never fire");\n'
                      "void real_function(void) {}\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertNotIn("cover_count", vocab.functions)
        self.assertIn("real_function", vocab.functions)

    def test_a_real_declaration_is_still_found(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.h",
                      "/* draws a rectangle */\n"
                      "void gfx_fill_rect(int x, int y, int w, int h);\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("gfx_fill_rect", vocab.functions)

    def test_a_real_call_site_is_still_found(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/gfx/gfx.c",
                      "void gfx_init(void) {\n"
                      "    gfx_reset_palette();\n"
                      "}\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("gfx_reset_palette", vocab.functions)

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
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("real_one", vocab.script_functions)

    def test_python_floor_division_does_not_blank_the_rest_of_the_line(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/example.py",
                      "def real_two():\n"
                      "    return a // real_two_helper()\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("real_two", vocab.script_functions)
        self.assertIn("real_two_helper", vocab.script_functions)

    def test_a_python_docstring_does_not_hide_the_function_after_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/example.py",
                      '"""Module docstring."""\n'
                      "def real_three():\n"
                      "    pass\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("real_three", vocab.script_functions)

    def test_an_mjs_string_constant_counts_and_its_function_names_do_not(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/gates/check.mjs",
                      "function render_block() {}\n"
                      "const extra = process.env['CHECK_EXTRA_ARGS'];\n"
                      "render_block(extra);\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("CHECK_EXTRA_ARGS", vocab.constants)
        self.assertNotIn("render_block", vocab.functions | vocab.script_functions)

    def test_node_modules_is_not_read(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "scripts/node_modules/lib/index.mjs", "const x = 'VENDORED_NAME';\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertNotIn("VENDORED_NAME", vocab.constants)

    def test_a_shell_variable_counts_and_one_only_its_comment_names_does_not(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/tools/profile.sh",
                       "# PROFILE_COMMENTED_ONLY is only named here\n"
                       "PROFILE_FREE_BYTES=51200\n"
                       'BASE="${PROFILE_GATE_BASE:-origin/main}"\n')
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("PROFILE_FREE_BYTES", vocab.constants)
        self.assertIn("PROFILE_GATE_BASE", vocab.constants)
        self.assertNotIn("PROFILE_COMMENTED_ONLY", vocab.constants)

    def test_in_git_an_ignored_build_directory_is_not_read_and_tools_build_is(self):
        # launcher/tools/build/ shares its name with a build directory, and
        # a skip by name hid every script in it.
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, ".gitignore", "launcher/build/\n")
            self.write(root, "launcher/build/generated.c", "void generated_only(void) {}\n")
            self.write(root, "launcher/tools/build/flash.sh", "FLASH_BAUD=921600\n")
            self.write(root, "launcher/main/new.c", "void not_yet_added(void) {}\n")
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "add", ".gitignore", "launcher/tools"], cwd=root, check=True)
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertNotIn("generated_only", vocab.functions)
        self.assertIn("FLASH_BAUD", vocab.constants)
        self.assertIn("not_yet_added", vocab.functions)

    def test_outside_git_every_file_but_a_skipped_directory_is_read(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/build/generated.c", "void generated_only(void) {}\n")
            self.write(root, "launcher/main/a.c", "void real_function(void) {}\n")
            vocab = code_vocabulary.vocabulary(str(root))
        self.assertIn("real_function", vocab.functions)
        self.assertNotIn("generated_only", vocab.functions)

    def test_any_other_git_failure_raises_with_gits_message(self):
        # In a container running as another user than the checkout's owner,
        # git refuses the repository, and a silent fallback to the directory
        # walk hid launcher/tools/build/ from the vocabulary.
        refused = subprocess.CompletedProcess(
            [], 128, "", "fatal: detected dubious ownership in repository at '/w'\n")
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            self.write(root, "launcher/main/a.c", "void real_function(void) {}\n")
            with mock.patch("subprocess.run", return_value=refused):
                with self.assertRaisesRegex(RuntimeError, "dubious ownership"):
                    code_vocabulary.vocabulary(str(root))


if __name__ == "__main__":
    unittest.main()

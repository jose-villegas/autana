"""The project settings file: what it accepts, and how loudly it refuses the rest.

    python -m unittest discover -s scripts/lib/tests
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

LIB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LIB))
import autana_config  # noqa: E402

DOCUMENT = LIB.parents[1] / "docs" / "tools" / "Autana-CLI.md"


class ConfigTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.project = Path(temp.name)

    def write(self, text, raw=None):
        (self.project / autana_config.CONFIG_NAME).write_bytes(
            raw if raw is not None else text.encode("utf-8"))

    def load(self, text=None, raw=None):
        if text is not None or raw is not None:
            self.write(text, raw)
        return autana_config.load(self.project)

    def refusal(self, text):
        with self.assertRaises(autana_config.ConfigError) as raised:
            self.load(text)
        return str(raised.exception)


class Reading(ConfigTest):
    def test_no_file_is_no_settings(self):
        self.assertEqual(autana_config.load(self.project), {})

    def test_every_key_takes_effect(self):
        settings = self.load(
            "docs_extra = ['a', \"b\"]\n"
            "records = 'C:\\history'\n"
            "lock_hook = 'notify --flag \"x\"'\n"
            "[docs.llama]\n"
            "home = 'models'\n"
            "port = 9911\n")
        self.assertEqual(settings, {
            "docs_extra": ["a", "b"], "records": "C:\\history",
            "lock_hook": 'notify --flag "x"', "docs.llama.home": "models",
            "docs.llama.port": 9911})

    def test_a_multiline_array_with_comments_and_a_trailing_comma(self):
        settings = self.load(
            "# what `autana docs` also reads\n"
            "docs_extra = [\n"
            "    'one',   # first\n"
            "    'two',\n"
            "]\n")
        self.assertEqual(settings, {"docs_extra": ["one", "two"]})

    def test_a_windows_editor_file_reads_the_same(self):
        settings = self.load(raw=b"\xef\xbb\xbfrecords = 'r'\r\nlock_hook = 'h'\r\n")
        self.assertEqual(settings, {"records": "r", "lock_hook": "h"})

    def test_basic_string_escapes(self):
        self.assertEqual(self.load('records = "a\\\\b\\"c"\n'), {"records": 'a\\b"c'})

    def test_every_key_is_listed_in_the_cli_document(self):
        text = DOCUMENT.read_text(encoding="utf-8")
        for key in autana_config.KEYS:
            self.assertTrue(key.rpartition(".")[2] in text, f"{key} is not in {DOCUMENT.name}")

    def test_help_lists_every_key_with_its_meaning(self):
        text = autana_config.help_text()
        for key, (_, meaning) in autana_config.KEYS.items():
            self.assertIn(key, text)
            self.assertIn(meaning, text)


class Refusing(ConfigTest):
    def test_an_unknown_key_names_the_file_the_line_and_the_key(self):
        message = self.refusal("records = 'r'\nrecods = 'x'\n")
        self.assertIn(autana_config.CONFIG_NAME, message)
        self.assertIn(":2:", message)
        self.assertIn('"recods"', message)

    def test_an_unknown_key_in_a_table_is_named_in_full(self):
        self.assertIn('"docs.llama.hme"', self.refusal("[docs.llama]\nhme = 'x'\n"))

    def test_a_value_of_the_wrong_type_names_the_key(self):
        for text in ("records = 5\n", "docs_extra = 'one'\n", "[docs.llama]\nport = 'x'\n"):
            with self.subTest(text=text):
                message = self.refusal(text)
                self.assertIn(autana_config.CONFIG_NAME, message)
                self.assertIn("must be a", message)

    def test_a_key_set_twice_is_refused(self):
        self.assertIn("twice", self.refusal("records = 'a'\nrecords = 'b'\n"))

    def test_toml_this_reader_does_not_understand_is_refused_with_its_line(self):
        for text in ("records = true\n", "records = {a = 1}\n", "[[docs]]\n",
                     "docs_extra = [1]\n", "records = 'open\n", "records\n", "= 'x'\n",
                     "records = 'a' 'b'\n", 'records = "\\q"\n'):
            with self.subTest(text=text):
                self.assertIn(":1:", self.refusal(text))

    def test_an_unreadable_file_is_refused(self):
        (self.project / autana_config.CONFIG_NAME).mkdir()
        with self.assertRaises(autana_config.ConfigError):
            autana_config.load(self.project)


class Paths(ConfigTest):
    def test_a_relative_path_is_relative_to_the_project(self):
        self.assertEqual(autana_config.path_value("a/b", self.project), self.project / "a" / "b")

    def test_an_absolute_path_is_kept(self):
        absolute = Path(tempfile.gettempdir()) / "x"
        self.assertEqual(autana_config.path_value(str(absolute), self.project), absolute)

    def test_a_home_relative_path_is_expanded(self):
        self.assertEqual(autana_config.path_value("~/x", self.project), Path.home() / "x")

    def test_the_project_a_command_acts_on_is_the_private_variable_else_the_cwd(self):
        with mock.patch.dict(os.environ, {autana_config.PROJECT_ENV: str(self.project)}):
            self.assertEqual(autana_config.project_dir(), self.project)
        with mock.patch.dict(os.environ, clear=True):
            self.assertEqual(autana_config.project_dir(), Path.cwd())

    def test_the_private_variables_keep_their_prefix(self):
        for name in (autana_config.PROJECT_ENV, autana_config.BOARD_ENV,
                     autana_config.TOKEN_ENV):
            self.assertTrue(name.startswith("_AUTANA_"), name)


if __name__ == "__main__":
    unittest.main()

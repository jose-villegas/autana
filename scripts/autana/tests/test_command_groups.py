"""COMMAND_GROUPS is the one command list: help is built from it, and
docs/tools/Autana-CLI.md must show the same groups, in the same order, with
every usage under its own group."""

import contextlib
import io
import re
import sys
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana  # noqa: E402

DOC = Path(__file__).resolve().parents[3] / "docs" / "tools" / "Autana-CLI.md"


def doc_sections():
    text = DOC.read_text(encoding="utf-8")
    parts = re.split(r"^## (.+)$", text, flags=re.MULTILINE)
    return {parts[i].strip(): parts[i + 1].replace("\\|", "|") for i in range(1, len(parts), 2)}, \
        [parts[i].strip() for i in range(1, len(parts), 2)]


class CommandGroupTests(unittest.TestCase):
    def test_the_doc_has_every_group_in_order(self):
        _, headings = doc_sections()
        titles = [title for _, title, _ in autana.COMMAND_GROUPS]
        self.assertEqual([h for h in headings if h in titles], titles)

    def test_every_usage_is_documented_under_its_group(self):
        sections, _ = doc_sections()
        for key, title, commands in autana.COMMAND_GROUPS:
            self.assertIn(f"`autana help {key}`", sections[title])
            for command in commands:
                for synopsis, _ in command.usages:
                    self.assertIn(f"autana {synopsis}", sections[title], title)

    def test_every_command_is_reachable_and_helped(self):
        for _, _, commands in autana.COMMAND_GROUPS:
            for command in commands:
                self.assertIs(autana.COMMANDS[command.name], command.handler)
                self.assertIn(f"autana {command.usages[0][0]}", autana.help_text([command.name]))

    def test_help_narrows_to_a_group_or_a_command(self):
        tests = autana.help_text(["tests"])
        self.assertIn("autana selftest", tests)
        self.assertNotIn("autana flash", tests)
        self.assertEqual(autana.help_text(["tap"]).splitlines()[1].split()[:2], ["autana", "tap"])
        self.assertIn("no command or group", autana.help_text(["nope"]))

    def test_session_help_drops_the_prefix(self):
        self.assertIn("\n  freeze ", autana.help_text(["frames"], prefix=""))

    def test_autana_help_prints_the_groups(self):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream), \
                mock.patch.object(sys, "argv", ["autana", "help"]), \
                self.assertRaises(SystemExit):
            autana.main()
        for _, title, _ in autana.COMMAND_GROUPS:
            self.assertIn(title, stream.getvalue())

    def test_help_topics_complete(self):
        self.assertEqual(autana.completion_candidates("help te", "te"), ["tests"])


if __name__ == "__main__":
    unittest.main()

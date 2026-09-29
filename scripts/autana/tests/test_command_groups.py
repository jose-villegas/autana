"""COMMAND_GROUPS is the one command list: help is built from it, and
docs/tools/Autana-CLI.md must show the same groups, in the same order, with
every usage under its own group."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import contextlib
import io
import re
import unittest
import os
from unittest import mock


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
        self.assertIn("\n  debug freeze", autana.help_text(["debug"], prefix=""))

    def test_autana_help_prints_the_groups_but_not_a_hidden_one(self):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream), \
                mock.patch.object(sys, "argv", ["autana", "help"]), \
                self.assertRaises(SystemExit):
            autana.main()
        text = stream.getvalue()
        for key, title, _ in autana.COMMAND_GROUPS:
            if key in autana.HIDDEN_GROUPS:
                self.assertNotIn(title, text)
            else:
                self.assertIn(title, text)

    def test_a_hidden_group_still_shows_in_full_when_asked_by_name(self):
        for key in autana.HIDDEN_GROUPS:
            title = next(title for k, title, _ in autana.COMMAND_GROUPS if k == key)
            self.assertNotIn(title, autana.help_text([]))
            self.assertIn(title, autana.help_text([key]))

    def test_the_quickstart_leads_the_bare_help(self):
        text = autana.help_text([])
        self.assertTrue(text.startswith("Most used"))
        for synopsis, _ in autana.QUICKSTART:
            self.assertIn(f"autana {synopsis}", text)
        self.assertLess(text.index("Most used"), text.index("Build and flash"))

    def test_help_topics_complete(self):
        self.assertEqual(autana.completion_candidates("help te", "te"), ["tests"])

    def test_no_usage_synopsis_repeats_a_shared_board_flag(self):
        """The board flags are documented once (`autana help flags`), not on
        every command's own usage line - that repetition is what made three
        newcomers give up on the help."""
        flag_names = [flag.split()[0] for flag, _, _ in autana.BOARD_FLAGS]
        for _, _, commands in autana.COMMAND_GROUPS:
            for command in commands:
                for synopsis, _ in command.usages:
                    if synopsis.startswith("lock hand"):
                        continue  # its own --wait means something else - see hand()'s docstring
                    words = re.split(r"[\s\[\]]+", synopsis)
                    for flag_name in flag_names:
                        self.assertNotIn(flag_name, words, f"{command.name}: {synopsis}")

    def test_help_flags_lists_every_shared_flag_and_its_commands(self):
        text = autana.help_text(["flags"])
        for flag, _, commands in autana.BOARD_FLAGS:
            self.assertIn(flag, text)
            self.assertIn(commands, text)

    def test_top_level_help_points_at_help_flags(self):
        self.assertIn("autana help flags", autana.help_text([]))

    def test_the_doc_has_a_board_flags_section_listing_every_flag(self):
        sections, _ = doc_sections()
        board = sections["Flags"]
        for flag, _, commands in autana.BOARD_FLAGS:
            self.assertIn(flag, board)
            self.assertIn(commands, board)


if __name__ == "__main__":
    unittest.main()

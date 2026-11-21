"""COMMAND_GROUPS is the one command list: help is built from it, and
cli_reference.py writes it into docs/cli/, one page per group."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import contextlib
import io
import re
import unittest
import os
import tempfile
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana  # noqa: E402

import cli_reference  # noqa: E402


class CommandGroupTests(unittest.TestCase):
    def test_every_group_has_a_page_with_a_current_table(self):
        self.assertEqual(cli_reference.run(check=True), [])

    def test_example_block_owned_by_another_generator_is_ignored(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            page = root / "docs/cli/Examples.md"
            page.parent.mkdir(parents=True)
            page.write_text(
                "<!-- generated: cli-x-example check: python launcher/tools/gen/examples_doc.py --check -->\n"
                "```c\nint x;\n```\n<!-- /generated: cli-x-example -->\n",
                encoding="utf-8")
            with mock.patch.object(cli_reference, "ROOT", root), \
                    mock.patch.object(cli_reference, "tables", return_value=[]):
                self.assertEqual(cli_reference.run(check=True), [])
                page.write_text(page.read_text(encoding="utf-8").replace(
                    "python launcher/tools/gen/examples_doc.py --check", cli_reference.CHECK),
                    encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "belongs to no command group"):
                    cli_reference.run(check=True)

    def test_a_group_without_a_page_fails_rather_than_falls_back(self):
        pages = {key: page for key, page in cli_reference.GROUP_PAGES.items() if key != "tune"}
        with mock.patch.object(cli_reference, "GROUP_PAGES", pages):
            with self.assertRaisesRegex(ValueError, r"missing \['tune'\]"):
                cli_reference.run(check=True)

    def test_every_usage_is_in_its_groups_table(self):
        for key, _, commands in autana.COMMAND_GROUPS:
            table = cli_reference.group_table(commands)
            for command in commands:
                for synopsis, _ in command.usages:
                    self.assertIn(cli_reference.cell(f"autana {synopsis}"), table, key)

    def test_every_command_is_reachable_and_helped(self):
        for _, _, commands in autana.COMMAND_GROUPS:
            for command in commands:
                self.assertIs(autana.COMMANDS[command.name], command.handler)
                self.assertIn(f"autana {command.usages[0][0]}", autana.help_text([command.name]))

    def test_single_usage_constructor_preserves_command_tuple(self):
        for _, _, commands in autana.COMMAND_GROUPS:
            for command in commands:
                if len(command.usages) == 1:
                    with self.subTest(command=command.name):
                        usage, what = command.usages[0]
                        self.assertEqual(
                            autana.one(command.name, command.handler, usage, what), command)

    def test_tone_is_a_test_command_without_a_debug_alias(self):
        self.assertIs(autana.COMMANDS["tone"], autana.tone)
        self.assertIn("autana tone <hz> <ms> [volume] [--pulse MS] [--internal] [--json]", autana.help_text(["tests"]))
        self.assertNotIn("tone", autana.DEBUG_VERBS)
        self.assertNotIn("debug tone", autana.help_text(["debug"]))
        with self.assertRaises(SystemExit):
            autana.debug(["tone", "440", "200"])

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
        """Shared board flags stay in help flags."""
        flag_names = [flag.split()[0] for flag, _, _ in autana.BOARD_FLAGS]
        for _, _, commands in autana.COMMAND_GROUPS:
            for command in commands:
                for synopsis, _ in command.usages:
                    if synopsis.startswith("lock hand"):
                        continue  # its own --wait means something else, see hand()'s docstring
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


if __name__ == "__main__":
    unittest.main()

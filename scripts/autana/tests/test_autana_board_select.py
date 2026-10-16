import contextlib
import io
import unittest
from unittest import mock

from test_autana import autana
import device

BOARD = "90:70:69:FE:A3:08"


class BoardSelectionTests(unittest.TestCase):
    def test_b_placement_and_full_serial_children(self):
        with mock.patch.object(device, "known_boards", return_value=[BOARD]):
            for words in (["-b", "a308", "status"], ["status", "-b", "a308"],
                          ["status", "-b=a308"], ["--board", BOARD.lower(), "status"]):
                with self.subTest(words=words):
                    flags, rest = autana.pop_globals(words)
                    self.assertEqual(flags, {"--board": BOARD})
                    self.assertEqual(rest, ["status"])
                    with autana.applied(flags):
                        self.assertEqual(autana.os.environ[autana.BOARD_ENV], BOARD)

    def test_console_pin_prompt_report_and_clear(self):
        with mock.patch.dict(autana.session_flags, {}, clear=True), \
             mock.patch.object(device, "known_boards", return_value=[BOARD]), \
             mock.patch.object(autana, "install_completion"), \
             mock.patch("builtins.input", side_effect=["use", "use a308", "use", "use -", "quit"]) as read, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(autana.console(), 0)
            self.assertEqual([call.args[0] for call in read.call_args_list],
                             ["autana> ", "autana> ", "autana@A308> ", "autana@A308> ", "autana> "])
            self.assertIn("no board pinned", output.getvalue())
            self.assertIn(BOARD, output.getvalue())
            self.assertNotIn("--board", autana.session_flags)

    def test_console_starts_pinned_and_completes_known_ids(self):
        with mock.patch.dict(autana.session_flags, {}, clear=True), \
             mock.patch.object(device, "known_boards", return_value=[BOARD]), \
             mock.patch.object(autana.sys, "argv", ["autana", "-b", "a308"]), \
             mock.patch.object(autana, "install_completion"), \
             mock.patch("builtins.input", return_value="quit") as read, \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
            self.assertEqual(stop.exception.code, 0)
            read.assert_called_once_with("autana@A308> ")
            self.assertEqual(autana.completion_candidates("use A", "A"), ["A308"])


if __name__ == "__main__":
    unittest.main()

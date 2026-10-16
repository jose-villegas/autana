import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_autana import autana
import isolation  # noqa: F401
import device

BOARD = "90:70:69:FE:A3:08"


class BoardSelectionTests(unittest.TestCase):
    def test_short_id_reaches_device_without_parent_pyserial(self):
        with tempfile.TemporaryDirectory() as folder:
            probe = Path(folder)
            (probe / "sitecustomize.py").write_text(
                "import os, sys\n"
                "sys.modules['serial'] = None\n"
                "if sys.argv[0].endswith('device.py'):\n"
                f"    print('device board=' + os.environ.get({autana.BOARD_ENV!r}, ''), flush=True)\n",
                encoding="utf-8")
            runner = (
                "import sys; sys.path.insert(0, sys.argv.pop(1)); import autana; "
                "autana.idf_python = lambda: sys.executable; autana.main()")
            environment = dict(os.environ, PYTHONPATH=str(probe))
            for selector in ("20e8", "FFFF", BOARD.lower()):
                with self.subTest(selector=selector):
                    result = subprocess.run(
                        [sys.executable, "-c", runner, str(Path(autana.__file__).parent),
                         "-b", selector, "status"], env=environment,
                        capture_output=True, text=True, timeout=30)
                    self.assertIn("device board=" + device.board_selector(selector), result.stdout, result.stderr)

    def test_b_placement_and_unchanged_selector_children(self):
        with mock.patch.object(device, "known_boards", side_effect=AssertionError("USB lookup")):
            for words in (["-b", "a308", "status"], ["status", "-b", "a308"],
                          ["status", "-b=a308"], ["--board", BOARD.lower(), "status"]):
                with self.subTest(words=words):
                    flags, rest = autana.pop_globals(words)
                    self.assertEqual(flags, {"--board": words[-2].upper() if words[0] == "--board" else "A308"})
                    self.assertEqual(rest, ["status"])
                    with autana.applied(flags):
                        self.assertEqual(autana.os.environ[autana.BOARD_ENV], flags["--board"])

    def test_console_pin_prompt_report_and_clear(self):
        with mock.patch.dict(autana.session_flags, {}, clear=True), \
             mock.patch.object(device, "known_boards", side_effect=AssertionError("USB lookup")), \
             mock.patch.object(autana, "install_completion"), \
             mock.patch("builtins.input", side_effect=["use", "use a308", "use", "use -", "quit"]) as read, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(autana.console(), 0)
            self.assertEqual([call.args[0] for call in read.call_args_list],
                             ["autana> ", "autana> ", "autana@A308> ", "autana@A308> ", "autana> "])
            self.assertIn("no board pinned", output.getvalue())
            self.assertIn("A308", output.getvalue())
            self.assertNotIn("--board", autana.session_flags)

    def test_console_starts_pinned_without_usb_completion(self):
        with mock.patch.dict(autana.session_flags, {}, clear=True), \
             mock.patch.object(device, "known_boards", side_effect=AssertionError("USB lookup")), \
             mock.patch.object(autana.sys, "argv", ["autana", "-b", "a308"]), \
             mock.patch.object(autana, "install_completion"), \
             mock.patch("builtins.input", return_value="quit") as read, \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as stop:
                autana.main()
            self.assertEqual(stop.exception.code, 0)
            read.assert_called_once_with("autana@A308> ")
            self.assertEqual(autana.completion_candidates("use A", "A"), [])


if __name__ == "__main__":
    unittest.main()

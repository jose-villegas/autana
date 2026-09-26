"""`autana docs` answers from the worktree it is run in, and never reaches the board.

    python -m unittest discover -s scripts/autana/tests
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import contextlib
import io
import subprocess
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import autana  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "docs"))
import docs_llama  # noqa: E402


def worktree():
    root = Path(tempfile.mkdtemp())
    (root / "docs").mkdir()
    (root / "docs" / "Memory.md").write_text(
        "# Memory\n\n## Framebuffer\n\nThe framebuffer lives in PSRAM.\n", encoding="utf-8")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    return root


class DocsCommandTests(unittest.TestCase):
    def run_docs(self, *args):
        root = worktree()
        stream = io.StringIO()
        with mock.patch.object(autana, "engine_worktree", return_value=str(root)), \
                mock.patch.object(autana, "send", side_effect=AssertionError("board")), \
                mock.patch.object(autana, "device_command", side_effect=AssertionError("board")), \
                mock.patch.object(docs_llama, "installed", return_value=False), \
                contextlib.redirect_stdout(stream):
            code = autana.COMMANDS["docs"](list(args))
        return code, stream.getvalue()

    def test_a_question_is_answered_from_the_worktree(self):
        code, text = self.run_docs("where", "does", "the", "framebuffer", "live")
        self.assertEqual(code, 0)
        self.assertIn("docs/Memory.md:3-5  Memory > Framebuffer", text)

    def test_flags_reach_the_search(self):
        code, text = self.run_docs("--outline", "docs/Memory.md")
        self.assertEqual(code, 0)
        self.assertEqual(text.splitlines()[0], "docs/Memory.md  Memory")

    def test_docs_is_a_command_not_a_line_for_the_board(self):
        self.assertIs(autana.COMMANDS.get("docs"), autana.docs)
        docs = mock.Mock(return_value=0)
        with mock.patch.object(sys, "argv", ["autana", "docs", "--outline", "docs/Memory.md"]), \
                mock.patch.object(autana, "forward", side_effect=AssertionError("forwarded")), \
                mock.patch.dict(autana.COMMANDS, {"docs": docs}), \
                self.assertRaises(SystemExit):
            autana.main()
        docs.assert_called_once_with(["--outline", "docs/Memory.md"])


if __name__ == "__main__":
    unittest.main()

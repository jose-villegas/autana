"""Regression tests for scripts/gates/check_autana_source.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

GATE = pathlib.Path(__file__).resolve().parents[1] / "check_autana_source.py"

TOOL = '''from pathlib import Path


def read(root):
    return (Path(root) / "launcher" / "main" / "x.c").read_text()
'''

TEST = '''import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts" / "autana"))
import tool


class T(unittest.TestCase):
    def test_a_test_may_read_the_source(self):
        (ROOT / "launcher" / "packages" / "y.h").read_text()

    def test_the_tool_may_not(self):
        {call}
'''


class AutanaSourceTest(unittest.TestCase):
    def gate(self, call):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            for name, text in (("launcher/main/x.c", ""), ("launcher/packages/y.h", ""),
                               ("scripts/autana/tool.py", TOOL),
                               ("scripts/autana/tests/test_tool.py", TEST.format(call=call))):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            return subprocess.run([sys.executable, str(GATE), str(root)], capture_output=True, text=True)

    def test_an_open_by_the_tool_fails_and_names_it(self):
        result = self.gate("tool.read(ROOT)")
        self.assertEqual(result.returncode, 1)
        self.assertIn("scripts/autana/tool.py: opened", result.stderr)
        self.assertIn("launcher/main/x.c", result.stderr)
        self.assertNotIn("y.h", result.stderr)

    def test_opens_by_tests_alone_pass(self):
        result = self.gate("pass")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

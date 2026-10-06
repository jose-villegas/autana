"""The generated-file gate finds banners by their marker, runs each banner's
command away from the tracked file, and fails a file it cannot reproduce."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from check_generated_files import MARKER, check, generated_files  # noqa: E402

# `gen.py VALUE` prints the header for VALUE; `gen.py VALUE PATH` writes it
# to PATH. Like a real generator, its banner names its output by a fixed
# name, never by the path it was handed, and the marker it writes sits below
# its own opening lines.
GENERATOR = '''# A test generator.
#
# Prints or writes one header.

import sys
value, out = sys.argv[1], sys.argv[2:]
command = "python tools/gen.py " + value + (" NAME" if out else " > NAME")
text = "/* MARKER\\n *\\n *     " + command + "\\n */\\nint value = " + value + ";\\n"
if out:
    open(out[0], "w", newline="\\n").write(text)
else:
    sys.stdout.write(text)
'''


def header(command, value):
    return f"/* {MARKER}\n *\n *     {command}\n */\nint value = {value};\n"


class GateTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.generator("tools", "main/a.h")
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)

    def generator(self, folder, name):
        path = self.root / folder / "gen.py"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(GENERATOR.replace("NAME", name).replace("MARKER", MARKER), encoding="utf-8")

    def add(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        return path

    def test_finds_only_tracked_files_that_open_with_the_marker(self):
        self.add("main/a.h", header("python tools/gen.py 1 > main/a.h", 1))
        self.add("main/late.h", "\n" * 6 + f"/* {MARKER} */\n")
        (self.root / "main" / "untracked.h").write_text(f"/* {MARKER} */\n", encoding="utf-8")
        self.assertEqual(generated_files(self.root), ["main/a.h"])

    def test_stdout_output_matches_and_a_hand_edit_fails_showing_the_change(self):
        command = "python tools/gen.py 7 > main/a.h"
        path = self.add("main/a.h", header(command, 7))
        self.assertIsNone(check(self.root, "main/a.h"))
        path.write_text(header(command, 7).replace("value = 7", "value = 8"), encoding="utf-8", newline="\n")
        problem = check(self.root, "main/a.h")
        self.assertIn("-int value = 8;", problem)
        self.assertIn("+int value = 7;", problem)

    def test_an_output_argument_is_redirected_so_the_tracked_file_is_never_written(self):
        command = "python tools/gen.py 3 main/a.h"
        path = self.add("main/a.h", header(command, 3))
        self.assertIsNone(check(self.root, "main/a.h"))
        edited = header(command, 3).replace("value = 3", "value = 4")
        path.write_text(edited, encoding="utf-8", newline="\n")
        self.assertIn("differs", check(self.root, "main/a.h"))
        self.assertEqual(path.read_text(encoding="utf-8"), edited)

    def test_the_command_runs_from_the_nearest_folder_holding_its_script(self):
        self.generator("main/tools", "a.h")
        self.add("main/a.h", header("python tools/gen.py 5 > a.h", 5))
        self.assertIsNone(check(self.root, "main/a.h"))

    def test_a_placeholder_input_fails(self):
        self.add("main/a.h", header("python tools/gen.py <screenshot.png> > main/a.h", 1))
        self.assertIn("<screenshot.png> is not in the repository", check(self.root, "main/a.h"))

    def test_a_command_that_does_not_name_its_own_file_fails(self):
        self.add("main/a.h", header("python tools/gen.py 1 > main/b.h", 1))
        self.assertIn("not to this file", check(self.root, "main/a.h"))
        self.add("main/a.h", header("python tools/gen.py 1", 1))
        self.assertIn("names neither", check(self.root, "main/a.h"))

    def test_a_missing_script_fails(self):
        self.add("main/a.h", header("python tools/gone.py 1 > main/a.h", 1))
        self.assertIn("tools/gone.py is in no folder", check(self.root, "main/a.h"))

    def test_a_failing_command_fails_with_its_error(self):
        self.add("main/a.h", header("python tools/gen.py > main/a.h", 1))
        self.assertIn("IndexError", check(self.root, "main/a.h"))


if __name__ == "__main__":
    unittest.main()

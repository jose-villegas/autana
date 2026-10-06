"""The generated-file gate finds banners by their marker, runs each banner's
command away from the tracked file, fails a file it cannot reproduce, and
keeps the generators' table current."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from check_generated_files import MARKER, TABLE_BLOCK, TABLE_DOC, check, generated_files, table  # noqa: E402

GATE = pathlib.Path(__file__).resolve().parents[1] / "check_generated_files.py"

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

# `crlf.py` prints, or with PATH writes, the header for value 1 with every
# line ending in CRLF, as a Windows console writes it, on any platform.
CRLF_GENERATOR = '''# A test generator.
#
# Writes CRLF.

import sys
out = sys.argv[1:]
command = "python tools/crlf.py NAME" if out else "python tools/crlf.py > NAME"
text = ("/* MARKER\\n *\\n *     " + command + "\\n */\\nint value = 1;\\n").replace("\\n", "\\r\\n").encode()
if out:
    open(out[0], "wb").write(text)
else:
    sys.stdout.buffer.write(text)
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

    def generator(self, folder, name, source=GENERATOR, file="gen.py"):
        path = self.root / folder / file
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source.replace("NAME", name).replace("MARKER", MARKER), encoding="utf-8")

    def add(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        return path

    def test_finds_only_tracked_files_with_the_marker_in_their_first_five_lines(self):
        self.add("main/a.h", header("python tools/gen.py 1 > main/a.h", 1))
        self.add("main/fifth.h", "\n" * 4 + f"/* {MARKER} */\n")
        self.add("main/sixth.h", "\n" * 5 + f"/* {MARKER} */\n")
        (self.root / "main" / "untracked.h").write_text(f"/* {MARKER} */\n", encoding="utf-8")
        self.assertEqual(generated_files(self.root), ["main/a.h", "main/fifth.h"])

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

    def test_banner_may_name_the_output_argument_without_hiding_payload_drift(self):
        source = GENERATOR.replace("import sys", "import pathlib\nimport shlex\nimport sys")
        source = source.replace('(" NAME" if out else " > NAME")',
                                '(" " + shlex.quote(pathlib.Path(out[0]).as_posix()) if out else " > NAME")')
        self.generator("tools", "main/a.h", source)
        path = self.add("main/a.h", header("python tools/gen.py 3 main/a.h", 3))
        self.assertIsNone(check(self.root, "main/a.h"))
        edited = header("python tools/gen.py 3 main/a.h", 4)
        path.write_text(edited, encoding="utf-8", newline="\n")
        self.assertIn("differs", check(self.root, "main/a.h"))
        self.assertEqual(path.read_text(encoding="utf-8"), edited)

    def test_crlf_from_a_generator_counts_as_lf_in_both_output_modes(self):
        for command in ("python tools/crlf.py > main/a.h", "python tools/crlf.py main/a.h"):
            with self.subTest(command=command):
                self.generator("tools", "main/a.h", CRLF_GENERATOR, "crlf.py")
                self.add("main/a.h", header(command, 1))
                self.assertIsNone(check(self.root, "main/a.h"))
                self.add("main/a.h", header(command, 2))
                self.assertIn("differs", check(self.root, "main/a.h"))

    def test_the_command_runs_from_the_nearest_folder_holding_its_script(self):
        self.generator("main/tools", "a.h")
        self.add("main/a.h", header("python tools/gen.py 5 > a.h", 5))
        self.assertIsNone(check(self.root, "main/a.h"))

    def test_a_marker_with_no_command_under_it_fails(self):
        self.add("main/a.h", f"/* {MARKER} */\n")
        self.assertIn("names no command", check(self.root, "main/a.h"))

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

    def test_the_table_names_each_output_its_generator_folder_and_command(self):
        command = "python tools/gen.py 1 > main/a.h"
        self.add("main/a.h", header(command, 1))
        (self.root / TABLE_DOC).parent.mkdir(parents=True)
        rows = table(self.root, generated_files(self.root)).splitlines()
        self.assertEqual(rows[2], "| [a.h](../../../main/a.h) | [gen.py](../../../tools/gen.py) | `./` | `" + command + "` |")

    def test_the_table_shows_a_subfolder_a_command_runs_in_and_dashes_when_it_cannot_run(self):
        self.generator("main/tools", "a.h")
        self.add("main/a.h", header("python tools/gen.py 1 > a.h", 1))
        self.add("main/b.h", header("python tools/gone.py 1 > main/b.h", 1))
        (self.root / TABLE_DOC).parent.mkdir(parents=True)
        rows = table(self.root, generated_files(self.root)).splitlines()
        self.assertIn("| [gen.py](../../../main/tools/gen.py) | `main/` |", rows[2])
        self.assertIn("| [b.h](../../../main/b.h) | - | - |", rows[3])

    def test_a_pipe_in_a_command_does_not_split_its_table_row(self):
        self.add("main/a.h", header("python tools/gen.py 1 | tee > main/a.h", 1))
        (self.root / TABLE_DOC).parent.mkdir(parents=True)
        row = table(self.root, generated_files(self.root)).splitlines()[2]
        self.assertEqual(row.replace("\\|", "").count("|"), 5)

    def test_a_full_run_fails_on_a_stale_table_and_passes_once_it_is_rewritten(self):
        self.add("main/a.h", header("python tools/gen.py 1 > main/a.h", 1))
        doc = self.add(TABLE_DOC, f"<!-- generated: {TABLE_BLOCK} -->\n<!-- /generated: {TABLE_BLOCK} -->\n")

        def gate(*args):
            return subprocess.run([sys.executable, str(GATE), "--root", str(self.root), *args],
                                  capture_output=True, text=True, check=False)

        stale = gate()
        self.assertEqual(stale.returncode, 1, stale.stdout)
        self.assertIn(f"FAIL {TABLE_DOC}", stale.stdout)
        self.assertEqual(gate(str(self.root / "main" / "a.h")).returncode, 0)
        self.assertEqual(gate("--write-table").returncode, 0)
        self.assertIn("[a.h]", doc.read_text(encoding="utf-8"))
        fresh = gate()
        self.assertEqual(fresh.returncode, 0, fresh.stdout)


if __name__ == "__main__":
    unittest.main()

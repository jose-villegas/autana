"""check_comment_layers skips the figure index's generated block on whatever page holds it, reads the rest of
that page, and runs without Pillow or numpy."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_comment_layers  # noqa: E402

BLOCK = check_comment_layers.FIGURE_BLOCK
OPEN = f"<!-- generated: {BLOCK} check: python gen.py --check -->"
CLOSE = f"<!-- /generated: {BLOCK} -->"
GATE = pathlib.Path(check_comment_layers.__file__)


class FigureBlockTests(unittest.TestCase):
    def problems(self, files):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "launcher/main/apps/sand").mkdir(parents=True)
            for path, text in files.items():
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text(text, encoding="utf-8", newline="\n")
            return check_comment_layers.problems(str(root))

    def test_the_block_is_skipped_on_any_page_that_holds_it(self):
        text = f"# Other\n{OPEN}\n| sand | ![](sand.png) |\n{CLOSE}\n"
        self.assertEqual(self.problems({"docs/Other.md": text, "docs/tools/Figures.md": text}), [])

    def test_the_index_page_without_the_block_is_read_like_any_other(self):
        self.assertEqual(self.problems({"docs/tools/Figures.md": "# Figures\n\nSand pictures.\n"}),
                         ["docs/tools/Figures.md:3: document names sand"])

    def test_a_page_holding_another_generated_block_is_not_skipped(self):
        text = f"<!-- generated: budget check: python gen.py -->\nSand row\n<!-- /generated: budget -->\n"
        self.assertEqual(self.problems({"docs/Gfx.md": text}), ["docs/Gfx.md:2: document names sand"])

    def test_a_line_after_the_block_keeps_its_line_number(self):
        text = f"# T\n{OPEN}\nsand one\nsand two\nsand three\n{CLOSE}\nSand after.\n"
        self.assertEqual(self.problems({"docs/Gfx.md": text}), ["docs/Gfx.md:7: document names sand"])

    def test_malformed_markers_do_not_crash_and_leave_the_text_read(self):
        for text in (f"{OPEN}\nSand row\n", f"Sand row\n{CLOSE}\n", f"{OPEN}\nSand row\n{OPEN}\n{CLOSE}\n",
                     "<!-- generated: figure-index\nSand row\n"):
            with self.subTest(text=text):
                found = self.problems({"docs/Gfx.md": text})
                self.assertIn("document names sand", " ".join(found))

    def test_blank_block_keeps_every_line_and_every_character_position(self):
        text = f"a\n{OPEN}\nrow | x\n{CLOSE}\nz\n"
        blanked = check_comment_layers.blank_block(text, BLOCK)
        self.assertEqual(len(blanked), len(text))
        self.assertEqual(blanked.count("\n"), text.count("\n"))
        self.assertTrue(blanked.startswith("a\n"))
        self.assertTrue(blanked.endswith("\nz\n"))
        self.assertNotIn("row", blanked)

    def test_blank_block_of_a_block_the_page_lacks_is_the_text(self):
        text = "a\n<!-- generated: budget check: python g.py -->\nrow\n<!-- /generated: budget -->\n"
        self.assertEqual(check_comment_layers.blank_block(text, BLOCK), text)

    def test_the_gate_imports_with_pillow_and_numpy_blocked(self):
        code = ("import sys\nsys.modules.update(numpy=None, PIL=None)\n"
                "sys.path.insert(0, sys.argv[1])\nimport check_comment_layers\n"
                "assert check_comment_layers.FIGURE_BLOCK\n")
        result = subprocess.run([sys.executable, "-c", code, str(GATE.parent)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

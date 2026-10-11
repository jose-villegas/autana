"""launcher/tools/device/panel_size.py: the panel's size, read from the
board header rather than copied.

    python -m unittest discover -s launcher/tools/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))

from build.c_header import read_define  # noqa: E402
import panel_size  # noqa: E402


class ReadDefineTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.header = Path(directory.name) / "constants.h"

    def test_missing_define_names_the_header_read(self):
        self.header.write_text("#define PRESENT 1\n", encoding="utf-8")
        with self.assertRaises(RuntimeError) as error:
            read_define(self.header, "ABSENT")
        self.assertEqual(str(error.exception), f"{self.header} defines no ABSENT")

    def test_reads_a_value_with_or_without_parentheses(self):
        text = "#define BSP_LCD_H_RES              (368)\n#define BSP_LCD_V_RES 448\n"
        self.header.write_text(text, encoding="utf-8")
        self.assertEqual(read_define(self.header, "BSP_LCD_H_RES"), 368)
        self.assertEqual(read_define(self.header, "BSP_LCD_V_RES"), 448)

    def test_does_not_take_a_longer_name_that_starts_the_same(self):
        text = "#define BSP_LCD_H_RES_MAX (999)\n#define BSP_LCD_H_RES (368)\n"
        self.header.write_text(text, encoding="utf-8")
        self.assertEqual(read_define(self.header, "BSP_LCD_H_RES"), 368)

    def test_a_missing_define_is_an_error_naming_it(self):
        self.header.write_text("#define BSP_LCD_H_RES (368)\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "BSP_LCD_V_RES"):
            read_define(self.header, "BSP_LCD_V_RES")

    def test_the_board_header_gives_a_portrait_panel(self):
        self.assertGreater(panel_size.PANEL_HEIGHT, panel_size.PANEL_WIDTH)


if __name__ == "__main__":
    unittest.main()

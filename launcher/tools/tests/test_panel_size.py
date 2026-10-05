"""launcher/tools/device/panel_size.py: the panel's size, read from the
board header rather than copied.

    python -m unittest discover -s launcher/tools/tests
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))

import panel_size  # noqa: E402


class ReadDefineTests(unittest.TestCase):
    def test_reads_a_value_with_or_without_parentheses(self):
        text = "#define BSP_LCD_H_RES              (368)\n#define BSP_LCD_V_RES 448\n"
        self.assertEqual(panel_size.read_define(text, "BSP_LCD_H_RES"), 368)
        self.assertEqual(panel_size.read_define(text, "BSP_LCD_V_RES"), 448)

    def test_does_not_take_a_longer_name_that_starts_the_same(self):
        text = "#define BSP_LCD_H_RES_MAX (999)\n#define BSP_LCD_H_RES (368)\n"
        self.assertEqual(panel_size.read_define(text, "BSP_LCD_H_RES"), 368)

    def test_a_missing_define_is_an_error_naming_it(self):
        with self.assertRaisesRegex(RuntimeError, "BSP_LCD_V_RES"):
            panel_size.read_define("#define BSP_LCD_H_RES (368)\n", "BSP_LCD_V_RES")

    def test_the_board_header_gives_a_portrait_panel(self):
        self.assertGreater(panel_size.PANEL_HEIGHT, panel_size.PANEL_WIDTH)


if __name__ == "__main__":
    unittest.main()

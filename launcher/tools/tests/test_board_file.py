"""launcher/tools/device/board.py and tools/gen/gen_board_config.py: a board read from its
board.toml, and the board_config.h written from it; and build/c_header.py, which reads C constants.

    python -m unittest discover -s launcher/tools/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path

# The generator puts tools/device on the path, and board.py puts tools/ there.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gen"))

import gen_board_config  # noqa: E402
import board  # noqa: E402
from build.c_header import read_define  # noqa: E402
from build.packages import board_include_dir  # noqa: E402

# The built board's mount: chip X up the screen, Y right, Z out of the back.
UPRIGHT_MOUNT = {"axis_x": [0, 1, 0], "axis_y": [1, 0, 0], "axis_z": [0, 0, -1]}


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

    def test_reads_a_string_constant(self):
        self.header.write_text('#define SUFFIX ".skin"\n', encoding="utf-8")
        self.assertEqual(read_define(self.header, "SUFFIX"), ".skin")

    def test_reads_float_and_typed_vector(self):
        self.header.write_text("#define GAMMA 2.2F\n#define AXIS ((vec3f_t){0.0F, 1.0F, 0.0F})\n", encoding="utf-8")
        self.assertEqual(read_define(self.header, "GAMMA"), 2.2)
        self.assertEqual(read_define(self.header, "AXIS"), (0.0, 1.0, 0.0))

    def test_hex_digits_are_not_float_suffixes(self):
        self.header.write_text("#define LIMIT 0xFFFF\n#define SUFFIXED 0xFFFFU\n", encoding="utf-8")
        self.assertEqual(read_define(self.header, "LIMIT"), 0xFFFF)
        self.assertEqual(read_define(self.header, "SUFFIXED"), 0xFFFF)

    def test_string_literals_keep_braces_and_comment_markers(self):
        self.header.write_text('#define URL ("https://example/{value}") /* literal */\n', encoding="utf-8")
        self.assertEqual(read_define(self.header, "URL"), "https://example/{value}")

    def test_the_board_header_gives_a_portrait_panel(self):
        self.assertGreater(board.PANEL_HEIGHT, board.PANEL_WIDTH)

    def test_the_size_is_the_chosen_boards_and_the_framebuffers(self):
        config = board_include_dir() / "board_config.h"
        gfx = Path(__file__).resolve().parents[2] / "main" / "gfx" / "gfx.h"
        self.assertEqual((board.PANEL_WIDTH, board.PANEL_HEIGHT),
                         (read_define(config, "BOARD_PANEL_WIDTH"), read_define(config, "BOARD_PANEL_HEIGHT")))
        self.assertEqual((board.PANEL_WIDTH, board.PANEL_HEIGHT),
                         (read_define(gfx, "GFX_WIDTH"), read_define(gfx, "GFX_HEIGHT")))


class ChipMountTests(unittest.TestCase):
    def test_upright_chip_x_lands_on_screen_down(self):
        # Held upright the chip reads +1 g on X; the code calls that down, screen +y.
        self.assertEqual(gen_board_config.to_screen({"imu": UPRIGHT_MOUNT}), "{-(y), (x), (z)}")
        self.assertEqual(gen_board_config.up({"imu": UPRIGHT_MOUNT}),
                         [("BOARD_IMU_UP_X", "1"), ("BOARD_IMU_UP_Y", "0"), ("BOARD_IMU_UP_Z", "0")])

    def test_a_chip_turned_a_quarter_maps_its_axes_with_it(self):
        # Chip X right, Y down the screen, Z out of the glass.
        mount = {"axis_x": [1, 0, 0], "axis_y": [0, -1, 0], "axis_z": [0, 0, 1]}
        self.assertEqual(gen_board_config.to_screen({"imu": mount}), "{-(x), -(y), -(z)}")
        self.assertEqual(gen_board_config.up({"imu": mount}),
                         [("BOARD_IMU_UP_X", "0"), ("BOARD_IMU_UP_Y", "(-1)"), ("BOARD_IMU_UP_Z", "0")])
        self.assertEqual(board.screen_label_axes({"imu": mount}),
                         [[-1, 0, 0], [0, 1, 0], [0, 0, -1]])

    def test_an_axis_off_the_device_axes_is_refused(self):
        with self.assertRaisesRegex(ValueError, "imu.axis_y"):
            board.chip_axes({"imu": {**UPRIGHT_MOUNT, "axis_y": [1, 1, 0]}})

    def test_two_chip_axes_on_one_device_axis_are_refused(self):
        with self.assertRaisesRegex(ValueError, "different device axis"):
            board.chip_axes({"imu": {**UPRIGHT_MOUNT, "axis_y": [0, -1, 0]}})


class BoardConfigTests(unittest.TestCase):
    def test_each_value_takes_its_c_form(self):
        rows = dict(gen_board_config.defines({
            "name": 'A "quoted" board',
            "panel": {"width": 368, "x_gap": 0x10},
            "imu": {"chip": "imu_chip_qmi8658", "centre_x_mm": -0.53, "axis_z": [0, 0, -1]},
            "feature": [{"side": "left"}],
        }))
        self.assertEqual(rows, {
            "BOARD_NAME": '"A \\"quoted\\" board"',
            "BOARD_PANEL_WIDTH": "368",
            "BOARD_PANEL_X_GAP": "16",
            "BOARD_IMU_CHIP": "imu_chip_qmi8658",
            "BOARD_IMU_CENTRE_X_MM": "(-0.53F)",
            "BOARD_IMU_AXIS_Z": "{0, 0, (-1)}",
        })

    def test_two_keys_that_write_one_name_are_refused(self):
        clash = {"name": "x", "panel_width": 1, "panel": {"width": 2}, "imu": UPRIGHT_MOUNT}
        with self.assertRaisesRegex(SystemExit, "BOARD_PANEL_WIDTH"):
            gen_board_config.render(clash, "board.toml", "board_config.h")

    def test_a_chip_that_is_not_a_c_symbol_is_refused(self):
        with self.assertRaisesRegex(SystemExit, "pmu.chip"):
            gen_board_config.defines({"pmu": {"chip": "power chip"}})


if __name__ == "__main__":
    unittest.main()

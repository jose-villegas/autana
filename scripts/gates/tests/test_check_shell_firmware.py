"""Regression tests for scripts/gates/check_shell_firmware.py."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_shell_firmware  # noqa: E402

TOUCH_H = """#pragma once
#define TOUCH_POLL_HZ 100
void touch_start(void);
typedef enum {
    TOUCH_GESTURE_NONE,
    TOUCH_GESTURE_TAP,
} touch_gesture_completion_t;
bool touch_gesture_take_completion(touch_gesture_completion_t* completion);
void touch_read(input_t* out);
"""
BUTTONS_H = """#pragma once
typedef struct { bool pressed; } button_t;
void buttons_start(void);
void buttons_read(button_t* boot, button_t* power);
"""
IMU_H = """#pragma once
bool imu_init(void);
bool imu_ready(void);
bool imu_read(imu_sample_t* out);
"""


class ProblemsTest(unittest.TestCase):
    def problems(self, main_c, headers=(TOUCH_H, BUTTONS_H, IMU_H)):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            inputs = root / "launcher/main/input"
            inputs.mkdir(parents=True)
            for name, text in zip(("touch.h", "buttons.h", "imu.h"), headers):
                (inputs / name).write_text(text, encoding="utf-8")
            if main_c is not None:
                (root / "launcher/main/main.c").write_text(main_c, encoding="utf-8")
            return check_shell_firmware.problems(str(root))

    def test_a_shell_that_goes_through_the_modules_passes(self):
        self.assertEqual(self.problems(
            '#include "input/input.h"\n#include "util/timing.h"\n'
            "void f(input_t* in) { input_poll(in); timing_yield(); }\n"), [])

    def test_each_firmware_include_fails(self):
        for header in ("esp_log.h", "esp_timer.h", "nvs_flash.h", "nvs.h", "freertos/task.h", "bsp/esp-bsp.h",
                       "driver/gpio.h", "input/touch.h", "input/buttons.h", "input/imu.h"):
            with self.subTest(header=header):
                found = self.problems(f'#include "{header}"\n')
                self.assertEqual(found, [f"launcher/main/main.c:1: includes {header}"])

    def test_an_angle_bracket_include_fails_too(self):
        self.assertEqual(self.problems("#include <esp_log.h>\n"), ["launcher/main/main.c:1: includes esp_log.h"])

    def test_an_include_inside_a_conditional_fails(self):
        found = self.problems('#if CONFIG_X\n#include "esp_heap_caps.h"\n#endif\n')
        self.assertEqual(found, ["launcher/main/main.c:2: includes esp_heap_caps.h"])

    def test_the_pure_input_headers_are_fine(self):
        self.assertEqual(self.problems('#include "input/imu_sample.h"\n#include "input/input.h"\n'), [])

    def test_a_firmware_prefix_call_fails(self):
        for call in ("ESP_LOGI(TAG, 1)", "heap_caps_get_free_size(MALLOC_CAP_DMA)", "nvs_flash_init()",
                     "vTaskDelay(1)", "xTaskCreate(f)", "bsp_display_start()", "esp_timer_get_time()"):
            with self.subTest(call=call):
                found = self.problems(f"void f(void) {{ {call}; }}\n")
                self.assertTrue(found and all(line.startswith("launcher/main/main.c:1: uses ") for line in found))

    def test_a_driver_function_fails_by_the_name_its_header_declares(self):
        for name in ("touch_start", "touch_read", "touch_gesture_take_completion", "buttons_start", "buttons_read",
                     "imu_init", "imu_ready", "imu_read"):
            with self.subTest(name=name):
                self.assertEqual(self.problems(f"void f(void) {{ {name}(); }}\n"),
                                 [f"launcher/main/main.c:1: uses {name}"])

    def test_a_driver_type_macro_and_enum_constant_fail(self):
        for name in ("touch_gesture_completion_t", "button_t", "TOUCH_POLL_HZ", "TOUCH_GESTURE_TAP"):
            with self.subTest(name=name):
                self.assertEqual(self.problems(f"int x = {name};\n"), [f"launcher/main/main.c:1: uses {name}"])

    def test_a_driver_call_added_to_a_header_is_covered_without_editing_the_gate(self):
        headers = (TOUCH_H + "void touch_calibrate(void);\n", BUTTONS_H, IMU_H)
        self.assertEqual(self.problems("void f(void) { touch_calibrate(); }\n", headers),
                         ["launcher/main/main.c:1: uses touch_calibrate"])

    def test_a_pure_helper_sharing_a_driver_prefix_is_not_a_driver_call(self):
        self.assertEqual(self.problems("int x = imu_gravity_screen_x(&s) + imu_rotation_level(&s);\n"), [])

    def test_a_comment_or_string_naming_firmware_is_not_code(self):
        self.assertEqual(self.problems(
            '/* esp_timer_get_time() and vTaskDelay */\n// nvs_flash_init\nconst char* s = "touch_read";\n'), [])

    def test_a_comment_spanning_lines_keeps_later_line_numbers(self):
        found = self.problems("/* one\ntwo */\nint x = touch_read;\n")
        self.assertEqual(found, ["launcher/main/main.c:3: uses touch_read"])

    def test_a_name_is_reported_once_per_line(self):
        found = self.problems("void f(void) { vTaskDelay(1); vTaskDelay(2); }\n")
        self.assertEqual(found, ["launcher/main/main.c:1: uses vTaskDelay"])

    def test_a_missing_main_is_a_problem_not_a_pass(self):
        found = self.problems(None)
        self.assertEqual(len(found), 1)
        self.assertIn("not found", found[0])

    def test_a_missing_driver_header_is_a_problem_not_a_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "launcher/main").mkdir(parents=True)
            (root / "launcher/main/main.c").write_text("int x;\n", encoding="utf-8")
            found = check_shell_firmware.problems(str(root))
        self.assertEqual(len(found), 1)
        self.assertIn("not found", found[0])


if __name__ == "__main__":
    unittest.main()

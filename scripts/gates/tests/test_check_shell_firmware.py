"""Regression tests for scripts/gates/check_shell_firmware.py."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import check_shell_firmware  # noqa: E402


def write_main(root, main_c):
    (pathlib.Path(root) / "launcher/main").mkdir(parents=True, exist_ok=True)
    (pathlib.Path(root) / "launcher/main/main.c").write_text(main_c, encoding="utf-8")


class ProblemsTest(unittest.TestCase):
    def problems(self, main_c):
        with tempfile.TemporaryDirectory() as temp:
            if main_c is not None:
                write_main(temp, main_c)
            return check_shell_firmware.problems(temp)

    def test_a_shell_that_goes_through_the_modules_passes(self):
        self.assertEqual(self.problems(
            '#include "input/input_shell.h"\n#include "util/runtime/timing.h"\n'
            "void f(input_t* in) { input_poll(in); timing_yield(); }\n"), [])

    def test_this_firmwares_own_driver_headers_and_calls_are_not_vendor_code(self):
        self.assertEqual(self.problems(
            '#include "input/touch.h"\n#include "input/imu.h"\n#include "input/buttons.h"\n'
            "void f(imu_sample_t* s) { touch_start(); imu_read(s); buttons_start(); }\n"), [])

    def test_each_vendor_include_fails(self):
        for header in ("esp_timer.h", "esp_err.h", "esp_heap_caps.h", "nvs_flash.h", "nvs.h", "freertos/task.h",
                       "bsp/esp-bsp.h", "driver/gpio.h", "hal/gpio_ll.h", "soc/soc.h", "rom/ets_sys.h"):
            with self.subTest(header=header):
                self.assertEqual(self.problems(f'#include "{header}"\n'),
                                 [f"launcher/main/main.c:1: includes {header}"])

    def test_an_angle_bracket_include_fails_too(self):
        self.assertEqual(self.problems("#include <esp_timer.h>\n"), ["launcher/main/main.c:1: includes esp_timer.h"])

    def test_an_include_inside_a_conditional_fails(self):
        found = self.problems('#if CONFIG_X\n#include "esp_heap_caps.h"\n#endif\n')
        self.assertEqual(found, ["launcher/main/main.c:2: includes esp_heap_caps.h"])

    def test_logging_is_not_firmware_but_the_timer_beside_it_is(self):
        logging = ('#include "esp_log.h"\nvoid f(void) { ESP_LOGI(T, "x"); ESP_LOGW(T, "x"); ESP_LOGE(T, "x"); '
                   'ESP_LOGD(T, "x"); ESP_LOGV(T, "x"); }\n')
        self.assertEqual(self.problems(logging), [])
        self.assertEqual(self.problems("long f(void) { return esp_timer_get_time(); }\n"),
                         ["launcher/main/main.c:1: uses esp_timer_get_time"])

    def test_a_vendor_name_fails(self):
        for name in ("esp_err_to_name", "ESP_ERROR_CHECK", "nvs_flash_init", "NVS_READONLY", "heap_caps_malloc",
                     "MALLOC_CAP_DMA", "bsp_display_start", "vTaskDelay", "xTaskCreate", "xQueueSend",
                     "uxTaskGetStackHighWaterMark", "ulTaskNotifyTake", "pvPortMalloc", "portMAX_DELAY",
                     "configTICK_RATE_HZ", "pdMS_TO_TICKS", "TaskHandle_t", "SemaphoreHandle_t", "TickType_t"):
            with self.subTest(name=name):
                self.assertEqual(self.problems(f"int x = {name};\n"), [f"launcher/main/main.c:1: uses {name}"])

    def test_this_firmwares_own_names_that_resemble_vendor_ones_pass(self):
        self.assertEqual(self.problems("int x = vec_len(a) + value_of(b) + extent + pdf_pages + config_load();\n"), [])

    def test_a_comment_or_string_naming_vendor_code_is_not_code(self):
        self.assertEqual(self.problems(
            '/* esp_timer_get_time() and vTaskDelay */\n// nvs_flash_init\nconst char* s = "esp_err_t";\n'), [])

    def test_a_comment_spanning_lines_keeps_later_line_numbers(self):
        self.assertEqual(self.problems("/* one\ntwo */\nint x = vTaskDelay;\n"),
                         ["launcher/main/main.c:3: uses vTaskDelay"])

    def test_a_name_is_reported_once_per_line(self):
        self.assertEqual(self.problems("void f(void) { vTaskDelay(1); vTaskDelay(2); }\n"),
                         ["launcher/main/main.c:1: uses vTaskDelay"])

    def test_a_missing_main_is_a_problem_not_a_pass(self):
        found = self.problems(None)
        self.assertEqual(len(found), 1)
        self.assertIn("not found", found[0])


class ShellFolderTest(unittest.TestCase):
    """main.c and everything under launcher/main/shell/ are held to one rule:
    what main.c may not name, no file under shell/ may."""

    def problems(self, files):
        with tempfile.TemporaryDirectory() as temp:
            write_main(temp, '#include "shell/shell.h"\nvoid app_main(void) { shell_run(); }\n')
            for rel, text in files.items():
                write_file(temp, rel, text)
            return check_shell_firmware.problems(temp)

    def test_a_vendor_include_or_name_anywhere_in_the_shell_folder_fails(self):
        cases = (
            ("launcher/main/shell/shell.c", '#include "esp_timer.h"\n', ["includes esp_timer.h"]),
            ("launcher/main/shell/shell.h", '#include "freertos/task.h"\n', ["includes freertos/task.h"]),
            ("launcher/main/shell/shell_apps.c", "void f(void) { vTaskDelay(1); }\n", ["uses vTaskDelay"]),
            ("launcher/main/shell/deeper/x.c", "int x = ESP_OK;\n", ["uses ESP_OK"]),
        )
        for rel, text, reasons in cases:
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: text}), [f"{rel}:1: {reason}" for reason in reasons])

    def test_logging_and_the_shells_own_modules_pass_and_other_layers_are_not_the_shell(self):
        cases = (
            ("launcher/main/shell/shell.c",
             '#include "esp_log.h"\n#include "util/runtime/timing.h"\nvoid f(void) { ESP_LOGI("t", "x"); timing_yield(); }\n'),
            ("launcher/main/ui/ui.c", "void f(void) { vTaskDelay(1); }\n"),
            ("launcher/main/shellish.c", '#include "esp_timer.h"\n'),
        )
        for rel, text in cases:
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: text}), [])

    def test_main_c_comes_first_then_the_shell_folder_in_path_order_and_only_c_and_h_files(self):
        with tempfile.TemporaryDirectory() as temp:
            write_main(temp, "int x = ESP_OK;\n")
            files = {
                "launcher/main/shell/zeta.c": "void f(void) { vTaskDelay(1); }\n",
                "launcher/main/shell/alpha.h": '#include "esp_timer.h"\n',
                "launcher/main/shell/README.md": "vTaskDelay is named here, in prose\n",
                "launcher/main/shell/notes.txt": "int y = ESP_OK;\n",
            }
            for rel, text in files.items():
                write_file(temp, rel, text)
            self.assertEqual(check_shell_firmware.problems(temp), [
                "launcher/main/main.c:1: uses ESP_OK",
                "launcher/main/shell/alpha.h:1: includes esp_timer.h",
                "launcher/main/shell/zeta.c:1: uses vTaskDelay",
            ])


def write_file(root, rel, text):
    path = pathlib.Path(root) / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


CLOCK_READ = "long f(void) { return esp_timer_get_time(); }\n"
HEAP_CALL = "void* f(void) { return heap_caps_malloc(8, MALLOC_CAP_DMA); }\n"


class OwnerProblemsTest(unittest.TestCase):
    def problems(self, files):
        with tempfile.TemporaryDirectory() as temp:
            for rel, text in files.items():
                write_file(temp, rel, text)
            return check_shell_firmware.owner_problems(temp)

    def test_a_clock_or_heap_call_outside_its_owner_fails_wherever_the_firmware_or_a_suite_has_it(self):
        cases = (
            ("launcher/main/gfx/gfx.c", CLOCK_READ, ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
            ("launcher/main/util/runtime/job.c", HEAP_CALL, ["uses heap_caps_malloc; only util/runtime/memory and a driver may",
                                                     "uses MALLOC_CAP_DMA; only util/runtime/memory and a driver may"]),
            ("launcher/main/apps/x/tests/suite_x.c", CLOCK_READ,
             ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
            ("launcher/test/suites/suite_y.c", HEAP_CALL, ["uses heap_caps_malloc; only util/runtime/memory and a driver may",
                                                          "uses MALLOC_CAP_DMA; only util/runtime/memory and a driver may"]),
            ("launcher/tools/render/render_host.c", CLOCK_READ,
             ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
            ("launcher/main/ui/ui.h", "#define NOW() esp_timer_get_time()\n",
             ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
            ("launcher/test/suites/suite_z_device.c", HEAP_CALL,
             ["uses heap_caps_malloc; only util/runtime/memory and a driver may",
              "uses MALLOC_CAP_DMA; only util/runtime/memory and a driver may"]),
            ("launcher/main/apps/x/tests/suite_x_device.c", CLOCK_READ,
             ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
            ("launcher/tools/render/render_device.c", CLOCK_READ,
             ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
            ("launcher/main/apps/board/board.c", CLOCK_READ,
             ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]),
        )
        for rel, text, reasons in cases:
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: text}), [f"{rel}:1: {reason}" for reason in reasons])

    def test_each_owner_and_every_driver_may_call_what_it_owns(self):
        cases = (
            ("launcher/main/util/runtime/timing.h", CLOCK_READ),
            ("launcher/main/util/runtime/timing_wheel.h", CLOCK_READ),
            ("launcher/main/util/runtime/memory.c", HEAP_CALL),
            ("launcher/main/util/runtime/memory_extra.c", HEAP_CALL),
            ("launcher/main/gfx/gfx_null_panel_device.c", CLOCK_READ + HEAP_CALL),
            ("launcher/main/input/input_device.c", CLOCK_READ),
            ("launcher/main/board/board.h", "#define FB_CAPS (MALLOC_CAP_SPIRAM)\n"),
        )
        for rel, text in cases:
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: text}), [])

    def test_an_owner_owns_only_its_own_vendor_names(self):
        cases = (
            ("launcher/main/util/runtime/timing.h", HEAP_CALL, "heap_caps_malloc; only util/runtime/memory"),
            ("launcher/main/util/runtime/memory.c", CLOCK_READ, "esp_timer_get_time; only util/runtime/timing"),
        )
        for rel, text, reason in cases:
            with self.subTest(rel=rel):
                found = self.problems({rel: text})
                self.assertTrue(found and found[0].startswith(f"{rel}:1: uses {reason}"), found)

    def test_a_name_only_resembling_an_owners_is_not_the_owner(self):
        clock = ["uses esp_timer_get_time; only util/runtime/timing and a driver may"]
        heap = ["uses heap_caps_malloc; only util/runtime/memory and a driver may",
                "uses MALLOC_CAP_DMA; only util/runtime/memory and a driver may"]
        cases = (
            ("launcher/main/util/runtime/memorize.c", HEAP_CALL, heap),
            ("launcher/main/util/runtime/timings.c", CLOCK_READ, clock),
            ("launcher/main/gfx/timing.c", CLOCK_READ, clock),
            ("launcher/main/util/math/timing.c", CLOCK_READ, clock),
            ("launcher/main/util/runtime/memory/memory.c", HEAP_CALL, heap),
            ("launcher/main/input/touch_device.h", CLOCK_READ, clock),
        )
        for rel, text, reasons in cases:
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: text}), [f"{rel}:1: {reason}" for reason in reasons])

    def test_a_name_used_twice_on_a_line_is_reported_once(self):
        rel = "launcher/main/gfx/gfx.c"
        self.assertEqual(self.problems({rel: "long d = esp_timer_get_time() - esp_timer_get_time();\n"}),
                         [f"{rel}:1: uses esp_timer_get_time; only util/runtime/timing and a driver may"])

    def test_a_shell_file_is_left_to_the_stricter_shell_rule(self):
        for rel in ("launcher/main/shell/shell.c", "launcher/main/shell/shell_apps.h"):
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: CLOCK_READ}), [])

    def test_the_host_platform_beneath_the_suites_and_vendored_code_are_not_checked(self):
        for rel in ("launcher/test/heap_arena.c", "launcher/test/timing.c", "launcher/test/stubs/esp_heap_caps.h",
                    "launcher/components/microui/src/microui.c", "editor/runtime/runtime.c",
                    "launcher/main/util/notes.md"):
            with self.subTest(rel=rel):
                self.assertEqual(self.problems({rel: CLOCK_READ + HEAP_CALL}), [])

    def test_a_comment_or_string_naming_the_clock_or_heap_is_not_a_call(self):
        self.assertEqual(self.problems({"launcher/main/gfx/gfx.c": (
            '/* esp_timer_get_time() */\n// heap_caps_malloc(8, MALLOC_CAP_DMA)\n'
            'const char* s = "heap_caps_get_free_size";\n')}), [])

    def test_main_c_is_left_to_the_shell_rule_so_a_line_is_reported_once(self):
        self.assertEqual(self.problems({"launcher/main/main.c": CLOCK_READ}), [])


class CommandLineTest(unittest.TestCase):
    def run_gate(self, main_c, *arguments):
        with tempfile.TemporaryDirectory() as temp:
            write_main(temp, main_c)
            script = pathlib.Path(check_shell_firmware.__file__)
            return subprocess.run([sys.executable, str(script), *arguments], cwd=temp, capture_output=True,
                                  text=True)

    def test_a_clean_main_exits_zero(self):
        result = self.run_gate("int x;\n")
        self.assertEqual(result.returncode, 0)
        self.assertIn("0 vendor firmware use(s)", result.stdout)

    def test_a_main_that_touches_vendor_code_exits_one_and_names_the_line(self):
        result = self.run_gate('#include "esp_timer.h"\n')
        self.assertEqual(result.returncode, 1)
        self.assertIn("launcher/main/main.c:1: includes esp_timer.h", result.stdout)

    def test_an_argument_exits_two(self):
        self.assertEqual(self.run_gate("int x;\n", "main.c").returncode, 2)

    def test_a_clock_read_outside_its_owner_exits_one_with_a_clean_main(self):
        with tempfile.TemporaryDirectory() as temp:
            write_main(temp, "int x;\n")
            write_file(temp, "launcher/main/gfx/gfx.c", CLOCK_READ)
            script = pathlib.Path(check_shell_firmware.__file__)
            result = subprocess.run([sys.executable, str(script)], cwd=temp, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("launcher/main/gfx/gfx.c:1: uses esp_timer_get_time", result.stdout)
        self.assertIn("1 clock or heap use(s) outside util/runtime/timing, util/runtime/memory and the drivers", result.stdout)


SYNTHETIC_MAIN = """#include "esp_timer.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "shell";

void
app_main(void) {
    nvs_flash_init();
    void* p = heap_caps_malloc(8, MALLOC_CAP_DMA);
    vTaskDelay(1);
    ESP_LOGI(TAG, "logging is fine");
}
"""


class SyntheticMainTest(unittest.TestCase):
    def test_each_kind_of_violation_is_caught_and_logging_is_not(self):
        with tempfile.TemporaryDirectory() as temp:
            write_main(temp, SYNTHETIC_MAIN)
            found = check_shell_firmware.problems(temp)
        for expected in ("includes esp_timer.h", "includes freertos/task.h", "uses nvs_flash_init",
                         "uses heap_caps_malloc", "uses MALLOC_CAP_DMA", "uses vTaskDelay"):
            self.assertTrue(any(line.endswith(expected) for line in found), expected)
        self.assertFalse(any("esp_log" in line or "ESP_LOGI" in line for line in found))


if __name__ == "__main__":
    unittest.main()

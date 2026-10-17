"""The device frame entry rejects an orientation chosen by gravity."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class TestFrameHold(unittest.TestCase):
    @unittest.skipUnless(shutil.which("gcc"), "needs gcc")
    def test_frame_requires_a_hold_before_any_frame_work(self):
        source = pathlib.Path(__file__).resolve().parents[2] / "tests/render_lab_test.c"
        text = source.read_text()
        constants = "\n".join(line for line in source.with_suffix(".h").read_text().splitlines()
                              if line.startswith("#define RENDER_LAB_TEST_"))
        frame = text[text.index("int64_t\nrender_lab_test_frame"):text.rindex("#endif")]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            probe = root / "probe.c"
            probe.write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
static jmp_buf reached;
static bool held;
static bool display_quarter_held(void) { return held; }
#define assert(condition) do { if (!(condition)) longjmp(reached, 1); } while (0)
typedef struct { int unused; } input_t;
static void begin(void) { longjmp(reached, 2); }
#define gfx_present_begin() begin()
#define render_context_set_dynamic_resolution(...) ((void)0)
#define render_context_set_scale(...) ((void)0)
#define timing_now_us() 0
#define scene_shell_render(...) ((void)0)
#define gfx_present_wait() ((void)0)
#define ui_clear_band_overlay() ((void)0)
#define scene_shell_compose(...) ((void)0)
#define TEST_ASSERT_TRUE_MESSAGE(...) ((void)0)
#define TEST_ASSERT_FALSE_MESSAGE(...) ((void)0)
#define TEST_ASSERT_NOT_NULL(...) ((void)0)
#define gfx_present() ((void)0)
static void callback(int dt, const input_t* input) { (void)dt; (void)input; }
static struct { void (*update)(int, const input_t*); void (*frame)(int, const input_t*); }
app_render_lab = {callback, callback};
''' + constants + '\n' + frame + r'''
int main(void) {
    int result = setjmp(reached);
    if (result == 0) render_lab_test_frame(false, NULL);
    if (result != 1) return 1;
    held = true;
    result = setjmp(reached);
    if (result == 0) render_lab_test_frame(false, NULL);
    return result == 2 ? 0 : 1;
}
''')
            binary = root / "probe.exe"
            subprocess.check_call(["gcc", "-std=c11", str(probe), "-o", str(binary)])
            subprocess.check_call([str(binary)])


    @unittest.skipUnless(shutil.which("gcc"), "needs gcc")
    def test_device_hold_notifies_the_shell_when_the_quarter_changes(self):
        main = pathlib.Path(__file__).resolve().parents[4]
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            device = work / "display_device.c"
            source = (main / "display/display_device.c").read_text()
            state = "\n".join(line for line in source.splitlines() if line.startswith("static display_"))
            functions = source[source.index("void\ndisplay_reset_quarter"):source.index("int\ndisplay_system_panel_clock_hz")]
            device.write_text('#include "display/display_shell.h"\n#include <stddef.h>\n' + state + "\n" + functions)
            driver = work / "hold.c"
            driver.write_text(r"""
#include "display/display_shell.h"
static int calls;
static int applied;
static void changed(int quarter) { calls++; applied = quarter; }
int main(void) {
    display_reset_quarter();
    display_set_quarter_handler(changed);
    display_hold_quarter(DISPLAY_PORTRAIT);
    if (calls != 1 || applied != DISPLAY_PORTRAIT || !display_quarter_held()) return 1;
    display_hold_quarter(DISPLAY_PORTRAIT);
    if (calls != 1) return 2;
    if (display_quarter_now() != DISPLAY_PORTRAIT) return 3;
    display_hold_quarter(DISPLAY_LANDSCAPE);
    if (calls != 2 || applied != DISPLAY_LANDSCAPE) return 4;
    display_release_quarter();
    if (display_quarter_held()) return 5;
    return 0;
}
""")
            command = ["gcc", "-std=c11"]
            for include in (main, main.parent / "test/stubs", main.parent / "packages/math/include"):
                command += ["-I", str(include)]
            output = work / "hold.exe"
            command += [str(driver), str(device), str(main / "display/display.c"), "-lm", "-o", str(output)]
            subprocess.check_call(command)
            subprocess.check_call([str(output)])


if __name__ == "__main__":
    unittest.main()

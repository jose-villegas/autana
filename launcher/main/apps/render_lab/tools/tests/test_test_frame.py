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
    def test_harness_hold_sets_transform_for_each_quarter(self):
        source = pathlib.Path(__file__).resolve().parents[2] / "tests/render_lab_test.c"
        text = source.read_text()
        start = text.index("void\nrender_lab_test_hold_quarter")
        helper = text[start:text.index("\nvoid\n", start + 1)]
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            probe = work / "hold.c"
            probe.write_text(r"""
static int held, applied, calls;
#define GFX_WIDTH 368
#define GFX_HEIGHT 448
static void display_hold_quarter(int quarter) { held = quarter; }
static int ui_transform_quarter_turn(int quarter, int width, int height) {
    return quarter + width + height;
}
static void ui_set_transform(int transform) { applied = transform; calls++; }
""" + helper + r"""
int main(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        render_lab_test_hold_quarter(quarter);
        if (held != quarter || applied != quarter + GFX_WIDTH + GFX_HEIGHT) return 1;
        if (calls != quarter + 1) return 2;
    }
    return 0;
}
""")
            output = work / "hold.exe"
            subprocess.check_call(["gcc", "-std=c11", str(probe), "-o", str(output)])
            subprocess.check_call([str(output)])


if __name__ == "__main__":
    unittest.main()

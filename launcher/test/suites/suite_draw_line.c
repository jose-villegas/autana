#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_test.h"
#include "test_cleanup.h"

enum { LINE_START = 10, LINE_STEPS = 9, DASH_ON = 2, DASH_OFF = 2 };

#define LINE_INK GFX_RGB(0xFFFFFF)

static void
fixture(void) {
    TEST_ASSERT_TRUE(gfx_init());
    suite_set_test_cleanup(gfx_reset_for_test);
    draw_clear(0);
}

static void
assert_line(int dx, int dy, unsigned pattern, unsigned expected) {
    draw_clear(0);
    draw_line_ex(LINE_START, LINE_START, LINE_START + (dx * (LINE_STEPS - 1)), LINE_START + (dy * (LINE_STEPS - 1)),
                 LINE_INK, pattern);
    gfx_surface_t surface;
    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
    for (int step = 0; step < LINE_STEPS; step++) {
        const gfx_color_t pixel = gfx_target_row(surface.target, LINE_START + (dy * step))[LINE_START + (dx * step)];
        TEST_ASSERT_EQUAL_HEX16((expected & (1u << step)) ? LINE_INK : 0, pixel);
    }
}

static void
test_horizontal_dash_pixels(void) {
    fixture();
    assert_line(1, 0, DRAW_LINE_DASH(DASH_ON, DASH_OFF), 0x133);
    assert_line(-1, 0, DRAW_LINE_DASH(3, 1), 0x177);
}

static void
test_vertical_dash_pixels(void) {
    fixture();
    assert_line(0, 1, DRAW_LINE_DASH(DASH_ON, DASH_OFF), 0x133);
    assert_line(0, -1, DRAW_LINE_DASH(1, 3), 0x111);
}

static void
test_diagonal_dash_pixels(void) {
    fixture();
    assert_line(1, 1, DRAW_LINE_DASH(DASH_ON, DASH_OFF), 0x133);
    assert_line(-1, -1, DRAW_LINE_DASH(0, 0), 0x1FF);
    assert_line(1, -1, DRAW_LINE_DASH(0, DASH_OFF), 0);
}

static void
test_open_dash_keeps_its_phase(void) {
    fixture();
    assert_line(1, 1, DRAW_LINE_DASH(DASH_ON, DASH_OFF) | DRAW_LINE_OPEN, 0x132);
    assert_line(1, 0, DRAW_LINE_DASH(3, 1) | DRAW_LINE_OPEN | DRAW_LINE_ADD, 0x176);
}

void
suite_draw_line(void) {
    RUN_TEST(test_horizontal_dash_pixels);
    RUN_TEST(test_vertical_dash_pixels);
    RUN_TEST(test_diagonal_dash_pixels);
    RUN_TEST(test_open_dash_keeps_its_phase);
}

SUITE_REGISTER(suite_draw_line);
#endif

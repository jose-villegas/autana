#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_test.h"
#include "testkit/cleanup.h"

#include <stdlib.h>
#include <string.h>
#include "image/pixel/rgb565.h"

enum { LINE_START = 10, LINE_STEPS = 9, DASH_ON = 2, DASH_OFF = 2 };

#define LINE_INK   RGB565BE(0xFFFFFF)
#define LINE_BASE  RGB565BE(0x203040)

/* The band the band-target test draws into, away from the frame's edges. */
#define BAND_ROW   (GFX_BAND_HEIGHT * 4)
/* Where the band test's long lines end across the screen. */
#define LINE_FAR_X (GFX_WIDTH / 4)

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
        const gfx_color_t pixel = gfx_target_color(surface.target, LINE_START + (dy * step))[LINE_START + (dx * step)];
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

static gfx_color_t* full_rows;

static void
release_full_rows(void) {
    free(full_rows);
    full_rows = NULL;
    gfx_reset_for_test();
}

/* A band holds only some of a line's rows: those it holds get exactly the
 * full frame's pixels, dash phase and additive blend included, and the
 * framebuffer outside the band is untouched. */
static void
test_band_target_matches_the_full_frame(void) {
    fixture();
    const size_t row_bytes = GFX_WIDTH * sizeof *full_rows;
    full_rows = malloc(GFX_BAND_HEIGHT * row_bytes);
    suite_set_test_cleanup(release_full_rows);
    TEST_ASSERT_NOT_NULL(full_rows);
    const int top = BAND_ROW;
    const int bottom = BAND_ROW + GFX_BAND_HEIGHT - 1;
    static const unsigned patterns[] = {0, DRAW_LINE_DASH(DASH_ON, DASH_OFF) | DRAW_LINE_OPEN | DRAW_LINE_ADD};
    const int lines[][4] = {
        {LINE_START, top - LINE_STEPS, LINE_START + LINE_STEPS, bottom + LINE_STEPS}, /* through both edges */
        {LINE_START, top - LINE_STEPS, LINE_FAR_X, top + LINE_STEPS},                 /* shallow, through the top */
        {LINE_FAR_X, bottom + LINE_STEPS, LINE_START, bottom - LINE_STEPS}, /* backwards, through the bottom */
        {LINE_START, top + 1, LINE_FAR_X, bottom - 1},                      /* inside the band */
    };
    for (size_t p = 0; p < sizeof patterns / sizeof *patterns; p++) {
        for (size_t l = 0; l < sizeof lines / sizeof *lines; l++) {
            draw_clear(LINE_BASE);
            draw_line_ex(lines[l][0], lines[l][1], lines[l][2], lines[l][3], LINE_INK, patterns[p]);
            memcpy(full_rows, gfx_framebuffer() + (size_t)BAND_ROW * GFX_WIDTH, GFX_BAND_HEIGHT * row_bytes);
            draw_clear(LINE_BASE);
            gfx_band_begin_for_test(BAND_ROW);
            draw_line_ex(lines[l][0], lines[l][1], lines[l][2], lines[l][3], LINE_INK, patterns[p]);
            gfx_band_end_for_test();
            const gfx_color_t* pixels = gfx_framebuffer();
            for (int y = 0; y < GFX_HEIGHT; y++) {
                const gfx_color_t* row = pixels + ((size_t)y * GFX_WIDTH);
                if (y >= BAND_ROW && y < BAND_ROW + GFX_BAND_HEIGHT) {
                    TEST_ASSERT_EQUAL_MEMORY(full_rows + ((size_t)(y - BAND_ROW) * GFX_WIDTH), row, row_bytes);
                } else {
                    TEST_ASSERT_EACH_EQUAL_HEX16(LINE_BASE, row, GFX_WIDTH);
                }
            }
        }
    }
}

void
suite_draw_line(void) {
    RUN_TEST(test_horizontal_dash_pixels);
    RUN_TEST(test_vertical_dash_pixels);
    RUN_TEST(test_diagonal_dash_pixels);
    RUN_TEST(test_open_dash_keeps_its_phase);
    RUN_TEST(test_band_target_matches_the_full_frame);
}

SUITE_REGISTER(suite_draw_line);
#endif

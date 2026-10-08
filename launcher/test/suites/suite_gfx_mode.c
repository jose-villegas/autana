/*
 * Portable suite: gfx_mode_resolve(), the mode-grant arithmetic behind
 * gfx_mode_enter() (gfx_mode.c), driven directly since gfx_mode.h carries no
 * ESP-IDF dependency. gfx_mode.c's own allocation and geometry-swap side of
 * gfx_mode_enter()/gfx_mode_exit() needs real device memory and is not
 * covered here; see suite_gfx.c for that half.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/present/gfx_mode.h"

#define FULL_W 368
#define FULL_H 448
#define BAND_H 32

static gfx_mode_request_t
request(gfx_layout_t layout, bool interlace_x, bool interlace_y) {
    gfx_mode_request_t r = {0};
    r.layout = layout;
    r.interlace_x = interlace_x;
    r.interlace_y = interlace_y;
    return r;
}

static void
test_full_fb_full_res_grants_the_panels_own_geometry(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_FULL_FB, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_FULL_FB, g.layout);
    TEST_ASSERT_EQUAL_INT(FULL_W, g.width);
    TEST_ASSERT_EQUAL_INT(FULL_H, g.height);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.band_height, "full-fb mode has no band height");
}

static void
test_bands_at_full_res_grants_the_compile_time_band_height(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_BANDS, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, g.layout);
    TEST_ASSERT_EQUAL_INT(FULL_W, g.width);
    TEST_ASSERT_EQUAL_INT(FULL_H, g.height);
    TEST_ASSERT_EQUAL_INT(BAND_H, g.band_height);
}

static void
test_interlace_choice_passes_through_unchanged(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_FULL_FB, true, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_TRUE(g.interlace_x);
    TEST_ASSERT_FALSE(g.interlace_y);
}

/* GFX_LAYOUT_INDEXED and its index-image geometry pass through the same
 * "request wins, system caps nothing yet" arithmetic every other field
 * already does; gfx_mode_resolve() has no opinion of its own about them. */
static void
test_indexed_layout_and_index_geometry_pass_through_unchanged(void) {
    gfx_mode_request_t r = request(GFX_LAYOUT_INDEXED, false, false);
    r.index_grid_w = 92;
    r.index_grid_h = 112;
    r.cell_size = 4;

    const gfx_mode_t g = gfx_mode_resolve(&r, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, g.layout);
    TEST_ASSERT_EQUAL_INT(0, g.band_height);
    TEST_ASSERT_EQUAL_INT(92, g.index_grid_w);
    TEST_ASSERT_EQUAL_INT(112, g.index_grid_h);
    TEST_ASSERT_EQUAL_INT(4, g.cell_size);
}

void
run_gfx_mode_suite(void) {
    RUN_TEST(test_full_fb_full_res_grants_the_panels_own_geometry);
    RUN_TEST(test_bands_at_full_res_grants_the_compile_time_band_height);
    RUN_TEST(test_interlace_choice_passes_through_unchanged);
    RUN_TEST(test_indexed_layout_and_index_geometry_pass_through_unchanged);
}

SUITE_REGISTER(run_gfx_mode_suite);

/*
 * Portable suite: gfx_mode_resolve() - the mode-grant arithmetic behind
 * gfx_mode_enter() (gfx.c), driven directly since gfx_mode.h carries no
 * ESP-IDF dependency. gfx.c's own allocation and geometry-swap side of
 * gfx_mode_enter()/gfx_mode_exit() needs real device memory and is not
 * covered here - see suite_gfx.c for that half.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_mode.h"

#define FULL_W 368
#define FULL_H 448
#define BAND_H 32

static gfx_mode_request_t
request(gfx_layout_t layout, gfx_resolution_t resolution, bool interlace_x, bool interlace_y) {
    gfx_mode_request_t r = {0};
    r.layout = layout;
    r.resolution = resolution;
    r.interlace_x = interlace_x;
    r.interlace_y = interlace_y;
    r.pixfmt = GFX_PIXFMT_RGB565;
    return r;
}

static void
test_full_fb_full_res_grants_the_panels_own_geometry(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_FULL_FB, GFX_RESOLUTION_FULL, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_FULL_FB, g.layout);
    TEST_ASSERT_EQUAL_INT(GFX_RESOLUTION_FULL, g.resolution);
    TEST_ASSERT_EQUAL_INT(FULL_W, g.width);
    TEST_ASSERT_EQUAL_INT(FULL_H, g.height);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.band_height, "full-fb mode has no band height");
}

static void
test_bands_at_full_res_grants_the_compile_time_band_height(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_BANDS, GFX_RESOLUTION_FULL, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, g.layout);
    TEST_ASSERT_EQUAL_INT(FULL_W, g.width);
    TEST_ASSERT_EQUAL_INT(FULL_H, g.height);
    TEST_ASSERT_EQUAL_INT(BAND_H, g.band_height);
}

/* min(request, system max): the app's own request is honoured even when
 * the system would allow more. */
static void
test_a_half_res_request_is_granted_under_a_full_res_system_max(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_FULL_FB, GFX_RESOLUTION_HALF, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_RESOLUTION_HALF, g.resolution);
    TEST_ASSERT_EQUAL_INT(FULL_W / 2, g.width);
    TEST_ASSERT_EQUAL_INT(FULL_H / 2, g.height);
}

/* min(request, system max): the system cap restricts a full-res request. */
static void
test_a_full_res_request_is_capped_by_a_half_res_system_max(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_FULL_FB, GFX_RESOLUTION_FULL, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_HALF, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_RESOLUTION_HALF, g.resolution);
    TEST_ASSERT_EQUAL_INT(FULL_W / 2, g.width);
    TEST_ASSERT_EQUAL_INT(FULL_H / 2, g.height);
}

static void
test_half_res_bands_halves_the_band_height_too(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_BANDS, GFX_RESOLUTION_HALF, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(BAND_H / 2, g.band_height);
}

static void
test_interlace_choice_passes_through_unchanged(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_FULL_FB, GFX_RESOLUTION_FULL, true, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_TRUE(g.interlace_x);
    TEST_ASSERT_FALSE(g.interlace_y);
}

/* GFX_PIXFMT_INDEXED8 and its index-image geometry pass through the same
 * "request wins, system caps nothing yet" arithmetic every other field
 * already does - gfx_mode_resolve() has no opinion of its own about them. */
static void
test_indexed8_pixfmt_and_index_geometry_pass_through_unchanged(void) {
    gfx_mode_request_t r = request(GFX_LAYOUT_BANDS, GFX_RESOLUTION_FULL, false, false);
    r.pixfmt = GFX_PIXFMT_INDEXED8;
    r.index_grid_w = 92;
    r.index_grid_h = 112;
    r.cell_size = 4;

    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_PIXFMT_INDEXED8, g.pixfmt);
    TEST_ASSERT_EQUAL_INT(92, g.index_grid_w);
    TEST_ASSERT_EQUAL_INT(112, g.index_grid_h);
    TEST_ASSERT_EQUAL_INT(4, g.cell_size);
}

/* A plain RGB565 band request (an app-driven band renderer's own path
 * today) still reports the default pixfmt - adding INDEXED8 must not
 * change what an existing caller that never sets `pixfmt` gets granted. */
static void
test_rgb565_is_still_the_default_band_pixfmt(void) {
    const gfx_mode_request_t r = request(GFX_LAYOUT_BANDS, GFX_RESOLUTION_FULL, false, false);
    const gfx_mode_t g = gfx_mode_resolve(&r, GFX_RESOLUTION_FULL, FULL_W, FULL_H, BAND_H);

    TEST_ASSERT_EQUAL_INT(GFX_PIXFMT_RGB565, g.pixfmt);
}

void
run_gfx_mode_suite(void) {
    RUN_TEST(test_full_fb_full_res_grants_the_panels_own_geometry);
    RUN_TEST(test_bands_at_full_res_grants_the_compile_time_band_height);
    RUN_TEST(test_a_half_res_request_is_granted_under_a_full_res_system_max);
    RUN_TEST(test_a_full_res_request_is_capped_by_a_half_res_system_max);
    RUN_TEST(test_half_res_bands_halves_the_band_height_too);
    RUN_TEST(test_interlace_choice_passes_through_unchanged);
    RUN_TEST(test_indexed8_pixfmt_and_index_geometry_pass_through_unchanged);
    RUN_TEST(test_rgb565_is_still_the_default_band_pixfmt);
}

SUITE_REGISTER(run_gfx_mode_suite);

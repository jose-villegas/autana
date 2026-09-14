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
    gfx_mode_request_t r;
    r.layout = layout;
    r.resolution = resolution;
    r.interlace_x = interlace_x;
    r.interlace_y = interlace_y;
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

void
run_gfx_mode_suite(void) {
    RUN_TEST(test_full_fb_full_res_grants_the_panels_own_geometry);
    RUN_TEST(test_bands_at_full_res_grants_the_compile_time_band_height);
    RUN_TEST(test_a_half_res_request_is_granted_under_a_full_res_system_max);
    RUN_TEST(test_a_full_res_request_is_capped_by_a_half_res_system_max);
    RUN_TEST(test_half_res_bands_halves_the_band_height_too);
    RUN_TEST(test_interlace_choice_passes_through_unchanged);
}

SUITE_REGISTER(run_gfx_mode_suite);

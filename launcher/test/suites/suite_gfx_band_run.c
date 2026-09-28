/* Host tests for gfx's transient-picture loop over the live gfx build. */

#include "suites.h"
#include "unity.h"

#include <stdlib.h>

#include "gfx/gfx.h"
#include "gfx/gfx_band_run.h"

typedef struct {
    int draw_calls;
    int overlay_calls;
    int capacity;
    int* row0;
    int* row1;
} band_probe_t;

static band_probe_t* probe;

static void
fixture(void) {
#ifndef DEVICE_BUILD
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
    gfx_set_debug_overlay(false);
    gfx_set_leaf_overlay(false);
#endif
    probe = NULL;
}

static band_probe_t*
probe_new(void) {
    const int capacity = GFX_HEIGHT / GFX_BAND_HEIGHT;
    band_probe_t* const state = calloc(1, sizeof(*state));
    TEST_ASSERT_NOT_NULL(state);
    state->row0 = calloc((size_t)capacity, sizeof(*state->row0));
    state->row1 = calloc((size_t)capacity, sizeof(*state->row1));
    TEST_ASSERT_NOT_NULL(state->row0);
    TEST_ASSERT_NOT_NULL(state->row1);
    state->capacity = capacity;
    return state;
}

static void
probe_free(band_probe_t* state) {
    free(state->row1);
    free(state->row0);
    free(state);
}

static void
draw(int row0, int row1, gfx_color_t* target) {
    TEST_ASSERT_TRUE(probe->draw_calls < probe->capacity);
    probe->row0[probe->draw_calls] = row0;
    probe->row1[probe->draw_calls++] = row1;
    target[0] = 0x1111;
}

static void
overlay(int row0, int row1, gfx_color_t* target) {
    TEST_ASSERT_TRUE(probe->overlay_calls < probe->capacity);
    TEST_ASSERT_EQUAL_HEX16(0x1111, target[0]);
    probe->row0[probe->overlay_calls] = row0;
    probe->row1[probe->overlay_calls++] = row1;
    target[0] = 0x2222;
}

static void
enter_bands(void) {
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_BANDS};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, gfx_mode_enter(&request)->layout);
}

static void
test_dirty_bands_are_drawn_then_overlaid_and_submitted(void) {
    fixture();
    enter_bands();

    TEST_ASSERT_EQUAL_INT(GFX_READBACK_PENDING, gfx_readback_begin());
    probe = probe_new();
    TEST_ASSERT_TRUE(gfx_band_run(draw, NULL));
    probe_free(probe);

    probe = probe_new();
    gfx_mark_dirty(0, 2 * GFX_BAND_HEIGHT, 1, 1);
    TEST_ASSERT_TRUE(gfx_band_run(draw, overlay));
    TEST_ASSERT_EQUAL_INT(1, probe->draw_calls);
    TEST_ASSERT_EQUAL_INT(1, probe->overlay_calls);
    TEST_ASSERT_EQUAL_INT(2 * GFX_BAND_HEIGHT, probe->row0[0]);
    TEST_ASSERT_EQUAL_INT(3 * GFX_BAND_HEIGHT, probe->row1[0]);

    gfx_color_t row[GFX_WIDTH];
    gfx_read_panel_row(2 * GFX_BAND_HEIGHT, row);
    TEST_ASSERT_EQUAL_HEX16(0x2222, row[0]);

    probe_free(probe);
    gfx_readback_end();
    gfx_mode_exit();
}

static void
test_full_dirty_frame_covers_every_row_once_in_order(void) {
    fixture();
    enter_bands();

    probe = probe_new();
    TEST_ASSERT_TRUE(gfx_band_run(draw, NULL));
    TEST_ASSERT_EQUAL_INT(probe->capacity, probe->draw_calls);
    for (int i = 0; i < probe->capacity; i++) {
        TEST_ASSERT_EQUAL_INT(i * GFX_BAND_HEIGHT, probe->row0[i]);
        TEST_ASSERT_EQUAL_INT((i + 1) * GFX_BAND_HEIGHT, probe->row1[i]);
    }

    probe_free(probe);
    gfx_mode_exit();
}

static void
test_draw_null_never_enters_the_loop(void) {
    fixture();
    enter_bands();

    probe = probe_new();
    TEST_ASSERT_FALSE(gfx_band_run(NULL, overlay));
    TEST_ASSERT_EQUAL_INT(0, probe->draw_calls);
    TEST_ASSERT_EQUAL_INT(0, probe->overlay_calls);

    probe_free(probe);
    gfx_mode_exit();
}

static void
test_an_indexed_picture_never_enters_the_transient_loop(void) {
    fixture();
    const gfx_mode_request_t request = {
        .layout = GFX_LAYOUT_INDEXED,
        .index_grid_w = 1,
        .index_grid_h = 1,
        .cell_size = 1,
    };
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, gfx_mode_enter(&request)->layout);

    probe = probe_new();
    TEST_ASSERT_FALSE(gfx_band_run(draw, overlay));
    TEST_ASSERT_EQUAL_INT(0, probe->draw_calls);
    TEST_ASSERT_EQUAL_INT(0, probe->overlay_calls);

    probe_free(probe);
    gfx_mode_exit();
}

void
run_gfx_band_run_suite(void) {
    RUN_TEST(test_dirty_bands_are_drawn_then_overlaid_and_submitted);
    RUN_TEST(test_full_dirty_frame_covers_every_row_once_in_order);
    RUN_TEST(test_draw_null_never_enters_the_loop);
    RUN_TEST(test_an_indexed_picture_never_enters_the_transient_loop);
}

SUITE_REGISTER(run_gfx_band_run_suite);

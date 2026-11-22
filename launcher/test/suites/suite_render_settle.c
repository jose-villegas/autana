#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565_layout.h"
#include "raster_rig.h"
#include "render/context/render_context.h"
#include "render/settle/render_settle.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "unity.h"
#ifndef ESP_PLATFORM
#include "clock_fake.h"
#endif

#define W                  96
#define H                  64
#define CARD               1
#define HOLD               3
#define ROW_COST           100
#define FAR_EYE_Z          -740.0F
#define SLIDE              6.0F
#define TEX_LOG2           3
#define TEX_LEVELS         2
#define TEX_SIDE           (1 << TEX_LOG2)
#define TEX_BYTES          (TEX_SIDE * TEX_SIDE / 2)
#define TEX_TILES          16
#define TEX_CHECKER_EVEN   0x01
#define TEX_CHECKER_ODD    0x10
#define TEX_UV_SPAN        ((TEX_SIDE << R3D_TEXTURE_UV_SHIFT) * TEX_TILES)
#define TEX_BOTH_TRIANGLES (1U | (1U << R3D_TEXTURE_UVS_CODE_BITS))
#define CONTEXTS           3
#define LIVE               2
#define BAND_HEIGHT        (H / RENDER_SETTLE_BANDS)

static void
test_middle_out_and_one_band_despite_a_tiny_budget(void) {
    render_settle_t s = {0};
    const render_settle_settings_t settings = {1, HOLD};
    for (int i = 0; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, render_settle_next(&s, H, settings).end);
        render_settle_age(&s, H, (render_rows_t){0, 0});
    }
    for (int i = 0; i < RENDER_SETTLE_BANDS; i++) {
        const int band = (RENDER_SETTLE_BANDS / 2) + (i % 2 == 0 ? i / 2 : -(i + 1) / 2);
        const render_rows_t rows = render_settle_next(&s, H, settings);
        TEST_ASSERT_EQUAL_INT(band * H / RENDER_SETTLE_BANDS, rows.first);
        TEST_ASSERT_EQUAL_INT((band + 1) * H / RENDER_SETTLE_BANDS, rows.end);
        render_settle_done(&s, H, rows, ROW_COST * (rows.end - rows.first));
    }
    TEST_ASSERT_EQUAL_INT(0, render_settle_next(&s, H, settings).end);
}

static void
test_quiet_saturates_and_drawn_rows_reset_intersected_bands(void) {
    render_settle_t s = {0};
    for (int i = 0; i < RENDER_SETTLE_HOLD_MAX + HOLD; i++) {
        render_settle_age(&s, H, (render_rows_t){0, 0});
    }
    render_settle_done(&s, H, (render_rows_t){0, H}, H * ROW_COST);
    render_settle_age(&s, H, (render_rows_t){1, 3});
    for (int i = 0; i < RENDER_SETTLE_BANDS; i++) {
        TEST_ASSERT_EQUAL_INT(i < 2 ? 0 : RENDER_SETTLE_HOLD_MAX, s.quiet[i]);
        TEST_ASSERT_EQUAL(i >= 2, s.refined[i]);
    }
    const int64_t learned_cost = s.row_us;
    render_settle_forget(&s);
    const render_settle_t empty = {0};
    TEST_ASSERT_EQUAL_MEMORY(empty.quiet, s.quiet, sizeof s.quiet);
    TEST_ASSERT_EQUAL_MEMORY(empty.refined, s.refined, sizeof s.refined);
    TEST_ASSERT_EQUAL_INT64(learned_cost, s.row_us);
}

static void
test_cost_learns_and_a_run_stops_at_a_band_that_is_not_quiet(void) {
    render_settle_t s = {0};
    for (int i = 0; i < HOLD; i++) {
        render_settle_age(&s, H, (render_rows_t){0, 0});
    }
    const render_rows_t first = {0, BAND_HEIGHT};
    render_settle_done(&s, H, first, BAND_HEIGHT * ROW_COST);
    const int64_t initial_cost = s.row_us;
    render_settle_done(&s, H, first, BAND_HEIGHT * (ROW_COST + 1));
    TEST_ASSERT_GREATER_THAN_INT64(initial_cost, s.row_us);
    render_settle_age(&s, H, (render_rows_t){H / 2 + BAND_HEIGHT, H / 2 + 2 * BAND_HEIGHT});
    const render_rows_t rows = render_settle_next(&s, H, (render_settle_settings_t){8 * (ROW_COST + 1), HOLD});
    TEST_ASSERT_EQUAL_INT(8, rows.end - rows.first);
    TEST_ASSERT_EQUAL_INT(H / 2 + BAND_HEIGHT, rows.end);
}

static void
test_band_edges_and_growth_below_the_first_due_band(void) {
    render_settle_t s = {0};
    for (int i = 0; i < HOLD; i++) {
        render_settle_age(&s, H, (render_rows_t){0, 0});
    }
    const int middle = H / 2;
    const render_rows_t above = {middle - BAND_HEIGHT, middle};
    render_settle_age(&s, H, above);
    TEST_ASSERT_EQUAL_INT(0, s.quiet[RENDER_SETTLE_BANDS / 2 - 1]);
    TEST_ASSERT_EQUAL_INT(HOLD + 1, s.quiet[RENDER_SETTLE_BANDS / 2]);

    render_settle_done(&s, H, (render_rows_t){middle, middle + BAND_HEIGHT - 1}, (BAND_HEIGHT - 1) * ROW_COST);
    TEST_ASSERT_FALSE(s.refined[RENDER_SETTLE_BANDS / 2]);

    const int two_bands = 2 * BAND_HEIGHT;
    const render_settle_settings_t short_budget = {(two_bands - 1) * ROW_COST, HOLD};
    const render_rows_t short_run = render_settle_next(&s, H, short_budget);
    TEST_ASSERT_EQUAL_INT(middle, short_run.first);
    TEST_ASSERT_EQUAL_INT(middle + BAND_HEIGHT, short_run.end);

    const render_rows_t grown = render_settle_next(&s, H, (render_settle_settings_t){two_bands * ROW_COST, HOLD});
    TEST_ASSERT_EQUAL_INT(middle, grown.first);
    TEST_ASSERT_EQUAL_INT(middle + two_bands, grown.end);
}

static void
test_short_and_uneven_heights_cover_each_row(void) {
    const int heights[] = {1, 17, 65};
    for (size_t k = 0; k < sizeof heights / sizeof heights[0]; k++) {
        render_settle_t s = {0};
        render_settle_age(&s, heights[k], (render_rows_t){0, 0});
        int total = 0;
        for (int i = 0; i < RENDER_SETTLE_BANDS; i++) {
            const render_rows_t rows = render_settle_next(&s, heights[k], (render_settle_settings_t){1, 1});
            total += rows.end - rows.first;
            render_settle_done(&s, heights[k], rows, ROW_COST);
        }
        TEST_ASSERT_EQUAL_INT(heights[k], total);
    }
}

typedef struct {
    render_context_t c[CONTEXTS];
    render_settings_t settings;
    render_view_t view;
    uint16_t picture[CONTEXTS][W * H];
    r3d_texture_t texture;
    uint8_t texels[TEX_BYTES];
    r3d_mesh_texturing_t texturing;
} fixture_t;

static fixture_t* f;
static const box3f_t change_at_eye = {{0, 0, FAR_EYE_Z}, {0, 0, FAR_EYE_Z}};

/* Keep view construction off the stack while frame() draws. */
static __attribute__((noinline)) void
set_view(float eye_x) {
    fixture_camera_t camera = raster_rig_camera;
    camera.eye.z = FAR_EYE_Z;
    camera.eye.x = eye_x;
    f->view = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
}

static void
cleanup(void) {
#ifndef ESP_PLATFORM
    clock_fake_reset();
#endif
    if (f != NULL) {
        for (int k = 0; k < CONTEXTS; k++) {
            render_context_release(&f->c[k]);
        }
        free(f);
        f = NULL;
    }
    raster_rig_release();
}

static raster_rig_t*
fixture(int budget) {
    static const uint8_t gradient[4][3] = {{32, 80, 96}, {128, 112, 48}, {224, 160, 144}, {48, 96, 80}};
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 1U << CARD, W, H, 0);
    suite_set_test_cleanup(cleanup);
#ifndef ESP_PLATFORM
    clock_fake_set(0);
#endif
    f = calloc(1, sizeof(*f));
    TEST_ASSERT_NOT_NULL(f);
    set_view(raster_rig_camera.eye.x);
    static const int16_t uvs[4][2] = {{0, 0}, {TEX_UV_SPAN, 0}, {TEX_UV_SPAN, TEX_UV_SPAN}, {0, TEX_UV_SPAN}};
    static const uint8_t material[1][R3D_TEXTURE_CHARTS] = {{0, R3D_TEXTURE_NONE}};
    static const int32_t shift[1] = {0};
    static const uint8_t codes[1] = {TEX_BOTH_TRIANGLES};
    for (int i = 0; i < TEX_BYTES; i++) {
        f->texels[i] = (uint8_t)((i % 2 == 0) ? TEX_CHECKER_EVEN : TEX_CHECKER_ODD);
    }
    f->texture = (r3d_texture_t){.format = R3D_TEXTURE_INDEX4, .size_log2 = TEX_LOG2, .levels = TEX_LEVELS};
    for (int i = 0; i < TEX_LEVELS; i++) {
        f->texture.texels[i] = f->texels;
    }
    for (int light = 0; light < R3D_TEXTURE_LIGHT_LEVELS; light++) {
        for (int color = 0; color < R3D_TEXTURE_COLOURS; color++) {
            f->texture.colormap[light][color] = color % 2 == 0 ? RGB565_RED_MASK : RGB565_GREEN_MASK;
        }
    }
    f->texturing = (r3d_mesh_texturing_t){uvs, material, shift, codes, &f->texture, 1, 4, 1, 2, 4};
    r->quad[0].mesh.texturing = &f->texturing;
    r->quad[CARD].mesh.colors = gradient;
    r->model[CARD] = mat4f_identity();
    f->settings = RENDER_SETTINGS_DEFAULT;
    f->settings.settle = (render_settle_settings_t){budget, HOLD};
    f->settings.small_fill = SMALL_FILL_FAST_DITHER;
    f->settings.texture.far_level = 1;
    f->settings.lod.pick_every = 1;
    for (int k = 0; k < CONTEXTS; k++) {
        f->c[k] = RENDER_CONTEXT_DEFAULT;
        render_context_set_scale(&f->c[k], 100);
        render_settings_t settings = k == 1 ? render_settle_quality(&f->settings) : f->settings;
        if (k > 0) {
            settings.partial_redraw = k == LIVE;
            settings.settle.frame_us = 0;
        }
        render_context_set_settings(&f->c[k], &settings);
    }
    return r;
}

static render_context_frame_t
frame(raster_rig_t* r, const box3f_t* changed, int count) {
    render_context_set_changed(&f->c[0], changed, count);
    render_context_set_changed(&f->c[LIVE], changed, count);
    for (int k = 0; k < CONTEXTS; k++) {
        TEST_ASSERT_TRUE(render_context_draw(&f->c[k], r->instance, RASTER_RIG_QUADS, &f->view, 0));
        (void)render_context_compose(&f->c[k], f->picture[k], NULL, NULL);
    }
    return render_context_frame(&f->c[0]);
}

static void
assert_picture(int oracle) {
    TEST_ASSERT_EQUAL_MEMORY(f->picture[oracle], f->picture[0], sizeof f->picture[0]);
}

static raster_rig_t*
settle_held(int budget) {
    raster_rig_t* r = fixture(budget);
    (void)frame(r, NULL, -1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(f->picture[1], f->picture[LIVE], sizeof f->picture[0]));
    for (int i = 1; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
        assert_picture(LIVE);
    }
    const int rounds = budget == 1 ? RENDER_SETTLE_BANDS : 2;
    for (int i = 0; i < rounds; i++) {
        const render_context_frame_t out = frame(r, NULL, 0);
        TEST_ASSERT_FALSE(out.unchanged);
        const int band = RENDER_SETTLE_BANDS / 2 + (i % 2 == 0 ? i / 2 : -(i + 1) / 2);
        const int first = band * H / RENDER_SETTLE_BANDS;
        const int end = (band + 1) * H / RENDER_SETTLE_BANDS;
        TEST_ASSERT_EQUAL_INT(budget != 1 && i == 1 ? H : end - first, out.settled_rows);
        if (budget == 1 || i == 0) {
            TEST_ASSERT_EQUAL_INT(first, f->c[0].raster.first_row);
            TEST_ASSERT_EQUAL_INT(end, f->c[0].raster.end_row);
        }
        TEST_ASSERT_EQUAL_MEMORY(&f->c[0].drawn_settings, &f->c[0].partial.settings, sizeof f->settings);
    }
    assert_picture(1);
    const render_context_frame_t after = frame(r, NULL, 0);
    TEST_ASSERT_TRUE(after.unchanged);
    TEST_ASSERT_EQUAL_INT(0, after.settled_rows);
    assert_picture(1);
    return r;
}

static void
test_a_held_picture_samples_one_band_before_spending_a_large_budget(void) {
    settle_held(RENDER_SETTLE_FRAME_US_MAX);
}

static void
test_a_tiny_budget_refines_one_band_each_frame(void) {
    settle_held(1);
}

static box3f_t
slide(raster_rig_t* r, int i) {
    const box3f_t was = r3d_lit_mesh_world_bounds(&r->quad[CARD].mesh, &r->model[CARD]);
    transformf_t pose = transformf_identity();
    transformf_set_position(&pose, (vec3f_t){i % 2 == 0 ? SLIDE : -SLIDE, 0, 0});
    r->model[CARD] = transformf_compute_matrix(&pose);
    return box3f_union(was, r3d_lit_mesh_world_bounds(&r->quad[CARD].mesh, &r->model[CARD]));
}

static void
test_changed_rows_stay_live_and_partial_redraw_survives_settling(void) {
    raster_rig_t* r = settle_held(RENDER_SETTLE_FRAME_US_MAX);
    for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
        const box3f_t changed = slide(r, i);
        const render_context_frame_t out = frame(r, &changed, 1);
        const render_context_frame_t live = render_context_frame(&f->c[LIVE]);
        TEST_ASSERT_LESS_THAN_INT(H, live.rows_end - live.rows_first);
        TEST_ASSERT_EQUAL_INT(live.rows_first, out.rows_first);
        TEST_ASSERT_EQUAL_INT(live.rows_end, out.rows_end);
        const raster_t* raster = &f->c[LIVE].raster;
        int live_bands = 0;
        int refined_bands = 0;
        for (int b = 0; b < RENDER_SETTLE_BANDS; b++) {
            const int first = b * H / RENDER_SETTLE_BANDS;
            const int end = (b + 1) * H / RENDER_SETTLE_BANDS;
            const bool draws_live = first < raster->end_row && end > raster->first_row;
            live_bands += draws_live;
            refined_bands += !draws_live;
            TEST_ASSERT_EQUAL(!draws_live, f->c[0].settle.refined[b]);
            TEST_ASSERT_EQUAL_MEMORY(&f->picture[draws_live ? LIVE : 1][first * W], &f->picture[0][first * W],
                                     (end - first) * W * sizeof(uint16_t));
        }
        TEST_ASSERT_GREATER_THAN_INT(0, live_bands);
        TEST_ASSERT_GREATER_THAN_INT(0, refined_bands);
    }
}

static void
test_full_live_redraw_refines_no_band(void) {
    raster_rig_t* r = fixture(1);
    (void)frame(r, NULL, -1);
    for (int i = 1; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
    }
    TEST_ASSERT_EQUAL_INT(BAND_HEIGHT, frame(r, NULL, 0).settled_rows);
    const render_context_frame_t out = frame(r, &change_at_eye, 1);
    TEST_ASSERT_EQUAL_INT(0, out.rows_first);
    TEST_ASSERT_EQUAL_INT(H, out.rows_end);
    TEST_ASSERT_EQUAL_INT(0, out.settled_rows);
    assert_picture(LIVE);
}

static void
test_a_moved_camera_cancels_and_restarts_the_hold(void) {
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    (void)frame(r, NULL, -1);
    for (int i = 0; i < HOLD + 1; i++) {
        (void)frame(r, NULL, 0);
    }
    set_view(SLIDE);
    for (int i = 0; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
        assert_picture(LIVE);
    }
    TEST_ASSERT_EQUAL_INT(H, frame(r, NULL, 0).settled_rows);
    assert_picture(1);
}

static void
test_disabled_and_already_refined_settings_never_settle(void) {
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    (void)frame(r, NULL, -1);
    for (int i = 0; i < HOLD + 1; i++) {
        (void)frame(r, NULL, 0);
    }
    assert_picture(1);
    f->settings.settle.frame_us = 0;
    render_context_set_settings(&f->c[0], &f->settings);
    for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
    }
    assert_picture(LIVE);
    f->settings.settle.frame_us = RENDER_SETTLE_FRAME_US_MAX;
    render_context_set_settings(&f->c[0], &f->settings);
    for (int i = 0; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
    }
    TEST_ASSERT_EQUAL_INT(H, frame(r, NULL, 0).settled_rows);
    assert_picture(1);
    f->settings = render_settle_quality(&f->settings);
    render_context_set_settings(&f->c[0], &f->settings);
    for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
    }
    assert_picture(1);
}

#ifndef ESP_PLATFORM
static void
test_dynamic_resolution_settles_at_the_top_without_pricing_refinement(void) {
    static const resolution_step_t steps[] = {{W, H}, {W / 2, H / 2}};
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    const resolution_config_t config = resolution_config(steps, 2, 2, RENDER_SETTLE_FRAME_US_MAX);
    render_context_t* c = &f->c[0];
    render_context_set_dynamic_resolution(c, &config, NULL, 1);
    for (int i = 0; i < HOLD + 1; i++) {
        (void)frame(r, NULL, 0);
    }
    const int32_t live_us = 1;
    const int32_t over_us = config.budget_us + 1;
    /* Presentation, live draw, settle draw, then compose. */
    const int64_t settle_only[] = {0, 0, live_us, live_us, live_us + over_us, live_us + over_us, live_us + over_us};
    clock_fake_script(settle_only, sizeof settle_only / sizeof settle_only[0]);
    const render_context_frame_t refined = frame(r, NULL, 0);
    TEST_ASSERT_TRUE(c->still_judged);
    TEST_ASSERT_EQUAL_INT(0, refined.step);
    TEST_ASSERT_GREATER_THAN_INT(0, refined.settled_rows);
    TEST_ASSERT_EQUAL_INT(over_us, refined.settle_us);
    TEST_ASSERT_EQUAL_INT(live_us, refined.draw_us);
    TEST_ASSERT_FALSE(c->still_over);

    const int64_t live_only[] = {0, 0, over_us, over_us, over_us};
    clock_fake_script(live_only, sizeof live_only / sizeof live_only[0]);
    const render_context_frame_t over = frame(r, &change_at_eye, 1);
    TEST_ASSERT_TRUE(c->still_judged);
    TEST_ASSERT_EQUAL_INT(over_us, over.draw_us);
    TEST_ASSERT_EQUAL_INT(0, over.settle_us);
    TEST_ASSERT_TRUE(c->still_over);
}
#endif

void
run_render_settle_suite(void) {
    RUN_TEST(test_middle_out_and_one_band_despite_a_tiny_budget);
    RUN_TEST(test_quiet_saturates_and_drawn_rows_reset_intersected_bands);
    RUN_TEST(test_cost_learns_and_a_run_stops_at_a_band_that_is_not_quiet);
    RUN_TEST(test_band_edges_and_growth_below_the_first_due_band);
    RUN_TEST(test_short_and_uneven_heights_cover_each_row);
    RUN_TEST(test_a_held_picture_samples_one_band_before_spending_a_large_budget);
    RUN_TEST(test_a_tiny_budget_refines_one_band_each_frame);
    RUN_TEST(test_changed_rows_stay_live_and_partial_redraw_survives_settling);
    RUN_TEST(test_full_live_redraw_refines_no_band);
    RUN_TEST(test_a_moved_camera_cancels_and_restarts_the_hold);
    RUN_TEST(test_disabled_and_already_refined_settings_never_settle);
#ifndef ESP_PLATFORM
    RUN_TEST(test_dynamic_resolution_settles_at_the_top_without_pricing_refinement);
#endif
}

SUITE_REGISTER(run_render_settle_suite);

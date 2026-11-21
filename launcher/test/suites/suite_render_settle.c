#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565_layout.h"
#include "raster_rig.h"
#include "render/context/render_context.h"
#include "render/settle/render_settle.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "unity.h"

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
    render_settle_forget(&s);
    const render_settle_t empty = {0};
    TEST_ASSERT_EQUAL_MEMORY(&empty, &s, sizeof s);
}

static void
test_cost_learns_and_a_run_stops_at_a_band_that_is_not_quiet(void) {
    render_settle_t s = {0};
    for (int i = 0; i < HOLD; i++) {
        render_settle_age(&s, H, (render_rows_t){0, 0});
    }
    render_settle_done(&s, H, (render_rows_t){0, 2}, 2 * ROW_COST);
    TEST_ASSERT_EQUAL_INT(ROW_COST, s.row_us);
    render_settle_done(&s, H, (render_rows_t){0, 2}, 4 * ROW_COST);
    TEST_ASSERT_EQUAL_INT(ROW_COST + ROW_COST / RENDER_SETTLE_COST_WEIGHT_DIVISOR, s.row_us);
    render_settle_age(&s, H, (render_rows_t){H / 2 + 2, H / 2 + 4});
    const render_rows_t rows = render_settle_next(&s, H, (render_settle_settings_t){8 * s.row_us, HOLD});
    TEST_ASSERT_EQUAL_INT(8, rows.end - rows.first);
    TEST_ASSERT_EQUAL_INT(H / 2 + 2, rows.end);
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
    uint16_t picture[CONTEXTS][W * H];
    r3d_texture_t texture;
    uint8_t texels[TEX_BYTES];
    r3d_mesh_texturing_t texturing;
} fixture_t;

static fixture_t* f;

static void
cleanup(void) {
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
    f = calloc(1, sizeof(*f));
    TEST_ASSERT_NOT_NULL(f);
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
            settings.partial_redraw = false;
            settings.settle.frame_us = 0;
        }
        render_context_set_settings(&f->c[k], &settings);
    }
    return r;
}

static render_view_t
view(void) {
    fixture_camera_t camera = raster_rig_camera;
    camera.eye.z = FAR_EYE_Z;
    return render_view_fixture_at(&camera, (viewport_t){W, H, 0});
}

static render_context_frame_t
frame(raster_rig_t* r, const render_view_t* v, const box3f_t* changed, int count) {
    render_context_set_changed(&f->c[0], changed, count);
    for (int k = 0; k < CONTEXTS; k++) {
        TEST_ASSERT_TRUE(render_context_draw(&f->c[k], r->instance, RASTER_RIG_QUADS, v, 0));
        (void)render_context_compose(&f->c[k], f->picture[k], NULL, NULL);
    }
    return render_context_frame(&f->c[0]);
}

static void
assert_picture(int oracle) {
    TEST_ASSERT_EQUAL_MEMORY(f->picture[oracle], f->picture[0], sizeof f->picture[0]);
}

static void
settle_held(int budget) {
    raster_rig_t* r = fixture(budget);
    const render_view_t v = view();
    (void)frame(r, &v, NULL, -1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(f->picture[1], f->picture[LIVE], sizeof f->picture[0]));
    for (int i = 1; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, &v, NULL, 0).settled_rows);
        assert_picture(LIVE);
    }
    const int rounds = budget == 1 ? RENDER_SETTLE_BANDS : 1;
    int rows = 0;
    for (int i = 0; i < rounds; i++) {
        const render_context_frame_t out = frame(r, &v, NULL, 0);
        TEST_ASSERT_FALSE(out.unchanged);
        rows += out.settled_rows;
        TEST_ASSERT_EQUAL_MEMORY(&f->c[0].drawn_settings, &f->c[0].partial.settings, sizeof f->settings);
    }
    TEST_ASSERT_EQUAL_INT(H, rows);
    assert_picture(1);
    TEST_ASSERT_TRUE(frame(r, &v, NULL, 0).unchanged);
}

static void
test_a_held_picture_refines_in_one_large_budget_frame(void) {
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
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    const render_view_t v = view();
    (void)frame(r, &v, NULL, -1);
    for (int i = 0; i < HOLD; i++) {
        (void)frame(r, &v, NULL, 0);
    }
    assert_picture(1);
    for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
        const box3f_t changed = slide(r, i);
        const render_context_frame_t out = frame(r, &v, &changed, 1);
        TEST_ASSERT_LESS_THAN_INT(H, out.rows_end - out.rows_first);
        render_settle_t* s = &f->c[0].settle;
        for (int b = 0; b < RENDER_SETTLE_BANDS; b++) {
            for (int y = b * H / RENDER_SETTLE_BANDS; y < (b + 1) * H / RENDER_SETTLE_BANDS; y++) {
                TEST_ASSERT_EQUAL_MEMORY(&f->picture[s->refined[b] ? 1 : LIVE][y * W], &f->picture[0][y * W],
                                         W * sizeof(uint16_t));
            }
        }
    }
}

static void
test_a_moved_camera_cancels_and_restarts_the_hold(void) {
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    render_view_t v = view();
    (void)frame(r, &v, NULL, -1);
    for (int i = 0; i < HOLD; i++) {
        (void)frame(r, &v, NULL, 0);
    }
    fixture_camera_t camera = raster_rig_camera;
    camera.eye.z = FAR_EYE_Z;
    camera.eye.x = SLIDE;
    v = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
    for (int i = 0; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, &v, NULL, 0).settled_rows);
        assert_picture(LIVE);
    }
    TEST_ASSERT_EQUAL_INT(H, frame(r, &v, NULL, 0).settled_rows);
    assert_picture(1);
}

static void
test_disabled_and_already_refined_settings_never_settle(void) {
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    const render_view_t v = view();
    (void)frame(r, &v, NULL, -1);
    for (int i = 0; i < HOLD; i++) {
        (void)frame(r, &v, NULL, 0);
    }
    assert_picture(1);
    f->settings.settle.frame_us = 0;
    render_context_set_settings(&f->c[0], &f->settings);
    for (int k = 0; k < 2; k++) {
        if (k != 0) {
            f->settings = render_settle_quality(&f->settings);
            f->settings.settle.frame_us = RENDER_SETTLE_FRAME_US_MAX;
            render_context_set_settings(&f->c[0], &f->settings);
        }
        for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
            const render_context_frame_t out = frame(r, &v, NULL, 0);
            TEST_ASSERT_EQUAL_INT(0, out.settle_us);
            TEST_ASSERT_EQUAL_INT(0, out.settled_rows);
        }
        TEST_ASSERT_FALSE(k != 0 && render_settle_refines(&f->settings));
        assert_picture(k == 0 ? LIVE : 1);
    }
}

static void
test_dynamic_resolution_settles_at_the_top_without_pricing_refinement(void) {
    static const resolution_step_t steps[] = {{W, H}, {W / 2, H / 2}};
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX);
    const render_view_t v = view();
    const resolution_config_t config = resolution_config(steps, 2, 2, RENDER_SETTLE_FRAME_US_MAX);
    render_context_set_dynamic_resolution(&f->c[0], &config, NULL, 1);
    for (int i = 0; i < HOLD + 2; i++) {
        (void)frame(r, &v, NULL, 0);
    }
    TEST_ASSERT_EQUAL_INT(0, f->c[0].frame.step);
    TEST_ASSERT_FALSE(f->c[0].still_over);
    TEST_ASSERT_TRUE(f->c[0].settle.refined[RENDER_SETTLE_BANDS / 2]);
    assert_picture(1);
    render_context_t* c = &f->c[0];
    render_context_set_changed(c, NULL, 0);
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &v, 0));
    c->frame.settle_us = config.budget_us + 1;
    (void)render_context_compose(c, f->picture[0], NULL, NULL);
    TEST_ASSERT_FALSE(c->still_over);
}

void
run_render_settle_suite(void) {
    RUN_TEST(test_middle_out_and_one_band_despite_a_tiny_budget);
    RUN_TEST(test_quiet_saturates_and_drawn_rows_reset_intersected_bands);
    RUN_TEST(test_cost_learns_and_a_run_stops_at_a_band_that_is_not_quiet);
    RUN_TEST(test_short_and_uneven_heights_cover_each_row);
    RUN_TEST(test_a_held_picture_refines_in_one_large_budget_frame);
    RUN_TEST(test_a_tiny_budget_refines_one_band_each_frame);
    RUN_TEST(test_changed_rows_stay_live_and_partial_redraw_survives_settling);
    RUN_TEST(test_a_moved_camera_cancels_and_restarts_the_hold);
    RUN_TEST(test_disabled_and_already_refined_settings_never_settle);
    RUN_TEST(test_dynamic_resolution_settles_at_the_top_without_pricing_refinement);
}

SUITE_REGISTER(run_render_settle_suite);

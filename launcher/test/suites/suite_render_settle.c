#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565_layout.h"
#include "raster_rig.h"
#include "render/context/render_context.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_span.h"
#include "render/settle/render_settle.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "unity.h"
#ifndef ESP_PLATFORM
#include "clock_fake.h"
#endif

#define W                     96
#define H                     64
#define CARD                  1
#define HOLD                  3
#define ROW_COST              100
#define FAR_EYE_Z             -740.0F
#define SLIDE                 6.0F
#define TEX_LOG2              3
#define TEX_LEVELS            2
#define TEX_SIDE              (1 << TEX_LOG2)
#define TEX_BYTES             (TEX_SIDE * TEX_SIDE / 2)
#define TEX_TILES             16
#define TEX_CHECKER_EVEN      0x01
#define TEX_CHECKER_ODD       0x10
#define TEX_UV_SPAN           ((TEX_SIDE << R3D_TEXTURE_UV_SHIFT) * TEX_TILES)
#define TEX_BOTH_TRIANGLES    (1U | (1U << R3D_TEXTURE_UVS_CODE_BITS))
#define CONTEXTS              3
#define LIVE                  2
#define BAND_HEIGHT           (H / RENDER_SETTLE_BANDS)
#define CARD_GROUPS           1
#define CARD_FINE_TRIANGLES   2
#define CARD_COARSE_TRIANGLES 1
#define CARD_GROUP_RADIUS     57
#define CARD_Z                -100
#define CARD_FINE_Z           -300
#define CARD_CORNERS          4
#define CARD_FINE_CORNER      3
#define SPREAD_PICKS          2
#define FOLLOWUP_FRAMES       3
#define CLOSE_EYE_Z           -350.0F
#define MOVE_TOLERANCE        0.2F
#define CHANGE_ABOVE_CARD     17.0F
#define CARD_ERROR            1

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
    int16_t positions[CARD_CORNERS][3];
    r3d_lit_cluster_t clusters[2];
    r3d_lit_node_t nodes[3];
    r3d_instance_t instances[CONTEXTS][RASTER_RIG_QUADS];
    uint32_t lod[CONTEXTS][R3D_LOD_STATE_WORDS(CARD_GROUPS)];
} fixture_t;

static fixture_t* f;
static const box3f_t change_at_eye = {{0, 0, FAR_EYE_Z}, {0, 0, FAR_EYE_Z}};

/* Keep view construction off the stack while frame() draws. */
static __attribute__((noinline)) void
set_view(float eye_x, float eye_z) {
    fixture_camera_t camera = raster_rig_camera;
    camera.eye.z = eye_z;
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

static void
level_card(r3d_quad_t* card, bool coarse_near) {
    static const uint16_t triangles[][3] = {{0, 1, 2}, {0, 2, 3}, {0, 1, 2}};
    static const r3d_lit_cluster_lod_t levels[] = {{R3D_LOD_NONE, 0}, {0, R3D_LOD_NONE}};
    static const r3d_lit_lod_group_t groups[] = {
        {{{0, 0, CARD_Z}, CARD_GROUP_RADIUS}, CARD_ERROR, CARD_FINE_TRIANGLES - CARD_COARSE_TRIANGLES},
    };
    f->clusters[0] = card->cluster;
    f->clusters[1] = card->cluster;
    f->clusters[1].triangle_first = CARD_FINE_TRIANGLES;
    f->clusters[1].triangle_count = CARD_COARSE_TRIANGLES;
    f->nodes[0] = (r3d_lit_node_t){card->node.bounds, 1, 2, false};
    f->nodes[1] = card->node;
    f->nodes[2] = (r3d_lit_node_t){card->node.bounds, 1, 1, true};
    memcpy(f->positions, card->mesh.positions, sizeof f->positions);
    if (coarse_near) {
        f->clusters[1].bounds.lo.z = CARD_FINE_Z;
        f->nodes[2].bounds.lo.z = CARD_FINE_Z;
    } else {
        f->positions[CARD_FINE_CORNER][2] = CARD_FINE_Z;
        f->clusters[0].bounds.lo.z = CARD_FINE_Z;
        f->nodes[1].bounds.lo.z = CARD_FINE_Z;
    }
    f->nodes[0].bounds.lo.z = CARD_FINE_Z;
    card->mesh.positions = (const int16_t (*)[3])f->positions;
    card->mesh.triangles = triangles;
    card->mesh.triangle_count = CARD_FINE_TRIANGLES + CARD_COARSE_TRIANGLES;
    card->mesh.clusters = f->clusters;
    card->mesh.cluster_count = sizeof levels / sizeof levels[0];
    card->mesh.nodes = f->nodes;
    card->mesh.node_count = sizeof f->nodes / sizeof f->nodes[0];
    card->mesh.cluster_lod = levels;
    card->mesh.lod_groups = groups;
    card->mesh.lod_group_count = CARD_GROUPS;
    card->mesh.lod_error_least = CARD_ERROR;
    r3d_lit_mesh_find_levels(&card->mesh);
}

static void
texture_wall(r3d_lit_mesh_t* mesh) {
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
    mesh->texturing = &f->texturing;
}

static raster_rig_t*
fixture(int budget, bool levels, bool coarse_near) {
    static const uint8_t gradient[4][3] = {{32, 80, 96}, {128, 112, 48}, {224, 160, 144}, {48, 96, 80}};
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 1U << CARD, W, H, 0);
    suite_set_test_cleanup(cleanup);
#ifndef ESP_PLATFORM
    clock_fake_set(0);
#endif
    f = calloc(1, sizeof(*f));
    TEST_ASSERT_NOT_NULL(f);
    set_view(raster_rig_camera.eye.x, FAR_EYE_Z);
    texture_wall(&r->quad[0].mesh);
    r->quad[CARD].mesh.colors = gradient;
    r->model[CARD] = mat4f_identity();
    f->settings = RENDER_SETTINGS_DEFAULT;
    f->settings.settle = (render_settle_settings_t){budget, HOLD};
    f->settings.small_fill = SMALL_FILL_FAST_DITHER;
    f->settings.texture.far_level = 1;
    if (levels) {
        level_card(&r->quad[CARD], coarse_near);
        const render_settings_t quality = render_settle_quality(&f->settings);
        f->settings.texture = quality.texture;
        f->settings.small_fill = quality.small_fill;
        f->settings.lod.blend = RENDER_LOD_BLEND_ON;
        f->settings.lod.triangle_budget = r->quad[0].mesh.triangle_count + CARD_FINE_TRIANGLES;
    } else {
        f->settings.lod.pick_every = 1;
    }
    for (int k = 0; k < CONTEXTS; k++) {
        f->c[k] = RENDER_CONTEXT_DEFAULT;
        render_context_set_scale(&f->c[k], 100);
        render_settings_t settings = k == 1 ? render_settle_quality(&f->settings) : f->settings;
        if (levels && k == 1) {
            settings.lod.tolerance_px = 0.0F;
            settings.lod.triangle_budget = 0;
            settings.lod.blend = RENDER_LOD_BLEND_SWITCH;
        }
        if (levels) {
            memcpy(f->instances[k], r->instance, sizeof r->instance);
            f->instances[k][CARD].lod_coarse = f->lod[k];
        }
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
        const r3d_instance_t* instances = r->quad[CARD].mesh.lod_group_count > 0 ? f->instances[k] : r->instance;
        TEST_ASSERT_TRUE(render_context_draw(&f->c[k], instances, RASTER_RIG_QUADS, &f->view, 0));
        (void)render_context_compose(&f->c[k], f->picture[k], NULL, NULL);
    }
    return render_context_frame(&f->c[0]);
}

static void
assert_picture(int oracle) {
    TEST_ASSERT_EQUAL_MEMORY(f->picture[oracle], f->picture[0], sizeof f->picture[0]);
}

static bool
assert_band_depth(int first, int end, float live_near) {
    raster_t* oracle = &f->c[1].raster;
    *raster_carve(oracle).depth_near = live_near;
    raster_draw_culled(oracle, &f->view);
    const uint16_t* expected = raster_depth(oracle);
    const int offset = first * W;
    const size_t pixels = (size_t)(end - first) * W;
    TEST_ASSERT_EQUAL_MEMORY(expected + offset, raster_depth(&f->c[0].raster) + offset, pixels * sizeof(*expected));
    bool clamped = false;
    for (int j = offset; j < end * W; j++) {
        clamped |= expected[j] == R3D_DEPTH_NEAREST;
    }
    return clamped;
}

static raster_rig_t*
settle_held(int budget, bool levels, bool coarse_near) {
    raster_rig_t* r = fixture(budget, levels, coarse_near);
    (void)frame(r, NULL, -1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(f->picture[1], f->picture[LIVE], sizeof f->picture[0]));
    for (int i = 1; i < HOLD; i++) {
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
        assert_picture(LIVE);
    }
    const float live_near = raster_depth_near(&f->c[LIVE].raster);
    int card_bands = 0;
    bool clamped = false;
    if (levels) {
        TEST_ASSERT_TRUE(r3d_lod_any(f->lod[LIVE], CARD_GROUPS));
        const float finest_near = raster_depth_near(&f->c[1].raster);
        TEST_ASSERT_TRUE(coarse_near ? finest_near > live_near : finest_near < live_near);
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
        if (budget == 1) {
            TEST_ASSERT_EQUAL_MEMORY(&f->picture[1][first * W], &f->picture[0][first * W],
                                     (end - first) * W * sizeof(uint16_t));
        }
        TEST_ASSERT_EQUAL_FLOAT(live_near, raster_depth_near(&f->c[0].raster));
        if (levels && budget == 1) {
            const size_t bytes = (end - first) * W * sizeof(uint16_t);
            const int offset = first * W;
            card_bands += memcmp(f->picture[1] + offset, f->picture[LIVE] + offset, bytes) != 0;
            clamped |= assert_band_depth(first, end, live_near);
        }
        TEST_ASSERT_EQUAL_MEMORY(&f->c[0].drawn_settings, &f->c[0].partial.settings, sizeof f->settings);
    }
    if (levels && budget == 1) {
        TEST_ASSERT_GREATER_THAN_INT(1, card_bands);
        TEST_ASSERT_EQUAL(!coarse_near, clamped);
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
    settle_held(RENDER_SETTLE_FRAME_US_MAX, false, false);
}

static void
test_a_tiny_budget_refines_one_band_each_frame(void) {
    settle_held(1, false, false);
}

static void
test_held_lod_refines_to_the_full_finest_picture(void) {
    settle_held(RENDER_SETTLE_FRAME_US_MAX, true, false);
}

static void
test_each_lod_band_matches_the_same_finest_picture(void) {
    settle_held(1, true, false);
}

static void
test_lod_bands_keep_the_nearer_live_fit(void) {
    settle_held(1, true, true);
}

static void
assert_live_lod(void) {
    TEST_ASSERT_EQUAL_MEMORY(f->lod[LIVE], f->lod[0], sizeof f->lod[0]);
    TEST_ASSERT_EQUAL_UINT32(f->c[LIVE].raster.pick_round, f->c[0].raster.pick_round);
    TEST_ASSERT_EQUAL_FLOAT(raster_depth_near(&f->c[LIVE].raster), raster_depth_near(&f->c[0].raster));
    TEST_ASSERT_EQUAL_INT(f->c[LIVE].frame.stats.triangles, f->c[0].frame.stats.triangles);
    TEST_ASSERT_EQUAL_INT(f->c[LIVE].frame.stats.clusters, f->c[0].frame.stats.clusters);
    const r3d_lod_steer_t* live = &f->c[LIVE].steer;
    const r3d_lod_steer_t* settled = &f->c[0].steer;
    TEST_ASSERT_EQUAL_FLOAT(live->tolerance_px, settled->tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(live->picked_tolerance_px, settled->picked_tolerance_px);
    TEST_ASSERT_EQUAL_INT(live->picked_coverage_permille, settled->picked_coverage_permille);
    TEST_ASSERT_EQUAL_INT(live->bin, settled->bin);
    for (size_t i = 0; i < sizeof live->halves / sizeof live->halves[0]; i++) {
        TEST_ASSERT_EQUAL_INT32_ARRAY(live->halves[i].saves, settled->halves[i].saves, R3D_LOD_BINS);
        TEST_ASSERT_EQUAL_INT32(live->halves[i].finest, settled->halves[i].finest);
        TEST_ASSERT_EQUAL_FLOAT(live->halves[i].unit, settled->halves[i].unit);
    }
    TEST_ASSERT_EQUAL_INT64(live->drawn, settled->drawn);
    TEST_ASSERT_EQUAL_INT(live->frames, settled->frames);
    TEST_ASSERT_EQUAL(live->settled, settled->settled);
}

static void
test_lod_settle_leaves_live_picks_and_steering_untouched(void) {
    raster_rig_t* r = fixture(1, true, false);
    (void)frame(r, NULL, -1);
    TEST_ASSERT_TRUE(r3d_lod_any(f->lod[0], CARD_GROUPS));
    for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
        assert_live_lod();
        (void)frame(r, NULL, 0);
    }
    assert_live_lod();
    set_view(SLIDE, raster_rig_camera.eye.z);
    (void)frame(r, NULL, 0);
    assert_live_lod();
    assert_picture(LIVE);
}

static void
test_live_pick_round_advances_only_when_picks_run(void) {
    const int budgets[] = {0, 1};
    for (size_t b = 0; b < sizeof budgets / sizeof budgets[0]; b++) {
        for (int partial = 0; partial <= 1; partial++) {
            raster_rig_t* r = fixture(budgets[b], false, false);
            render_settings_t settings = f->c[0].settings;
            settings.partial_redraw = partial != 0;
            render_context_set_settings(&f->c[0], &settings);
            for (int i = 0; i <= HOLD; i++) {
                const uint32_t before = f->c[0].raster.pick_round;
                const render_context_frame_t out = frame(r, NULL, i == 0 ? -1 : 0);
                const bool settle = partial && budgets[b] > 0 && i == HOLD;
                TEST_ASSERT_EQUAL_UINT32(before + (i == 0 || !partial), f->c[0].raster.pick_round);
                TEST_ASSERT_EQUAL_INT(settle ? BAND_HEIGHT : 0, out.settled_rows);
                TEST_ASSERT_EQUAL(partial && i > 0 && !settle, out.unchanged);
            }
            cleanup();
        }
    }
}

static void
test_moving_after_settle_takes_the_next_spread_pick(void) {
    raster_rig_t* r = fixture(1, true, false);
    for (int i = 0; i < CONTEXTS; i += LIVE) {
        render_settings_t settings = f->c[i].settings;
        settings.lod.tolerance_px = MOVE_TOLERANCE;
        settings.lod.hysteresis_px = 0.0F;
        settings.lod.blend = RENDER_LOD_BLEND_SWITCH;
        settings.lod.triangle_budget = 0;
        settings.lod.pick_every = SPREAD_PICKS;
        render_context_set_settings(&f->c[i], &settings);
    }
    for (int i = 0; i < CONTEXTS; i++) {
        const r3d_instance_t wall = f->instances[i][0];
        f->instances[i][0] = f->instances[i][CARD];
        f->instances[i][CARD] = wall;
    }
    (void)frame(r, NULL, -1);
    const uint32_t held_cut = f->lod[0][0];
    TEST_ASSERT_TRUE(r3d_lod_any(f->lod[0], CARD_GROUPS));
    for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
        (void)frame(r, NULL, 0);
    }
    assert_picture(1);
    for (int i = 0; i < FOLLOWUP_FRAMES; i++) {
        set_view(raster_rig_camera.eye.x + (i + 1) * SLIDE, CLOSE_EYE_Z);
        TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
        TEST_ASSERT_NOT_EQUAL(held_cut, f->lod[0][0]);
        assert_live_lod();
        assert_picture(LIVE);
    }
}

static void
test_refines_coarse_or_blended_settings_with_refined_textures(void) {
    const render_settings_t defaults = RENDER_SETTINGS_DEFAULT;
    render_settings_t settings = render_settle_quality(&defaults);
    TEST_ASSERT_FALSE(render_settle_refines(&settings));
    settings.lod.tolerance_px = defaults.lod.tolerance_px;
    TEST_ASSERT_TRUE(render_settle_refines(&settings));
    settings.lod.tolerance_px = 0.0F;
    settings.lod.blend = RENDER_LOD_BLEND_ON;
    TEST_ASSERT_TRUE(render_settle_refines(&settings));
    settings.lod.blend = RENDER_LOD_BLEND_SWITCH;
    settings.lod.triangle_budget = CARD_FINE_TRIANGLES;
    TEST_ASSERT_TRUE(render_settle_refines(&settings));
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
assert_partial_redraw_bands(bool levels) {
    const raster_t* raster = &f->c[LIVE].raster;
    bool partial_band = false;
    int live_bands = 0;
    int refined_bands = 0;
    for (int b = 0; b < RENDER_SETTLE_BANDS; b++) {
        const int first = b * H / RENDER_SETTLE_BANDS;
        const int end = (b + 1) * H / RENDER_SETTLE_BANDS;
        const bool draws_live = first < raster->end_row && end > raster->first_row;
        partial_band |= draws_live && (raster->first_row > first || raster->end_row < end);
        live_bands += draws_live;
        refined_bands += !draws_live;
        TEST_ASSERT_EQUAL(!draws_live, f->c[0].settle.refined[b]);
        TEST_ASSERT_EQUAL_MEMORY(&f->picture[draws_live ? LIVE : 1][first * W], &f->picture[0][first * W],
                                 (end - first) * W * sizeof(uint16_t));
    }
    if (levels) {
        TEST_ASSERT_TRUE(partial_band);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, live_bands);
    TEST_ASSERT_GREATER_THAN_INT(0, refined_bands);
}

static void
test_changed_rows_stay_live_and_partial_redraw_survives_settling(void) {
    for (int levels = 0; levels <= 1; levels++) {
        raster_rig_t* r = settle_held(RENDER_SETTLE_FRAME_US_MAX, levels != 0, false);
        for (int i = 0; i < HOLD + RENDER_SETTLE_BANDS; i++) {
            box3f_t changed = slide(r, i);
            if (levels) {
                changed.hi.y += CHANGE_ABOVE_CARD;
            }
            const render_context_frame_t out = frame(r, &changed, 1);
            const render_context_frame_t live = render_context_frame(&f->c[LIVE]);
            TEST_ASSERT_LESS_THAN_INT(H, live.rows_end - live.rows_first);
            TEST_ASSERT_EQUAL_INT(live.rows_first, out.rows_first);
            TEST_ASSERT_EQUAL_INT(live.rows_end, out.rows_end);
            const raster_t* raster = &f->c[LIVE].raster;
            if (levels) {
                assert_picture(LIVE);
                TEST_ASSERT_EQUAL_MEMORY(raster_depth(raster), raster_depth(&f->c[0].raster), sizeof f->picture[0]);
            }
            assert_partial_redraw_bands(levels != 0);
        }
        cleanup();
    }
}

static void
test_full_live_redraw_refines_no_band(void) {
    raster_rig_t* r = fixture(1, false, false);
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
    for (int fully_settled = 0; fully_settled <= 1; fully_settled++) {
        raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX, false, false);
        (void)frame(r, NULL, -1);
        for (int i = 1; i < HOLD; i++) {
            TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
            assert_picture(LIVE);
        }
        TEST_ASSERT_EQUAL_INT(BAND_HEIGHT, frame(r, NULL, 0).settled_rows);
        TEST_ASSERT_TRUE(f->c[0].settle.refined[RENDER_SETTLE_BANDS / 2]);
        TEST_ASSERT_FALSE(f->c[0].settle.refined[0]);
        if (fully_settled) {
            TEST_ASSERT_EQUAL_INT(H, frame(r, NULL, 0).settled_rows);
            assert_picture(1);
        }
        set_view(SLIDE, FAR_EYE_Z);
        for (int i = 0; i < HOLD; i++) {
            TEST_ASSERT_EQUAL_INT(0, frame(r, NULL, 0).settled_rows);
            assert_picture(LIVE);
        }
        TEST_ASSERT_EQUAL_INT(H, frame(r, NULL, 0).settled_rows);
        assert_picture(1);
        cleanup();
    }
}

static void
test_disabled_and_already_refined_settings_never_settle(void) {
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX, false, false);
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
    raster_rig_t* r = fixture(RENDER_SETTLE_FRAME_US_MAX, false, false);
    const resolution_config_t config = resolution_config(steps, 2, 2, RENDER_SETTLE_FRAME_US_MAX);
    render_context_t* c = &f->c[0];
    render_context_set_dynamic_resolution(c, &config, NULL, 1);
    TEST_ASSERT_EQUAL_UINT32(0, c->raster.pick_round);
    (void)frame(r, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(1, c->raster.pick_round);
    for (int i = 1; i < HOLD + 1; i++) {
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
    const uint32_t before = c->raster.pick_round;
    const render_context_frame_t over = frame(r, &change_at_eye, 1);
    TEST_ASSERT_EQUAL_UINT32(before + 1, c->raster.pick_round);
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
    RUN_TEST(test_held_lod_refines_to_the_full_finest_picture);
    RUN_TEST(test_each_lod_band_matches_the_same_finest_picture);
    RUN_TEST(test_lod_bands_keep_the_nearer_live_fit);
    RUN_TEST(test_lod_settle_leaves_live_picks_and_steering_untouched);
    RUN_TEST(test_refines_coarse_or_blended_settings_with_refined_textures);
    RUN_TEST(test_live_pick_round_advances_only_when_picks_run);
    RUN_TEST(test_moving_after_settle_takes_the_next_spread_pick);
    RUN_TEST(test_changed_rows_stay_live_and_partial_redraw_survives_settling);
    RUN_TEST(test_full_live_redraw_refines_no_band);
    RUN_TEST(test_a_moved_camera_cancels_and_restarts_the_hold);
    RUN_TEST(test_disabled_and_already_refined_settings_never_settle);
#ifndef ESP_PLATFORM
    RUN_TEST(test_dynamic_resolution_settles_at_the_top_without_pricing_refinement);
#endif
}

SUITE_REGISTER(run_render_settle_suite);

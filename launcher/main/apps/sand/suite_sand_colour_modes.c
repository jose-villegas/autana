/*
 * Device suite: FULL vs 256 vs 16 present cost on landscape sand scenes.
 *
 * RUNSUITE run_sand_colour_modes_suite (main/util/screenshot.c) - not run
 * at ordinary boot, since three present passes per scene at full grid size
 * is a deliberate, explicit measurement, not a startup check.
 *
 * Drives the real sand_t simulation and the real gfx.c present pipeline
 * directly, at a fixed NORMAL-quality (4 px) cell size - not app_sand.c,
 * which owns quality/colour-mode state this file has no access to and is
 * not part of any library this can link against selectively. Every cell,
 * every measured frame is repainted (gfx_mark_all_dirty()) rather than
 * only the changed spans #203 tracks in the real app: this suite compares
 * the three pixel formats' own draw/present cost against each other and
 * confirms real work happens, which a worst-case full redraw shows just as
 * well and more simply than reproducing the app's own dirty-row/run
 * bookkeeping here a second time.
 */
#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD

#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "material.h"
#include "material_palette.h"
#include "sand.h"
#include "sand_palette256.h"

static const char* const TAG = "device_tests";

#define CM_CELL            4 /* NORMAL quality, the sand app's own default */
#define CM_GRID_W          (GFX_WIDTH / CM_CELL)
#define CM_GRID_H          (GFX_HEIGHT / CM_CELL)

/* Landscape: gravity runs down grid +X - the same convention
 * shading_palette.c's scene renderer uses for the same reason. */
#define CM_GRAVITY_X       1000
#define CM_GRAVITY_Y       0

#define CM_MEASURED_FRAMES 10

typedef enum {
    CM_MODE_FULL,
    CM_MODE_256,
    CM_MODE_16,
} colour_mode_t;

typedef struct {
    const char* name;
    void (*build)(sand_t* s);
} colour_scene_t;

static void
scene_mixed_flip(sand_t* s) {
    for (int x = 0; x < CM_GRID_W / 2; x++) {
        for (int y = CM_GRID_H / 2; y < CM_GRID_H; y++) {
            sand_set(s, x, y, SAND_FIRST_SHADE);
        }
    }
    for (int x = CM_GRID_W / 2; x < CM_GRID_W; x++) {
        for (int y = CM_GRID_H / 2; y < CM_GRID_H; y++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int y = 0; y < CM_GRID_H; y++) {
        sand_set(s, CM_GRID_W / 2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int i = 0; i < 60; i++) {
        sand_step(s, CM_GRAVITY_X, CM_GRAVITY_Y, 0);
    }
    /* The flip itself: gravity now pulls the other way along the same axis,
     * so the measured frames below are the mixed pile actually falling. */
}

static void
scene_gas_over_pile(sand_t* s) {
    for (int x = 0; x < CM_GRID_W; x++) {
        for (int y = (CM_GRID_H * 2) / 3; y < CM_GRID_H; y++) {
            sand_set(s, x, y, SAND_FIRST_SHADE);
        }
    }
    for (int i = 0; i < 40; i++) {
        sand_spawn(s, CM_GRID_W / 4, CM_GRID_H / 6, 3, MAT_GAS);
        sand_spawn(s, (3 * CM_GRID_W) / 4, CM_GRID_H / 6, 3, MAT_GAS);
        sand_step(s, CM_GRAVITY_X, CM_GRAVITY_Y, 0);
    }
}

static void
scene_levelling_pool(sand_t* s) {
    sand_spawn(s, CM_GRID_W / 6, CM_GRID_H - 4, CM_GRID_H / 6, MAT_STONE);
    sand_spawn(s, (5 * CM_GRID_W) / 6, 4, CM_GRID_H / 6, MAT_STONE);
    for (int i = 0; i < 200; i++) {
        sand_spawn_cell(s, CM_GRID_W / 2, 2, 3, CELL_MAKE(MAT_WATER, 0));
        sand_step(s, CM_GRAVITY_X, CM_GRAVITY_Y, 0);
    }
}

static const colour_scene_t scenes[] = {
    {"mixed_flip", scene_mixed_flip},
    {"gas_over_pile", scene_gas_over_pile},
    {"levelling_pool", scene_levelling_pool},
};
#define SCENE_COUNT ((int)(sizeof scenes / sizeof scenes[0]))

typedef struct {
    int64_t draw_us;
    int64_t present_us;
    int64_t bytes_sent;
} mode_result_t;

static void
paint_full_frame_full(const uint8_t* grid) {
    gfx_color_t* fb = gfx_framebuffer();
    for (int cy = 0; cy < CM_GRID_H; cy++) {
        for (int cx = 0; cx < CM_GRID_W; cx++) {
            const unsigned hash = material_grain_hash(cx, cy);
            gfx_color_t col[3];
            material_colours(grid[cy * CM_GRID_W + cx], hash, 0u, 0u, col);
            gfx_color_t* p = fb + (size_t)(cy * CM_CELL) * GFX_WIDTH + (size_t)(cx * CM_CELL);
            for (int dy = 0; dy < CM_CELL; dy++) {
                for (int dx = 0; dx < CM_CELL; dx++) {
                    p[dy * GFX_WIDTH + dx] = col[0];
                }
            }
        }
    }
}

static void
paint_full_frame_indexed(const uint8_t* grid) {
    uint8_t* img = gfx_indexed_image();
    for (int cy = 0; cy < CM_GRID_H; cy++) {
        for (int cx = 0; cx < CM_GRID_W; cx++) {
            const unsigned hash = material_grain_hash(cx, cy);
            gfx_color_t col[3];
            material_colours(grid[cy * CM_GRID_W + cx], hash, 0u, 0u, col);
            img[cy * CM_GRID_W + cx] = (uint8_t)material_palette256_index(col[0]);
        }
    }
}

static void
measure_mode(const colour_scene_t* scene, colour_mode_t mode, mode_result_t* out) {
    uint8_t* grid = malloc((size_t)CM_GRID_W * CM_GRID_H);
    TEST_ASSERT_NOT_NULL(grid);

    sand_t sim;
    sand_init(&sim, grid, CM_GRID_W, CM_GRID_H, 0xC0107000u);
    scene->build(&sim);

    const bool indexed = mode != CM_MODE_FULL;
    if (indexed) {
        gfx_mode_request_t req = {0};
        req.layout = GFX_LAYOUT_BANDS;
        req.resolution = GFX_RESOLUTION_FULL;
        req.pixfmt = GFX_PIXFMT_INDEXED8;
        req.index_grid_w = CM_GRID_W;
        req.index_grid_h = CM_GRID_H;
        req.cell_size = CM_CELL;
        const gfx_mode_t* granted = gfx_mode_enter(&req);
        TEST_ASSERT_TRUE_MESSAGE(granted->layout == GFX_LAYOUT_BANDS, "GFX_PIXFMT_INDEXED8 could not be granted");
        gfx_indexed_set_lut(sand_palette256_lut);
        gfx_indexed_set_lut16(sand_palette16_lut, sand_palette16_dither);
        gfx_indexed_set_dither16(mode == CM_MODE_16);
    }

    gfx_reset_strip_send_counts();
    int64_t draw_total = 0, present_total = 0;

    for (int i = 0; i < CM_MEASURED_FRAMES; i++) {
        /* Gravity flips along its own (landscape) axis at the midpoint of
         * the run for scene_mixed_flip's own sake; the other two scenes
         * just keep falling the way they were already headed. */
        const int gx = (i < CM_MEASURED_FRAMES / 2) ? CM_GRAVITY_X : -CM_GRAVITY_X;
        sand_step(&sim, gx, CM_GRAVITY_Y, 0);

        const int64_t t0 = esp_timer_get_time();
        if (indexed) {
            paint_full_frame_indexed(grid);
        } else {
            paint_full_frame_full(grid);
        }
        gfx_mark_all_dirty();
        const int64_t t1 = esp_timer_get_time();

        gfx_present_begin();
        gfx_present_wait();
        const int64_t t2 = esp_timer_get_time();

        draw_total += t1 - t0;
        present_total += t2 - t1;
    }

    out->draw_us = draw_total / CM_MEASURED_FRAMES;
    out->present_us = present_total / CM_MEASURED_FRAMES;
    out->bytes_sent = gfx_get_bytes_sent();

    if (indexed) {
        gfx_mode_exit();
    }
    free(grid);
}

static const char* const mode_names[] = {"FULL", "256", "16"};

static void
log_and_check(const colour_scene_t* scene, colour_mode_t mode, const mode_result_t* r) {
    const int64_t frame_us = r->draw_us + r->present_us;
    ESP_LOGI(TAG, "colour mode %s, %s: sand draw %lld us, present %lld us, bytes sent %lld, frame %lld us/frame",
             mode_names[mode], scene->name, (long long)r->draw_us, (long long)r->present_us, (long long)r->bytes_sent,
             (long long)frame_us);

    /* Sanity, not a frame-budget target: proves work actually happened in
     * this mode rather than measuring an accidental no-op. int32/boolean
     * only - the device Unity build has no 64-bit assert. */
    TEST_ASSERT_TRUE_MESSAGE(r->present_us > 0, "present() reported no time at all");
    TEST_ASSERT_TRUE_MESSAGE(r->bytes_sent > 0, "nothing was sent to the panel");
    TEST_ASSERT_TRUE_MESSAGE(frame_us > 0, "a full frame reported no time at all");
}

static void
test_colour_modes_on_scene(int scene_index) {
    const colour_scene_t* scene = &scenes[scene_index];
    mode_result_t full, c256, c16;

    measure_mode(scene, CM_MODE_FULL, &full);
    measure_mode(scene, CM_MODE_256, &c256);
    measure_mode(scene, CM_MODE_16, &c16);

    log_and_check(scene, CM_MODE_FULL, &full);
    log_and_check(scene, CM_MODE_256, &c256);
    log_and_check(scene, CM_MODE_16, &c16);
}

static void
test_mixed_flip(void) {
    test_colour_modes_on_scene(0);
}

static void
test_gas_over_pile(void) {
    test_colour_modes_on_scene(1);
}

static void
test_levelling_pool(void) {
    test_colour_modes_on_scene(2);
}

_Static_assert(SCENE_COUNT == 3, "test_mixed_flip/test_gas_over_pile/test_levelling_pool index scenes[] positionally");

#endif /* DEVICE_BUILD */

void
run_sand_colour_modes_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_mixed_flip);
    RUN_TEST(test_gas_over_pile);
    RUN_TEST(test_levelling_pool);
#endif
}

SUITE_REGISTER(run_sand_colour_modes_suite);

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
 * not part of any library this can link against selectively. Every grid
 * cell is repainted every measured frame (not only the changed spans #203
 * tracks in the real app - a worst-case draw cost, simpler than
 * reproducing that bookkeeping here a second time), but only the real
 * changed bounding box (diff_bounding_box()) is marked dirty, so present
 * cost and bytes sent answer the real question instead of all reading the
 * same worst case regardless of pixel format. */
#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD

#include <stdlib.h>
#include <string.h>

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

/* One entry per gfx_dither_mode_t, CM_MODE_16's own axis - measured
 * alongside FULL and 256 on the same landscape scenes, not a separate
 * suite: the maintainer's own ask is one table to read, not several. */
static const gfx_dither_mode_t dither_modes[] = {
    GFX_DITHER_NONE,           GFX_DITHER_CELL_CHECKER, GFX_DITHER_CELL_BAYER2,
    GFX_DITHER_PIXEL_CHECKER2, GFX_DITHER_PIXEL_BAYER4,
};
#define DITHER_MODE_COUNT ((int)(sizeof dither_modes / sizeof dither_modes[0]))
_Static_assert(DITHER_MODE_COUNT == GFX_DITHER_MODE_COUNT, "dither_modes must list every gfx_dither_mode_t");

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
    int64_t bytes_per_frame;
    int64_t cells_marked_per_frame;
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

/* app_sand.c's own sand_indexed_cell_needs_repaint(), the classify tables
 * this file has no access to already computed by the caller - lever 1's
 * per-mode rule: exact per cell for the two CELL modes, class equality for
 * the two PIXEL ones, raw equality in 256 mode (dither16_on false). */
static bool
cell_needs_repaint(gfx_dither_mode_t mode, bool dither16_on, uint8_t old_idx, uint8_t new_idx,
                   const uint8_t dither16_class[GFX_INDEXED_PALETTE_SIZE],
                   const uint8_t checker2_class[GFX_INDEXED_PALETTE_SIZE], int cx, int cy) {
    if (!dither16_on) {
        return old_idx != new_idx;
    }
    switch (mode) {
        case GFX_DITHER_NONE: return sand_dither_none_lut[old_idx] != sand_dither_none_lut[new_idx];
        case GFX_DITHER_CELL_CHECKER:
            return gfx_indexed_cell_dither_changed(old_idx, new_idx, sand_dither_cell_checker, false, cx, cy);
        case GFX_DITHER_CELL_BAYER2:
            return gfx_indexed_cell_dither_changed(old_idx, new_idx, sand_dither_cell_bayer2, true, cx, cy);
        case GFX_DITHER_PIXEL_CHECKER2: return gfx_indexed_cell_changed(old_idx, new_idx, true, checker2_class);
        case GFX_DITHER_PIXEL_BAYER4:
        default: return gfx_indexed_cell_changed(old_idx, new_idx, true, dither16_class);
    }
}

/* Unlike paint_full_frame_full(), this ALSO applies lever 1's own
 * suppression (cell_needs_repaint()) instead of writing every cell
 * unconditionally: the whole point of measuring it here is the narrower
 * region it marks, not the wider one paint_full_frame_full() still uses.
 * Returns how many cells its own bounding box covers - 0 with the box
 * fields untouched if nothing changed at all. */
static int
paint_full_frame_indexed(const uint8_t* grid, gfx_dither_mode_t mode, bool dither16_on,
                         const uint8_t dither16_class[GFX_INDEXED_PALETTE_SIZE],
                         const uint8_t checker2_class[GFX_INDEXED_PALETTE_SIZE], int* out_x0, int* out_y0, int* out_x1,
                         int* out_y1) {
    uint8_t* img = gfx_indexed_image();
    int x0 = CM_GRID_W, y0 = CM_GRID_H, x1 = 0, y1 = 0;
    for (int cy = 0; cy < CM_GRID_H; cy++) {
        for (int cx = 0; cx < CM_GRID_W; cx++) {
            const unsigned hash = material_grain_hash(cx, cy);
            gfx_color_t col[3];
            material_colours(grid[cy * CM_GRID_W + cx], hash, 0u, 0u, col);
            const int i = cy * CM_GRID_W + cx;
            const uint8_t new_idx = (uint8_t)material_palette256_index(col[0]);
            const uint8_t old_idx = img[i];
            if (!cell_needs_repaint(mode, dither16_on, old_idx, new_idx, dither16_class, checker2_class, cx, cy)) {
                continue;
            }
            img[i] = new_idx;
            x0 = cx < x0 ? cx : x0;
            y0 = cy < y0 ? cy : y0;
            x1 = cx + 1 > x1 ? cx + 1 : x1;
            y1 = cy + 1 > y1 ? cy + 1 : y1;
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    *out_x0 = x0;
    *out_y0 = y0;
    *out_x1 = x1;
    *out_y1 = y1;
    return (x1 - x0) * (y1 - y0);
}

static uint8_t prev_grid[CM_GRID_W * CM_GRID_H];
static bool prev_grid_valid;

/* Real per-frame dirty extent, from the grid itself - not gfx_mark_all_
 * dirty() every frame, which sends every strip regardless of pixel format
 * and would make FULL/256/16 present the same bytes for no reason but this
 * suite's own shortcut. `false` (no change at all) leaves nothing marked;
 * the caller still owns whether that is expected this frame. */
static bool
diff_bounding_box(const uint8_t* grid, int* out_x0, int* out_y0, int* out_x1, int* out_y1) {
    int x0 = CM_GRID_W, y0 = CM_GRID_H, x1 = 0, y1 = 0;
    bool any = false;
    for (int cy = 0; cy < CM_GRID_H; cy++) {
        for (int cx = 0; cx < CM_GRID_W; cx++) {
            const int i = cy * CM_GRID_W + cx;
            if (prev_grid_valid && grid[i] == prev_grid[i]) {
                continue;
            }
            any = true;
            x0 = cx < x0 ? cx : x0;
            y0 = cy < y0 ? cy : y0;
            x1 = cx + 1 > x1 ? cx + 1 : x1;
            y1 = cy + 1 > y1 ? cy + 1 : y1;
        }
    }
    memcpy(prev_grid, grid, sizeof prev_grid);
    prev_grid_valid = true;
    if (any) {
        *out_x0 = x0;
        *out_y0 = y0;
        *out_x1 = x1;
        *out_y1 = y1;
    }
    return any;
}

static const gfx_color_t*
dither_table_for(gfx_dither_mode_t mode) {
    switch (mode) {
        case GFX_DITHER_NONE: return sand_dither_none_lut;
        case GFX_DITHER_CELL_CHECKER: return sand_dither_cell_checker;
        case GFX_DITHER_CELL_BAYER2: return sand_dither_cell_bayer2;
        case GFX_DITHER_PIXEL_CHECKER2: return sand_dither_pixel_checker2;
        case GFX_DITHER_PIXEL_BAYER4:
        default: return sand_palette16_dither_rgb;
    }
}

/* `dither_mode` only matters when `mode` is CM_MODE_16 - FULL and 256 both
 * ignore it. */
static void
measure_mode(const colour_scene_t* scene, colour_mode_t mode, gfx_dither_mode_t dither_mode, mode_result_t* out) {
    uint8_t* grid = malloc((size_t)CM_GRID_W * CM_GRID_H);
    TEST_ASSERT_NOT_NULL(grid);
    prev_grid_valid = false;

    sand_t sim;
    sand_init(&sim, grid, CM_GRID_W, CM_GRID_H, 0xC0107000u);
    scene->build(&sim);

    const bool indexed = mode != CM_MODE_FULL;
    static uint8_t dither16_class[GFX_INDEXED_PALETTE_SIZE];
    static uint8_t checker2_class[GFX_INDEXED_PALETTE_SIZE];
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
        memset(gfx_indexed_image(), 0, (size_t)CM_GRID_W * CM_GRID_H);
        gfx_indexed_set_lut(sand_palette256_lut);
        gfx_indexed_set_dither16(mode == CM_MODE_16);
        if (mode == CM_MODE_16) {
            gfx_indexed_set_dither(dither_mode, dither_table_for(dither_mode));
            gfx_indexed_dither16_classify(sand_palette16_dither_rgb, dither16_class);
            gfx_indexed_classify(sand_dither_pixel_checker2,
                                 GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX, checker2_class);
        }
    }

    gfx_reset_strip_send_counts();
    int64_t draw_total = 0, present_total = 0, cells_marked_total = 0;

    for (int i = 0; i < CM_MEASURED_FRAMES; i++) {
        /* Gravity flips along its own (landscape) axis at the midpoint of
         * the run for scene_mixed_flip's own sake; the other two scenes
         * just keep falling the way they were already headed. */
        const int gx = (i < CM_MEASURED_FRAMES / 2) ? CM_GRAVITY_X : -CM_GRAVITY_X;
        sand_step(&sim, gx, CM_GRAVITY_Y, 0);

        int dx0, dy0, dx1, dy1;
        const bool changed = diff_bounding_box(grid, &dx0, &dy0, &dx1, &dy1);

        const int64_t t0 = esp_timer_get_time();
        if (indexed) {
            /* Lever 1's own narrower box - some cells diff_bounding_box()
             * above already called changed dither to the SAME 16-colour
             * (or, in 256 mode, the same index), so this can be, and often
             * is, smaller than [dx0,dx1)x[dy0,dy1). */
            int ix0, iy0, ix1, iy1;
            const int cells = paint_full_frame_indexed(grid, dither_mode, mode == CM_MODE_16, dither16_class,
                                                       checker2_class, &ix0, &iy0, &ix1, &iy1);
            if (cells > 0) {
                gfx_mark_dirty(ix0 * CM_CELL, iy0 * CM_CELL, (ix1 - ix0) * CM_CELL, (iy1 - iy0) * CM_CELL);
            }
            cells_marked_total += cells;
        } else {
            paint_full_frame_full(grid);
            if (changed) {
                gfx_mark_dirty(dx0 * CM_CELL, dy0 * CM_CELL, (dx1 - dx0) * CM_CELL, (dy1 - dy0) * CM_CELL);
                cells_marked_total += (int64_t)(dx1 - dx0) * (dy1 - dy0);
            }
        }
        const int64_t t1 = esp_timer_get_time();

        gfx_present_begin();
        gfx_present_wait();
        const int64_t t2 = esp_timer_get_time();

        draw_total += t1 - t0;
        present_total += t2 - t1;
    }

    out->draw_us = draw_total / CM_MEASURED_FRAMES;
    out->present_us = present_total / CM_MEASURED_FRAMES;
    out->bytes_per_frame = gfx_get_bytes_sent() / CM_MEASURED_FRAMES;
    out->cells_marked_per_frame = cells_marked_total / CM_MEASURED_FRAMES;

    if (indexed) {
        gfx_mode_exit();
    }
    free(grid);
}

static const char* const mode_names[] = {"FULL", "256", "16"};
static const char* const dither_mode_names[] = {"NONE", "CELL_CHECKER", "CELL_BAYER2", "PIXEL_CHECKER2",
                                                "PIXEL_BAYER4"};

/* `dither_label` is NULL for FULL/256, which have no dither pattern of
 * their own. */
static void
log_and_check(const colour_scene_t* scene, colour_mode_t mode, const char* dither_label, const mode_result_t* r) {
    const int64_t frame_us = r->draw_us + r->present_us;
    ESP_LOGI(TAG,
             "colour mode %s%s%s, %s: sand draw %lld us, present %lld us, %lld bytes/frame, %lld cells/frame, "
             "frame %lld us/frame",
             mode_names[mode], dither_label != NULL ? " " : "", dither_label != NULL ? dither_label : "", scene->name,
             (long long)r->draw_us, (long long)r->present_us, (long long)r->bytes_per_frame,
             (long long)r->cells_marked_per_frame, (long long)frame_us);

    /* Sanity, not a frame-budget target: proves work actually happened in
     * this mode rather than measuring an accidental no-op. int32/boolean
     * only - the device Unity build has no 64-bit assert. */
    TEST_ASSERT_TRUE_MESSAGE(r->present_us > 0, "present() reported no time at all");
    TEST_ASSERT_TRUE_MESSAGE(r->bytes_per_frame > 0, "nothing was sent to the panel");
    TEST_ASSERT_TRUE_MESSAGE(r->cells_marked_per_frame > 0, "no cell was ever marked dirty");
    TEST_ASSERT_TRUE_MESSAGE(frame_us > 0, "a full frame reported no time at all");
}

static void
test_colour_modes_on_scene(int scene_index) {
    const colour_scene_t* scene = &scenes[scene_index];
    mode_result_t full, c256;

    measure_mode(scene, CM_MODE_FULL, GFX_DITHER_PIXEL_BAYER4, &full);
    measure_mode(scene, CM_MODE_256, GFX_DITHER_PIXEL_BAYER4, &c256);
    log_and_check(scene, CM_MODE_FULL, NULL, &full);
    log_and_check(scene, CM_MODE_256, NULL, &c256);

    for (int d = 0; d < DITHER_MODE_COUNT; d++) {
        mode_result_t c16;
        measure_mode(scene, CM_MODE_16, dither_modes[d], &c16);
        log_and_check(scene, CM_MODE_16, dither_mode_names[d], &c16);
    }
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

/* Defined in app_sand.c (CONFIG_LAUNCHER_SELFTEST only), which is not part
 * of any library this could declare through a shared header - one function
 * does not earn an app_sand.h, the same reasoning suite_sand_perf.c's own
 * sand_app_alloc_selfcheck() declaration gives. Drives the real app through
 * the exact sequence the reported crash reproduced: a sim entered in the
 * given colour mode, then back to the launch menu. */
bool sand_app_test_survives_indexed_then_menu(int mode);

static void
test_indexed_mode_does_not_survive_a_return_to_the_menu(void) {
    TEST_ASSERT_TRUE_MESSAGE(sand_app_test_survives_indexed_then_menu(1 /* 256 */),
                             "indexed mode survived a return to the menu after a 256 sim");
    TEST_ASSERT_TRUE_MESSAGE(sand_app_test_survives_indexed_then_menu(2 /* 16 */),
                             "indexed mode survived a return to the menu after a 16 sim");
}

/* Same reasoning as the declaration above - drives the START button itself,
 * not its outcome (a real device crash on the pre-fix code, not a Unity
 * failure line: gfx_fb_guard_ok() only counts on a host or a non-development
 * build, and this is neither - see gfx_fb_guard.h). */
bool sand_app_test_start_button_survives_the_ui_build(int mode);

static void
test_the_start_button_itself_does_not_crash_the_menu(void) {
    TEST_ASSERT_TRUE_MESSAGE(sand_app_test_start_button_survives_the_ui_build(1 /* 256 */),
                             "START did not reach RUNNING in indexed mode 256");
    TEST_ASSERT_TRUE_MESSAGE(sand_app_test_start_button_survives_the_ui_build(2 /* 16 */),
                             "START did not reach RUNNING in indexed mode 16");
}

#endif /* DEVICE_BUILD */

void
run_sand_colour_modes_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_indexed_mode_does_not_survive_a_return_to_the_menu);
    RUN_TEST(test_the_start_button_itself_does_not_crash_the_menu);
    RUN_TEST(test_mixed_flip);
    RUN_TEST(test_gas_over_pile);
    RUN_TEST(test_levelling_pool);
#endif
}

SUITE_REGISTER(run_sand_colour_modes_suite);

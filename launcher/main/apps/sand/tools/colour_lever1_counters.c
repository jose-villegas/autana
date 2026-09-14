/*
 * colour_lever1_counters - host prediction of lever 1's own win: cells
 * marked and bytes sent per frame, before and after gfx_indexed_cell_
 * changed()'s suppression, on a busy landscape scene. A counter delta
 * predicts WORK saved, never the time a real present costs - device
 * confirmation still needs the real panel and present pipeline.
 *
 * Mirrors suite_sand_colour_modes.c's own measurement shape (a bounding box
 * of changed cells drives gfx_mark_dirty() in the real suite) at NORMAL
 * quality (4 px cells), but drives the real sand_t simulation directly
 * rather than through app_sand.c, which is not host-portable.
 *
 *     main/apps/sand/tools/report_colour_lever1_counters.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx_indexed.h"
#include "material.h"
#include "material_palette.h"
#include "sand.h"
#include "sand_palette256.h"

#define CELL      4 /* NORMAL quality, the sand app's own default */
#define GRID_W    (368 / CELL)
#define GRID_H    (448 / CELL)

#define GRAVITY_X 1000
#define GRAVITY_Y 0

#define STEPS     40

static void
build_scene(sand_t* s) {
    for (int x = 0; x < GRID_W / 2; x++) {
        for (int y = GRID_H / 2; y < GRID_H; y++) {
            sand_set(s, x, y, SAND_FIRST_SHADE);
        }
    }
    for (int x = GRID_W / 2; x < GRID_W; x++) {
        for (int y = GRID_H / 2; y < GRID_H; y++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int y = 0; y < GRID_H; y++) {
        sand_set(s, GRID_W / 2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int i = 0; i < 60; i++) {
        sand_step(s, GRAVITY_X, GRAVITY_Y, 0);
    }
}

/* This step's shading, as both a 256-index and a 16-colour class, one per
 * cell - material_colours() at a fixed hash/mask, the same simplification
 * suite_sand_colour_modes.c's own paint_full_frame_*() makes (no wake-tick
 * shine/wood-leaf state), EXCEPT depth: a real local-depth band is exactly
 * where 256 shades collapse onto far fewer of the 16, gravity's own axis
 * (+X, landscape) giving each run of consecutive water cells a rising
 * depth, capped at the real band width. */
static void
shade_frame(const uint8_t* grid, uint8_t* idx256, uint8_t* class16, const uint8_t dither_class[256]) {
    for (int cy = 0; cy < GRID_H; cy++) {
        unsigned water_depth = 0;
        for (int cx = 0; cx < GRID_W; cx++) {
            const cell_t c = grid[cy * GRID_W + cx];
            const unsigned hash = material_grain_hash(cx, cy);
            unsigned depth = 0;
            if (CELL_MATERIAL(c) == MAT_WATER) {
                depth = water_depth < MATERIAL_LIQUID_DEPTH_BAND - 1 ? water_depth : MATERIAL_LIQUID_DEPTH_BAND - 1;
                water_depth++;
            } else {
                water_depth = 0;
            }
            gfx_color_t col[3];
            material_colours(c, hash, 0u, depth, col);
            const uint8_t idx = (uint8_t)material_palette256_index(col[0]);
            idx256[cy * GRID_W + cx] = idx;
            class16[cy * GRID_W + cx] = dither_class[idx];
        }
    }
}

/* The sum of each ROW's own [x0,x1) bounding span where `cur[i] != prev[i]`
 * - app_sand.c's real granularity (row_changed_x0/x1, draw_dirty_rows()),
 * not one box over the whole grid: a scene where a wide region moves at
 * once would otherwise report the same huge box before and after,
 * whatever lever 1 suppressed within it. BEFORE and AFTER differ only in
 * which array is compared, not how the span is built. */
static long
changed_span_cells(const uint8_t* cur, const uint8_t* prev) {
    long total = 0;
    for (int cy = 0; cy < GRID_H; cy++) {
        int x0 = GRID_W, x1 = 0;
        for (int cx = 0; cx < GRID_W; cx++) {
            const int i = cy * GRID_W + cx;
            if (cur[i] == prev[i]) {
                continue;
            }
            x0 = cx < x0 ? cx : x0;
            x1 = cx + 1 > x1 ? cx + 1 : x1;
        }
        if (x1 > x0) {
            total += x1 - x0;
        }
    }
    return total;
}

int
main(void) {
    static uint8_t grid[GRID_W * GRID_H];
    sand_t sim;
    sand_init(&sim, grid, GRID_W, GRID_H, 0xC0107000u);
    build_scene(&sim);

    static uint8_t dither_class[GFX_INDEXED_PALETTE_SIZE];
    gfx_indexed_dither16_classify(sand_palette16_dither_rgb, dither_class);

    static uint8_t prev_grid[GRID_W * GRID_H], prev_idx256[GRID_W * GRID_H], prev_class16[GRID_W * GRID_H];
    static uint8_t cur_idx256[GRID_W * GRID_H], cur_class16[GRID_W * GRID_H];
    memset(prev_grid, 0, sizeof prev_grid);
    shade_frame(grid, prev_idx256, prev_class16, dither_class);

    long before_cells_total = 0, after256_cells_total = 0, after16_cells_total = 0;

    for (int i = 0; i < STEPS; i++) {
        const int gx = (i < STEPS / 2) ? GRAVITY_X : -GRAVITY_X;
        sand_step(&sim, gx, GRAVITY_Y, 0);
        shade_frame(grid, cur_idx256, cur_class16, dither_class);

        /* BEFORE: today's own reach - any sand-cell byte that moved, the
         * same union suite_sand_colour_modes.c's diff_bounding_box() (and
         * app_sand.c's own sim-driven dirty_x0/x1) already sends,
         * unfiltered by whether the SHADING actually changed. */
        const long before = changed_span_cells(grid, prev_grid);
        /* AFTER: lever 1's own narrower reach - only cells whose rendered
         * value actually differs, at 256-index or 16-colour-class
         * resolution respectively. */
        const long after256 = changed_span_cells(cur_idx256, prev_idx256);
        const long after16 = changed_span_cells(cur_class16, prev_class16);

        before_cells_total += before;
        after256_cells_total += after256;
        after16_cells_total += after16;

        memcpy(prev_grid, grid, sizeof prev_grid);
        memcpy(prev_idx256, cur_idx256, sizeof prev_idx256);
        memcpy(prev_class16, cur_class16, sizeof prev_class16);
    }

    const double before_cells = (double)before_cells_total / STEPS;
    const double after256_cells = (double)after256_cells_total / STEPS;
    const double after16_cells = (double)after16_cells_total / STEPS;
    /* RGB565 bytes per cell at NORMAL quality - what gfx.c's present path
     * would actually queue for a box this size, whichever pixel format the
     * mode expands to (INDEXED8 still sends RGB565 once expanded). */
    const double bytes_per_cell = (double)(CELL * CELL) * 2.0;

    printf("cells marked per frame (mean over %d steps), mixed_flip, NORMAL quality:\n", STEPS);
    printf("  before (today, unfiltered): %.1f cells, %.0f bytes\n", before_cells, before_cells * bytes_per_cell);
    printf("  after, 256 mode (index equality):        %.1f cells, %.0f bytes\n", after256_cells,
           after256_cells * bytes_per_cell);
    printf("  after, 16 mode (dither class equality):  %.1f cells, %.0f bytes\n", after16_cells,
           after16_cells * bytes_per_cell);
    return 0;
}

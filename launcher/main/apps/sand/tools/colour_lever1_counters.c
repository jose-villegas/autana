/*
 * colour_lever1_counters - host prediction of lever 1's own win: cells
 * marked and bytes sent per frame, before and after change suppression,
 * per GFX_DITHER_* mode, on a busy landscape scene. A counter delta
 * predicts WORK saved, never the time a real present costs.
 *
 * Mirrors suite_sand_colour_modes.c's own bounding-box shape at NORMAL
 * quality (4 px cells), but drives sand_t directly - app_sand.c is not
 * host-portable.
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

/* This step's 256-index per cell - material_colours() at a fixed hash/
 * mask, no wake-tick shine/wood-leaf state (the same simplification
 * suite_sand_colour_modes.c's own paint_full_frame_*() makes), EXCEPT
 * depth: a real local-depth band is exactly where 256 shades collapse
 * onto far fewer of the 16. */
static void
shade_frame(const uint8_t* grid, uint8_t* idx256) {
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
            idx256[cy * GRID_W + cx] = (uint8_t)material_palette256_index(col[0]);
        }
    }
}

/* This step's OUTPUT REPRESENTATIVE per cell, for one GFX_DITHER_* mode -
 * two indices sharing one render identically at (cx, cy), so comparing
 * representatives is exactly lever 1's own change test. CELL modes and
 * GFX_DITHER_NONE resolve to a colour; the PIXEL modes resolve to a class,
 * since their own cell spans more than one phase. */
static void
represent_frame(const uint8_t* idx256, gfx_dither_mode_t mode, const uint8_t dither16_class[256],
                const uint8_t checker2_class[256], uint16_t* out) {
    for (int cy = 0; cy < GRID_H; cy++) {
        for (int cx = 0; cx < GRID_W; cx++) {
            const uint8_t idx = idx256[cy * GRID_W + cx];
            uint16_t repr;
            switch (mode) {
                case GFX_DITHER_NONE: repr = sand_dither_none_lut[idx]; break;
                case GFX_DITHER_CELL_CHECKER: repr = sand_dither_cell_checker[idx * 2 + ((cx + cy) & 1)]; break;
                case GFX_DITHER_CELL_BAYER2: repr = sand_dither_cell_bayer2[idx * 4 + (cy & 1) * 2 + (cx & 1)]; break;
                case GFX_DITHER_PIXEL_CHECKER2: repr = checker2_class[idx]; break;
                case GFX_DITHER_PIXEL_BAYER4:
                default: repr = dither16_class[idx]; break;
            }
            out[cy * GRID_W + cx] = repr;
        }
    }
}

/* The sum of each ROW's own [x0,x1) bounding span where `cur[i] != prev[i]`
 * - app_sand.c's real granularity (row_changed_x0/x1, draw_dirty_rows()),
 * not one box over the whole grid: a scene where a wide region moves at
 * once would otherwise report the same huge box before and after,
 * whatever lever 1 suppressed within it. */
static long
changed_span_cells(const uint16_t* cur, const uint16_t* prev) {
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

static const char* const mode_names[] = {"NONE", "CELL_CHECKER", "CELL_BAYER2", "PIXEL_CHECKER2", "PIXEL_BAYER4"};

int
main(void) {
    static uint8_t grid[GRID_W * GRID_H];
    sand_t sim;
    sand_init(&sim, grid, GRID_W, GRID_H, 0xC0107000u);
    build_scene(&sim);

    static uint8_t dither16_class[GFX_INDEXED_PALETTE_SIZE];
    static uint8_t checker2_class[GFX_INDEXED_PALETTE_SIZE];
    gfx_indexed_dither16_classify(sand_palette16_dither_rgb, dither16_class);
    gfx_indexed_classify(sand_dither_pixel_checker2, GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX,
                         checker2_class);

    static uint8_t prev_grid[GRID_W * GRID_H], prev_idx256[GRID_W * GRID_H];
    static uint16_t prev_repr[GFX_DITHER_MODE_COUNT][GRID_W * GRID_H];
    static uint16_t cur_repr[GFX_DITHER_MODE_COUNT][GRID_W * GRID_H];
    memset(prev_grid, 0, sizeof prev_grid);
    shade_frame(grid, prev_idx256);
    for (int m = 0; m < GFX_DITHER_MODE_COUNT; m++) {
        represent_frame(prev_idx256, (gfx_dither_mode_t)m, dither16_class, checker2_class, prev_repr[m]);
    }

    long before_total = 0;
    long after_total[GFX_DITHER_MODE_COUNT] = {0};

    for (int i = 0; i < STEPS; i++) {
        const int gx = (i < STEPS / 2) ? GRAVITY_X : -GRAVITY_X;
        sand_step(&sim, gx, GRAVITY_Y, 0);

        static uint8_t cur_idx256[GRID_W * GRID_H];
        shade_frame(grid, cur_idx256);

        /* BEFORE: today's own reach - any sand-cell byte that moved,
         * unfiltered by whether the SHADING actually changed. The same
         * scan as changed_span_cells() below, on uint8_t grid bytes
         * instead of a uint16_t representative. */
        long before_row_total = 0;
        for (int cy = 0; cy < GRID_H; cy++) {
            int x0 = GRID_W, x1 = 0;
            for (int cx = 0; cx < GRID_W; cx++) {
                const int idx = cy * GRID_W + cx;
                if (grid[idx] == prev_grid[idx]) {
                    continue;
                }
                x0 = cx < x0 ? cx : x0;
                x1 = cx + 1 > x1 ? cx + 1 : x1;
            }
            if (x1 > x0) {
                before_row_total += x1 - x0;
            }
        }
        before_total += before_row_total;

        for (int m = 0; m < GFX_DITHER_MODE_COUNT; m++) {
            represent_frame(cur_idx256, (gfx_dither_mode_t)m, dither16_class, checker2_class, cur_repr[m]);
            after_total[m] += changed_span_cells(cur_repr[m], prev_repr[m]);
            memcpy(prev_repr[m], cur_repr[m], sizeof prev_repr[m]);
        }

        memcpy(prev_grid, grid, sizeof prev_grid);
        memcpy(prev_idx256, cur_idx256, sizeof prev_idx256);
    }

    /* RGB565 bytes per cell at NORMAL quality - what gfx.c's present path
     * would actually queue for a box this size, whichever pixel format the
     * mode expands to (INDEXED8 still sends RGB565 once expanded). */
    const double bytes_per_cell = (double)(CELL * CELL) * 2.0;
    const double before_cells = (double)before_total / STEPS;

    printf("cells marked per frame (mean over %d steps), mixed_flip, NORMAL quality:\n", STEPS);
    printf("  before (today, unfiltered): %.1f cells, %.0f bytes\n", before_cells, before_cells * bytes_per_cell);
    for (int m = 0; m < GFX_DITHER_MODE_COUNT; m++) {
        const double after_cells = (double)after_total[m] / STEPS;
        printf("  after, 16 %-14s: %.1f cells, %.0f bytes\n", mode_names[m], after_cells, after_cells * bytes_per_cell);
    }
    return 0;
}

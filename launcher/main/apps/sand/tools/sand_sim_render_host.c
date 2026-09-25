#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx.h"
#include "render_host.h"

#include "apps/sand/material_palette.h"
#include "apps/sand/sand.h"
#include "apps/sand/sand_paint.h"

#define CELL_SIZE 4
#define GRID_W    (GFX_WIDTH / CELL_SIZE)
#define GRID_H    (GFX_HEIGHT / CELL_SIZE)

static sand_t sim;
static uint8_t cells[GRID_W * GRID_H];

static bool
setup(int quarter) {
    (void)quarter;
    sand_init(&sim, cells, GRID_W, GRID_H, 0x53414e44u);
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 8; x < 84; x++) {
        sand_set(&sim, x, 78, CELL_MAKE(MAT_STONE, 3));
    }
    for (int y = 63; y < 78; y++) {
        sand_set(&sim, 8, y, CELL_MAKE(MAT_STONE, 3));
        sand_set(&sim, 83, y, CELL_MAKE(MAT_STONE, 3));
    }
    return true;
}

static void
pour(int frame) {
    if (frame < 90) {
        for (int dx = -3; dx <= 3; dx++) {
            sand_set(&sim, 25 + dx, 9, CELL_MAKE(MAT_SAND, (frame + dx) & 15));
            sand_set(&sim, 66 + dx, 14, CELL_MAKE(MAT_WATER, 15));
        }
    }
    if (frame >= 18 && frame < 75) {
        sand_set(&sim, 43, 11, CELL_MAKE(MAT_LAVA, 15));
        sand_set(&sim, 49, 11, CELL_MAKE(MAT_WATER, 15));
    }
}

static void
draw_grid(void) {
    gfx_color_t* fb = gfx_framebuffer();
    const uint8_t* grid = sim.cells;
    for (int cy = 0; cy < GRID_H; cy++) {
        const uint8_t* row = grid + cy * GRID_W;
        const uint8_t* above = cy > 0 ? row - GRID_W : NULL;
        const uint8_t* below = cy + 1 < GRID_H ? row + GRID_W : NULL;
        for (int cx = 0; cx < GRID_W; cx++) {
            const unsigned mask = sand_paint_edge_mask(above, row, below, cx, GRID_W);
            const unsigned hash = material_grain_hash(cx, cy);
            gfx_color_t colors[3];
            const material_pattern_t pattern = material_colours(row[cx], hash, mask, 0, colors);
            for (int py = 0; py < CELL_SIZE; py++) {
                for (int px = 0; px < CELL_SIZE; px++) {
                    const bool hatch =
                        pattern == MATERIAL_HATCHED && ((cx * CELL_SIZE + px + cy * CELL_SIZE + py) & 7) == 0;
                    fb[(cy * CELL_SIZE + py) * GFX_WIDTH + cx * CELL_SIZE + px] = hatch ? colors[2] : colors[0];
                }
            }
        }
    }
}

static void
draw(const render_frame_t* frame) {
    pour(frame->index);
    const int gx = frame->index < 85 ? 0 : 180;
    const int gy = frame->index < 85 ? 256 : 175;
    for (int i = 0; i < 2; i++) {
        sand_step(&sim, gx, gy, 0);
    }
    draw_grid();
}

const render_scene_t render_scene = {
    .name = "sand_sim",
    .quarter = 0,
    .frames = 145,
    .dt_ms = 33,
    .setup = setup,
    .draw = draw,
};

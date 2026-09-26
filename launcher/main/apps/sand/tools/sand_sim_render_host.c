#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gfx/gfx.h"
#include "input/tilt.h"
#include "render_host.h"

#include "apps/sand/material_palette.h"
#include "apps/sand/sand.h"
#include "apps/sand/sand_paint_row.h"

#define CELL_SIZE 4
#define GRID_W    (GFX_WIDTH / CELL_SIZE)
#define GRID_H    (GFX_HEIGHT / CELL_SIZE)
#define VIEW_W    GRID_H
#define VIEW_H    GRID_W

static sand_t sim;
static tilt_t tilt;
static uint8_t cells[GRID_W * GRID_H];
static impulse_t impulses[GRID_W * GRID_H];
static uint8_t depth_a[GRID_W_MAX];
static uint8_t depth_b[GRID_W_MAX];
static sand_paint_row_state_t paint = {
    .local_depth_cur_row = depth_a, .local_depth_prev_row = depth_b, .local_depth_prev_cy = LOCAL_DEPTH_NO_ROW};
static FILE* angles;
static int wood_top_down;
static int gunpowder_blasts;

static void
put(int x, int y, cell_t cell) {
    sand_set(&sim, VIEW_H - 1 - y, x, cell);
}

static void
ground(void) {
    for (int x = 0; x < VIEW_W; x++) {
        const int top = x < 49 ? 72 : x < 83 ? 78 : 67;
        for (int y = top; y < VIEW_H; y++) {
            const bool soil = y < 86;
            put(x, y, CELL_MAKE(soil ? MAT_DIRT : MAT_STONE, soil ? 13 : (x + y) & 7));
        }
    }
}

static void
mountain(void) {
    for (int x = 1; x < 57; x++) {
        const int dx = abs(x - 27);
        const int top = 8 + dx * 2;
        for (int y = top; y < 72; y++) {
            const bool vent = dx < 3 && y < 48;
            const bool chamber = dx < 10 && y >= 48 && y < 61;
            if (vent || chamber) {
                continue;
            }
            const bool soil = y < top + 9 || dx > 13;
            put(x, y, CELL_MAKE(soil ? MAT_DIRT : MAT_STONE, soil ? 13 : (x + y) & 7));
        }
    }
}

static void
magma(void) {
    for (int x = 18; x < 37; x++) {
        for (int y = 54; y < 61; y++) {
            put(x, y, CELL_MAKE(MAT_LAVA, 15));
        }
    }
    for (int y = 16; y < 48; y++) {
        for (int x = 25; x < 30; x++) {
            put(x, y, CELL_MAKE(MAT_LAVA, 15));
        }
    }
}

static void
lake_and_grove(void) {
    for (int x = 49; x < 83; x++) {
        for (int y = 53; y < 78; y++) {
            put(x, y, CELL_MAKE(MAT_WATER, 15));
        }
    }
    for (int x = 48; x < 55; x++) {
        for (int y = 52; y < 59; y++) {
            put(x, y, CELL_MAKE(MAT_DIRT, 13));
        }
    }
    for (int x = 83; x < VIEW_W; x++) {
        for (int y = 63; y < 67; y++) {
            put(x, y, CELL_MAKE(MAT_DIRT, 13));
        }
    }
}

static void
gunpowder_charge(void) {
    for (int x = 49; x < 56; x++) {
        for (int y = 44; y < 51; y++) {
            put(x, y, GUNPOWDER_CELL(0));
        }
    }
}

static void
terrain(void) {
    ground();
    mountain();
    magma();
    lake_and_grove();
}

static void
trees(void) {
    for (int i = 0; i < 4; i++) {
        put(85 + i * 7, 62, MATX(MATX_PLANT));
    }
}

static bool
setup(int quarter) {
    if (quarter != 1) {
        return false;
    }
    sand_init(&sim, cells, GRID_W, GRID_H, 0x564f4c43u);
    sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
    sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
    sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&sim, impulses, GRID_W * GRID_H);
    terrain();
    trees();
    sand_add_emitter(&sim, VIEW_H - 1 - 11, 27, CELL_MAKE(MAT_LAVA, 15));
    tilt_reset(&tilt, 256);
    const char* path = getenv("SAND_ANGLE_PATH");
    angles = path != NULL ? fopen(path, "w") : NULL;
    return path == NULL || angles != NULL;
}

static double
board_angle(int frame) {
    const double t = (double)frame / 239.0;
    return 7.0 * sin(t * 6.283185307179586) + 36.0 * (0.5 - 0.5 * cos(t * 6.283185307179586));
}

static void
drive_tilt(const render_frame_t* frame, int* gx, int* gy) {
    const double radians = board_angle(frame->index) * 0.017453292519943295;
    const int sx = (int)lround(256.0 * sin(radians));
    const int sy = (int)lround(256.0 * cos(radians));
    tilt_update(&tilt, -sy, sx, 0, 90, frame->dt_ms);
    *gx = tilt_x(&tilt);
    *gy = tilt_y(&tilt);
    if (angles != NULL) {
        fprintf(angles, "%.3f\n", board_angle(frame->index));
        fflush(angles);
    }
}

static void
paint_grid(const render_frame_t* frame, int gx, int gy) {
    sand_paint_frame_t pf = {.shine_period = 64,
                             .shine_offset = frame->index / 2,
                             .wood_leaf_wind_sign = 1,
                             .wood_leaf_time_ms = frame->elapsed_ms};
    material_set_gravity(gx, gy);
    material_set_foam_phase(frame->elapsed_ms / 90);
    material_shine_direction(gx, gy, &pf.shine_ux_q8, &pf.shine_uy_q8);
    material_wood_leaf_wind_axis(gx, gy, &pf.wood_leaf_wind_ux_q8, &pf.wood_leaf_wind_uy_q8);
    material_wood_leaf_top5(gx, gy, &wood_top_down, pf.wood_leaf_top5);
    update_local_depth_gravity(&paint, gx, gy, GRID_W, GRID_H);
    for (int cy = 0; cy < GRID_H; cy++) {
        paint_row_n(&paint, &pf, gfx_framebuffer(), NULL, NULL, cy, sim.cells + cy * GRID_W, CELL_SIZE, GRID_W, GRID_H,
                    0, GRID_W, true);
    }
}

static void
draw(const render_frame_t* frame) {
    int gx, gy;
    drive_tilt(frame, &gx, &gy);
    if (frame->index == 165) {
        gunpowder_charge();
        put(52, 43, CELL_MAKE(MAT_FIRE, 15));
    }
    const int steps = frame->index >= 165 && frame->index < 190 ? 1 : 4;
    for (int i = 0; i < steps; i++) {
        const uint8_t wait_before = sim.fuse_blast_wait;
        sand_step(&sim, gx, gy, 0);
        if (wait_before == 0 && sim.fuse_blast_wait != 0) {
            gunpowder_blasts++;
        }
    }
    paint_grid(frame, gx, gy);
    if (frame->index == frame->count - 1) {
        printf("sand_sim gunpowder blasts: %d\n", gunpowder_blasts);
    }
    if (frame->index == frame->count - 1 && angles != NULL) {
        fclose(angles);
        angles = NULL;
    }
}

const render_scene_t render_scene = {
    .name = "sand_sim",
    .quarter = 1,
    .frames = 240,
    .dt_ms = 33,
    .setup = setup,
    .draw = draw,
};

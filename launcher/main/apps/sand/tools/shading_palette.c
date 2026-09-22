/*
 * shading_palette - does sand's shading fit a 256-entry indexed palette?
 *
 * Three jobs, host only:
 *
 *   1. Sweep the real material_colours() over every cell byte, hash byte,
 *      edge mask and depth value, plus the per-frame state it reads
 *      (gravity's rim table, foam, cullet and glass phases), and collect
 *      the distinct colours per material group.
 *   2. Build a 256-entry palette: a fixed UI block, then per-group budgets
 *      grown greedily by weighted k-means in OKLab until the table is full.
 *   3. Build a 16-entry palette, shared and per scene, whose colours are
 *      approximated by gfx_dither_covers()'s ordered dither between pairs.
 *   4. Settle six landscape scenes with the real simulation, paint them
 *      through a mirror of the app's row painter, and write original,
 *      256-colour and 16-colour panels side by side as PNG.
 *
 * The painter mirror follows paint_row_n() in app_sand.c for a 2 px cell,
 * the way suite_sand_liquid_depth.c mirrors local depth: app_sand.c is not
 * host-portable. A scene pixel whose colour the sweep never produced is
 * counted and printed, so a mirror or sweep that drifts from the real code
 * shows up in the output rather than in a wrong verdict.
 *
 * No libraries beyond libc and libm; the PNG is written with stored
 * (uncompressed) deflate blocks.
 *
 *     main/apps/sand/tools/report_shading_palette.sh <results-dir>
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx_indexed.h"
#include "gfx/gfx_palette.h"
#include "gfx_palette_gen.h"
#include "material.h"
#include "material_palette.h"
#include "sand.h"
#include "util/intmath.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GRID_W       184
#define GRID_H       224
#define CELL_PX      2
#define PANEL_W      (GRID_W * CELL_PX)
#define PANEL_H      (GRID_H * CELL_PX)

/* Landscape: gravity runs down grid +X. Images are turned so it points down. */
#define VIEW_W       PANEL_H
#define VIEW_H       PANEL_W
#define GRAVITY_X    1000
#define GRAVITY_Y    0

#define KEYS         65536
#define PALETTE_SIZE 256
#define UI_ENTRIES   16

/* material groups */

typedef enum {
    G_EMPTY,
    G_SAND,
    G_CULLET,
    G_WATER,
    G_STONE,
    G_GAS,
    G_FIRE,
    G_WOOD,
    G_WOOD_LEAF,
    G_STEAM,
    G_SMOKE,
    G_OIL,
    G_LAVA,
    G_ACID,
    G_GLASS,
    G_SNOW,
    G_DIRT,
    G_ICE,
    G_PLANT,
    G_LEAF,
    G_METAL,
    G_ROOT,
    G_GUNPOWDER,
    G_SPARE,
    G_COUNT
} group_t;

static const char* const group_names[G_COUNT] = {
    "empty", "sand", "cullet", "water", "stone", "gas", "fire",  "wood", "wood-leaf", "steam", "smoke",     "oil",
    "lava",  "acid", "glass",  "snow",  "dirt",  "ice", "plant", "leaf", "metal",     "root",  "gunpowder", "spare",
};

static const char* const group_tags[G_COUNT] = {
    "EMP", "SND", "CUL", "WAT", "STN", "GAS", "FIR", "WOD", "WLF", "STM", "SMK", "OIL",
    "LAV", "ACD", "GLS", "SNW", "DRT", "ICE", "PLT", "LEF", "MTL", "ROT", "GUN", "SPR",
};

static group_t
group_of(cell_t c, unsigned depth) {
    const uint8_t m = CELL_MATERIAL(c);
    const uint8_t v = CELL_VARIANT(c);
    switch (m) {
        case MAT_EMPTY: return G_EMPTY;
        case MAT_SAND: return v >= SAND_CULLET_BASE ? G_CULLET : G_SAND;
        case MAT_WATER: return G_WATER;
        case MAT_STONE: return G_STONE;
        case MAT_GAS: return G_GAS;
        case MAT_FIRE: return G_FIRE;
        case MAT_WOOD: return (v == 0 && depth != 0) ? G_WOOD_LEAF : G_WOOD;
        case MAT_STEAM: return G_STEAM;
        case MAT_SMOKE: return G_SMOKE;
        case MAT_OIL: return G_OIL;
        case MAT_LAVA: return G_LAVA;
        case MAT_ACID: return G_ACID;
        case MAT_GLASS: return G_GLASS;
        case MAT_SNOW: return G_SNOW;
        case MAT_DIRT: return G_DIRT;
        default: break;
    }
    if (c >= GUNPOWDER_BASE) {
        return G_GUNPOWDER;
    }
    switch (v) {
        case MATX_ICE: return G_ICE;
        case MATX_PLANT: return G_PLANT;
        case MATX_LEAF: return G_LEAF;
        case MATX_METAL: return G_METAL;
        case MATX_ROOT: return G_ROOT;
        default: return G_SPARE;
    }
}

/* Native RGB565, not the panel's byte-swapped gfx_color_t. */
static uint16_t
native_key(gfx_color_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

static uint32_t
key_rgb888(uint16_t key) {
    const unsigned r5 = (key >> 11) & 0x1Fu, g6 = (key >> 5) & 0x3Fu, b5 = key & 0x1Fu;
    return ((r5 << 3 | r5 >> 2) << 16) | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
}

/* 1. the sweep */

/* One bit per restriction a call satisfied: the full sweep, then each input
 * pinned in turn, so "colours lost when X is pinned" names what X creates. */
#define PIN_NONE  (1u << 0)
#define PIN_HASH  (1u << 1)
#define PIN_MASK  (1u << 2)
#define PIN_DEPTH (1u << 3)
#define PIN_STATE (1u << 4)
#define PIN_KINDS 5

static uint8_t seen[G_COUNT][KEYS];
static uint32_t used[G_COUNT][KEYS];

static void
record(cell_t c, unsigned hash, unsigned mask, unsigned depth, bool base_state) {
    gfx_color_t out[3];
    const material_pattern_t pat = material_colours(c, hash, mask, depth, out);
    const group_t g = group_of(c, depth);
    uint8_t bits = PIN_NONE;
    bits |= base_state ? PIN_STATE : 0u;
    bits |= (base_state && hash == 0) ? PIN_HASH : 0u;
    bits |= (base_state && mask == 0) ? PIN_MASK : 0u;
    bits |= (base_state && depth == 0) ? PIN_DEPTH : 0u;
    const int n_out = pat == MATERIAL_HATCHED ? 3 : 1;
    for (int i = 0; i < n_out; i++) {
        seen[g][native_key(out[i])] |= bits;
    }
}

static void
set_base_state(void) {
    material_set_gravity(GRAVITY_X, GRAVITY_Y);
    material_set_foam_phase(0);
    material_set_cullet_phase(0);
    material_set_glass_phase(0);
}

/* Gravity only reaches material_colours() through the 16-entry rim table,
 * so a ring of directions covers every table it can build. */
#define GRAVITY_RING_STEPS 72

static void
enumerate_full_sweep(void) {
    for (unsigned c = 0; c < 256u; c++) {
        for (unsigned depth = 0; depth <= 256u; depth++) {
            for (unsigned mask = 0; mask < 256u; mask++) {
                for (unsigned hash = 0; hash < 256u; hash++) {
                    record((cell_t)c, hash, mask, depth, true);
                }
            }
        }
    }
}

static void
enumerate_liquid_masks(unsigned c) {
    for (unsigned mask = 0; mask < 256u; mask++) {
        for (unsigned hash = 0; hash < 16u; hash++) {
            record((cell_t)c, hash, mask, 0u, false);
        }
    }
}

static void
enumerate_liquid_phase(unsigned phase) {
    material_set_foam_phase(phase);
    for (unsigned c = 0; c < 256u; c++) {
        if (material_of((cell_t)c)->kind == KIND_LIQUID) {
            enumerate_liquid_masks(c);
        }
    }
}

static void
enumerate_liquid_gravity_step(int i) {
    const double a = 2.0 * M_PI * i / GRAVITY_RING_STEPS;
    const int gx = i == GRAVITY_RING_STEPS ? 0 : (int)lround(1000.0 * cos(a));
    const int gy = i == GRAVITY_RING_STEPS ? 0 : (int)lround(1000.0 * sin(a));
    material_set_gravity(gx, gy);
    for (unsigned phase = 0; phase < 16u; phase++) {
        enumerate_liquid_phase(phase);
    }
}

static void
enumerate_liquid_gravity(void) {
    for (int i = 0; i <= GRAVITY_RING_STEPS; i++) {
        enumerate_liquid_gravity_step(i);
    }
}

static void
enumerate_glass_cullet(void) {
    for (unsigned phase = 0; phase < 64u; phase++) {
        material_set_cullet_phase(phase);
        material_set_glass_phase((int)phase * 37);
        for (unsigned c = 0; c < 256u; c++) {
            const uint8_t m = CELL_MATERIAL(c);
            if (m != MAT_SAND && m != MAT_GLASS) {
                continue;
            }
            for (unsigned mask = 0; mask < 16u; mask++) {
                for (unsigned hash = 0; hash < 256u; hash++) {
                    record((cell_t)c, hash, mask, 0u, false);
                }
            }
        }
    }
}

static void
enumerate(void) {
    set_base_state();
    enumerate_full_sweep();

    enumerate_liquid_gravity();
    set_base_state();

    enumerate_glass_cullet();
    set_base_state();
}

static int
count_seen(group_t g, uint8_t bit) {
    int n = 0;
    for (int k = 0; k < KEYS; k++) {
        n += (seen[g][k] & bit) != 0;
    }
    return n;
}

/* 3a. scenes */

/* View coordinates: vx across the landscape screen, vy down it toward the
 * floor. Grid x is vy; grid y runs against vx. */
static void
set_v(sand_t* s, int vx, int vy, cell_t c) {
    sand_set(s, vy, GRID_H - 1 - vx, c);
}

static void
spawn_v(sand_t* s, int vx, int vy, int r, material_id_t m) {
    sand_spawn(s, vy, GRID_H - 1 - vx, r, m);
}

static void
spawn_cell_v(sand_t* s, int vx, int vy, int r, cell_t c) {
    sand_spawn_cell(s, vy, GRID_H - 1 - vx, r, c);
}

static void
rect_v(sand_t* s, int vx0, int vy0, int vx1, int vy1, cell_t c) {
    for (int vy = vy0; vy < vy1; vy++) {
        for (int vx = vx0; vx < vx1; vx++) {
            set_v(s, vx, vy, c);
        }
    }
}

static void
steps(sand_t* s, int n) {
    for (int i = 0; i < n; i++) {
        sand_step(s, GRAVITY_X, GRAVITY_Y, 0);
    }
}

static void
scene_sand_pile(sand_t* s) {
    rect_v(s, 0, 170, VIEW_W / CELL_PX, GRID_W, CELL_SOIL(MAT_DIRT, 2, 0));
    for (int i = 0; i < 700; i++) {
        spawn_v(s, 70 + (i / 40) % 3, 6, 3, MAT_SAND);
        spawn_v(s, 160, 6, 2, MAT_SAND);
        sand_step(s, GRAVITY_X, GRAVITY_Y, 0);
    }
    steps(s, 400);
}

static void
scene_water_pool(sand_t* s) {
    spawn_v(s, 20, 183, 40, MAT_STONE);
    spawn_v(s, 70, 190, 30, MAT_STONE);
    spawn_v(s, 205, 183, 45, MAT_STONE);
    spawn_v(s, 120, 130, 9, MAT_STONE);
    spawn_v(s, 160, 165, 12, MAT_STONE);
    for (int i = 0; i < 900; i++) {
        spawn_cell_v(s, 40 + (i * 7) % 150, 4, 3, CELL_MAKE(MAT_WATER, 0));
        sand_step(s, GRAVITY_X, GRAVITY_Y, 0);
    }
    steps(s, 600);
}

static void
scene_lava_heat(sand_t* s) {
    spawn_v(s, 112, 200, 70, MAT_STONE);
    rect_v(s, 30, 60, 34, 150, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
    rect_v(s, 190, 60, 194, 150, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
    for (int i = 0; i < 300; i++) {
        spawn_cell_v(s, 90 + (i * 5) % 50, 4, 3, CELL_MAKE(MAT_LAVA, 0));
        sand_step(s, GRAVITY_X, GRAVITY_Y, 0);
    }
    rect_v(s, 150, 70, 180, 80, CELL_MAKE(MAT_WOOD, 0));
    spawn_v(s, 60, 40, 8, MAT_WOOD);
    spawn_v(s, 60, 28, 4, MAT_FIRE);
    spawn_v(s, 165, 60, 4, MAT_FIRE);
    spawn_v(s, 20, 110, 6, MAT_OIL);
    steps(s, 160);
}

static void
scene_glass(sand_t* s) {
    rect_v(s, 0, 176, VIEW_W / CELL_PX, GRID_W, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    for (int v = 0; v < MATERIAL_VARIANTS; v++) {
        const int vx0 = 8 + v * 12;
        rect_v(s, vx0, 20, vx0 + 9, 70, CELL_MAKE(MAT_GLASS, (uint8_t)v));
    }
    rect_v(s, 10, 90, 100, 96, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
    rect_v(s, 10, 96, 14, 150, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
    rect_v(s, 96, 96, 100, 150, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
    rect_v(s, 120, 110, 210, 114, CELL_MAKE(MAT_GLASS, SAND_SHOCK_HEAT + 4));
    for (int vy = 150; vy < 176; vy++) {
        for (int vx = 110; vx < 220; vx++) {
            const int d = vx - 165;
            if (d * d / 60 < vy - 140) {
                set_v(s, vx, vy, CELL_MAKE(MAT_SAND, (uint8_t)(SAND_CULLET_BASE + ((vx * 7 + vy * 3) & 3))));
            }
        }
    }
    spawn_v(s, 55, 80, 5, MAT_SNOW);
    steps(s, 40);
}

static void
plant_canopy(sand_t* s, int cx, int top, int ground) {
    rect_v(s, cx - 2, top, cx + 2, ground, CELL_MAKE(MAT_WOOD, 0));
    for (int dy = -24; dy <= 24; dy++) {
        for (int dx = -26; dx <= 26; dx++) {
            if (dx * dx + dy * dy > 24 * 24) {
                continue;
            }
            const bool branch = ((dx + dy) & 3) == 0 && im_abs(dx) < 16;
            set_v(s, cx + dx, top + dy, branch ? CELL_MAKE(MAT_WOOD, 0) : MATX(MATX_LEAF));
        }
    }
}

static void
plant_roots(sand_t* s, int cx, int ground, int lean) {
    for (int r = 0; r < 18; r++) {
        set_v(s, cx + (r * lean) / 3, ground + r, MATX(MATX_ROOT));
        set_v(s, cx + (r * lean) / 3 + 1, ground + r, MATX(MATX_ROOT));
        set_v(s, cx - r / 2, ground + r / 2 + 2, MATX(MATX_ROOT));
    }
}

static void
plant_grass_row(sand_t* s, int ground) {
    for (int x = 8; x < 200; x += 9) {
        if ((x / 9) % 4 == 1) {
            rect_v(s, x, ground - 18 - (x % 13), x + 1, ground, MATX(MATX_PLANT));
        }
    }
}

static void
scene_plants(sand_t* s) {
    const int ground = 150;
    rect_v(s, 0, ground, VIEW_W / CELL_PX, GRID_W, CELL_SOIL(MAT_DIRT, 1, 0));
    rect_v(s, 0, ground + 14, VIEW_W / CELL_PX, GRID_W, CELL_WITH_MOISTURE(CELL_MAKE(MAT_DIRT, 0), 4));
    for (int t = 0; t < 3; t++) {
        const int cx = 40 + t * 72;
        plant_canopy(s, cx, ground - 80, ground);
        plant_roots(s, cx, ground, t - 1);
    }
    plant_grass_row(s, ground);
    steps(s, 20);
}

static void
scene_mixed(sand_t* s) {
    rect_v(s, 0, 176, VIEW_W / CELL_PX, GRID_W, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    rect_v(s, 20, 100, 60, 176, MATX(MATX_METAL));
    rect_v(s, 160, 130, 200, 176, MATX(MATX_ICE));
    spawn_v(s, 110, 170, 22, MAT_STONE);
    for (int i = 0; i < 250; i++) {
        spawn_cell_v(s, 70 + (i * 3) % 30, 4, 2, CELL_MAKE(MAT_WATER, 0));
        spawn_cell_v(s, 205 + (i * 3) % 14, 4, 2, CELL_MAKE(MAT_ACID, 0));
        if (i > 120) {
            spawn_cell_v(s, 80, 4, 2, CELL_MAKE(MAT_OIL, 0));
        }
        sand_step(s, GRAVITY_X, GRAVITY_Y, 0);
    }
    spawn_cell_v(s, 130, 150, 6, GUNPOWDER_CELL(0));
    spawn_v(s, 175, 110, 6, MAT_SNOW);
    spawn_v(s, 35, 90, 6, MAT_DIRT);
    spawn_v(s, 140, 120, 6, MAT_SAND);
    steps(s, 60);
    spawn_v(s, 60, 30, 10, MAT_GAS);
    spawn_v(s, 110, 40, 10, MAT_STEAM);
    spawn_v(s, 170, 40, 10, MAT_SMOKE);
    spawn_v(s, 212, 60, 4, MAT_FIRE);
    steps(s, 12);
}

typedef struct {
    const char* name;
    void (*build)(sand_t* s);
} scene_t;

static const scene_t scenes[] = {
    {"sand_pile", scene_sand_pile}, {"water_pool", scene_water_pool}, {"lava_heat", scene_lava_heat},
    {"glass", scene_glass},         {"plants", scene_plants},         {"mixed", scene_mixed},
};

#define SCENE_COUNT             ((int)(sizeof scenes / sizeof scenes[0]))

/* 3b. the painter mirror (paint_row_n in app_sand.c, n = 2) */

#define SHINE_PERIOD            64
#define FOAM_BLOB_SHIFT         3
#define WOOD_LEAF_SLOTS_CHECKED 5u
#define LOCAL_DEPTH_NO_ROW      (-2)

static unsigned depth_scale_q8, depth_ax, depth_ay;
static bool depth_vertical_dominant, depth_v_reverse, depth_h_reverse;
static uint8_t depth_row_a[GRID_W], depth_row_b[GRID_W], depth_top_row[GRID_W];
static uint8_t *depth_cur_row, *depth_prev_row;
static int depth_prev_cy;
static int shine_ux_q8, shine_uy_q8, wind_ux_q8, wind_uy_q8;
static int8_t leaf_top5[5][2];
static uint32_t leaf_time_ms;

static void
paint_begin(uint32_t time_ms) {
    const int gx = GRAVITY_X, gy = GRAVITY_Y;
    material_set_gravity(gx, gy);
    material_set_foam_phase(7);
    material_set_cullet_phase(5);
    material_set_glass_phase(0);
    material_shine_direction(gx, gy, &shine_ux_q8, &shine_uy_q8);
    material_wood_leaf_wind_axis(gx, gy, &wind_ux_q8, &wind_uy_q8);
    int last_down = 0;
    material_wood_leaf_top5(gx, gy, &last_down, leaf_top5);
    leaf_time_ms = time_ms;

    const int ax = im_abs(gx), ay = im_abs(gy);
    depth_vertical_dominant = ay >= ax;
    const unsigned dom = depth_vertical_dominant ? (unsigned)ay : (unsigned)ax;
    depth_scale_q8 = dom != 0u ? (256u * (unsigned)im_len(gx, gy)) / dom : 256u;
    depth_ax = (unsigned)ax;
    depth_ay = (unsigned)ay;
    depth_v_reverse = gy < 0;
    depth_h_reverse = gx < 0;
    memset(depth_row_a, 0, sizeof depth_row_a);
    memset(depth_row_b, 0, sizeof depth_row_b);
    memset(depth_top_row, 255, sizeof depth_top_row);
    depth_cur_row = depth_row_a;
    depth_prev_row = depth_row_b;
    depth_prev_cy = LOCAL_DEPTH_NO_ROW;
}

static void
put_px(gfx_color_t* fb, uint8_t* grp, int px, int py, gfx_color_t c, group_t g) {
    fb[py * PANEL_W + px] = c;
    grp[py * PANEL_W + px] = (uint8_t)g;
}

/* row is always non-NULL; above/below are NULL past the grid's top/bottom
 * edge, which this treats the same as an in-bounds non-empty neighbour. */
static bool
cell_empty_at(const uint8_t* row, int idx) {
    return row != NULL && CELL_IS_EMPTY(row[idx]);
}

static unsigned
cardinal_edge_mask(const uint8_t* row, const uint8_t* above, const uint8_t* below, int cx, int w) {
    unsigned mask = 0;
    if (cx > 0 && cell_empty_at(row, cx - 1)) {
        mask |= MATERIAL_EDGE_LEFT;
    }
    if (cx < w - 1 && cell_empty_at(row, cx + 1)) {
        mask |= MATERIAL_EDGE_RIGHT;
    }
    if (cell_empty_at(above, cx)) {
        mask |= MATERIAL_EDGE_UP;
    }
    if (cell_empty_at(below, cx)) {
        mask |= MATERIAL_EDGE_DOWN;
    }
    return mask;
}

static unsigned
diagonal_edge_mask(const uint8_t* above, const uint8_t* below, int cx, int w) {
    unsigned mask = 0;
    if (cx > 0 && cell_empty_at(above, cx - 1)) {
        mask |= MATERIAL_EDGE_UP_LEFT;
    }
    if (cx < w - 1 && cell_empty_at(above, cx + 1)) {
        mask |= MATERIAL_EDGE_UP_RIGHT;
    }
    if (cx > 0 && cell_empty_at(below, cx - 1)) {
        mask |= MATERIAL_EDGE_DOWN_LEFT;
    }
    if (cx < w - 1 && cell_empty_at(below, cx + 1)) {
        mask |= MATERIAL_EDGE_DOWN_RIGHT;
    }
    return mask;
}

static unsigned
cell_edge_mask(const uint8_t* row, const uint8_t* above, const uint8_t* below, int cx, int w) {
    const unsigned mask = cardinal_edge_mask(row, above, below, cx, w);
    if ((mask & MATERIAL_EDGE_CARDINAL) == 0 || CELL_MATERIAL(row[cx]) != MAT_WATER) {
        return mask;
    }
    return mask | diagonal_edge_mask(above, below, cx, w);
}

/* Bresenham-style: accumulates every call, so it must run once per column
 * in column order, matching how paint_row walks a row. */
static int
horiz_dominant_step(int* herr, int ysign) {
    if (depth_vertical_dominant) {
        return 0;
    }
    *herr += (int)depth_ay;
    if (depth_ax > 0u && *herr >= (int)depth_ax) {
        *herr -= (int)depth_ax;
        return ysign;
    }
    return 0;
}

/* The liquid depth band's running count: continues the source cell's count
 * one deeper on a same-material chain, restarts it at the first non-chained
 * liquid cell, or holds it at zero once a column has already committed. */
static unsigned
liquid_depth_count(int cx, int cy, bool here_liquid, bool same_material, bool cross_row, bool chain_ok,
                   unsigned src_count) {
    if (!here_liquid) {
        return 0u;
    }
    if (same_material) {
        if (!depth_vertical_dominant) {
            depth_top_row[cx] = 255u;
        }
        return src_count < MATERIAL_LIQUID_DEPTH_BAND ? src_count + 1u : MATERIAL_LIQUID_DEPTH_BAND;
    }
    const bool committed = depth_vertical_dominant ? depth_top_row[cx] == (uint8_t)cy : depth_top_row[cx] != 255u;
    if (committed) {
        return 0u;
    }
    const unsigned carry = (cross_row && !chain_ok) ? 0u : src_count;
    depth_top_row[cx] = depth_vertical_dominant ? (uint8_t)cy : 0u;
    return carry < MATERIAL_LIQUID_DEPTH_BAND ? carry + 1u : MATERIAL_LIQUID_DEPTH_BAND;
}

/* material_colours()'s depth input: root age, wood-leaf gust, or the liquid
 * depth band computed for every other cell. */
static unsigned
cell_shading_depth(const uint8_t* above, const uint8_t* row, const uint8_t* below, int cx, int w, unsigned hash, int cy,
                   unsigned depth_liquid) {
    if (row[cx] == MATX(MATX_ROOT)) {
        return material_root_neighbours(above, row, below, cx, w);
    }
    const bool wood_near_leaf =
        row[cx] == CELL_MAKE(MAT_WOOD, 0)
        && material_wood_near_leaf(above, row, below, cx, w, leaf_top5, hash, WOOD_LEAF_SLOTS_CHECKED);
    const bool leaf_shading = wood_near_leaf || row[cx] == MATX(MATX_LEAF);
    if (!leaf_shading) {
        return depth_liquid;
    }
    const int wind_pos = (cx * wind_ux_q8 + cy * wind_uy_q8) >> 8;
    return material_wood_leaf_wave(leaf_time_ms, wind_pos, w, hash) + 1u;
}

static void
paint_cell_block(gfx_color_t* fb, uint8_t* grp, int px0, int py0, const gfx_color_t* col, material_pattern_t pat,
                 group_t g) {
    if (pat != MATERIAL_HATCHED) {
        for (int dy = 0; dy < CELL_PX; dy++) {
            for (int dx = 0; dx < CELL_PX; dx++) {
                put_px(fb, grp, px0 + dx, py0 + dy, col[0], g);
            }
        }
        return;
    }
    const int shine_base_q8 = px0 * shine_ux_q8 + py0 * shine_uy_q8;
    for (int dy = 0; dy < CELL_PX; dy++) {
        for (int dx = 0; dx < CELL_PX; dx++) {
            const int shine_q8 = shine_base_q8 + dx * shine_ux_q8 + dy * shine_uy_q8;
            const int along = (shine_q8 >> 8) & (SHINE_PERIOD - 1);
            put_px(fb, grp, px0 + dx, py0 + dy, along < CELL_PX ? col[2] : col[0], g);
        }
    }
}

/* Every column's row_step to the source row, for the vertical-dominant case:
 * how far the reference column shifts per row, accumulated as gravity's
 * diagonal sweeps across the grid. */
static int
depth_row_step(int cy, int vdir) {
    if (!depth_vertical_dominant || depth_ay == 0u) {
        return 0;
    }
    const int xsign = depth_h_reverse ? 1 : -1;
    const int n = vdir > 0 ? cy : GRID_H - 1 - cy;
    const int cum_n = (int)(((long)n * (long)depth_ax) / (long)depth_ay);
    const int cum_n1 = (int)(((long)(n + 1) * (long)depth_ax) / (long)depth_ay);
    return xsign * (cum_n1 - cum_n);
}

typedef struct {
    const uint8_t* row;
    const uint8_t* above;
    const uint8_t* below;
    const uint8_t* toward_surface;
    bool chain_ok;
    int hdir;
    int row_step;
    int ysign;
    int w;
} paint_row_ctx_t;

static paint_row_ctx_t
paint_row_context(const uint8_t* grid, int cy) {
    paint_row_ctx_t ctx;
    ctx.w = GRID_W;
    ctx.row = &grid[cy * ctx.w];
    ctx.above = cy > 0 ? ctx.row - ctx.w : NULL;
    ctx.below = cy < GRID_H - 1 ? ctx.row + ctx.w : NULL;
    ctx.toward_surface = depth_v_reverse ? ctx.below : ctx.above;
    const int vdir = depth_v_reverse ? -1 : 1;
    ctx.chain_ok = depth_prev_cy == cy - vdir;
    ctx.hdir = depth_h_reverse ? -1 : 1;
    ctx.row_step = depth_row_step(cy, vdir);
    ctx.ysign = depth_v_reverse ? 1 : -1;
    return ctx;
}

/* The liquid depth band's source column: cross_row when gravity's dominant
 * axis carries the reference up a whole row, or a horizontal Bresenham step
 * lands on one this column. */
static void
column_liquid_source(const paint_row_ctx_t* ctx, int cx, int step, bool* cross_row, const uint8_t** src_ptr,
                     const uint8_t** src_arr) {
    *cross_row = depth_vertical_dominant || step != 0;
    *src_ptr = *cross_row ? ctx->toward_surface : ctx->row;
    *src_arr = *cross_row ? depth_prev_row : depth_cur_row;
}

static void
paint_column(const paint_row_ctx_t* ctx, int cy, int cx, int* herr, gfx_color_t* fb, uint8_t* grp) {
    const unsigned mask = cell_edge_mask(ctx->row, ctx->above, ctx->below, cx, ctx->w);
    const bool is_water = CELL_MATERIAL(ctx->row[cx]) == MAT_WATER;
    const unsigned hash =
        is_water ? material_grain_hash(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT) : material_grain_hash(cx, cy);

    const int step = horiz_dominant_step(herr, ctx->ysign);
    const int qx = depth_vertical_dominant ? cx + ctx->row_step : cx - ctx->hdir;
    const bool qx_ok = qx >= 0 && qx < ctx->w;
    bool cross_row;
    const uint8_t *src_ptr, *src_arr;
    column_liquid_source(ctx, cx, step, &cross_row, &src_ptr, &src_arr);
    const bool here_liquid = material_of(ctx->row[cx])->kind == KIND_LIQUID;
    const bool same_material =
        here_liquid && qx_ok && src_ptr != NULL && CELL_MATERIAL(src_ptr[qx]) == CELL_MATERIAL(ctx->row[cx]);
    const unsigned src_count = qx_ok ? src_arr[qx] : 0u;

    const unsigned count = liquid_depth_count(cx, cy, here_liquid, same_material, cross_row, ctx->chain_ok, src_count);
    depth_cur_row[cx] = (uint8_t)count;
    const unsigned depth_raw = (count * depth_scale_q8) >> 8;
    const unsigned depth_liquid = depth_raw < MATERIAL_LIQUID_DEPTH_BAND ? depth_raw : MATERIAL_LIQUID_DEPTH_BAND;
    const unsigned depth = cell_shading_depth(ctx->above, ctx->row, ctx->below, cx, ctx->w, hash, cy, depth_liquid);

    gfx_color_t col[3];
    const material_pattern_t pat = material_colours(ctx->row[cx], hash, mask, depth, col);
    const group_t g = group_of(ctx->row[cx], depth);
    paint_cell_block(fb, grp, cx * CELL_PX, cy * CELL_PX, col, pat, g);
}

static void
paint_row(const uint8_t* grid, int cy, gfx_color_t* fb, uint8_t* grp) {
    const paint_row_ctx_t ctx = paint_row_context(grid, cy);
    const int cx_first = depth_h_reverse ? ctx.w - 1 : 0;
    const int cx_step = depth_h_reverse ? -1 : 1;
    int herr = 0;

    for (int cx_i = 0; cx_i < ctx.w; cx_i++) {
        paint_column(&ctx, cy, cx_first + cx_i * cx_step, &herr, fb, grp);
    }

    uint8_t* tmp = depth_cur_row;
    depth_cur_row = depth_prev_row;
    depth_prev_row = tmp;
    depth_prev_cy = cy;
}

/* Two whole frames: the depth debounce commits a boundary on its second ask. */
static void
paint_frame(const uint8_t* grid, gfx_color_t* fb, uint8_t* grp, uint32_t time_ms) {
    paint_begin(time_ms);
    for (int frame = 0; frame < 2; frame++) {
        for (int cy = 0; cy < GRID_H; cy++) {
            paint_row(grid, cy, fb, grp);
        }
    }
}

/* 2. OKLab and the quantiser */

typedef struct {
    double l, a, b;
} lab_t;

typedef struct {
    double r, g, b;
} lin_t;

static double
srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
linear_to_srgb(double c) {
    c = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* Scaled by 100 so a distance reads like a CIE delta E: about 1-2 is a just
 * noticeable difference. */
static lin_t
rgb_to_lin(uint32_t rgb) {
    return (lin_t){
        srgb_to_linear(((rgb >> 16) & 0xFF) / 255.0),
        srgb_to_linear(((rgb >> 8) & 0xFF) / 255.0),
        srgb_to_linear((rgb & 0xFF) / 255.0),
    };
}

static lab_t
lin_to_lab(lin_t c) {
    const double l = cbrt(0.4122214708 * c.r + 0.5363325602 * c.g + 0.0514459929 * c.b);
    const double m = cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    const double s = cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    return (lab_t){
        100.0 * (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
        100.0 * (1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
        100.0 * (0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s),
    };
}

static uint16_t
lab_to_key(lab_t p) {
    const double L = p.l / 100.0, A = p.a / 100.0, B = p.b / 100.0;
    double l = L + 0.3963377774 * A + 0.2158037573 * B;
    double m = L - 0.1055613458 * A - 0.0638541728 * B;
    double s = L - 0.0894841775 * A - 1.2914855480 * B;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    const double r = linear_to_srgb(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    const double g = linear_to_srgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    const double b = linear_to_srgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
    const unsigned r5 = (unsigned)lround(r * 31.0), g6 = (unsigned)lround(g * 63.0), b5 = (unsigned)lround(b * 31.0);
    return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

static double
dist2(lab_t p, lab_t q) {
    const double dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

typedef struct {
    int n;
    uint16_t* key;
    lab_t* p;
    double* w;
    int k;
    lab_t* c;
    double sse;
    bool has_next;
    lab_t* next_c;
    double next_sse;
    double max_error;
    int fixed;
} gq_t;

static gq_t quant[G_COUNT];
static lab_t key_lab[KEYS];
static lin_t key_lin[KEYS];

static bool
lloyd_assign(const gq_t* q, const lab_t* c, int k, int* assign, int iter, double* sse) {
    bool moved = false;
    *sse = 0.0;
    for (int i = 0; i < q->n; i++) {
        int best = 0;
        double bd = dist2(q->p[i], c[0]);
        for (int j = 1; j < k; j++) {
            const double d = dist2(q->p[i], c[j]);
            if (d < bd) {
                bd = d;
                best = j;
            }
        }
        if (iter == 0 || assign[i] != best) {
            moved = true;
        }
        assign[i] = best;
        *sse += q->w[i] * bd;
    }
    return moved;
}

static void
lloyd_update_centers(const gq_t* q, lab_t* c, int k, const int* assign) {
    for (int j = q->fixed; j < k; j++) {
        double sl = 0, sa = 0, sb = 0, sw = 0;
        for (int i = 0; i < q->n; i++) {
            if (assign[i] == j) {
                sl += q->w[i] * q->p[i].l;
                sa += q->w[i] * q->p[i].a;
                sb += q->w[i] * q->p[i].b;
                sw += q->w[i];
            }
        }
        if (sw > 0.0) {
            c[j] = (lab_t){sl / sw, sa / sw, sb / sw};
        }
    }
}

static double
lloyd(const gq_t* q, lab_t* c, int k) {
    int* assign = malloc((size_t)q->n * sizeof *assign);
    double sse = 0.0;
    for (int iter = 0; iter < 60; iter++) {
        const bool moved = lloyd_assign(q, c, k, assign, iter, &sse);
        if (!moved) {
            break;
        }
        lloyd_update_centers(q, c, k, assign);
    }
    free(assign);
    return sse;
}

static double
group_max_error(const gq_t* q) {
    double worst = 0.0;
    for (int i = 0; i < q->n; i++) {
        double bd = dist2(q->p[i], q->c[0]);
        for (int j = 1; j < q->k; j++) {
            const double d = dist2(q->p[i], q->c[j]);
            bd = d < bd ? d : bd;
        }
        worst = bd > worst ? bd : worst;
    }
    return sqrt(worst);
}

/* Minimax splits whichever group has the colour furthest from its entry, so
 * no output moves more than the table can afford; the alternative spends
 * entries where weighted squared error drops most. */
static bool minimax = true;

static void
plan_next(gq_t* q) {
    q->has_next = false;
    if (q->k >= q->n) {
        return;
    }
    int worst = -1;
    double wd = 0.0;
    for (int i = 0; i < q->n; i++) {
        double bd = dist2(q->p[i], q->c[0]);
        for (int j = 1; j < q->k; j++) {
            const double d = dist2(q->p[i], q->c[j]);
            bd = d < bd ? d : bd;
        }
        const double score = minimax ? bd : bd * q->w[i];
        if (score > wd) {
            wd = score;
            worst = i;
        }
    }
    if (worst < 0) {
        return;
    }
    memcpy(q->next_c, q->c, (size_t)q->k * sizeof *q->c);
    q->next_c[q->k] = q->p[worst];
    q->next_sse = lloyd(q, q->next_c, q->k + 1);
    q->has_next = true;
}

static const uint32_t ui_block[UI_ENTRIES] = {
    0x000000, 0x1A1A1A, 0x333333, 0x4D4D4D, 0x808080, 0xB3B3B3, 0xD9D9D9, 0xFFFFFF,
    0xFF3B3B, 0xFF8A5C, 0xFFD84A, 0x5CD65C, 0x4AD8FF, 0x3B6BFF, 0xB35CFF, 0xFF00FF,
};

static uint16_t
ui_key(int i) {
    const uint32_t rgb = ui_block[i];
    return (uint16_t)((rgb >> 19) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
}

/* Sand entries beyond the UI block; a colour both need is stored once. */
static int
palette_distinct(void) {
    static uint32_t stamp[KEYS];
    static uint32_t epoch;
    epoch++;
    for (int i = 0; i < UI_ENTRIES; i++) {
        stamp[ui_key(i)] = epoch;
    }
    int n = 0;
    for (int g = 0; g < G_COUNT; g++) {
        for (int j = 0; j < quant[g].k; j++) {
            const uint16_t key = lab_to_key(quant[g].c[j]);
            if (stamp[key] != epoch) {
                stamp[key] = epoch;
                n++;
            }
        }
    }
    return n;
}

/* A liquid's body has only a handful of depth shades and shows them as broad
 * bands, where one merged step reads as a missing band. */
static bool pinned[G_COUNT][KEYS];

static void
pin_depth_steps(void) {
    for (unsigned m = MAT_EMPTY + 1u; m < MAT_COUNT; m++) {
        if (material_by_id((material_id_t)m)->kind != KIND_LIQUID) {
            continue;
        }
        for (unsigned d = 0; d <= MATERIAL_LIQUID_DEPTH_BAND; d++) {
            gfx_color_t out[3];
            material_colours(CELL_MAKE(m, MASS_MAX), 0u, 0u, d, out);
            pinned[group_of(CELL_MAKE(m, MASS_MAX), d)][native_key(out[0])] = true;
        }
    }
}

static uint16_t palette[PALETTE_SIZE];
static group_t palette_group[PALETTE_SIZE];
static int palette_used;
static int16_t map_index[G_COUNT][KEYS];

static void
quant_group_totals(uint64_t* total_used, int* active) {
    *total_used = 0;
    *active = 0;
    for (int g = 0; g < G_COUNT; g++) {
        if (g == G_SPARE) {
            continue;
        }
        for (int k = 0; k < KEYS; k++) {
            *total_used += used[g][k];
        }
        *active += count_seen((group_t)g, PIN_NONE) > 0;
    }
}

/* Half of the weight is how often a colour is actually on screen in the
 * scenes, half is spread evenly over every colour the sweep can produce, so
 * a material the scenes under-show still earns entries. */
static void
quant_populate_group(gq_t* q, group_t g, uint64_t total_used, int active) {
    q->n = g == G_SPARE ? 0 : count_seen(g, PIN_NONE);
    q->key = malloc((size_t)q->n * sizeof *q->key + 1);
    q->p = malloc((size_t)q->n * sizeof *q->p + 1);
    q->w = malloc((size_t)q->n * sizeof *q->w + 1);
    q->c = malloc((size_t)(q->n + 1) * sizeof *q->c);
    q->next_c = malloc((size_t)(q->n + 1) * sizeof *q->next_c);
    int i = 0;
    for (int k = 0; k < KEYS && q->n > 0; k++) {
        if (!(seen[g][k] & PIN_NONE)) {
            continue;
        }
        q->key[i] = (uint16_t)k;
        q->p[i] = key_lab[k];
        q->w[i] =
            0.5 * (double)used[g][k] / (double)(total_used ? total_used : 1) + 0.5 / ((double)active * (double)q->n);
        i++;
    }
    q->fixed = 0;
    for (int j = 0; j < q->n; j++) {
        if (pinned[g][q->key[j]]) {
            q->c[q->fixed++] = q->p[j];
        }
    }
    q->k = q->fixed;
}

static void
quant_seed_centers(gq_t* q) {
    if (q->fixed > 0) {
        q->sse = lloyd(q, q->c, q->k);
        q->max_error = group_max_error(q);
        plan_next(q);
        return;
    }
    if (q->n == 0) {
        return;
    }
    double sl = 0, sa = 0, sb = 0, sw = 0;
    for (int j = 0; j < q->n; j++) {
        sl += q->w[j] * q->p[j].l;
        sa += q->w[j] * q->p[j].a;
        sb += q->w[j] * q->p[j].b;
        sw += q->w[j];
    }
    q->c[0] = (lab_t){sl / sw, sa / sw, sb / sw};
    q->k = 1;
    q->sse = lloyd(q, q->c, 1);
    q->max_error = group_max_error(q);
    plan_next(q);
}

static void
quant_init_group(group_t g, uint64_t total_used, int active) {
    gq_t* q = &quant[g];
    quant_populate_group(q, g, total_used, active);
    quant_seed_centers(q);
}

static void
greedy_grow_palette(int budget) {
    for (;;) {
        int best = -1;
        double gain = -1.0;
        for (int g = 0; g < G_COUNT; g++) {
            const double score = minimax ? quant[g].max_error : quant[g].sse - quant[g].next_sse;
            if (quant[g].has_next && score > gain) {
                gain = score;
                best = g;
            }
        }
        if (best < 0) {
            return;
        }
        gq_t* q = &quant[best];
        lab_t* keep_c = q->c;
        const int keep_k = q->k;
        const double keep_sse = q->sse;
        q->c = q->next_c;
        q->next_c = keep_c;
        q->k++;
        q->sse = q->next_sse;
        if (palette_distinct() > budget) {
            q->next_c = q->c;
            q->c = keep_c;
            q->k = keep_k;
            q->sse = keep_sse;
            q->has_next = false;
            continue;
        }
        q->max_error = group_max_error(q);
        plan_next(q);
    }
}

static int
group_centers_sorted_by_lightness(group_t g, uint16_t* sorted) {
    const gq_t* q = &quant[g];
    int n = 0;
    for (int j = 0; j < q->k && n < PALETTE_SIZE; j++) {
        const uint16_t key = lab_to_key(q->c[j]);
        int at = n++;
        while (at > 0 && key_lab[sorted[at - 1]].l > key_lab[key].l) {
            sorted[at] = sorted[at - 1];
            at--;
        }
        sorted[at] = key;
    }
    return n;
}

static void
assign_palette_entries(int16_t* index_of) {
    for (int i = 0; i < UI_ENTRIES; i++) {
        palette[i] = ui_key(i);
        palette_group[i] = G_COUNT;
        index_of[palette[i]] = (int16_t)i;
    }
    palette_used = UI_ENTRIES;
    for (int g = 0; g < G_COUNT; g++) {
        uint16_t sorted[PALETTE_SIZE];
        const int n = group_centers_sorted_by_lightness((group_t)g, sorted);
        for (int j = 0; j < n; j++) {
            const uint16_t key = sorted[j];
            if (index_of[key] < 0 && palette_used < PALETTE_SIZE) {
                index_of[key] = (int16_t)palette_used;
                palette[palette_used] = key;
                palette_group[palette_used] = (group_t)g;
                palette_used++;
            }
        }
    }
}

static int
map_index_for_key(group_t g, uint16_t k, const int16_t* index_of) {
    const gq_t* q = &quant[g];
    int best = -1;
    double bd = 1e30;
    if (g == G_SPARE) {
        for (int j = 0; j < UI_ENTRIES; j++) {
            const double d = dist2(key_lab[k], key_lab[palette[j]]);
            if (d < bd) {
                bd = d;
                best = j;
            }
        }
        return best;
    }
    for (int j = 0; j < q->k; j++) {
        const int idx = index_of[lab_to_key(q->c[j])];
        const double d = dist2(key_lab[k], key_lab[palette[idx]]);
        if (d < bd) {
            bd = d;
            best = idx;
        }
    }
    return best;
}

/* A group maps only to its own entries (and the UI block for the spare
 * codes), so every output colour of a material has one fixed index. */
static void
build_map_index(const int16_t* index_of) {
    for (int g = 0; g < G_COUNT; g++) {
        for (int k = 0; k < KEYS; k++) {
            map_index[g][k] = -1;
        }
        for (int k = 0; k < KEYS; k++) {
            if (seen[g][k] & PIN_NONE) {
                map_index[g][k] = (int16_t)map_index_for_key((group_t)g, (uint16_t)k, index_of);
            }
        }
    }
}

static void
build_palette(void) {
    uint64_t total_used;
    int active;
    quant_group_totals(&total_used, &active);

    for (int g = 0; g < G_COUNT; g++) {
        quant_init_group((group_t)g, total_used, active);
    }

    greedy_grow_palette(PALETTE_SIZE - UI_ENTRIES);

    static int16_t index_of[KEYS];
    memset(index_of, -1, sizeof index_of);
    assign_palette_entries(index_of);
    build_map_index(index_of);
}

static double
map_error(group_t g, uint16_t key) {
    const int idx = map_index[g][key];
    return idx < 0 ? 1e9 : sqrt(dist2(key_lab[key], key_lab[palette[idx]]));
}

/* ramps */

#define RAMP_MAX 512

typedef struct {
    char name[48];
    int n;
    group_t g[RAMP_MAX];
    uint16_t key[RAMP_MAX];
    bool designed; /* a hand-authored step, not a fine blend fraction */
} ramp_t;

static ramp_t ramp;

static void
ramp_start(const char* name, bool designed) {
    snprintf(ramp.name, sizeof ramp.name, "%s", name);
    ramp.n = 0;
    ramp.designed = designed;
}

static void
ramp_add(cell_t c, unsigned hash, unsigned mask, unsigned depth) {
    gfx_color_t out[3];
    material_colours(c, hash, mask, depth, out);
    ramp.g[ramp.n] = group_of(c, depth);
    ramp.key[ramp.n] = native_key(out[0]);
    ramp.n++;
}

static int ramps_total, ramps_collapsed, ramps_broken;

typedef struct {
    int in, out, returns;
    double lost;
} ramp_stats_t;

static void
ramp_step(int i, bool* left, ramp_stats_t* st) {
    st->in += i == 0 || ramp.key[i] != ramp.key[i - 1];
    const int idx = map_index[ramp.g[i]][ramp.key[i]];
    const int prev = i == 0 ? -1 : map_index[ramp.g[i - 1]][ramp.key[i - 1]];
    if (i > 0 && idx == prev && ramp.key[i] != ramp.key[i - 1]) {
        const double d = sqrt(dist2(key_lab[ramp.key[i]], key_lab[ramp.key[i - 1]]));
        st->lost = d > st->lost ? d : st->lost;
    }
    if (idx == prev) {
        return;
    }
    st->out++;
    if (idx >= 0 && left[idx]) {
        st->returns++;
    }
    if (prev >= 0) {
        left[prev] = true;
    }
}

/* Collapsed: fewer distinct indices than distinct colours along the ramp.
 * Broken: an index comes back after the ramp has left it, which is what a
 * gradient inversion or a band re-appearing looks like. */
static void
ramp_finish(FILE* f) {
    static bool left[PALETTE_SIZE];
    memset(left, 0, sizeof left);
    ramp_stats_t st = {0, 0, 0, 0.0};
    for (int i = 0; i < ramp.n; i++) {
        ramp_step(i, left, &st);
    }
    ramps_total++;
    const bool collapsed = st.out < st.in;
    ramps_collapsed += collapsed && ramp.designed;
    ramps_broken += st.returns > 0;
    if ((collapsed && ramp.designed) || st.returns > 0) {
        fprintf(f, "  %-34s steps %3d  distinct in %3d  out %3d  largest merged step dE %5.2f  returns %d%s\n",
                ramp.name, ramp.n, st.in, st.out, st.lost, st.returns, ramp.designed ? "" : "  (blend)");
    }
}

static void
ramps_per_material(FILE* f) {
    char name[48];
    material_set_gravity(0, 0);
    const material_id_t row_mats[] = {MAT_WATER, MAT_OIL,   MAT_LAVA, MAT_ACID, MAT_GAS, MAT_FIRE,
                                      MAT_STEAM, MAT_SMOKE, MAT_SNOW, MAT_DIRT, MAT_WOOD};
    for (size_t i = 0; i < sizeof row_mats / sizeof row_mats[0]; i++) {
        const material_id_t m = row_mats[i];
        const bool liquid = material_by_id(m)->kind == KIND_LIQUID;
        snprintf(name, sizeof name, "%s variant 0-15", material_by_id(m)->name);
        ramp_start(name, true);
        for (unsigned v = (m == MAT_WOOD ? 1u : 0u); v < 16u; v++) {
            ramp_add(CELL_MAKE(m, v), 1u, liquid ? MATERIAL_EDGE_UP : 0u, 0u);
        }
        ramp_finish(f);
        if (!liquid) {
            continue;
        }
        snprintf(name, sizeof name, "%s depth 0-24", material_by_id(m)->name);
        ramp_start(name, true);
        for (unsigned d = 0; d <= MATERIAL_LIQUID_DEPTH_BAND; d++) {
            ramp_add(CELL_MAKE(m, MASS_MAX), 1u, 0u, d);
        }
        ramp_finish(f);
    }
    set_base_state();
}

static void
ramps_sand_and_cullet(FILE* f) {
    ramp_start("sand dune 0-11", true);
    for (unsigned v = 0; v < SAND_DUNE_SHADES; v++) {
        ramp_add(CELL_MAKE(MAT_SAND, v), 0u, 0u, 0u);
    }
    ramp_finish(f);

    ramp_start("cullet cycle phase 0-15", true);
    for (unsigned p = 0; p < 16u; p++) {
        material_set_cullet_phase(p);
        ramp_add(CELL_MAKE(MAT_SAND, SAND_CULLET_BASE), 1u, 0u, 0u);
    }
    ramp_finish(f);
    set_base_state();
}

static void
ramps_stone_grain_heat(FILE* f, int edge, unsigned mask) {
    char name[48];
    for (unsigned k = 0; k < 8u; k++) {
        snprintf(name, sizeof name, "stone%s grain %u heat 0-15", edge ? " edge" : "", k);
        ramp_start(name, true);
        for (unsigned v = 0; v < 16u; v++) {
            ramp_add(CELL_MAKE(MAT_STONE, v), k, mask, 0u);
        }
        ramp_finish(f);
    }
}

static void
ramps_stone_heat_grain(FILE* f, int edge, unsigned mask) {
    char name[48];
    for (unsigned v = 0; v < 16u; v++) {
        snprintf(name, sizeof name, "stone%s heat %u grain 0-7", edge ? " edge" : "", v);
        ramp_start(name, true);
        for (unsigned k = 0; k < 8u; k++) {
            ramp_add(CELL_MAKE(MAT_STONE, v), k, mask, 0u);
        }
        ramp_finish(f);
    }
}

static void
ramps_glass_heat(FILE* f, int edge, unsigned mask) {
    char name[48];
    snprintf(name, sizeof name, "glass%s heat 0-15", edge ? " edge" : "");
    ramp_start(name, true);
    for (unsigned v = 0; v < 16u; v++) {
        ramp_add(CELL_MAKE(MAT_GLASS, v), 0u, mask, 0u);
    }
    ramp_finish(f);
}

static void
ramps_glass_shimmer(FILE* f, int edge, unsigned mask) {
    char name[48];
    for (unsigned v = 0; v < 16u; v++) {
        snprintf(name, sizeof name, "glass%s heat %u shimmer 0-255", edge ? " edge" : "", v);
        ramp_start(name, false);
        for (unsigned h = 0; h < 256u; h++) {
            ramp_add(CELL_MAKE(MAT_GLASS, v), h, mask, 0u);
        }
        ramp_finish(f);
    }
}

static void
ramps_stone_and_glass_edge(FILE* f, int edge) {
    const unsigned mask = edge ? MATERIAL_EDGE_UP : 0u;
    ramps_stone_grain_heat(f, edge, mask);
    ramps_stone_heat_grain(f, edge, mask);
    ramps_glass_heat(f, edge, mask);
    ramps_glass_shimmer(f, edge, mask);
}

static void
ramps_stone_and_glass(FILE* f) {
    for (int edge = 0; edge < 2; edge++) {
        ramps_stone_and_glass_edge(f, edge);
    }
}

static void
ramps_wood_and_leaf(FILE* f) {
    char name[48];
    ramp_start("wood grain 0-7", true);
    for (unsigned k = 0; k < 8u; k++) {
        ramp_add(CELL_MAKE(MAT_WOOD, 0), k, 0u, 0u);
    }
    ramp_finish(f);
    ramp_start("wood-leaf gust 0-255", false);
    for (unsigned d = 1; d <= 256u; d++) {
        ramp_add(CELL_MAKE(MAT_WOOD, 0), 0u, 0u, d);
    }
    ramp_finish(f);

    const struct {
        const char* name;
        cell_t c;
    } grains[] = {{"plant", MATX(MATX_PLANT)}, {"ice", MATX(MATX_ICE)}, {"metal", MATX(MATX_METAL)}};

    for (size_t i = 0; i < sizeof grains / sizeof grains[0]; i++) {
        snprintf(name, sizeof name, "%s grain 0-7", grains[i].name);
        ramp_start(name, true);
        for (unsigned k = 0; k < 8u; k++) {
            ramp_add(grains[i].c, k, 0u, 0u);
        }
        ramp_finish(f);
    }
    for (unsigned k = 0; k < 8u; k++) {
        snprintf(name, sizeof name, "leaf grain %u gust 0-255", k);
        ramp_start(name, false);
        for (unsigned d = 1; d <= 256u; d++) {
            ramp_add(MATX(MATX_LEAF), k, 0u, d);
        }
        ramp_finish(f);
    }
    ramp_start("leaf grain 0-7 at rest", true);
    for (unsigned k = 0; k < 8u; k++) {
        ramp_add(MATX(MATX_LEAF), k, 0u, 1u);
    }
    ramp_finish(f);
}

static void
ramps_root_and_gunpowder(FILE* f) {
    char name[48];
    const unsigned root_depths[] = {0u, 2u, 3u, 4u};
    for (unsigned s = 0; s < 4u; s++) {
        snprintf(name, sizeof name, "root age %u grain 0-7", s);
        ramp_start(name, true);
        for (unsigned k = 0; k < 8u; k++) {
            ramp_add(MATX(MATX_ROOT), k, 0u, root_depths[s]);
        }
        ramp_finish(f);
    }
    ramp_start("root age 0-3", true);
    for (unsigned s = 0; s < 4u; s++) {
        ramp_add(MATX(MATX_ROOT), 0u, 0u, root_depths[s]);
    }
    ramp_finish(f);
    ramp_start("gunpowder moisture 0-4", true);
    ramp_add(GUNPOWDER_CELL(2), 0u, 0u, 0u);
    for (unsigned m = 3; m <= 6u; m++) {
        ramp_add(GUNPOWDER_CELL(m), 0u, 0u, 0u);
    }
    ramp_finish(f);
}

static void
report_ramps(FILE* f) {
    fprintf(f, "\nRAMPS: designed ramps whose steps collapsed, and any ramp that returns to an index.\n");
    fprintf(f, "  A merged step's dE is how far apart the two original neighbouring colours were.\n");
    ramps_per_material(f);
    ramps_sand_and_cullet(f);
    ramps_stone_and_glass(f);
    ramps_wood_and_leaf(f);
    ramps_root_and_gunpowder(f);
    fprintf(f, "  %d ramps checked: %d designed ramps collapsed a step, %d returned to an index\n", ramps_total,
            ramps_collapsed, ramps_broken);
}

/* PNG */

static uint32_t crc_table[256];

static void
crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

static uint32_t
crc_update(uint32_t crc, const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void
put_be32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void
png_chunk(FILE* f, const char* type, const uint8_t* data, size_t len) {
    put_be32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len > 0) {
        fwrite(data, 1, len, f);
    }
    uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t*)type, 4);
    crc = crc_update(crc, data, len);
    put_be32(f, crc ^ 0xFFFFFFFFu);
}

static bool
write_png(const char* path, const uint8_t* rgb, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    const uint8_t ihdr[13] = {(uint8_t)(w >> 24),
                              (uint8_t)(w >> 16),
                              (uint8_t)(w >> 8),
                              (uint8_t)w,
                              (uint8_t)(h >> 24),
                              (uint8_t)(h >> 16),
                              (uint8_t)(h >> 8),
                              (uint8_t)h,
                              8,
                              2,
                              0,
                              0,
                              0};
    png_chunk(f, "IHDR", ihdr, sizeof ihdr);

    const size_t raw_len = (size_t)h * (size_t)(w * 3 + 1);
    uint8_t* raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (size_t)(w * 3 + 1)] = 0;
        memcpy(raw + (size_t)y * (size_t)(w * 3 + 1) + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }
    const size_t blocks = raw_len / 65535 + 1;
    const size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t* z = malloc(z_len);
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t pos = 0; pos < raw_len || pos == 0;) {
        const size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[o++] = pos + n >= raw_len ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + pos, n);
        for (size_t i = 0; i < n; i++) {
            s1 = (s1 + raw[pos + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        o += n;
        pos += n;
        if (n == 0) {
            break;
        }
    }
    const uint32_t adler = (s2 << 16) | s1;
    z[o++] = (uint8_t)(adler >> 24);
    z[o++] = (uint8_t)(adler >> 16);
    z[o++] = (uint8_t)(adler >> 8);
    z[o++] = (uint8_t)adler;
    png_chunk(f, "IDAT", z, o);
    png_chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    return fclose(f) == 0;
}

/* a 3x5 font, for labels */

static const uint16_t font_digits[10] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
};

static const uint16_t font_letters[26] = {
    0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126A, 0x5BAD, 0x4927, 0x5FED,
    0x6B6D, 0x2B6A, 0x6BA4, 0x2B73, 0x6BAD, 0x388E, 0x7492, 0x5B6F, 0x5B6A, 0x5BFD, 0x5AAD, 0x5A92, 0x72A7,
};

/* 0 for anything outside A-Z0-9: this font has no lowercase or punctuation
 * glyph, and none of the real glyphs below happens to encode to 0. */
static uint16_t
glyph_for_char(char ch) {
    if (ch >= '0' && ch <= '9') {
        return font_digits[ch - '0'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return font_letters[ch - 'A'];
    }
    return 0;
}

static void
draw_glyph(uint8_t* rgb, int w, int x, int y, uint16_t glyph, int scale, uint32_t colour) {
    for (int gy = 0; gy < 5; gy++) {
        for (int gx = 0; gx < 3; gx++) {
            if (!(glyph & (1u << (14 - (gy * 3 + gx))))) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    uint8_t* p = rgb + ((y + gy * scale + sy) * w + x + gx * scale + sx) * 3;
                    p[0] = (uint8_t)(colour >> 16);
                    p[1] = (uint8_t)(colour >> 8);
                    p[2] = (uint8_t)colour;
                }
            }
        }
    }
}

static void
draw_text(uint8_t* rgb, int w, int x, int y, const char* s, int scale, uint32_t colour) {
    for (; *s != '\0'; s++, x += 4 * scale) {
        const uint16_t glyph = glyph_for_char(*s);
        if (glyph == 0) {
            continue;
        }
        draw_glyph(rgb, w, x, y, glyph, scale, colour);
    }
}

static void
fill(uint8_t* rgb, int w, int x0, int y0, int x1, int y1, uint32_t colour) {
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint8_t* p = rgb + (y * w + x) * 3;
            p[0] = (uint8_t)(colour >> 16);
            p[1] = (uint8_t)(colour >> 8);
            p[2] = (uint8_t)colour;
        }
    }
}

#define SWATCH_W 64
#define SWATCH_H 44

static void
write_swatches(const char* dir) {
    const int w = 16 * SWATCH_W, h = 16 * SWATCH_H;
    uint8_t* rgb = calloc((size_t)w * (size_t)h, 3);
    for (int i = 0; i < PALETTE_SIZE; i++) {
        const int x0 = (i % 16) * SWATCH_W, y0 = (i / 16) * SWATCH_H;
        if (i >= palette_used) {
            fill(rgb, w, x0, y0, x0 + SWATCH_W, y0 + SWATCH_H, 0x202020);
            continue;
        }
        const uint32_t c = key_rgb888(palette[i]);
        fill(rgb, w, x0 + 1, y0 + 1, x0 + SWATCH_W - 1, y0 + SWATCH_H - 1, c);
        const uint32_t ink = key_lab[palette[i]].l > 60.0 ? 0x000000 : 0xFFFFFF;
        char label[8];
        snprintf(label, sizeof label, "%d", i);
        draw_text(rgb, w, x0 + 4, y0 + 5, label, 3, ink);
        draw_text(rgb, w, x0 + 4, y0 + 26, palette_group[i] == G_COUNT ? "UI" : group_tags[palette_group[i]], 3, ink);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/palette_swatches.png", dir);
    write_png(path, rgb, w, h);
    free(rgb);
}

/* 16 colours, ordered dither */

#define EGA_ENTRIES      16
#define EGA_LEVELS       16
#define EGA_REFINE_PASS  8
#define EGA_POINTS_MAX   4096

/* A dithered pair is grainier the further apart its two entries are; this
 * much of their distance is charged against a pair so a closer one wins a
 * near tie. */
#define EGA_GRAIN_CHARGE 0.08

typedef struct {
    uint16_t key[EGA_ENTRIES];
    lin_t lin[EGA_ENTRIES];
    lab_t lab[EGA_ENTRIES];
} ega_palette_t;

typedef struct {
    uint8_t lo, hi, level;
    double error; /* perceived: the pattern's linear-light average */
    double grain; /* distance between the two entries it alternates */
} ega_choice_t;

typedef struct {
    int n;
    uint16_t key[EGA_POINTS_MAX];
    double w[EGA_POINTS_MAX];
} ega_points_t;

static void
ega_set(ega_palette_t* pal, int i, uint16_t key) {
    pal->key[i] = key;
    pal->lin[i] = key_lin[key];
    pal->lab[i] = key_lab[key];
}

static ega_choice_t
ega_nearest_single(const ega_palette_t* pal, lab_t target) {
    ega_choice_t best = {0, 0, 0, 1e30, 0.0};
    for (int i = 0; i < EGA_ENTRIES; i++) {
        const double e = sqrt(dist2(target, pal->lab[i]));
        if (e < best.error) {
            best = (ega_choice_t){(uint8_t)i, (uint8_t)i, 0, e, 0.0};
        }
    }
    return best;
}

/* Every achievable dither level between entries i and j (the level nearest
 * the target's least-squares projection onto that segment, plus one on
 * each side), keeping best/best_cost if one of them wins. */
static void
ega_try_pair(const ega_palette_t* pal, lin_t c, lab_t target, int i, int j, double* best_cost, ega_choice_t* best) {
    const lin_t d = {pal->lin[j].r - pal->lin[i].r, pal->lin[j].g - pal->lin[i].g, pal->lin[j].b - pal->lin[i].b};
    const double den = d.r * d.r + d.g * d.g + d.b * d.b;
    if (den <= 0.0) {
        return;
    }
    const double t = ((c.r - pal->lin[i].r) * d.r + (c.g - pal->lin[i].g) * d.g + (c.b - pal->lin[i].b) * d.b) / den;
    const int centre = (int)lround(t * EGA_LEVELS);
    const double grain = sqrt(dist2(pal->lab[i], pal->lab[j]));
    for (int level = centre - 1; level <= centre + 1; level++) {
        if (level < 1 || level >= EGA_LEVELS) {
            continue;
        }
        const double f = (double)level / EGA_LEVELS;
        const lin_t mix = {pal->lin[i].r + d.r * f, pal->lin[i].g + d.g * f, pal->lin[i].b + d.b * f};
        const double e = sqrt(dist2(target, lin_to_lab(mix)));
        const double cost = e + EGA_GRAIN_CHARGE * grain;
        if (cost < *best_cost) {
            *best_cost = cost;
            *best = (ega_choice_t){(uint8_t)i, (uint8_t)j, (uint8_t)level, e, grain};
        }
    }
}

static ega_choice_t
ega_choose(const ega_palette_t* pal, uint16_t key) {
    const lin_t c = key_lin[key];
    const lab_t target = key_lab[key];
    ega_choice_t best = ega_nearest_single(pal, target);
    double best_cost = best.error;
    for (int i = 0; i < EGA_ENTRIES; i++) {
        for (int j = i + 1; j < EGA_ENTRIES; j++) {
            ega_try_pair(pal, c, target, i, j, &best_cost, &best);
        }
    }
    return best;
}

static double
ega_cost(const ega_palette_t* pal, const ega_points_t* pts) {
    double sum = 0.0;
    for (int i = 0; i < pts->n; i++) {
        const ega_choice_t ch = ega_choose(pal, pts->key[i]);
        sum += pts->w[i] * (ch.error + EGA_GRAIN_CHARGE * ch.grain);
    }
    return sum;
}

/* Seeds every entry past the background on whichever remaining point is
 * furthest (weighted) from every centre chosen so far. */
static void
ega_farthest_seed(lab_t* c, const ega_points_t* pts) {
    for (int j = 1; j < EGA_ENTRIES; j++) {
        int far = 0;
        double fd = -1.0;
        for (int i = 0; i < pts->n; i++) {
            double bd = 1e30;
            for (int m = 0; m < j; m++) {
                const double d = dist2(key_lab[pts->key[i]], c[m]);
                bd = d < bd ? d : bd;
            }
            if (bd * pts->w[i] > fd) {
                fd = bd * pts->w[i];
                far = i;
            }
        }
        c[j] = key_lab[pts->key[far]];
    }
}

static void
ega_lloyd(lab_t* c, const ega_points_t* pts) {
    for (int iter = 0; iter < 40; iter++) {
        double sl[EGA_ENTRIES] = {0}, sa[EGA_ENTRIES] = {0}, sb[EGA_ENTRIES] = {0}, sw[EGA_ENTRIES] = {0};
        for (int i = 0; i < pts->n; i++) {
            const lab_t p = key_lab[pts->key[i]];
            int best = 0;
            for (int j = 1; j < EGA_ENTRIES; j++) {
                if (dist2(p, c[j]) < dist2(p, c[best])) {
                    best = j;
                }
            }
            sl[best] += pts->w[i] * p.l;
            sa[best] += pts->w[i] * p.a;
            sb[best] += pts->w[i] * p.b;
            sw[best] += pts->w[i];
        }
        for (int j = 1; j < EGA_ENTRIES; j++) {
            if (sw[j] > 0.0) {
                c[j] = (lab_t){sl[j] / sw[j], sa[j] / sw[j], sb[j] / sw[j]};
            }
        }
    }
}

/* One trial displacement of entry j along one axis; keeps the move only if
 * it lowers the dithered cost, restoring the entry otherwise. */
static bool
ega_try_move(ega_palette_t* pal, const ega_points_t* pts, int j, int axis, int sign, double step, double* cost) {
    lab_t moved = pal->lab[j];
    const double delta = sign * step;
    moved.l += axis == 0 ? delta : 0.0;
    moved.a += axis == 1 ? delta : 0.0;
    moved.b += axis == 2 ? delta : 0.0;
    const uint16_t key = lab_to_key(moved);
    if (key == pal->key[j]) {
        return false;
    }
    const uint16_t keep = pal->key[j];
    ega_set(pal, j, key);
    const double trial = ega_cost(pal, pts);
    if (trial < *cost) {
        *cost = trial;
        return true;
    }
    ega_set(pal, j, keep);
    return false;
}

static bool
ega_refine_pass(ega_palette_t* pal, const ega_points_t* pts, double step, double* cost) {
    bool improved = false;
    for (int j = 1; j < EGA_ENTRIES; j++) {
        for (int axis = 0; axis < 3; axis++) {
            for (int sign = -1; sign <= 1; sign += 2) {
                improved |= ega_try_move(pal, pts, j, axis, sign, step, cost);
            }
        }
    }
    return improved;
}

static void
ega_refine(ega_palette_t* pal, const ega_points_t* pts, double* cost) {
    static const double steps[] = {8.0, 4.0, 2.0, 1.0};
    for (size_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        for (int pass = 0; pass < EGA_REFINE_PASS; pass++) {
            if (!ega_refine_pass(pal, pts, steps[s], cost)) {
                break;
            }
        }
    }
}

/* Weighted k-means for a start, entry 0 held on the background, then a
 * coordinate search on the dithered cost itself: k-means centres sit inside
 * the data, and a dither can only reach colours between its entries. */
static void
ega_build(ega_palette_t* pal, const ega_points_t* pts, uint16_t background) {
    lab_t c[EGA_ENTRIES];
    c[0] = key_lab[background];
    ega_farthest_seed(c, pts);
    ega_lloyd(c, pts);
    for (int j = 0; j < EGA_ENTRIES; j++) {
        ega_set(pal, j, j == 0 ? background : lab_to_key(c[j]));
    }

    double cost = ega_cost(pal, pts);
    ega_refine(pal, pts, &cost);
}

/* Half on-screen share, half spread evenly over the groups on screen, the
 * same split the 256-entry palette uses. */
static void
ega_points(ega_points_t* pts, uint32_t (*counts)[KEYS]) {
    static double w[KEYS];
    memset(w, 0, sizeof w);
    uint64_t total = 0;
    int groups = 0;
    for (int g = 0; g < G_COUNT; g++) {
        uint64_t n = 0;
        for (int k = 0; k < KEYS; k++) {
            n += counts[g][k];
        }
        total += n;
        groups += n > 0;
    }
    for (int g = 0; g < G_COUNT; g++) {
        int distinct = 0;
        for (int k = 0; k < KEYS; k++) {
            distinct += counts[g][k] != 0;
        }
        for (int k = 0; k < KEYS; k++) {
            if (counts[g][k] != 0) {
                w[k] += 0.5 * (double)counts[g][k] / (double)total + 0.5 / ((double)groups * (double)distinct);
            }
        }
    }
    pts->n = 0;
    for (int k = 0; k < KEYS && pts->n < EGA_POINTS_MAX; k++) {
        if (w[k] > 0.0) {
            pts->key[pts->n] = (uint16_t)k;
            pts->w[pts->n] = w[k];
            pts->n++;
        }
    }
}

static uint32_t
ega_pixel(const ega_palette_t* pal, const ega_choice_t* ch, int px, int py) {
    const uint8_t alpha = ch->level == 0 ? 0u : (uint8_t)(ch->level * 16u);
    return key_rgb888(pal->key[gfx_dither_covers(px, py, alpha) ? ch->hi : ch->lo]);
}

static ega_palette_t ega_global;
static ega_palette_t ega_local[SCENE_COUNT];

/* scene output */

#define PANEL_GAP 8
#define LABEL_H   24

static void
view_put(uint8_t* rgb, int w, int ox, int oy, int px, int py, uint32_t colour) {
    const int vx = PANEL_H - 1 - py, vy = px;
    uint8_t* p = rgb + ((oy + vy) * w + ox + vx) * 3;
    p[0] = (uint8_t)(colour >> 16);
    p[1] = (uint8_t)(colour >> 8);
    p[2] = (uint8_t)colour;
}

static uint32_t scene_used[G_COUNT][KEYS];

static double ega_group_error[G_COUNT], ega_group_grain[G_COUNT], ega_group_max[G_COUNT];
static uint64_t ega_group_px[G_COUNT];

static void
panel_labels(uint8_t* rgb, int w, const char* a, const char* b, const char* c) {
    const int h = VIEW_H + LABEL_H;
    fill(rgb, w, 0, 0, w, h, 0x101010);
    draw_text(rgb, w, 6, 5, a, 3, 0xFFFFFF);
    draw_text(rgb, w, VIEW_W + PANEL_GAP + 6, 5, b, 3, 0xFFFFFF);
    draw_text(rgb, w, 2 * (VIEW_W + PANEL_GAP) + 6, 5, c, 3, 0xFFFFFF);
}

static uint64_t
scene_count_used(gfx_color_t* fb, uint8_t* grp) {
    memset(scene_used, 0, sizeof scene_used);
    uint64_t unseen = 0;
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        const uint16_t key = native_key(fb[i]);
        unseen += !(seen[grp[i]][key] & PIN_NONE);
        scene_used[grp[i]][key]++;
    }
    return unseen;
}

static void
scene_build_choices(int si, ega_choice_t* choice_global, ega_choice_t* choice_local) {
    for (int g = 0; g < G_COUNT; g++) {
        for (int k = 0; k < KEYS; k++) {
            if (scene_used[g][k] != 0) {
                choice_global[k] = ega_choose(&ega_global, (uint16_t)k);
                choice_local[k] = ega_choose(&ega_local[si], (uint16_t)k);
            }
        }
    }
}

typedef struct {
    double sum, worst, ega_sum, ega_worst, local_sum;
} scene_error_stats_t;

static void
scene_plot_pixel(int si, int px, int py, gfx_color_t* fb, uint8_t* grp, const ega_choice_t* choice_global,
                 const ega_choice_t* choice_local, uint8_t* rgb, uint8_t* rgb_local, int w, scene_error_stats_t* st) {
    const group_t g = (group_t)grp[py * PANEL_W + px];
    const uint16_t key = native_key(fb[py * PANEL_W + px]);
    const int idx = map_index[g][key];
    const double e = map_error(g, key);
    st->sum += e;
    st->worst = e > st->worst ? e : st->worst;

    const ega_choice_t* cg = &choice_global[key];
    const ega_choice_t* cl = &choice_local[key];
    st->ega_sum += cg->error;
    st->ega_worst = cg->error > st->ega_worst ? cg->error : st->ega_worst;
    st->local_sum += cl->error;
    ega_group_error[g] += cg->error;
    ega_group_grain[g] += cg->grain;
    ega_group_max[g] = cg->error > ega_group_max[g] ? cg->error : ega_group_max[g];
    ega_group_px[g]++;

    const uint32_t orig = key_rgb888(key);
    const uint32_t ega_rgb = ega_pixel(&ega_global, cg, px, py);
    view_put(rgb, w, 0, LABEL_H, px, py, orig);
    view_put(rgb, w, VIEW_W + PANEL_GAP, LABEL_H, px, py, idx < 0 ? 0xFF00FF : key_rgb888(palette[idx]));
    view_put(rgb, w, 2 * (VIEW_W + PANEL_GAP), LABEL_H, px, py, ega_rgb);
    view_put(rgb_local, w, 0, LABEL_H, px, py, orig);
    view_put(rgb_local, w, VIEW_W + PANEL_GAP, LABEL_H, px, py, ega_rgb);
    view_put(rgb_local, w, 2 * (VIEW_W + PANEL_GAP), LABEL_H, px, py, ega_pixel(&ega_local[si], cl, px, py));
}

static scene_error_stats_t
scene_plot_panels(int si, gfx_color_t* fb, uint8_t* grp, const ega_choice_t* choice_global,
                  const ega_choice_t* choice_local, uint8_t* rgb, uint8_t* rgb_local, int w) {
    scene_error_stats_t st = {0.0, 0.0, 0.0, 0.0, 0.0};
    for (int py = 0; py < PANEL_H; py++) {
        for (int px = 0; px < PANEL_W; px++) {
            scene_plot_pixel(si, px, py, fb, grp, choice_global, choice_local, rgb, rgb_local, w, &st);
        }
    }
    return st;
}

static int
scene_merge_used(void) {
    int distinct = 0;
    for (int g = 0; g < G_COUNT; g++) {
        for (int k = 0; k < KEYS; k++) {
            distinct += scene_used[g][k] != 0;
            used[g][k] += scene_used[g][k];
        }
    }
    return distinct;
}

static void
render_scene(const char* dir, int si, FILE* f, gfx_color_t* fb, uint8_t* grp) {
    const uint64_t unseen = scene_count_used(fb, grp);

    static ega_points_t pts;
    ega_points(&pts, scene_used);
    ega_build(&ega_local[si], &pts, native_key(material_palette()[SAND_EMPTY]));

    static ega_choice_t choice_global[KEYS], choice_local[KEYS];
    scene_build_choices(si, choice_global, choice_local);

    const int w = VIEW_W * 3 + PANEL_GAP * 2, h = VIEW_H + LABEL_H;
    uint8_t* rgb = calloc((size_t)w * (size_t)h, 3);
    uint8_t* rgb_local = calloc((size_t)w * (size_t)h, 3);
    panel_labels(rgb, w, "ORIGINAL", "256 COLOURS", "16 COLOURS DITHERED");
    panel_labels(rgb_local, w, "ORIGINAL", "16 SHARED", "16 FOR THIS SCENE");

    const scene_error_stats_t st = scene_plot_panels(si, fb, grp, choice_global, choice_local, rgb, rgb_local, w);

    char path[512];
    snprintf(path, sizeof path, "%s/scene_%s.png", dir, scenes[si].name);
    write_png(path, rgb, w, h);
    snprintf(path, sizeof path, "%s/scene_%s_16_per_scene.png", dir, scenes[si].name);
    write_png(path, rgb_local, w, h);
    free(rgb);
    free(rgb_local);

    const int distinct = scene_merge_used();
    const double px_total = PANEL_W * PANEL_H;
    fprintf(f,
            "  %-12s colours %4d  256: mean dE %.2f max %.2f  16 shared: mean %.2f max %.2f  16 per scene: mean %.2f"
            "  unswept px %llu\n",
            scenes[si].name, distinct, st.sum / px_total, st.worst, st.ega_sum / px_total, st.ega_worst,
            st.local_sum / px_total, (unsigned long long)unseen);
}

/*
 * production header
 *
 * sand_palette256.h: the 256-colour LUT (UI block 0-15, palette[] 16-255),
 * the shared 16-colour LUT, and one dither choice per 256-entry - the
 * device's GFX_PIXFMT_INDEXED8 path never recomputes any of this. Emitted
 * straight from the same build_palette()/ega_build() output the report
 * above is built from, not re-derived from mapping.csv, so the two cannot
 * drift apart.
 */

/* gfx_color_t is RGB565 with its bytes swapped (gfx_color.h) - the inverse
 * of native_key() above. */
static uint16_t
to_gfx_color(uint16_t native) {
    return (uint16_t)((native >> 8) | (native << 8));
}

/* A generator that half-works is worse than one that fails outright - see
 * docs/Launcher-Architecture.md's "Generated sources". */
static void
validate_palette_or_exit(void) {
    if (palette_used <= UI_ENTRIES || palette_used > PALETTE_SIZE) {
        fprintf(stderr, "build_palette() produced %d entries, expected (%d, %d]\n", palette_used, UI_ENTRIES,
                PALETTE_SIZE);
        exit(1);
    }
}

static void
write_gfx_color_array(FILE* f, const char* decl, const gfx_color_t* data, int count) {
    fprintf(f, "%s", decl);
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s0x%04X,", i % 8 == 0 ? "    " : " ", data[i]);
        if (i % 8 == 7) {
            fprintf(f, "\n");
        }
    }
    fprintf(f, "\n};\n\n");
}

/* Whichever group at key `k` has the on-scene pixel count (used[][]) large
 * enough to win when more than one group's sweep produced this colour;
 * *collisions counts every disagreeing group encountered along the way. */
static int
rgb565_best_group(int k, int* collisions) {
    int best_g = -1;
    uint32_t best_used = 0;
    for (int g = 0; g < G_COUNT; g++) {
        if (!(seen[g][k] & PIN_NONE) || map_index[g][k] < 0) {
            continue;
        }
        if (best_g < 0) {
            best_g = g;
            best_used = used[g][k];
            continue;
        }
        if (map_index[g][k] == map_index[best_g][k]) {
            continue;
        }
        (*collisions)++;
        if (used[g][k] > best_used) {
            best_g = g;
            best_used = used[g][k];
        }
    }
    return best_g;
}

static uint8_t
rgb565_nearest_palette_index(uint16_t k) {
    int best = UI_ENTRIES;
    double bd = 1e30;
    for (int j = UI_ENTRIES; j < palette_used; j++) {
        const double d = dist2(key_lab[k], key_lab[palette[j]]);
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    return (uint8_t)best;
}

/* Reverse map, RGB565 -> palette index: build_palette()'s own per-group
 * OKLab assignment (map_index[][]), not a fresh RGB-distance search. A key
 * no group's sweep reached falls back to one OKLab-nearest search, paid
 * once here rather than never. */
static void
build_rgb565_to_index(uint8_t* out, int* collisions, int* unswept) {
    *collisions = 0;
    *unswept = 0;
    for (int k = 0; k < KEYS; k++) {
        const int best_g = rgb565_best_group(k, collisions);
        if (best_g >= 0) {
            out[k] = (uint8_t)map_index[best_g][k];
            continue;
        }
        (*unswept)++;
        out[k] = rgb565_nearest_palette_index((uint16_t)k);
    }
}

/* palette[] already holds UI_ENTRIES entries (build_palette() seeds them
 * with ui_key()) - a plain 0..255 copy, no separate UI case. Unused
 * trailing entries (palette_used < PALETTE_SIZE) fall back to a valid,
 * already-built index rather than reading unwritten memory. */
static void
build_output_rgb(uint16_t* entry_key, gfx_color_t* sand256_rgb, gfx_color_t* sand16_rgb) {
    for (int i = 0; i < PALETTE_SIZE; i++) {
        entry_key[i] = i < palette_used ? palette[i] : palette[UI_ENTRIES];
        sand256_rgb[i] = to_gfx_color(entry_key[i]);
    }
    for (int i = 0; i < EGA_ENTRIES; i++) {
        sand16_rgb[i] = to_gfx_color(ega_global.key[i]);
    }
}

static void
write_header_banner(FILE* f) {
    fprintf(f,
            "/*=============================================================="
            "=============\n"
            " * GENERATED FILE - do not edit.\n"
            " *\n"
            " *     main/apps/sand/tools/report_shading_palette.sh\n"
            " *\n"
            " * The 256-entry sand palette (UI block 0-%d, sand %d-255) and its\n"
            " * 16-colour dithered counterpart - see docs/sand/Shading-and-Colour.md\n"
            " * and shading_palette.c's own top comment for how these are chosen.\n"
            " *========================================================================"
            "===*/\n"
            "#pragma once\n\n"
            "#include \"gfx/gfx_color.h\"\n"
            "#include \"gfx/gfx_indexed.h\"\n"
            "#include \"gfx/gfx_palette.h\"\n\n"
            "#define SAND_PALETTE_UI_ENTRIES %d\n\n",
            UI_ENTRIES - 1, UI_ENTRIES, UI_ENTRIES);
}

static void
write_reverse_index(FILE* f) {
    static uint8_t rgb565_to_index[KEYS];
    int collisions, unswept;
    build_rgb565_to_index(rgb565_to_index, &collisions, &unswept);
    fprintf(stderr, "reverse index: %d keys, %d group collisions, %d never swept (OKLab-nearest fallback)\n", KEYS,
            collisions, unswept);

    fprintf(f, "#define SAND_RGB565_INDEX_KEYS %d\n\n", KEYS);
    fprintf(f, "static const uint8_t sand_rgb565_to_index[SAND_RGB565_INDEX_KEYS] = {\n");
    for (int k = 0; k < KEYS; k++) {
        fprintf(f, "%s%d,", k % 16 == 0 ? "    " : " ", rgb565_to_index[k]);
        if (k % 16 == 15) {
            fprintf(f, "\n");
        }
    }
    fprintf(f, "\n};\n\n");
}

/* gfx_palette_gen.h (launcher/tools/) bakes every derived dither table from
 * a finished pair of palettes; this file only owns the FINAL sand256/sand16
 * palettes those bakes read, not any one table's own logic. */
static void
write_dither_tables(FILE* f, const gfx_palette_t* p256, const gfx_palette_t* p16) {
    static gfx_color_t dither_table[PALETTE_SIZE * 16];
    gfx_palette_gen_build_dither16(p256, p16, dither_table);
    write_gfx_color_array(f,
                          "static const gfx_color_t sand_palette16_dither_rgb[GFX_INDEXED_PALETTE_SIZE * "
                          "GFX_INDEXED_DITHER16_PHASES] = {\n",
                          dither_table, PALETTE_SIZE * 16);

    static gfx_color_t none_lut[PALETTE_SIZE];
    gfx_palette_gen_build_lut_nearest(p256, p16, none_lut);
    write_gfx_color_array(f, "static const gfx_color_t sand_dither_none_lut[GFX_INDEXED_PALETTE_SIZE] = {\n", none_lut,
                          PALETTE_SIZE);

    static gfx_color_t cell_checker[PALETTE_SIZE * GFX_INDEXED_CELL_CHECKER_PHASES];
    gfx_palette_gen_build_dither_cell(p256, p16, false, cell_checker);
    write_gfx_color_array(f,
                          "static const gfx_color_t sand_dither_cell_checker[GFX_INDEXED_PALETTE_SIZE * "
                          "GFX_INDEXED_CELL_CHECKER_PHASES] = {\n",
                          cell_checker, PALETTE_SIZE * GFX_INDEXED_CELL_CHECKER_PHASES);

    static gfx_color_t cell_bayer2[PALETTE_SIZE * GFX_INDEXED_CELL_BAYER2_PHASES];
    gfx_palette_gen_build_dither_cell(p256, p16, true, cell_bayer2);
    write_gfx_color_array(f,
                          "static const gfx_color_t sand_dither_cell_bayer2[GFX_INDEXED_PALETTE_SIZE * "
                          "GFX_INDEXED_CELL_BAYER2_PHASES] = {\n",
                          cell_bayer2, PALETTE_SIZE * GFX_INDEXED_CELL_BAYER2_PHASES);

    static gfx_color_t pixel_checker2[PALETTE_SIZE * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX];
    gfx_palette_gen_build_dither_checker2(p256, p16, pixel_checker2);
    write_gfx_color_array(f,
                          "static const gfx_color_t sand_dither_pixel_checker2[GFX_INDEXED_PALETTE_SIZE * "
                          "GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX] = {\n",
                          pixel_checker2,
                          PALETTE_SIZE * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX);
}

/* The 256-entry LUT is the palette sand actually registers through
 * gfx_palette.h; the 16-colour dither is a derived presentation of it, not
 * a second independent palette. */
static void
write_final_struct(FILE* f) {
    fprintf(f, "static const gfx_palette_t sand_palette256 = {\"sand256\", sand_palette256_lut, "
               "GFX_INDEXED_PALETTE_SIZE};\n");
}

static void
write_sand_palette_header(const char* path) {
    /* "wb", not "w": this output is committed, and every text file in the
     * tree is LF - text mode would translate it to CRLF on Windows. */
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    validate_palette_or_exit();

    uint16_t entry_key[PALETTE_SIZE];
    gfx_color_t sand256_rgb[PALETTE_SIZE], sand16_rgb[EGA_ENTRIES];
    build_output_rgb(entry_key, sand256_rgb, sand16_rgb);

    write_header_banner(f);
    write_gfx_color_array(f, "static const gfx_color_t sand_palette256_lut[GFX_INDEXED_PALETTE_SIZE] = {\n",
                          sand256_rgb, PALETTE_SIZE);
    write_reverse_index(f);

    const gfx_palette_t sand256_palette = {"sand256", sand256_rgb, PALETTE_SIZE};
    const gfx_palette_t sand16_palette = {"sand16", sand16_rgb, EGA_ENTRIES};
    write_dither_tables(f, &sand256_palette, &sand16_palette);
    write_final_struct(f);

    fclose(f);
}

/* main */

static uint8_t grids[SCENE_COUNT][GRID_W * GRID_H];
static gfx_color_t common_fb[PANEL_W * PANEL_H];
static uint8_t common_grp[PANEL_W * PANEL_H];

/* Sweep, settle every scene, and build both palettes - shared by the normal
 * report below and run_dither_pattern_compare() (dp_*), which needs the same
 * ega_global/palette/map_index and grids[] but writes no header and no
 * stats.txt. */
static void
common_setup(void) {
    crc_init();
    for (int k = 0; k < KEYS; k++) {
        key_lin[k] = rgb_to_lin(key_rgb888((uint16_t)k));
        key_lab[k] = lin_to_lab(key_lin[k]);
    }

    fprintf(stderr, "sweeping material_colours()...\n");
    enumerate();

    fprintf(stderr, "settling scenes...\n");
    for (int si = 0; si < SCENE_COUNT; si++) {
        static sand_t sim;
        sand_init(&sim, grids[si], GRID_W, GRID_H, 0x5A4D0000u + (uint32_t)si);
        sand_set_scatter(&sim, SAND_SCATTER_PER_MATERIAL);
        sand_set_decay(&sim, SAND_DECAY_PER_MATERIAL);
        sand_set_evaporates(&sim, SAND_EVAPORATES_PER_MATERIAL);
        sand_set_soak(&sim, SAND_SOAK_PER_MATERIAL);
        sand_set_mobility(&sim, SAND_MOBILITY_PER_MATERIAL);
        scenes[si].build(&sim);
    }

    /* Usage feeds the palette's weights, so scenes are painted once before it
     * is built and again, for the images, after. */
    for (int si = 0; si < SCENE_COUNT; si++) {
        paint_frame(grids[si], common_fb, common_grp, 1234u + 97u * (uint32_t)si);
        for (int i = 0; i < PANEL_W * PANEL_H; i++) {
            used[common_grp[i]][native_key(common_fb[i])]++;
        }
    }

    fprintf(stderr, "building palette...\n");
    pin_depth_steps();
    build_palette();

    fprintf(stderr, "building the shared 16-colour palette...\n");
    static ega_points_t ega_all;
    ega_points(&ega_all, used);
    ega_build(&ega_global, &ega_all, native_key(material_palette()[SAND_EMPTY]));
}

/*
 * dither pattern exploration (report only - writes no generated header,
 * not part of report_shading_palette.sh's own gate)
 */

/* The single nearest entry, never a blend - what ega_choose() itself starts
 * from before searching for a better pair. Variant (e)'s own choice. */
static ega_choice_t
ega_nearest(const ega_palette_t* pal, uint16_t key) {
    const lab_t target = key_lab[key];
    ega_choice_t best = {0, 0, 0, 1e30, 0.0};
    for (int i = 0; i < EGA_ENTRIES; i++) {
        const double e = sqrt(dist2(target, pal->lab[i]));
        if (e < best.error) {
            best = (ega_choice_t){(uint8_t)i, (uint8_t)i, 0, e, 0.0};
        }
    }
    return best;
}

typedef enum {
    DP_BAYER4X4_PIXEL,   /* a: today's gfx_dither_covers(), per panel pixel */
    DP_CHECKER2X2_PIXEL, /* b: solid lo / 50-50 checker / solid hi, per pixel */
    DP_CHECKER_CELL,     /* c: solid lo or hi, alternating by (cx+cy)&1 */
    DP_BAYER2X2_CELL,    /* d: gfx_dither_covers()'s idea, one cell = one tap */
    DP_NEAREST_CELL,     /* e: no dither, nearest of the 16 per cell */
    DP_VARIANT_COUNT,
} dp_variant_t;

/* draw_text()'s own 3x5 font (below) has no lowercase and no punctuation -
 * a label outside A-Z0-9 and space draws as a gap, not the character. */
static const char* const dp_names[DP_VARIANT_COUNT] = {
    "A BAYER 4X4 PX", "B CHECKER 2X2 PX", "C CHECKER CELL", "D BAYER 2X2 CELL", "E NEAREST CELL",
};

/* gfx_dither4x4 (gfx_color.h) at order 2: the same recursive Bayer
 * construction, its four taps spaced evenly over gfx_dither_level()'s
 * 0..16 domain - 5 achievable coverages per cell (0 to 4 of the
 * surrounding 2x2 block), once every cell renders solid. */
static const int dp_bayer2x2[2][2] = {
    {0, 8},
    {12, 4},
};

/* `ch` is variants a-d's shared blend choice, or e's own never-blended one.
 * `cx`/`cy` are this quality's CELL coordinates; `px`/`py` are absolute
 * panel pixels, the phase gfx_dither_covers() keys off. */
static uint32_t
dp_pixel(dp_variant_t v, const ega_palette_t* pal, const ega_choice_t* ch, int px, int py, int cx, int cy) {
    switch (v) {
        case DP_BAYER4X4_PIXEL: return ega_pixel(pal, ch, px, py);
        case DP_CHECKER2X2_PIXEL: {
            if (ch->level == 0) {
                return key_rgb888(pal->key[ch->lo]);
            }
            const bool hi = ((px + py) & 1) != 0;
            return key_rgb888(pal->key[hi ? ch->hi : ch->lo]);
        }
        case DP_CHECKER_CELL: {
            if (ch->level == 0) {
                return key_rgb888(pal->key[ch->lo]);
            }
            const bool hi = ((cx + cy) & 1) != 0;
            return key_rgb888(pal->key[hi ? ch->hi : ch->lo]);
        }
        case DP_BAYER2X2_CELL: {
            const bool hi = ch->level > dp_bayer2x2[cy & 1][cx & 1];
            return key_rgb888(pal->key[hi ? ch->hi : ch->lo]);
        }
        case DP_NEAREST_CELL:
        default: return key_rgb888(pal->key[ch->lo]);
    }
}

typedef struct {
    const char* name;
    int cell; /* panel pixels per side - CELL_PX is this study's own ULTRA */
} dp_quality_t;

static const dp_quality_t dp_qualities[] = {
    {"cell2", CELL_PX},
    {"cell4", CELL_PX * 2},
};
#define DP_QUALITY_COUNT ((int)(sizeof dp_qualities / sizeof dp_qualities[0]))

/* mixed and water_pool: one scene busy across every material group, one
 * dominated by a single liquid's own dither - scenes[]'s own declaration
 * order (top of this file) fixes these indices. */
static const int dp_scene_idx[] = {5, 1};
#define DP_SCENE_COUNT ((int)(sizeof dp_scene_idx / sizeof dp_scene_idx[0]))

/* One (scene, quality) case: coarsens the already-settled native (CELL_PX)
 * frame to `q->cell` by keeping each block's own top-left sub-cell, an
 * approximation good enough for a dither PATTERN comparison, not a second
 * simulation. Panels left to right: ORIGINAL, 256, each dp_variant_t. */
static void
dp_coarsen_cells(gfx_color_t* fb, uint8_t* grp, int cell, int qgw, int qgh, uint16_t* cell_key, uint8_t* cell_grp) {
    for (int qy = 0; qy < qgh; qy++) {
        for (int qx = 0; qx < qgw; qx++) {
            const int px = qx * cell, py = qy * cell;
            cell_key[qy * qgw + qx] = native_key(fb[py * PANEL_W + px]);
            cell_grp[qy * qgw + qx] = grp[py * PANEL_W + px];
        }
    }
}

static void
dp_choose_cells(int count, const uint16_t* cell_key, ega_choice_t* cell_choice, ega_choice_t* cell_nearest) {
    for (int c = 0; c < count; c++) {
        cell_choice[c] = ega_choose(&ega_global, cell_key[c]);
        cell_nearest[c] = ega_nearest(&ega_global, cell_key[c]);
    }
}

static void
dp_draw_labels(uint8_t* rgb, int w) {
    fill(rgb, w, 0, 0, w, LABEL_H, 0x101010);
    draw_text(rgb, w, 6, 5, "ORIGINAL", 3, 0xFFFFFF);
    draw_text(rgb, w, VIEW_W + PANEL_GAP + 6, 5, "256", 3, 0xFFFFFF);
    for (int v = 0; v < DP_VARIANT_COUNT; v++) {
        draw_text(rgb, w, (2 + v) * (VIEW_W + PANEL_GAP) + 6, 5, dp_names[v], 2, 0xFFFFFF);
    }
}

static void
dp_plot_pixel(int px, int py, int cell, int qgw, uint8_t* rgb, int w, const uint16_t* cell_key, const uint8_t* cell_grp,
              const ega_choice_t* cell_choice, const ega_choice_t* cell_nearest, double* sum_e, double* max_e) {
    const int cx = px / cell, cy = py / cell;
    const int c = cy * qgw + cx;
    const uint16_t key = cell_key[c];
    const int idx = map_index[cell_grp[c]][key];

    view_put(rgb, w, 0, LABEL_H, px, py, key_rgb888(key));
    view_put(rgb, w, VIEW_W + PANEL_GAP, LABEL_H, px, py, idx < 0 ? 0xFF00FF : key_rgb888(palette[idx]));
    for (int v = 0; v < DP_VARIANT_COUNT; v++) {
        const ega_choice_t* ch = v == DP_NEAREST_CELL ? &cell_nearest[c] : &cell_choice[c];
        const uint32_t colour = dp_pixel((dp_variant_t)v, &ega_global, ch, px, py, cx, cy);
        view_put(rgb, w, (2 + v) * (VIEW_W + PANEL_GAP), LABEL_H, px, py, colour);
        sum_e[v] += ch->error;
        max_e[v] = ch->error > max_e[v] ? ch->error : max_e[v];
    }
}

static void
dp_plot_panels(int cell, int qgw, uint8_t* rgb, int w, const uint16_t* cell_key, const uint8_t* cell_grp,
               const ega_choice_t* cell_choice, const ega_choice_t* cell_nearest, double* sum_e, double* max_e) {
    for (int py = 0; py < PANEL_H; py++) {
        for (int px = 0; px < PANEL_W; px++) {
            dp_plot_pixel(px, py, cell, qgw, rgb, w, cell_key, cell_grp, cell_choice, cell_nearest, sum_e, max_e);
        }
    }
}

static void
dp_write_stats(FILE* stats, int si, const dp_quality_t* q, const double* sum_e, const double* max_e) {
    const double n = (double)(PANEL_W * PANEL_H);
    fprintf(stats, "%-10s %-6s", scenes[si].name, q->name);
    for (int v = 0; v < DP_VARIANT_COUNT; v++) {
        fprintf(stats, "  %s mean %.2f max %.2f", dp_names[v], sum_e[v] / n, max_e[v]);
    }
    fprintf(stats, "\n");
}

static void
run_one_dp_case(const char* out_dir, int si, const dp_quality_t* q, FILE* stats) {
    paint_frame(grids[si], common_fb, common_grp, 1234u + 97u * (uint32_t)si);

    const int cell = q->cell;
    const int qgw = PANEL_W / cell, qgh = PANEL_H / cell;
    static uint16_t cell_key[(PANEL_W / CELL_PX) * (PANEL_H / CELL_PX)];
    static uint8_t cell_grp[(PANEL_W / CELL_PX) * (PANEL_H / CELL_PX)];
    dp_coarsen_cells(common_fb, common_grp, cell, qgw, qgh, cell_key, cell_grp);

    static ega_choice_t cell_choice[(PANEL_W / CELL_PX) * (PANEL_H / CELL_PX)];
    static ega_choice_t cell_nearest[(PANEL_W / CELL_PX) * (PANEL_H / CELL_PX)];
    dp_choose_cells(qgw * qgh, cell_key, cell_choice, cell_nearest);

    const int panels = 2 + DP_VARIANT_COUNT;
    const int w = VIEW_W * panels + PANEL_GAP * (panels - 1), h = VIEW_H + LABEL_H;
    uint8_t* rgb = calloc((size_t)w * (size_t)h, 3);
    dp_draw_labels(rgb, w);

    double sum_e[DP_VARIANT_COUNT] = {0}, max_e[DP_VARIANT_COUNT] = {0};
    dp_plot_panels(cell, qgw, rgb, w, cell_key, cell_grp, cell_choice, cell_nearest, sum_e, max_e);

    char path[512];
    snprintf(path, sizeof path, "%s/dither_%s_%s.png", out_dir, scenes[si].name, q->name);
    write_png(path, rgb, w, h);
    free(rgb);
    fprintf(stderr, "wrote %s\n", path);

    dp_write_stats(stats, si, q, sum_e, max_e);
}

static int
run_dither_pattern_compare(const char* out_dir) {
    common_setup();

    char path[512];
    snprintf(path, sizeof path, "%s/dither_patterns.txt", out_dir);
    FILE* stats = fopen(path, "w");
    if (stats == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    fprintf(stats, "dE is OKLab x100, the blend's own perceived error - identical for a/b/c/d\n");
    fprintf(stats, "(same blend ratio, different spatial arrangement); e alone never blends.\n");

    for (int s = 0; s < DP_SCENE_COUNT; s++) {
        for (int q = 0; q < DP_QUALITY_COUNT; q++) {
            run_one_dp_case(out_dir, dp_scene_idx[s], &dp_qualities[q], stats);
        }
    }
    fclose(stats);
    fprintf(stderr, "wrote %s\n", path);
    return 0;
}

static void
report_sweep_group(FILE* f, group_t g, uint8_t* any_seen, uint8_t* any_used, int* total_all, int* total_used) {
    const int n = count_seen(g, PIN_NONE);
    int on_scene = 0;
    double max_e = 0.0, sum_e = 0.0, scene_sum = 0.0;
    uint64_t scene_px = 0;
    for (int k = 0; k < KEYS; k++) {
        if (seen[g][k] & PIN_NONE) {
            any_seen[k] = 1;
            const double e = map_error(g, (uint16_t)k);
            max_e = e > max_e ? e : max_e;
            sum_e += e;
        }
        if (used[g][k] != 0) {
            any_used[k] = 1;
            on_scene++;
            scene_sum += map_error(g, (uint16_t)k) * used[g][k];
            scene_px += used[g][k];
        }
    }
    if (n == 0) {
        return;
    }
    int budget = 0;
    for (int i = UI_ENTRIES; i < palette_used; i++) {
        budget += palette_group[i] == g;
    }
    *total_all += n;
    *total_used += on_scene;
    fprintf(f, "  %-10s %8d %8d %8d %8d %8d %8d %8d %8.2f %9.2f %9.2f\n", group_names[g], n, count_seen(g, PIN_HASH),
            count_seen(g, PIN_MASK), count_seen(g, PIN_DEPTH), count_seen(g, PIN_STATE), on_scene, budget, max_e,
            sum_e / n, scene_px ? scene_sum / (double)scene_px : 0.0);
}

static void
report_sweep_table(FILE* f) {
    fprintf(f, "SWEEP: distinct RGB565 colours per group\n");
    fprintf(f, "  %-10s %8s %8s %8s %8s %8s %8s %8s %8s %9s %9s\n", "group", "all", "hash=0", "mask=0", "depth=0",
            "S0 only", "on-scene", "budget", "maxdE", "meandE", "scenedE");
    int total_all = 0, total_used = 0;
    static uint8_t any_seen[KEYS], any_used[KEYS];
    for (int g = 0; g < G_COUNT; g++) {
        report_sweep_group(f, (group_t)g, any_seen, any_used, &total_all, &total_used);
    }
    int union_all = 0, union_used = 0;
    for (int k = 0; k < KEYS; k++) {
        union_all += any_seen[k];
        union_used += any_used[k];
    }
    fprintf(f, "  summed over groups: %d swept, %d on scenes; distinct across groups: %d swept, %d on scenes\n",
            total_all, total_used, union_all, union_used);
    fprintf(f, "  palette: %d UI + %d sand = %d entries\n", UI_ENTRIES, palette_used - UI_ENTRIES, palette_used);
}

static void
report_scenes(const char* dir, FILE* f, gfx_color_t* fb, uint8_t* grp) {
    fprintf(f, "\nSCENES (dE is OKLab x100; a dithered colour's dE is its pattern's average)\n");
    memset(used, 0, sizeof used);
    for (int si = 0; si < SCENE_COUNT; si++) {
        fprintf(stderr, "rendering %s...\n", scenes[si].name);
        paint_frame(grids[si], fb, grp, 1234u + 97u * (uint32_t)si);
        render_scene(dir, si, f, fb, grp);
    }
    write_swatches(dir);
}

static void
report_ega16(FILE* f) {
    fprintf(f, "\n16 COLOURS, SHARED BY ALL SCENES: per group, over scene pixels\n");
    fprintf(f, "  grain is the dE between the two entries a pixel alternates, 0 for a solid entry\n");
    fprintf(f, "  %-10s %10s %9s %9s %9s\n", "group", "pixels", "mean dE", "max dE", "grain");
    for (int g = 0; g < G_COUNT; g++) {
        if (ega_group_px[g] == 0) {
            continue;
        }
        const double n = (double)ega_group_px[g];
        fprintf(f, "  %-10s %10llu %9.2f %9.2f %9.2f\n", group_names[g], (unsigned long long)ega_group_px[g],
                ega_group_error[g] / n, ega_group_max[g], ega_group_grain[g] / n);
    }
    fprintf(f, "  shared palette:");
    for (int i = 0; i < EGA_ENTRIES; i++) {
        fprintf(f, " %06X", (unsigned)key_rgb888(ega_global.key[i]));
    }
    fprintf(f, "\n");
    for (int si = 0; si < SCENE_COUNT; si++) {
        fprintf(f, "  %-12s", scenes[si].name);
        for (int i = 0; i < EGA_ENTRIES; i++) {
            fprintf(f, " %06X", (unsigned)key_rgb888(ega_local[si].key[i]));
        }
        fprintf(f, "\n");
    }
}

static void
report_palette_listing(FILE* f) {
    fprintf(f, "\nPALETTE (index rgb888 group)\n");
    for (int i = 0; i < palette_used; i++) {
        fprintf(f, "  %3d 0x%06X %s\n", i, (unsigned)key_rgb888(palette[i]),
                palette_group[i] == G_COUNT ? "ui" : group_names[palette_group[i]]);
    }
}

static void
write_mapping_csv(const char* dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/mapping.csv", dir);
    FILE* m = fopen(path, "w");
    if (m == NULL) {
        return;
    }
    fprintf(m, "group,rgb888,index,palette_rgb888,dE\n");
    for (int g = 0; g < G_COUNT; g++) {
        for (int k = 0; k < KEYS; k++) {
            if (seen[g][k] & PIN_NONE) {
                fprintf(m, "%s,0x%06X,%d,0x%06X,%.2f\n", group_names[g], (unsigned)key_rgb888((uint16_t)k),
                        map_index[g][k], (unsigned)key_rgb888(palette[map_index[g][k]]),
                        map_error((group_t)g, (uint16_t)k));
            }
        }
    }
    fclose(m);
}

int
main(int argc, char** argv) {
    if (argc > 2 && strcmp(argv[1], "dither-patterns") == 0) {
        return run_dither_pattern_compare(argv[2]);
    }
    if (argc < 2) {
        fprintf(stderr, "usage: shading_palette <results-dir> [minimax|sse] [header-path]\n");
        fprintf(stderr, "       shading_palette dither-patterns <results-dir>\n");
        return 2;
    }
    const char* dir = argv[1];
    minimax = !(argc > 2 && strcmp(argv[2], "sse") == 0);
    const char* header_path = argc > 3 ? argv[3] : NULL;
    char path[512];
    snprintf(path, sizeof path, "%s/stats.txt", dir);
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    common_setup();

    if (header_path != NULL) {
        fprintf(stderr, "writing %s...\n", header_path);
        write_sand_palette_header(header_path);
    }

    report_sweep_table(f);
    report_ramps(f);
    report_scenes(dir, f, common_fb, common_grp);
    report_ega16(f);
    report_palette_listing(f);
    fclose(f);

    write_mapping_csv(dir);
    fprintf(stderr, "wrote %s\n", dir);
    return 0;
}

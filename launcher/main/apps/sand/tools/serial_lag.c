/*
 * serial_lag - how far a two-core step falls behind a serial one.
 *
 * The two-core split defers work at a stripe boundary: a cell that crossed
 * into a guard row is skipped, and the liquid pass holds back mass that
 * arrived during the phases. A deferral only matters when SERIAL would have
 * done something different in the same step, and that is what this measures:
 * the same scene, from the same seed, stepped twice - once split, once serial
 * - compared cell for cell after every step.
 *
 * Each step starts from the same board, because the two paths are not
 * order-equivalent (see Sand-Simulation.md's "The seam fix") and left to run
 * on they diverge into two valid but different worlds. One step from one
 * state isolates what the split defers: cells it left where serial moved
 * them, and for liquids the mass that sits in a different row.
 *
 * Build and run: main/apps/sand/tools/report_serial_lag.sh
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sand.h"
#include "sand_priv.h"

#define LAG_W     184
#define LAG_H     224
#define LAG_STEPS 40

typedef void (*lag_scene_fn)(sand_t* s);

/* Gravity runs from (gx0, gy0) to (gx1, gy1) across the steps. A held device
 * sweeps through angles rather than sitting on one, and the sweep crosses the
 * lean where build_xflow() swaps which axis is major - the moment the
 * maintainer sees powder pause. Equal endpoints hold one angle. */
typedef struct {
    const char* name;
    lag_scene_fn build;
    int gx0, gy0;
    int gx1, gy1;
} lag_case_t;

static int
lerp_step(int from, int to, int step, int steps) {
    return from + ((to - from) * step) / (steps - 1);
}

static uint8_t cells_before[LAG_W * LAG_H];
static uint8_t cells_split[LAG_W * LAG_H];
static uint8_t cells_serial[LAG_W * LAG_H];
static uint8_t blocks_split[((LAG_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((LAG_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)];
static uint8_t blocks_serial[sizeof blocks_split];

static void
fill_rect(sand_t* s, int x0, int y0, int x1, int y1, cell_t c) {
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            sand_set(s, x, y, c);
        }
    }
}

/* A column of water falling across every stripe boundary: the case the
 * maintainer sees break up on the device. */
static void
scene_water_column(sand_t* s) {
    fill_rect(s, LAG_W / 2 - 6, 0, LAG_W / 2 + 6, LAG_H / 2, CELL_MAKE(MAT_WATER, MASS_MAX));
}

/* A pool deep enough that cross-flow runs every step, straddling boundaries. */
static void
scene_water_pool(sand_t* s) {
    fill_rect(s, 0, LAG_H / 2, LAG_W, LAG_H, CELL_MAKE(MAT_WATER, MASS_MAX));
    fill_rect(s, LAG_W / 3, LAG_H / 4, (LAG_W * 2) / 3, LAG_H / 2, CELL_MAKE(MAT_WATER, MASS_MAX));
}

/* Powder only, so any lag here belongs to the sweep rather than to liquid. */
static void
scene_sand_pile(sand_t* s) {
    fill_rect(s, LAG_W / 4, 0, (LAG_W * 3) / 4, LAG_H / 2, SAND_FIRST_SHADE);
}

/* Gas rising through every boundary, the gas walk's own deferral. */
static void
scene_gas_rise(sand_t* s) {
    fill_rect(s, 0, LAG_H / 2, LAG_W, LAG_H, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
}

/* Sand over water: both passes cross boundaries in the same step. */
static void
scene_mixed(sand_t* s) {
    fill_rect(s, LAG_W / 4, 0, (LAG_W * 3) / 4, LAG_H / 3, SAND_FIRST_SHADE);
    fill_rect(s, 0, (LAG_H * 2) / 3, LAG_W, LAG_H, CELL_MAKE(MAT_WATER, MASS_MAX));
}

static const lag_case_t cases[] = {
    {"water column", scene_water_column, 0, 1000, 0, 1000},
    {"water pool", scene_water_pool, 0, 1000, 0, 1000},
    {"sand pile", scene_sand_pile, 0, 1000, 0, 1000},
    {"gas rise", scene_gas_rise, 0, 1000, 0, 1000},
    {"sand over water", scene_mixed, 0, 1000, 0, 1000},
    /* Landscape: gravity along x, the shipping orientation. */
    {"water column (landscape)", scene_water_column, 1000, 0, 1000, 0},
    {"sand pile (landscape)", scene_sand_pile, 1000, 0, 1000, 0},
    /* A held device never reads an exact axis. These leans are what the
     * accelerometer actually produces, and the liquid flow's diagonal ray
     * crosses rows for any of them - the axis-aligned rows above are the
     * one case where it does not. */
    {"water column (portrait, 17 deg)", scene_water_column, 300, 1000, 300, 1000},
    {"water column (portrait, 35 deg)", scene_water_column, 700, 1000, 700, 1000},
    {"water pool (portrait, 17 deg)", scene_water_pool, 300, 1000, 300, 1000},
    {"water column (landscape, 17 deg)", scene_water_column, 1000, 300, 1000, 300},
    /* Turning the device: gravity sweeps portrait to landscape across the
     * run, crossing every lean between them. */
    {"water column (turning)", scene_water_column, 0, 1000, 1000, 0},
    {"water pool (turning)", scene_water_pool, 0, 1000, 1000, 0},
    {"sand pile (turning)", scene_sand_pile, 0, 1000, 1000, 0},
};

/* Mass a cell holds, for the row-mass comparison: a liquid carries its mass in
 * the variant nibble, anything else counts as one. */
static int
cell_mass(cell_t c) {
    if (CELL_IS_EMPTY(c)) {
        return 0;
    }
    return ((liquid_mask() >> CELL_MATERIAL(c)) & 1u) != 0 ? CELL_VARIANT(c) : 1;
}

typedef struct {
    int steps_differing;
    int worst_cells;
    int worst_row_mass;
    int worst_held;  /* cells serial moved and the split left sitting */
    int worst_early; /* cells the split moved and serial did not */
    int first_step;
} lag_result_t;

typedef struct {
    int cells, row_mass, held, early;
} lag_step_t;

/* One step's difference between the two arms, against the board they both
 * started from: a cell serial moved and the split did not is held back, the
 * reverse is early, and a row's mass difference is the shape a liquid delay
 * takes. */
static lag_step_t
compare_step(const sand_t* split, const sand_t* serial) {
    lag_step_t d = {0, 0, 0, 0};

    for (int y = 0; y < LAG_H; y++) {
        int mass_split = 0, mass_serial = 0;
        for (int x = 0; x < LAG_W; x++) {
            const cell_t a = sand_at(split, x, y);
            const cell_t b = sand_at(serial, x, y);
            const cell_t was = cells_before[(size_t)y * (size_t)LAG_W + (size_t)x];
            if (a != b) {
                d.cells++;
            }
            if (b != was && a == was) {
                d.held++;
            } else if (a != was && b == was) {
                d.early++;
            }
            mass_split += cell_mass(a);
            mass_serial += cell_mass(b);
        }
        d.row_mass += (mass_split > mass_serial) ? mass_split - mass_serial : mass_serial - mass_split;
    }
    return d;
}

/* A scene's row is the worst any of its steps managed, so a single bad step
 * is not averaged away by the ones around it. */
static void
keep_worst(lag_result_t* out, lag_step_t d, int step) {
    if (d.cells == 0) {
        return;
    }
    out->steps_differing++;
    if (out->first_step < 0) {
        out->first_step = step;
    }
    if (d.cells > out->worst_cells) {
        out->worst_cells = d.cells;
    }
    if (d.row_mass > out->worst_row_mass) {
        out->worst_row_mass = d.row_mass;
    }
    if (d.held > out->worst_held) {
        out->worst_held = d.held;
    }
    if (d.early > out->worst_early) {
        out->worst_early = d.early;
    }
}

static lag_result_t
measure(const lag_case_t* c) {
    sand_t split, serial;
    sand_init(&split, cells_split, LAG_W, LAG_H, 7u);
    sand_init(&serial, cells_serial, LAG_W, LAG_H, 7u);
    sand_enable_sleeping(&split, blocks_split);
    sand_enable_sleeping(&serial, blocks_serial);
    c->build(&split);
    c->build(&serial);
    /* Both arms draw through the hash, so the boards differ only by the order
     * the split imposes - not by the sequence a serial pass would draw. */
    sand_force_hashed_rng(true);

    lag_result_t out = {0, 0, 0, 0, 0, -1};
    for (int step = 0; step < LAG_STEPS; step++) {
        /* Both arms start this step from the SAME board: the split and the
         * serial path are not order-equivalent, so left to run on they
         * diverge into two valid but different worlds and the difference
         * stops meaning anything. One step from one state is the question. */
        memcpy(cells_before, cells_serial, sizeof cells_before);
        memcpy(cells_split, cells_serial, sizeof cells_split);
        memcpy(blocks_split, blocks_serial, sizeof blocks_split);
        split.step_phase = serial.step_phase;

        sand_set_two_core_step(true);
        const int gx = lerp_step(c->gx0, c->gx1, step, LAG_STEPS);
        const int gy = lerp_step(c->gy0, c->gy1, step, LAG_STEPS);
        sand_step(&split, gx, gy, 0);
        sand_set_two_core_step(false);
        sand_step(&serial, gx, gy, 0);

        keep_worst(&out, compare_step(&split, &serial), step);
    }
    return out;
}

int
main(void) {
    printf("# Two-core step against the serial path\n\n");
    printf("%d steps per scene on a %dx%d grid, same seed both ways.\n\n", LAG_STEPS, LAG_W, LAG_H);
    printf("| Scene | Steps differing | Worst cells | Worst held back | Worst early | Worst row-mass |\n");
    printf("|---|---:|---:|---:|---:|---:|\n");

    int total = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const lag_result_t r = measure(&cases[i]);
        total += r.steps_differing;
        printf("| %s | %d | %d | %d | %d | %d |\n", cases[i].name, r.steps_differing, r.worst_cells, r.worst_held,
               r.worst_early, r.worst_row_mass);
    }

    printf("\n%s\n", total == 0 ? "The split matches the serial path in every scene and step."
                                : "Held back is the stall: cells serial moved and the split left sitting.");
    return 0;
}

/* Host pass timing; the linker wrapper leaves every other pass serial. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "sand_priv.h"
#include "suite_sand_common.h"
#include "suite_sand_scenes.h"

static bool split_liquid;
static double liquid_us;

static double
now_us(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart * 1000000.0 / (double)frequency.QuadPart;
#else
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000000.0 + (double)t.tv_nsec / 1000.0;
#endif
}

void __real_sand_step_liquids(sand_t* s, const xflow_t* flow, int dx, int dy);

void
__wrap_sand_step_liquids(sand_t* s, const xflow_t* flow, int dx, int dy) {
    sand_set_two_core_step(split_liquid);
    const double start = now_us();
    __real_sand_step_liquids(s, flow, dx, dy);
    liquid_us += now_us() - start;
    sand_set_two_core_step(false);
}

void
setUp(void) {}

void
tearDown(void) {}

static void
measure(bool slope, bool split) {
    const size_t block_count =
        ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H);
    uint8_t* cells = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(block_count);
    if (cells == NULL || blocks == NULL) {
        exit(1);
    }
    sand_t s;
    sand_init(&s, cells, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&s, blocks);
    split_liquid = false;
    if (slope) {
        build_water_slope_scene(&s);
    } else {
        build_submerged_pile_scene(&s);
    }
    split_liquid = split;
    liquid_us = 0;
    const unsigned moves = sand_liquid_moves;
    const unsigned probes = sand_liquid_crossflow_probes;
    const int steps = slope ? WATER_SLOPE_COVER_STEPS : 600;
    for (int i = 0; i < steps; i++) {
        if (slope) {
            water_slope_water_pour(&s, i);
        }
        sand_step(&s, LANDSCAPE_GX, 0, 0);
    }
    printf("%s,%s,%d,%.3f,%u,%u\n", slope ? "water-slope" : "submerged-pile", split ? "split" : "serial", steps,
           liquid_us / steps, sand_liquid_moves - moves, sand_liquid_crossflow_probes - probes);
    free(blocks);
    free(cells);
}

int
main(void) {
    puts("scene,mode,steps,liquid_us_per_step,moves,probes");
    for (int repeat = 0; repeat < 3; repeat++) {
        measure(true, false);
        measure(true, true);
        measure(false, false);
        measure(false, true);
    }
    return 0;
}

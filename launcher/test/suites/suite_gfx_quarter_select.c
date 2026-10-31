#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "esp_cpu.h"
#include "esp_log.h"

#include "core/memory.h"
#include "gfx/gfx.h"
#include "gfx/present/expand/gfx_quarter_select.h"

/* A half row: the blended expand's unit of work. */
#define ROW_PIXELS (GFX_WIDTH / 2)
#define ROW_STEPS  (ROW_PIXELS / GFX_QUARTER_LANES)
#define ROWS       (GFX_HEIGHT / 2)
#define CHECKS     64
#define ALIGN      GFX_QUARTER_ALIGN
#define ROW_BYTES  (ROW_PIXELS * sizeof(uint16_t))

_Static_assert(ROW_PIXELS % GFX_QUARTER_LANES == 0, "a half row is whole PIE steps");

/* A block of `bytes` at ALIGN; blocks[n] is what to free. */
static void*
aligned(size_t bytes, memory_kind_t kind, void** block) {
    const memory_aligned_t got = memory_alloc_aligned(bytes, ALIGN, kind);
    TEST_ASSERT_NOT_NULL(got.block);
    *block = got.block;
    return got.at;
}

/* A pixel for check `check`: every fourth check runs the channel extremes. */
static uint16_t
check_pixel(int check) {
    if (check % 4 != 0) {
        return (uint16_t)rand();
    }
    return rand() & 1 ? UINT16_MAX : 0;
}

/* Check `check`'s rows and keep bytes: none kept, all kept, then random. */
static void
fill_check(int check, uint16_t* mine, uint16_t* other, uint8_t* keep) {
    for (int x = 0; x < ROW_PIXELS; x++) {
        mine[x] = check_pixel(check);
        other[x] = check_pixel(check);
    }
    static const uint8_t fixed[] = {0, UINT8_MAX};
    for (int step = 0; step < ROW_STEPS; step++) {
        keep[step] = check % 3 < 2 ? fixed[check % 3] : (uint8_t)rand();
    }
}

static void
test_quarter_select_matches_scalar(void) {
    void* blocks[4];
    uint16_t* mine = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[0]);
    uint16_t* other = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[1]);
    uint16_t* pie = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[2]);
    uint16_t* scalar = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[3]);
    uint8_t keep[ROW_STEPS];
    srand(1);
    for (int check = 0; check < CHECKS; check++) {
        fill_check(check, mine, other, keep);
        gfx_quarter_select_scalar(mine, other, keep, scalar, ROW_STEPS);
        gfx_quarter_select(mine, other, keep, pie, ROW_STEPS);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(scalar, pie, ROW_PIXELS);
    }
    for (int i = 0; i < 4; i++) {
        memory_free(blocks[i]);
    }
}

/* One frame of half rows, each blended with the next, from PSRAM as the
 * present reads the half picture, into one internal row as a strip. */
static void
test_quarter_select_cost(void) {
    void* blocks[4];
    uint16_t* frame = aligned((size_t)ROW_PIXELS * (ROWS + 1) * sizeof(uint16_t), MEMORY_PSRAM, &blocks[0]);
    uint16_t* out = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[1]);
    uint8_t* keep = aligned((size_t)ROW_STEPS * ROWS, MEMORY_INTERNAL, &blocks[2]);
    for (size_t i = 0; i < (size_t)ROW_PIXELS * (ROWS + 1); i++) {
        frame[i] = (uint16_t)rand();
    }
    for (size_t i = 0; i < (size_t)ROW_STEPS * ROWS; i++) {
        keep[i] = rand() % 8 == 0 ? (uint8_t)rand() : 0;
    }
    uint32_t start = esp_cpu_get_cycle_count();
    for (int y = 0; y < ROWS; y++) {
        gfx_quarter_select_scalar(frame + ((size_t)y * ROW_PIXELS), frame + ((size_t)(y + 1) * ROW_PIXELS),
                                  keep + (y * ROW_STEPS), out, ROW_STEPS);
    }
    const uint32_t scalar_cycles = esp_cpu_get_cycle_count() - start;
    start = esp_cpu_get_cycle_count();
    for (int y = 0; y < ROWS; y++) {
        gfx_quarter_select(frame + ((size_t)y * ROW_PIXELS), frame + ((size_t)(y + 1) * ROW_PIXELS),
                           keep + (y * ROW_STEPS), out, ROW_STEPS);
    }
    const uint32_t pie_cycles = esp_cpu_get_cycle_count() - start;
    /* The same work on two internal rows: the kernel's own cost, no PSRAM. */
    uint16_t* rows = aligned(2 * ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[3]);
    memcpy(rows, frame, 2 * ROW_PIXELS * sizeof(uint16_t));
    start = esp_cpu_get_cycle_count();
    for (int y = 0; y < ROWS; y++) {
        gfx_quarter_select(rows, rows + ROW_PIXELS, keep + (y * ROW_STEPS), out, ROW_STEPS);
    }
    const uint32_t internal_cycles = esp_cpu_get_cycle_count() - start;
    const double pixels = (double)ROW_PIXELS * ROWS;
    ESP_LOGI("quarter_select",
             "quarter select, one half frame: scalar %.2f cycles/pixel, pie %.2f cycles/pixel from PSRAM, %.2f from "
             "internal rows",
             scalar_cycles / pixels, pie_cycles / pixels, internal_cycles / pixels);
    for (int i = 0; i < 4; i++) {
        memory_free(blocks[i]);
    }
}

/* The spread's rows: mine and both neighbours, the panel row out from each
 * body, and the left keeps then the right. */
typedef struct {
    void* blocks[5];
    uint16_t *mine, *left, *right, *pie, *scalar;
    uint8_t keeps[2 * ROW_STEPS];
} spread_rows_t;

static void
spread_rows_alloc(spread_rows_t* rows) {
    rows->mine = aligned(ROW_BYTES, MEMORY_INTERNAL, &rows->blocks[0]);
    rows->left = aligned(ROW_BYTES, MEMORY_INTERNAL, &rows->blocks[1]);
    rows->right = aligned(ROW_BYTES, MEMORY_INTERNAL, &rows->blocks[2]);
    rows->pie = aligned(2 * ROW_BYTES, MEMORY_INTERNAL, &rows->blocks[3]);
    rows->scalar = aligned(2 * ROW_BYTES, MEMORY_INTERNAL, &rows->blocks[4]);
}

static void
spread_rows_free(spread_rows_t* rows) {
    for (int i = 0; i < 5; i++) {
        memory_free(rows->blocks[i]);
    }
}

static void
test_quarter_spread_matches_scalar(void) {
    spread_rows_t rows;
    spread_rows_alloc(&rows);
    srand(2);
    for (int check = 0; check < CHECKS; check++) {
        fill_check(check, rows.mine, rows.left, rows.keeps);
        fill_check(check / 3, rows.right, rows.right, rows.keeps + ROW_STEPS); /* the right keeps differ */
        gfx_quarter_spread_scalar(rows.mine, rows.left, rows.right, rows.keeps, rows.scalar, ROW_STEPS);
        gfx_quarter_spread(rows.mine, rows.left, rows.right, rows.keeps, rows.pie, ROW_STEPS);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(rows.scalar, rows.pie, 2 * ROW_PIXELS);
    }
    spread_rows_free(&rows);
}

/* One frame of half rows doubled across, all internal: the spread against
 * two selects into internal rows and the interleave that zips them. */
static void
test_quarter_spread_cost(void) {
    spread_rows_t rows;
    spread_rows_alloc(&rows);
    for (int x = 0; x < ROW_PIXELS; x++) {
        rows.mine[x] = (uint16_t)rand();
        rows.left[x] = (uint16_t)rand();
        rows.right[x] = (uint16_t)rand();
    }
    for (int step = 0; step < 2 * ROW_STEPS; step++) {
        rows.keeps[step] = rand() % 8 == 0 ? (uint8_t)rand() : 0;
    }
    uint16_t* even = rows.scalar;
    uint16_t* odd = rows.scalar + ROW_PIXELS;
    uint32_t start = esp_cpu_get_cycle_count();
    for (int y = 0; y < ROWS; y++) {
        gfx_quarter_select(rows.mine, rows.left, rows.keeps, even, ROW_STEPS);
        gfx_quarter_select(rows.mine, rows.right, rows.keeps + ROW_STEPS, odd, ROW_STEPS);
        for (int x = 0; x < ROW_PIXELS; x++) {
            rows.pie[2 * x] = even[x];
            rows.pie[(2 * x) + 1] = odd[x];
        }
    }
    const uint32_t select_cycles = esp_cpu_get_cycle_count() - start;
    start = esp_cpu_get_cycle_count();
    for (int y = 0; y < ROWS; y++) {
        gfx_quarter_spread(rows.mine, rows.left, rows.right, rows.keeps, rows.pie, ROW_STEPS);
    }
    const uint32_t spread_cycles = esp_cpu_get_cycle_count() - start;
    const double pixels = (double)ROW_PIXELS * ROWS;
    ESP_LOGI("quarter_select",
             "doubled across, one half frame: two selects and the zip %.2f cycles/half pixel, spread %.2f",
             select_cycles / pixels, spread_cycles / pixels);
    spread_rows_free(&rows);
}

void
run_gfx_quarter_select_suite(void) {
    RUN_TEST(test_quarter_select_matches_scalar);
    RUN_TEST(test_quarter_select_cost);
    RUN_TEST(test_quarter_spread_matches_scalar);
    RUN_TEST(test_quarter_spread_cost);
}

#else

void
run_gfx_quarter_select_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_gfx_quarter_select_suite);

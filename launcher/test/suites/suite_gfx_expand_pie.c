#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "esp_cpu.h"
#include "esp_log.h"

#include "core/memory.h"
#include "gfx/gfx.h"
#include "gfx/present/expand/gfx_expand_pie.h"
#include "image/pixel/rgb565.h"

/* A half row: the blended expand's unit of work. */
#define ROW_PIXELS (GFX_WIDTH / 2)
#define ROW_STEPS  (ROW_PIXELS / GFX_PIE_LANES)
#define ROWS       (GFX_HEIGHT / 2)
#define CHECKS     64
#define PIE_ALIGN  16

_Static_assert(ROW_PIXELS % GFX_PIE_LANES == 0, "a half row is whole PIE steps");

/* The scalar expand's choice, the reference the PIE routine must match. */
static uint16_t
scalar_quarter_select(uint16_t mine, uint16_t other, bool keep) {
    if (keep) {
        return mine;
    }
    return rgb565be_unspread(rgb565_spread_quarter(rgb565be_spread(mine), rgb565be_spread(other)));
}

static void
scalar_row(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out) {
    for (int x = 0; x < ROW_PIXELS; x++) {
        out[x] = scalar_quarter_select(mine[x], other[x], (keep[x / GFX_PIE_LANES] >> (x % GFX_PIE_LANES)) & 1U);
    }
}

/* A block of `bytes` at a PIE_ALIGN boundary; *block is what to free. */
static void*
aligned(size_t bytes, memory_kind_t kind, void** block) {
    *block = memory_alloc(bytes + PIE_ALIGN - 1, kind);
    TEST_ASSERT_NOT_NULL(*block);
    return (void*)(((uintptr_t)*block + PIE_ALIGN - 1) & ~(uintptr_t)(PIE_ALIGN - 1));
}

static void
test_pie_quarter_select_matches_scalar(void) {
    void* blocks[4];
    uint16_t* mine = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[0]);
    uint16_t* other = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[1]);
    uint16_t* pie = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[2]);
    uint16_t* scalar = aligned(ROW_PIXELS * sizeof(uint16_t), MEMORY_INTERNAL, &blocks[3]);
    uint8_t keep[ROW_STEPS];
    srand(1);
    for (int check = 0; check < CHECKS; check++) {
        for (int x = 0; x < ROW_PIXELS; x++) {
            /* Every fourth check runs the channel extremes. */
            mine[x] = check % 4 == 0 ? (rand() & 1 ? UINT16_MAX : 0) : (uint16_t)rand();
            other[x] = check % 4 == 0 ? (rand() & 1 ? UINT16_MAX : 0) : (uint16_t)rand();
        }
        for (int step = 0; step < ROW_STEPS; step++) {
            keep[step] = check % 3 == 0 ? 0 : check % 3 == 1 ? UINT8_MAX : (uint8_t)rand();
        }
        scalar_row(mine, other, keep, scalar);
        gfx_pie_quarter_select(mine, other, keep, pie, ROW_STEPS);
        for (int x = 0; x < ROW_PIXELS; x++) {
            if (pie[x] != scalar[x]) {
                ESP_LOGE("expand_pie", "check %d x %d mine %04x other %04x keep %d: pie %04x scalar %04x", check, x,
                         mine[x], other[x], (keep[x / GFX_PIE_LANES] >> (x % GFX_PIE_LANES)) & 1, pie[x], scalar[x]);
            }
        }
        TEST_ASSERT_EQUAL_HEX16_ARRAY(scalar, pie, ROW_PIXELS);
    }
    for (int i = 0; i < 4; i++) {
        memory_free(blocks[i]);
    }
}

/* One frame of half rows, each blended with the next, from PSRAM as the
 * present reads the half picture, into one internal row as a strip. */
static void
test_pie_quarter_select_cost(void) {
    void* blocks[3];
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
        scalar_row(frame + ((size_t)y * ROW_PIXELS), frame + ((size_t)(y + 1) * ROW_PIXELS), keep + (y * ROW_STEPS),
                   out);
    }
    const uint32_t scalar_cycles = esp_cpu_get_cycle_count() - start;
    start = esp_cpu_get_cycle_count();
    for (int y = 0; y < ROWS; y++) {
        gfx_pie_quarter_select(frame + ((size_t)y * ROW_PIXELS), frame + ((size_t)(y + 1) * ROW_PIXELS),
                               keep + (y * ROW_STEPS), out, ROW_STEPS);
    }
    const uint32_t pie_cycles = esp_cpu_get_cycle_count() - start;
    const double pixels = (double)ROW_PIXELS * ROWS;
    ESP_LOGI("expand_pie", "quarter select, one half frame: scalar %.2f cycles/pixel, pie %.2f cycles/pixel",
             scalar_cycles / pixels, pie_cycles / pixels);
    for (int i = 0; i < 3; i++) {
        memory_free(blocks[i]);
    }
}

void
run_gfx_expand_pie_suite(void) {
    RUN_TEST(test_pie_quarter_select_matches_scalar);
    RUN_TEST(test_pie_quarter_select_cost);
}

#else

void
run_gfx_expand_pie_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_gfx_expand_pie_suite);

/*
 * Device-only suite, on request: what a 1-bit card mask costs a span. One
 * core fills a render-sized colour and depth target in PSRAM, where the mesh
 * raster keeps its own, with square cards of side 4 to 64 pixels, one span
 * per card row: with the Gouraud fill, and with the same fill testing a
 * 32x32 1-bit mask per pixel. The masks are two cards baked from Sponza's
 * vines. Each line gives the median cycles of a whole target with its spans,
 * pixels and written pixels, so cycles per span, per pixel and per written
 * pixel can be fitted.
 *
 * Runs under DEVICE_BUILD only - needs PSRAM and a cycle counter.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "testkit/bench.h"
#include "unity.h"

#include "esp_attr.h"
#include "esp_log.h"

#include "../sponza_content.h"
#include "core/memory.h"
#include "render/r3d_span_internal.h"

static const char* TAG = "masked_span";

#define TARGET_WIDTH  SPONZA_RENDER_WIDTH
#define TARGET_HEIGHT SPONZA_RENDER_HEIGHT
#define MASK_SIDE     32
#define MASK_COUNT    2
/* Mask coordinates step in 16.16 fixed point across a span. */
#define MASK_U_SHIFT  16

/* Two 32x32 card masks, one bit per texel; their opaque share in mask_permille. */
static const uint32_t masks[MASK_COUNT][MASK_SIDE] = {
    {0x00003E00U, 0x0003F800U, 0x00EFE000U, 0x03FFC000U, 0x0877E0C0U, 0x01C003FCU, 0x2FFF07FDU, 0x1FFFFB7BU,
     0xFFF1FFFFU, 0xFFCBF7FFU, 0xCB01C3FFU, 0x8003CFFFU, 0x81FF00FFU, 0x03E007E7U, 0x00003FF0U, 0x04001C00U,
     0x04000000U, 0x08040000U, 0x18380000U, 0x10E00000U, 0x01E00000U, 0x07800000U, 0x0E000000U, 0x18000000U,
     0xF0000000U, 0xC0000000U, 0x40000000U, 0x80000000U, 0x80000000U, 0x00000000U, 0x0000001FU, 0x000004FFU},
    {0xFFFCB800U, 0xFFFFB800U, 0x9FFF9800U, 0x5FFFB800U, 0xFFBFF000U, 0xBFFFF800U, 0xEDFFF800U, 0xB9BFF800U,
     0xF3FFF806U, 0xFFDFF406U, 0xFF1FFC06U, 0xF27BFE06U, 0x36FFFE07U, 0x7FFFFE0FU, 0x3FEFFF1FU, 0x3FEFBF0EU,
     0x3EFF0D01U, 0x3FFFFFB7U, 0x31DFFF9FU, 0x31FF5F1FU, 0x30EF3F1FU, 0x31FF111FU, 0x30FFF03CU, 0x74EFE07AU,
     0x717FC066U, 0x723F804CU, 0x761F800CU, 0x747FC07CU, 0x71FFC078U, 0x7BFFE0F8U, 0x7FFFE0FDU, 0x7F3F8CFFU},
};
static const int mask_permille[MASK_COUNT] = {285, 631};

static const int card_sides[] = {4, 8, 16, 32, 64};

/* A Gouraud ramp across every span: start colour and per-pixel steps. */
#define RAMP_R                30000
#define RAMP_G                40000
#define RAMP_B                20000
#define RAMP_DZ               3
#define RAMP_DR               40
#define RAMP_DG               (-30)
#define RAMP_DB               20
/* Each timed run draws this much nearer than the last, so every run passes
 * the depth test; the target's widest card ramp stays below it. */
#define RUN_DEPTH_STEP        (TARGET_WIDTH * RAMP_DZ + MASK_SIDE)

/* The fills run from IRAM as the draw path's do, so the mask test is timed
 * where it would run, without spending the draw path's own budget.
 * IRAM budget: diag 223 B */
#define MASKED_SPAN_IRAM_ATTR IRAM_ATTR __attribute__((aligned(4)))

#if defined(__XTENSA__) && __has_attribute(optimize)
#define MASKED_FILL_ATTR __attribute__((optimize("no-schedule-insns")))
#else
#define MASKED_FILL_ATTR
#endif

/* r3d_span's Gouraud fill loop, so the masked arm below differs from it by
 * the mask test alone. */
static MASKED_SPAN_IRAM_ATTR __attribute__((noinline)) MASKED_FILL_ATTR int
fill_opaque(uint16_t* depth, uint16_t* color, int count, int32_t z, int32_t r, int32_t g, int32_t b, int32_t dz,
            int32_t dr, int32_t dg, int32_t db) {
    for (int i = 0; i < count; i++) {
        const uint16_t zq = (uint16_t)(z >> R3D_DEPTH_SHIFT);
        if (zq > depth[i]) {
            depth[i] = zq;
            color[i] = r3d_span_pack(r, g, b);
        }
        z += dz;
        r += dr;
        g += dg;
        b += db;
    }
    return count;
}

static MASKED_SPAN_IRAM_ATTR __attribute__((noinline)) MASKED_FILL_ATTR int
fill_masked(uint16_t* depth, uint16_t* color, int count, int32_t z, int32_t r, int32_t g, int32_t b, int32_t dz,
            int32_t dr, int32_t dg, int32_t db, uint32_t mask_row, uint32_t u, uint32_t du) {
    int written = 0;
    for (int i = 0; i < count; i++) {
        const uint16_t zq = (uint16_t)(z >> R3D_DEPTH_SHIFT);
        if (((mask_row >> (u >> MASK_U_SHIFT)) & 1U) && zq > depth[i]) {
            depth[i] = zq;
            color[i] = r3d_span_pack(r, g, b);
            written++;
        }
        z += dz;
        r += dr;
        g += dg;
        b += db;
        u += du;
    }
    return written;
}

typedef struct {
    uint16_t* depth;
    uint16_t* color;
    int side;
    int mask; /* -1: opaque */
    int run;
    int spans, pixels, written;
} cards_t;

/* Square cards of side `side` tiled over the target, one span per card row;
 * each row's setup (mask row, u start and step) is timed with it. */
static void
fill_cards(void* ctx) {
    cards_t* c = ctx;
    const uint32_t du = ((uint32_t)MASK_SIDE << MASK_U_SHIFT) / (uint32_t)c->side;
    c->run++;
    c->spans = c->pixels = c->written = 0;
    for (int y = 0; y + c->side <= TARGET_HEIGHT; y += c->side) {
        for (int row = 0; row < c->side; row++) {
            const int v = (row * MASK_SIDE) / c->side;
            uint16_t* drow = c->depth + ((y + row) * TARGET_WIDTH);
            uint16_t* crow = c->color + ((y + row) * TARGET_WIDTH);
            for (int x = 0; x + c->side <= TARGET_WIDTH; x += c->side) {
                const int32_t z = (c->run * RUN_DEPTH_STEP + x) << R3D_DEPTH_SHIFT;
                c->written += c->mask < 0 ? fill_opaque(drow + x, crow + x, c->side, z, RAMP_R, RAMP_G, RAMP_B, RAMP_DZ,
                                                        RAMP_DR, RAMP_DG, RAMP_DB)
                                          : fill_masked(drow + x, crow + x, c->side, z, RAMP_R, RAMP_G, RAMP_B, RAMP_DZ,
                                                        RAMP_DR, RAMP_DG, RAMP_DB, masks[c->mask][v], du / 2U, du);
                c->spans++;
                c->pixels += c->side;
            }
        }
    }
}

static void
test_masked_span_cost(void) {
    const size_t bytes = (size_t)TARGET_WIDTH * TARGET_HEIGHT * sizeof(uint16_t);
    uint16_t* depth = memory_alloc(bytes, MEMORY_PSRAM);
    uint16_t* color = memory_alloc(bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(depth);
    TEST_ASSERT_NOT_NULL(color);
    for (int s = 0; s < (int)(sizeof card_sides / sizeof card_sides[0]); s++) {
        for (int mask = -1; mask < MASK_COUNT; mask++) {
            memset(depth, 0, bytes);
            cards_t cards = {depth, color, card_sides[s], mask, 0, 0, 0, 0};
            const float cycles = bench_cycle_median(fill_cards, &cards, false);
            /* Every run draws nearer, so each one writes what the mask keeps. */
            TEST_ASSERT_TRUE(mask < 0 ? cards.written == cards.pixels : cards.written < cards.pixels);
            ESP_LOGI(TAG, "masked_span arm=%s side=%d coverage=%d cycles=%.0f spans=%d pixels=%d written=%d",
                     mask < 0 ? "opaque" : "mask1", cards.side, mask < 0 ? 1000 : mask_permille[mask], (double)cycles,
                     cards.spans, cards.pixels, cards.written);
        }
    }
    memory_free(depth);
    memory_free(color);
}

void
run_masked_span_suite(void) {
    RUN_TEST(test_masked_span_cost);
}

#else /* !DEVICE_BUILD */

void
run_masked_span_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER_ON_REQUEST(run_masked_span_suite);

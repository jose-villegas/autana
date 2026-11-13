/*
 * Device-only suite: draw_sprite() cost on a screen of nine-sliced,
 * translucent tiles (the sprite_test pack's tile), read from the mapped pack
 * in flash and from a resident copy in on-chip RAM. Each row is a whole
 * screen of tiles drawn into the framebuffer, not presented, upright and at a
 * quarter turn. Then the blend alone, per framebuffer pixel, by region: a
 * flat colour at one alpha (a flat centre), stretched edges (runs of one
 * source pixel), corners (a source pixel each), and the sprite's own path
 * over a whole screen, against simd_copy over the same rows, the
 * framebuffer's bandwidth floor.
 */
#include "suites.h" /* portable: needed by SUITE_REGISTER() even on host */

#define SPRITE_PERF_PACK "sprite_test"

#ifdef DEVICE_BUILD

#include <stdint.h>

#include "unity.h"

#include "esp_log.h"

#include "asset/asset_store.h"
#include "core/memory.h"
#include "core/timing.h"
#include "draw/draw.h"
#include "draw/draw_sprite.h"
#include "gfx/gfx.h"
#include "math/scalar/units.h"
#include "simd/copy.h"
#include "testkit/bench.h"

static const char* TAG = "sprite_perf";

#define SPRITE_PERF_TILE "tile"
#define SAMPLES          50
#define BACKGROUND       RGB565BE(0x204060)
#define BLEND_COLOR      RGB565BE(0x4B4B4B)
#define BLEND_ALPHA      128

/* Three columns by four rows of tiles across the panel, a gap between them:
 * the launcher-home scale, every tile nine-sliced up from its 48 px art. */
enum {
    TILE_COLUMNS = 3,
    TILE_ROWS = 4,
    TILE_GAP = 8,
    TILE_W = (GFX_WIDTH - ((TILE_COLUMNS + 1) * TILE_GAP)) / TILE_COLUMNS,
    TILE_H = (GFX_HEIGHT - ((TILE_ROWS + 1) * TILE_GAP)) / TILE_ROWS,
};

static void
draw_screen(const sprite_t* tile, int turns) {
    draw_clear(BACKGROUND);
    for (int row = 0; row < TILE_ROWS; row++) {
        for (int column = 0; column < TILE_COLUMNS; column++) {
            const recti_t at = {TILE_GAP + (column * (TILE_W + TILE_GAP)), TILE_GAP + (row * (TILE_H + TILE_GAP)),
                                TILE_W, TILE_H};
            draw_sprite(tile, at, turns);
        }
    }
}

typedef void (*draw_fn)(const sprite_t* tile, int turns);

/* SAMPLES timed calls of `draw`, after one to warm. */
static bench_stats_t
time_draws(draw_fn draw, const sprite_t* tile, int turns) {
    int32_t* micros = memory_alloc(SAMPLES * sizeof *micros, MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(micros);
    draw(tile, turns);
    for (int i = 0; i < SAMPLES; i++) {
        const int64_t start = timing_now_us();
        draw(tile, turns);
        micros[i] = (int32_t)(timing_now_us() - start);
    }
    const bench_stats_t stats = bench_stats_compute(micros, SAMPLES);
    memory_free(micros);
    return stats;
}

static void
report(const char* row, const sprite_t* tile, int turns) {
    const bench_stats_t stats = time_draws(draw_screen, tile, turns);
    ESP_LOGI(TAG, "%s one core: mean %lldus (min %lld, max %lld, %d tiles of %dx%d)", row, (long long)stats.avg,
             (long long)stats.min, (long long)stats.max, TILE_COLUMNS * TILE_ROWS, TILE_W, TILE_H);
}

/* The mean of `draw` over `pixels` framebuffer pixels, in ns a pixel. */
static void
report_per_pixel(const char* row, draw_fn draw, const sprite_t* tile, int64_t pixels) {
    const bench_stats_t stats = time_draws(draw, tile, 0);
    ESP_LOGI(TAG, "%s one core: mean %lldus over %lld px: %lld ns/px", row, (long long)stats.avg, (long long)pixels,
             (long long)(stats.avg * MATH_NS_PER_US / pixels));
}

static void
blend_flat_screen(const sprite_t* tile, int turns) {
    (void)tile;
    (void)turns;
    draw_rect_blend(0, 0, GFX_WIDTH, GFX_HEIGHT, BLEND_COLOR, BLEND_ALPHA);
}

/* The sprite stretched over the screen with no flat centre: every pixel
 * through the per-pixel blend. */
static void
blend_sprite_screen(const sprite_t* tile, int turns) {
    sprite_t each_pixel = *tile;
    each_pixel.flat_centre = false;
    draw_sprite(&each_pixel, (recti_t){0, 0, GFX_WIDTH, GFX_HEIGHT}, turns);
}

/* The tile at its corners alone: drawn the size of its two borders, again
 * and again across the screen. */
static int64_t
corner_pixels(const sprite_t* tile) {
    const int w = tile->border.left + tile->border.right;
    const int h = tile->border.top + tile->border.bottom;
    return (int64_t)(GFX_WIDTH / w) * w * (GFX_HEIGHT / h) * h;
}

static void
blend_corners_screen(const sprite_t* tile, int turns) {
    const int w = tile->border.left + tile->border.right;
    const int h = tile->border.top + tile->border.bottom;
    for (int y = 0; y + h <= GFX_HEIGHT; y += h) {
        for (int x = 0; x + w <= GFX_WIDTH; x += w) {
            draw_sprite(tile, (recti_t){x, y, w, h}, turns);
        }
    }
}

/* Strips the screen wide and two borders tall, clipped to their stretched
 * top and bottom edges: every row a run of one source pixel. */
static int64_t
edge_run_pixels(const sprite_t* tile) {
    const int h = tile->border.top + tile->border.bottom;
    return (int64_t)(GFX_WIDTH - tile->border.left - tile->border.right) * (GFX_HEIGHT / h) * h;
}

static void
blend_edge_runs_screen(const sprite_t* tile, int turns) {
    const int h = tile->border.top + tile->border.bottom;
    draw_set_clip(tile->border.left, 0, GFX_WIDTH - tile->border.left - tile->border.right, GFX_HEIGHT);
    for (int y = 0; y + h <= GFX_HEIGHT; y += h) {
        draw_sprite(tile, (recti_t){0, y, GFX_WIDTH, h}, turns);
    }
    draw_clear_clip();
}

/* Every framebuffer row read and written once, as a blend does. */
static void
copy_screen_rows(const sprite_t* tile, int turns) {
    (void)tile;
    (void)turns;
    gfx_color_t* fb = gfx_framebuffer();
    for (int y = 0; y + 1 < GFX_HEIGHT; y++) {
        simd_copy(fb + ((size_t)y * GFX_WIDTH), fb + ((size_t)(y + 1) * GFX_WIDTH), GFX_WIDTH * sizeof *fb);
    }
}

static handle_t
open_tile(sprite_t* tile) {
    const handle_t load = asset_store_load(SPRITE_PERF_PACK);
    const asset_pack_t* pack = asset_store_loaded(load);
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "no sprite_test pack: see the log above");
    TEST_ASSERT_EQUAL_INT(ASSET_OK, draw_sprite_open(pack, SPRITE_PERF_TILE, tile));
    return load;
}

/* `turns` 1 reads the source a column at a time, by stride: the case a
 * baked quarter turn avoids for pictures, measured here for sprites. */
static void
measure_flash(const char* row, int turns) {
    sprite_t tile;
    const handle_t load = open_tile(&tile);
    report(row, &tile, turns);
    asset_store_release(load);
}

static void
measure_resident(const char* row, int turns) {
    sprite_t tile;
    const handle_t load = open_tile(&tile);
    void* ram = memory_alloc(draw_sprite_bytes(&tile), MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(ram);
    draw_sprite_make_resident(&tile, ram);
    asset_store_release(load);
    report(row, &tile, turns);
    memory_free(ram);
}

static void
test_sprite_screen_flash(void) {
    measure_flash("sprite_screen_flash", 0);
}

static void
test_sprite_screen_resident(void) {
    measure_resident("sprite_screen_resident", 0);
}

static void
test_sprite_screen_flash_turned(void) {
    measure_flash("sprite_screen_flash_turned", 1);
}

static void
test_sprite_screen_resident_turned(void) {
    measure_resident("sprite_screen_resident_turned", 1);
}

static void
test_blend_against_the_copy_floor(void) {
    sprite_t tile;
    const handle_t load = open_tile(&tile);
    const int64_t screen = (int64_t)GFX_WIDTH * GFX_HEIGHT;
    report_per_pixel("blend_flat", blend_flat_screen, &tile, screen);
    report_per_pixel("blend_edge_runs", blend_edge_runs_screen, &tile, edge_run_pixels(&tile));
    report_per_pixel("blend_corners", blend_corners_screen, &tile, corner_pixels(&tile));
    report_per_pixel("blend_sprite_pixels", blend_sprite_screen, &tile, screen);
    report_per_pixel("copy_floor", copy_screen_rows, &tile, (int64_t)GFX_WIDTH * (GFX_HEIGHT - 1));
    asset_store_release(load);
}

void
run_sprite_perf_suite(void) {
    RUN_TEST(test_sprite_screen_flash);
    RUN_TEST(test_sprite_screen_resident);
    RUN_TEST(test_sprite_screen_flash_turned);
    RUN_TEST(test_sprite_screen_resident_turned);
    RUN_TEST(test_blend_against_the_copy_floor);
}

#else

void
run_sprite_perf_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_sprite_perf_suite);
SUITE_READS(run_sprite_perf_suite, SPRITE_PERF_PACK);

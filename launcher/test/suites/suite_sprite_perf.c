/*
 * Device-only suite: draw_sprite() cost on a screen of nine-sliced,
 * translucent tiles (the sprite_test pack's tile), read from the mapped pack
 * in flash and from a resident copy in on-chip RAM. Each row is a whole
 * screen of tiles drawn into the framebuffer, not presented, upright and at a
 * quarter turn.
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
#include "testkit/bench.h"

static const char* TAG = "sprite_perf";

#define SPRITE_PERF_TILE "tile"
#define SAMPLES          50
#define BACKGROUND       RGB565BE(0x204060)

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

static void
report(const char* row, const sprite_t* tile, int turns) {
    int32_t* micros = memory_alloc(SAMPLES * sizeof *micros, MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL(micros);
    draw_screen(tile, turns); /* warm */
    for (int i = 0; i < SAMPLES; i++) {
        const int64_t start = timing_now_us();
        draw_screen(tile, turns);
        micros[i] = (int32_t)(timing_now_us() - start);
    }
    const bench_stats_t stats = bench_stats_compute(micros, SAMPLES);
    memory_free(micros);
    ESP_LOGI(TAG, "%s one core: mean %lldus (min %lld, max %lld, %d tiles of %dx%d)", row, (long long)stats.avg,
             (long long)stats.min, (long long)stats.max, TILE_COLUMNS * TILE_ROWS, TILE_W, TILE_H);
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

void
run_sprite_perf_suite(void) {
    RUN_TEST(test_sprite_screen_flash);
    RUN_TEST(test_sprite_screen_resident);
    RUN_TEST(test_sprite_screen_flash_turned);
    RUN_TEST(test_sprite_screen_resident_turned);
}

#else

void
run_sprite_perf_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_sprite_perf_suite);
SUITE_READS(run_sprite_perf_suite, SPRITE_PERF_PACK);

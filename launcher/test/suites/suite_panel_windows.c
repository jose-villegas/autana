/* Device-only, on request: a still test card for the panel link. A grey
 * frame with rulers along every edge, and one coloured window per band of
 * rows re-sent in a loop at the fast clock with nothing else sending. A
 * window the panel takes wrong leaves its colour on the grey, where it
 * stays: one photo at the end shows which band's window strayed and where
 * on the rulers its pixels landed. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/memory.h"
#include "core/timing.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_present.h"
#include "ridge_arms.h"
#include "shell/ui/ui_launcher.h"
#include "shell/ui/ui_ridge.h"

static const char* TAG = "panel_windows";

#define LOOP_MS     20000
#define HOLD_MS     30000
#define BAND_PX     64
#define BAND_INSET  8
#define TICK_STEP   16
#define TICK_MINOR  4
#define TICK_MAJOR  10
#define LABEL_STEP  64
#define DIGIT_W     3
#define DIGIT_H     5
#define DIGIT_SCALE 2
#define STRIPE_ROWS 4

/* One window per band: its columns, and the colour it carries. */
typedef struct {
    int x0, x1;
    uint32_t rgb;
} band_window_t;

static const band_window_t band_windows[] = {
    {24, 120, 0xFFFFFF},  {104, 200, 0xFF3030}, {184, 280, 0x30FF30}, {248, 344, 0x3060FF},
    {280, 368, 0xFFFF30}, {160, 352, 0xFF30FF}, {24, 344, 0x30FFFF},
};
#define BAND_COUNT ((int)(sizeof band_windows / sizeof band_windows[0]))
_Static_assert(BAND_COUNT* BAND_PX <= GFX_HEIGHT, "one band of rows per window");

/* 3x5 digits, one row of three bits per nibble, top row first. */
static const uint16_t digit_rows[10] = {
    075557, 022222, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
};

static void
put(int x, int y, gfx_color_t c) {
    if (x >= 0 && x < GFX_WIDTH && y >= 0 && y < GFX_HEIGHT) {
        gfx_framebuffer()[(y * GFX_WIDTH) + x] = c;
    }
}

static void
fill(int x0, int y0, int x1, int y1, gfx_color_t c) {
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            put(x, y, c);
        }
    }
}

static void
draw_number(int x, int y, int value, gfx_color_t c) {
    char text[4];
    int n = 0;
    do {
        text[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0 && n < (int)sizeof text);
    for (int i = 0; i < n; i++) {
        const uint16_t glyph = digit_rows[text[n - 1 - i] - '0'];
        for (int row = 0; row < DIGIT_H; row++) {
            for (int col = 0; col < DIGIT_W; col++) {
                if (glyph & (1U << (((DIGIT_H - 1 - row) * DIGIT_W) + (DIGIT_W - 1 - col)))) {
                    const int px = x + ((i * (DIGIT_W + 1)) + col) * DIGIT_SCALE;
                    fill(px, y + (row * DIGIT_SCALE), px + DIGIT_SCALE, y + ((row + 1) * DIGIT_SCALE), c);
                }
            }
        }
    }
}

/* Ticks every TICK_STEP along all four edges, long ones with a label every
 * LABEL_STEP, in panel coordinates. */
static void
draw_rulers(gfx_color_t c) {
    for (int x = 0; x < GFX_WIDTH; x += TICK_STEP) {
        const int len = x % LABEL_STEP == 0 ? TICK_MAJOR : TICK_MINOR;
        fill(x, 0, x + 1, len, c);
        fill(x, GFX_HEIGHT - len, x + 1, GFX_HEIGHT, c);
        if (x % LABEL_STEP == 0) {
            draw_number(x + 2, TICK_MAJOR + 2, x, c);
        }
    }
    for (int y = 0; y < GFX_HEIGHT; y += TICK_STEP) {
        const int len = y % LABEL_STEP == 0 ? TICK_MAJOR : TICK_MINOR;
        fill(0, y, len, y + 1, c);
        fill(GFX_WIDTH - len, y, GFX_WIDTH, y + 1, c);
        if (y % LABEL_STEP == 0 && y > 0) {
            draw_number(TICK_MAJOR + 2, y + 2, y, c);
        }
    }
}

static gfx_send_window_t
band_window(int band) {
    const band_window_t* b = &band_windows[band];
    const int y0 = (band * BAND_PX) + BAND_INSET;
    return (gfx_send_window_t){(int16_t)b->x0, (int16_t)y0, (int16_t)b->x1, (int16_t)(y0 + BAND_PX - (2 * BAND_INSET))};
}

/* Each window in its colour, striped with black every STRIPE_ROWS rows. */
static void
draw_windows(void) {
    for (int band = 0; band < BAND_COUNT; band++) {
        const gfx_send_window_t w = band_window(band);
        for (int y = w.y0; y < w.y1; y++) {
            const bool stripe = ((y - w.y0) / STRIPE_ROWS) % 2 == 1;
            fill(w.x0, y, w.x1, y + 1, stripe ? gfx_rgb(0x000000) : gfx_rgb(band_windows[band].rgb));
        }
    }
}

void
test_panel_windows_card(void) {
    gfx_heal_set_budget(0);
    gfx_heal_set_rolling(0);
    gfx_set_panel_clock_hz(GFX_PANEL_CLOCK_FAST_HZ);
    fill(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x404040));
    draw_rulers(gfx_rgb(0xFFFFFF));
    draw_windows();
    gfx_present_raw_full_frame_for_test();

    gfx_send_window_t windows[BAND_COUNT];
    for (int band = 0; band < BAND_COUNT; band++) {
        windows[band] = band_window(band);
        ESP_LOGI(TAG, "BAND %d window x=%d..%d y=%d..%d rgb=%06lx", band, windows[band].x0, windows[band].x1,
                 windows[band].y0, windows[band].y1, (unsigned long)band_windows[band].rgb);
    }
    int loops = 0;
    const int64_t began = timing_now_us();
    while (timing_now_us() - began < (int64_t)LOOP_MS * 1000) {
        gfx_send_windows_for_test(windows, BAND_COUNT);
        loops++;
        if (loops % 64 == 0) {
            vTaskDelay(1);
        }
    }
    ESP_LOGI(TAG, "LOOPED %d times in %d ms; holding %d ms for a photo", loops, LOOP_MS, HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
    gfx_heal_restore_defaults();
    TEST_PASS();
}

#define RIDGE_SETTLE_FRAMES 300
#define RIDGE_LOGGED_FRAMES 60
#define RIDGE_WINDOWS_MAX   16384

/* The launcher ridge's own partial sends in landscape-inverted, logged over
 * RIDGE_LOGGED_FRAMES frames, then replayed at the ridge's frame pace over
 * the grey card: each window's area striped, everything else grey, so a
 * pixel that lands outside them stays visible against the rulers. */
void
test_panel_windows_ridge_replay(void) {
    gfx_send_window_t* const log = memory_alloc(sizeof(gfx_send_window_t) * RIDGE_WINDOWS_MAX, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(log);
    int frame_start[RIDGE_LOGGED_FRAMES + 1];
    ui_ridge_reset_for_test();
    ui_launcher_heal_opt_in();
    ui_ridge_set_ambient(true);
    ui_ridge_set_gravity(256, 0, 256, 0);
    for (int frame = 0; frame < RIDGE_SETTLE_FRAMES; frame++) {
        ui_ridge_step(&ridge_arm_idle_input, RIDGE_ARM_FRAME_DT_MS);
        gfx_present();
    }
    gfx_set_send_window_log(log, RIDGE_WINDOWS_MAX);
    for (int frame = 0; frame < RIDGE_LOGGED_FRAMES; frame++) {
        frame_start[frame] = gfx_send_window_count();
        ui_ridge_step(&ridge_arm_idle_input, RIDGE_ARM_FRAME_DT_MS);
        gfx_present();
    }
    frame_start[RIDGE_LOGGED_FRAMES] = gfx_send_window_count();
    gfx_set_send_window_log(NULL, 0);
    ui_ridge_release_for_test();

    /* Partial windows only: full-width sends never strayed. */
    int kept = 0;
    for (int f = 0; f < RIDGE_LOGGED_FRAMES; f++) {
        const int from = frame_start[f];
        frame_start[f] = kept;
        for (int i = from; i < frame_start[f + 1]; i++) {
            if (log[i].x0 != 0 || log[i].x1 != GFX_WIDTH) {
                log[kept++] = log[i];
            }
        }
    }
    frame_start[RIDGE_LOGGED_FRAMES] = kept;
    ESP_LOGI(TAG, "RIDGE partial windows=%d over %d frames", kept, RIDGE_LOGGED_FRAMES);

    gfx_heal_set_budget(0);
    gfx_heal_set_rolling(0);
    gfx_set_panel_clock_hz(GFX_PANEL_CLOCK_FAST_HZ);
    fill(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x404040));
    for (int i = 0; i < kept; i++) {
        for (int y = log[i].y0; y < log[i].y1; y++) {
            fill(log[i].x0, y, log[i].x1, y + 1, (y / STRIPE_ROWS) % 2 ? gfx_rgb(0x000000) : gfx_rgb(0xFFFFFF));
        }
    }
    draw_rulers(gfx_rgb(0xFFFFFF));
    gfx_present_raw_full_frame_for_test();

    int frames = 0;
    const int64_t began = timing_now_us();
    while (timing_now_us() - began < (int64_t)LOOP_MS * 1000) {
        const int f = frames % RIDGE_LOGGED_FRAMES;
        gfx_send_windows_for_test(&log[frame_start[f]], frame_start[f + 1] - frame_start[f]);
        vTaskDelay(pdMS_TO_TICKS(RIDGE_ARM_FRAME_DT_MS));
        frames++;
    }
    ESP_LOGI(TAG, "RIDGE replayed %d frames in %d ms; holding %d ms for a photo", frames, LOOP_MS, HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
    gfx_heal_restore_defaults();
    memory_free(log);
    TEST_PASS();
}

void
run_panel_windows_suite(void) {
    RUN_TEST(test_panel_windows_ridge_replay);
}

#else

void
run_panel_windows_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_panel_windows_suite);

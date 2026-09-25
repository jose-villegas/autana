/*
 * app_input_lab - measures how precisely the touch panel reports a tap. A
 * small magenta square appears somewhere on the screen; tap it, and the
 * press is scored against its centre before the next one appears. The
 * running hit rate and offset sit at the top, and every tap is logged to
 * the console for analysis off the board. BOOT starts a fresh round.
 *
 * The raw press is read, not microui's pointer, so whatever correction the
 * UI layer applies to touches never colours the measurement.
 */

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "app.h"
#include "apps/input_lab/touch_probe.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

static const char* TAG = "input_lab";

#define TARGET_SIDE    12
#define TARGET_MARGIN  4
#define HUD_TOP        24
#define HUD_LINE_H     20
#define HUD_SCALE      2
#define MARK_SIDE      4
#define COL_BACKGROUND 0x0A0C14

static uint32_t rng;
static touch_probe_target_t target;
static touch_probe_stats_t stats;
static int screen_w, screen_h;
static bool have_last_tap;
static int last_x, last_y;
static bool last_hit;

static void
place_target(void) {
    screen_w = ui_width();
    screen_h = ui_height();
    target = touch_probe_next(&rng, screen_w, screen_h, TARGET_SIDE, TARGET_MARGIN);
}

static void
start_round(void) {
    stats = (touch_probe_stats_t){0};
    have_last_tap = false;
    place_target();
}

static void
input_lab_enter(void) {
    rng = (uint32_t)esp_timer_get_time() | 1u;
    start_round();
    ui_invalidate();
}

static void
to_screen(int px, int py, int* x, int* y) {
    ui_transform_t inverse;
    const ui_transform_t shell = ui_transform_quarter_turn(display_shell_quarter(), GFX_WIDTH, GFX_HEIGHT);
    if (!ui_transform_invert(shell, &inverse)) {
        *x = px;
        *y = py;
        return;
    }
    ui_transform_point(inverse, px, py, x, y);
}

/* Tenths, since this build's printf may carry no float support. */
static void
format_tenths(char* out, size_t len, float v) {
    const int t = (int)(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
    snprintf(out, len, "%s%d.%d", t < 0 ? "-" : "+", abs(t) / 10, abs(t) % 10);
}

static void
take_tap(const input_t* input) {
    int x, y;
    to_screen(input->press_x, input->press_y, &x, &y);
    int dx, dy;
    touch_probe_offset(target, x, y, &dx, &dy);
    last_hit = touch_probe_record(&stats, target, x, y);
    last_x = x;
    last_y = y;
    have_last_tap = true;
    ESP_LOGI(TAG, "tap %d aim (%d,%d) hit (%d,%d) d (%+d,%+d) %s screen %dx%d", stats.taps, target.x + target.side / 2,
             target.y + target.side / 2, x, y, dx, dy, last_hit ? "HIT" : "miss", screen_w, screen_h);
    place_target();
}

static void
draw_hud(mu_Context* ctx) {
    char line[64];
    char mean[24], spread[24];
    const mu_Color ink = ui_rgb(0xE0E0E0);
    const int hit_pct = stats.taps > 0 ? (100 * stats.hits + stats.taps / 2) / stats.taps : 0;

    snprintf(line, sizeof line, "TAPS %d  HITS %d%%", stats.taps, hit_pct);
    ui_text_in(ctx, mu_rect(0, HUD_TOP, screen_w, HUD_LINE_H), line, ink, HUD_SCALE, UI_ALIGN_CENTRE);

    format_tenths(mean, sizeof mean, touch_probe_mean_dx(&stats));
    format_tenths(spread, sizeof spread, touch_probe_spread_dx(&stats));
    snprintf(line, sizeof line, "DX %s SD %s", mean, spread + 1);
    ui_text_in(ctx, mu_rect(0, HUD_TOP + HUD_LINE_H, screen_w, HUD_LINE_H), line, ink, HUD_SCALE, UI_ALIGN_CENTRE);

    format_tenths(mean, sizeof mean, touch_probe_mean_dy(&stats));
    format_tenths(spread, sizeof spread, touch_probe_spread_dy(&stats));
    snprintf(line, sizeof line, "DY %s SD %s", mean, spread + 1);
    ui_text_in(ctx, mu_rect(0, HUD_TOP + 2 * HUD_LINE_H, screen_w, HUD_LINE_H), line, ink, HUD_SCALE, UI_ALIGN_CENTRE);
}

static void
input_lab_frame(uint32_t dt_ms, const input_t* input) {
    (void)dt_ms;
    if (input->boot.pressed) {
        start_round();
    }
    if (ui_width() != screen_w || ui_height() != screen_h) {
        place_target();
    }
    if (input->pressed) {
        take_tap(input);
    }

    mu_Context* ctx = ui_context();
    ui_begin(input);
    if (ui_begin_screen(ctx, "Input Lab", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        draw_hud(ctx);
        if (have_last_tap) {
            const mu_Color mark = last_hit ? ui_rgb(0x40E060) : ui_rgb(0xFFB020);
            mu_draw_rect(ctx, mu_rect(last_x - MARK_SIDE / 2, last_y - MARK_SIDE / 2, MARK_SIDE, MARK_SIDE), mark);
        }
        mu_draw_rect(ctx, mu_rect(target.x, target.y, target.side, target.side), ui_rgb(0xFF00FF));
        mu_end_window(ctx);
    }
    ui_end(COL_BACKGROUND);
}

static void
input_lab_exit(void) {}

app_t app_input_lab = {
    .name = "Input Lab",
    .summary = "Touch precision: tap the magenta squares",
    .enter = input_lab_enter,
    .frame = input_lab_frame,
    .exit = input_lab_exit,
    .invalidate = ui_invalidate,
    .home_gesture = true,
};

APP_REGISTER(app_input_lab);

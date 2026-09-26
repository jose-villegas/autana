/*
 * app_input_lab - measures how precisely the touch panel reports a tap. A
 * small magenta square appears on the screen; tap it, and on release the tap
 * is scored against its centre before the next one appears. The running hit
 * rate and offset sit at the top, and every tap is logged to the console as
 * one "probe" line for analysis off the board. BOOT switches between random
 * targets, a shuffled 5x5 grid and a bezel page, and starts a fresh round.
 *
 * Each tap is logged three ways - the first contact, where it settled, and
 * the release - in screen and in raw panel coordinates, so a calibration can
 * be fitted in whichever frame the error proves to live in. The raw touch is
 * read, not microui's pointer, so no correction in the UI layer colours it.
 *
 * The bezel page draws an arc in every corner; dragging up or down changes
 * its radius until the arcs sit on the edge of the glass, and each release
 * logs it. Sliding off each edge there fills in the raw range the panel can
 * report, logged at the end of every round.
 */

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "app.h"
#include "apps/input_lab/corner_arc.h"
#include "apps/input_lab/touch_probe.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"
#include "util/tune.h"

static const char* TAG = "input_lab";

#define TARGET_SIDE    12
#define TARGET_MARGIN  4
#define GRID_COLS      5
#define GRID_ROWS      5
#define GRID_COUNT     (GRID_COLS * GRID_ROWS)
#define SETTLE_FROM_MS 30
#define SETTLE_TO_MS   80
#define SAMPLES_MAX    128
#define HUD_TOP        24
#define HUD_LINE_H     20
#define HUD_SCALE      2
#define MARK_SIDE      4
#define COL_BACKGROUND 0x0A0C14
#define BEZEL_RADIUS   30
#define BEZEL_MAX      80
#define BEZEL_DRAG_PX  4
#define ARC_STEP       2

typedef enum { MODE_RANDOM, MODE_GRID, MODE_BEZEL, MODE_COUNT } lab_mode_t;

static const char* const MODE_NAMES[MODE_COUNT] = {"random", "grid", "bezel"};

static lab_mode_t mode;
static uint32_t rng;
static int grid_order[GRID_COUNT];
static int grid_next;
static touch_probe_target_t target;
static touch_probe_stats_t stats;
static int screen_w, screen_h;

static touch_probe_sample_t samples[SAMPLES_MAX];
static int sample_count;
static bool tracking;
static int held_ms, idle_ms, idle_before_press;
static int raw_min_x, raw_max_x, raw_min_y, raw_max_y;

static bool have_last_tap;
static int last_x, last_y;
static bool last_hit;

static int bezel_radius = BEZEL_RADIUS;
static int radius_at_press;

static void
place_target(void) {
    screen_w = ui_width();
    screen_h = ui_height();
    if (mode == MODE_RANDOM) {
        target = touch_probe_next(&rng, screen_w, screen_h, TARGET_SIDE, TARGET_MARGIN);
        return;
    }
    if (grid_next == 0) {
        touch_probe_shuffle(&rng, grid_order, GRID_COUNT);
    }
    target =
        touch_probe_grid(grid_order[grid_next], GRID_COLS, GRID_ROWS, screen_w, screen_h, TARGET_SIDE, TARGET_MARGIN);
}

static void
advance_target(void) {
    grid_next = (grid_next + 1) % GRID_COUNT;
    place_target();
}

static void
log_raw_range(void) {
    if (raw_max_x >= 0) {
        ESP_LOGI(TAG, "range raw x %d..%d y %d..%d", raw_min_x, raw_max_x, raw_min_y, raw_max_y);
    }
}

static void
start_round(void) {
    log_raw_range();
    stats = (touch_probe_stats_t){0};
    have_last_tap = false;
    grid_next = 0;
    raw_min_x = raw_min_y = 1 << 20;
    raw_max_x = raw_max_y = -1;
    place_target();
    ESP_LOGI(TAG, "round %s screen %dx%d quarter %d", MODE_NAMES[mode], screen_w, screen_h, display_shell_quarter());
}

static void
input_lab_enter(void) {
    rng = (uint32_t)esp_timer_get_time() | 1u;
    raw_max_x = -1;
    mode = MODE_RANDOM;
    tracking = false;
    idle_ms = 0;
    start_round();
    ui_invalidate();
}

static ui_transform_t
shell_transform(void) {
    return ui_transform_quarter_turn(display_shell_quarter(), GFX_WIDTH, GFX_HEIGHT);
}

static void
to_screen(int px, int py, int* x, int* y) {
    ui_transform_t inverse;
    if (!ui_transform_invert(shell_transform(), &inverse)) {
        *x = px;
        *y = py;
        return;
    }
    ui_transform_point(inverse, px, py, x, y);
}

static void
add_sample(int x, int y, int t_ms) {
    if (sample_count < SAMPLES_MAX) {
        samples[sample_count++] = (touch_probe_sample_t){.x = x, .y = y, .t_ms = t_ms};
    }
    raw_min_x = x < raw_min_x ? x : raw_min_x;
    raw_max_x = x > raw_max_x ? x : raw_max_x;
    raw_min_y = y < raw_min_y ? y : raw_min_y;
    raw_max_y = y > raw_max_y ? y : raw_max_y;
}

/* Whether the touch driver's correction was on for a tap, so a capture says
 * which frame its raw coordinates are in; -1 where it cannot be asked. */
static int
calibration_on(void) {
#if TUNE_ENABLED
    const tune_entry_t* entry = tune_find(tune_shared(), "touch.calibrate");
    return entry != NULL ? (int)*entry->value : -1;
#else
    return 1;
#endif
}

static void
finish_tap(int release_x, int release_y) {
    const touch_probe_sample_t first = samples[0];
    int settled_x, settled_y;
    touch_probe_settled(samples, sample_count, SETTLE_FROM_MS, SETTLE_TO_MS, &settled_x, &settled_y);

    int fx, fy, sx, sy, rx, ry, aim_raw_x, aim_raw_y;
    to_screen(first.x, first.y, &fx, &fy);
    to_screen(settled_x, settled_y, &sx, &sy);
    to_screen(release_x, release_y, &rx, &ry);
    const int aim_x = target.x + target.side / 2;
    const int aim_y = target.y + target.side / 2;
    ui_transform_point(shell_transform(), aim_x, aim_y, &aim_raw_x, &aim_raw_y);

    last_hit = touch_probe_record(&stats, target, fx, fy);
    last_x = fx;
    last_y = fy;
    have_last_tap = true;

    ESP_LOGI(TAG,
             "probe %d %s q%d aim %d,%d aim_raw %d,%d first %d,%d settled %d,%d release %d,%d raw_first %d,%d "
             "raw_settled %d,%d raw_release %d,%d held %d idle %d samples %d %s cal %d",
             stats.taps, MODE_NAMES[mode], display_shell_quarter(), aim_x, aim_y, aim_raw_x, aim_raw_y, fx, fy, sx, sy,
             rx, ry, first.x, first.y, settled_x, settled_y, release_x, release_y, held_ms, idle_before_press,
             sample_count, last_hit ? "HIT" : "miss", calibration_on());
    advance_target();
}

/* One frame of the raw touch: a press starts a sample run, each frame down
 * adds to it, and the release scores it. A tap can press and release within
 * the same frame. */
static void
track_touch(uint32_t dt_ms, const input_t* input) {
    if (input->pressed) {
        tracking = true;
        sample_count = 0;
        held_ms = 0;
        idle_before_press = idle_ms;
        add_sample(input->press_x, input->press_y, 0);
        if (input->x != input->press_x || input->y != input->press_y) {
            add_sample(input->x, input->y, 0);
        }
    } else if (tracking && input->down) {
        held_ms += (int)dt_ms;
        add_sample(input->x, input->y, held_ms);
    } else if (!tracking) {
        idle_ms += (int)dt_ms;
    }

    if (tracking && input->released) {
        tracking = false;
        idle_ms = 0;
        if (mode == MODE_BEZEL) {
            ESP_LOGI(TAG, "bezel radius %d quarter %d", bezel_radius, display_shell_quarter());
        } else {
            finish_tap(input->x, input->y);
        }
    }
}

/* Up the screen grows the arcs, down shrinks them, BEZEL_DRAG_PX to a pixel. */
static void
drag_bezel(const input_t* input) {
    if (input->pressed) {
        radius_at_press = bezel_radius;
    }
    if (!input->down) {
        return;
    }
    int x, press_y, y;
    to_screen(input->press_x, input->press_y, &x, &press_y);
    to_screen(input->x, input->y, &x, &y);
    const int radius = radius_at_press + (press_y - y) / BEZEL_DRAG_PX;
    bezel_radius = radius < 0 ? 0 : radius > BEZEL_MAX ? BEZEL_MAX : radius;
}

/* Every ARC_STEP-th row of each corner, run out to where the row above
 * began so the steep end of the curve has no gaps. */
static void
draw_corner_arcs(mu_Context* ctx) {
    const mu_Color ink = ui_rgb(0xFF00FF);
    for (int row = 0; row < bezel_radius; row += ARC_STEP) {
        const int inset = corner_arc_inset(bezel_radius, row);
        const int before = row >= ARC_STEP ? corner_arc_inset(bezel_radius, row - ARC_STEP) : inset + 1;
        const int w = before - inset > 1 ? before - inset : 1;
        const int bottom = screen_h - row - ARC_STEP;
        mu_draw_rect(ctx, mu_rect(inset, row, w, ARC_STEP), ink);
        mu_draw_rect(ctx, mu_rect(screen_w - inset - w, row, w, ARC_STEP), ink);
        mu_draw_rect(ctx, mu_rect(inset, bottom, w, ARC_STEP), ink);
        mu_draw_rect(ctx, mu_rect(screen_w - inset - w, bottom, w, ARC_STEP), ink);
    }
}

/* Tenths, since this build's printf may carry no float support. */
static void
format_tenths(char* out, size_t len, float v) {
    const int t = (int)(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
    snprintf(out, len, "%s%d.%d", t < 0 ? "-" : "+", abs(t) / 10, abs(t) % 10);
}

static void
hud_line(mu_Context* ctx, int row, const char* text) {
    ui_text_in(ctx, mu_rect(0, HUD_TOP + row * HUD_LINE_H, screen_w, HUD_LINE_H), text, ui_rgb(0xE0E0E0), HUD_SCALE,
               UI_ALIGN_CENTRE);
}

static void
draw_offsets(mu_Context* ctx) {
    char line[64];
    char mean[24], spread[24];
    const int hit_pct = stats.taps > 0 ? (100 * stats.hits + stats.taps / 2) / stats.taps : 0;

    snprintf(line, sizeof line, "%s %d  HITS %d%%", mode == MODE_GRID ? "GRID" : "RANDOM", stats.taps, hit_pct);
    hud_line(ctx, 0, line);

    format_tenths(mean, sizeof mean, touch_probe_mean_dx(&stats));
    format_tenths(spread, sizeof spread, touch_probe_spread_dx(&stats));
    snprintf(line, sizeof line, "DX %s SD %s", mean, spread + 1);
    hud_line(ctx, 1, line);

    format_tenths(mean, sizeof mean, touch_probe_mean_dy(&stats));
    format_tenths(spread, sizeof spread, touch_probe_spread_dy(&stats));
    snprintf(line, sizeof line, "DY %s SD %s", mean, spread + 1);
    hud_line(ctx, 2, line);
}

static void
draw_hud(mu_Context* ctx) {
    char line[64];
    if (mode == MODE_BEZEL) {
        snprintf(line, sizeof line, "BEZEL R %d", bezel_radius);
        hud_line(ctx, 0, line);
        hud_line(ctx, 1, "DRAG UP OR DOWN");
    } else {
        draw_offsets(ctx);
    }
    if (raw_max_x >= 0) {
        snprintf(line, sizeof line, "RAW X %d-%d Y %d-%d", raw_min_x, raw_max_x, raw_min_y, raw_max_y);
        hud_line(ctx, 3, line);
    }
}

static void
draw_target(mu_Context* ctx) {
    if (have_last_tap) {
        const mu_Color mark = last_hit ? ui_rgb(0x40E060) : ui_rgb(0xFFB020);
        mu_draw_rect(ctx, mu_rect(last_x - MARK_SIDE / 2, last_y - MARK_SIDE / 2, MARK_SIDE, MARK_SIDE), mark);
    }
    mu_draw_rect(ctx, mu_rect(target.x, target.y, target.side, target.side), ui_rgb(0xFF00FF));
}

static void
input_lab_frame(uint32_t dt_ms, const input_t* input) {
    if (input->boot.pressed) {
        mode = (lab_mode_t)((mode + 1) % MODE_COUNT);
        start_round();
    }
    if (ui_width() != screen_w || ui_height() != screen_h) {
        place_target();
    }
    track_touch(dt_ms, input);
    if (mode == MODE_BEZEL) {
        drag_bezel(input);
    }

    mu_Context* ctx = ui_context();
    ui_begin(input);
    if (ui_begin_screen(ctx, "Input Lab", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        draw_hud(ctx);
        if (mode == MODE_BEZEL) {
            draw_corner_arcs(ctx);
        } else {
            draw_target(ctx);
        }
        mu_end_window(ctx);
    }
    ui_end(COL_BACKGROUND);
}

static void
input_lab_exit(void) {
    log_raw_range();
}

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

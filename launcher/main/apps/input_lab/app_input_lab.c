/*
 * app_input_lab: measures how precisely the touch panel reports a tap. A
 * small magenta square appears on the screen; tap it, and on release the tap
 * is scored against its centre before the next one appears. The running hit
 * rate and offset sit at the top, and every tap is logged to the console as
 * one "probe" line for probe_fit.py. BOOT cycles random targets, a shuffled
 * 5x5 grid and a bezel page, and starts a fresh round.
 *
 * Each probe records its first, settled and release samples in screen and
 * panel coordinates, as the touch driver delivers them; its `cal` field says
 * whether the driver's correction was on, so only `cal 0` rounds refit it.
 *
 * The bezel page draws an arc in every corner; dragging up or down changes
 * its radius until the arcs sit on the edge of the glass, and each release
 * logs it. Sliding off each edge there fills in the panel range, logged at
 * the end of every round.
 *
 * The motion page shows acceleration, gravity angles, a bubble level and
 * the tilt filter, with a 10 Hz "motion ..." console line. Numbers stay in
 * input axes; the bubble turns with the screen.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

#include "app/app.h"
#include "apps/input_lab/motion_probe.h"
#include "apps/input_lab/touch_probe.h"
#include "core/timing.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "input/tilt.h"
#include "services/tune.h"
#include "ui/ui.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

static const char* TAG = "input_lab";

#define TARGET_SIDE          12
#define TARGET_MARGIN        4
#define GRID_COLS            5
#define GRID_ROWS            5
#define GRID_COUNT           (GRID_COLS * GRID_ROWS)
#define SETTLE_FROM_MS       30
#define SETTLE_TO_MS         80
#define HUD_TOP              24
#define HUD_LINE_H           20
#define HUD_SCALE            2
#define MARK_SIDE            4
#define COL_BACKGROUND       0x0A0C14
#define BEZEL_MAX            80
#define BEZEL_DRAG_PX        4
#define ARC_STEP             2
#define MOTION_G_TEXT_SIZE   16
#define MOTION_HUD_TEXT_SIZE 96
#define MOTION_LOG_PERIOD_MS 100
#define MOTION_RING_RADIUS   88
#define MOTION_DOT_SIDE      7
#define MOTION_FILTER_SIDE   3
#define MOTION_MARGIN        16
#define MOTION_HUD_ROWS      4
#define MOTION_HUD_SCALE     1
#define COL_HIT              0x40E060
#define COL_MISS             0xFFB020
#define COL_TARGET           0xFF00FF
#define COL_HUD              0xE0E0E0
#define COL_MOTION_FILTER    0x40E0FF
#define COL_MOTION_RING      0x607080

typedef enum { MODE_RANDOM, MODE_GRID, MODE_BEZEL, MODE_MOTION, MODE_COUNT } lab_mode_t;

static const char* const MODE_NAMES[MODE_COUNT] = {"random", "grid", "bezel", "motion"};

static lab_mode_t mode;
static tilt_t tilt;
static input_motion_t motion;
static motion_probe_t motion_probe;
static bool have_motion;
static uint32_t motion_log_ms;
static rng_t rng;
static int grid_order[GRID_COUNT];
static int grid_next;
static touch_probe_target_t target;
static touch_probe_stats_t stats;
static int screen_w, screen_h;

static touch_probe_tap_t tap;
static int panel_min_x, panel_max_x, panel_min_y, panel_max_y;

static bool have_last_tap;
static int last_x, last_y;
static bool last_hit;

static int bezel_radius = DISPLAY_PANEL_CORNER_RADIUS;
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
log_panel_range(void) {
    if (panel_max_x >= 0) {
        ESP_LOGI(TAG, "range panel x %d..%d y %d..%d", panel_min_x, panel_max_x, panel_min_y, panel_max_y);
    }
}

static void
start_round(void) {
    log_panel_range();
    stats = (touch_probe_stats_t){0};
    have_last_tap = false;
    grid_next = 0;
    panel_min_x = panel_min_y = 1 << 20;
    panel_max_x = panel_max_y = -1;
    if (mode == MODE_MOTION) {
        tilt_reset(&tilt, INPUT_ACCEL_ONE_G);
        have_motion = false;
        motion_log_ms = 0;
        screen_w = ui_width();
        screen_h = ui_height();
    } else {
        place_target();
    }
    ESP_LOGI(TAG, "round %s screen %dx%d quarter %d", MODE_NAMES[mode], screen_w, screen_h, display_quarter_now());
}

static void
input_lab_enter(void) {
    rng_seed(&rng, (uint32_t)timing_now_us());
    panel_max_x = -1;
    mode = MODE_RANDOM;
    tap = (touch_probe_tap_t){0};
    start_round();
    ui_invalidate();
}

static ui_transform_t
shell_transform(void) {
    return ui_transform_quarter_turn(display_quarter_now(), GFX_WIDTH, GFX_HEIGHT);
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
widen_panel_range(int x, int y) {
    panel_min_x = x < panel_min_x ? x : panel_min_x;
    panel_max_x = x > panel_max_x ? x : panel_max_x;
    panel_min_y = y < panel_min_y ? y : panel_min_y;
    panel_max_y = y > panel_max_y ? y : panel_max_y;
}

/* Whether the touch driver's correction was on for a tap, so a capture says
 * which frame its panel coordinates are in; -1 where it cannot be asked. */
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
    const touch_probe_sample_t first = tap.samples[0];
    int settled_x, settled_y;
    touch_probe_settled(tap.samples, tap.count, SETTLE_FROM_MS, SETTLE_TO_MS, &settled_x, &settled_y);

    int fx, fy, sx, sy, rx, ry, aim_panel_x, aim_panel_y;
    to_screen(first.x, first.y, &fx, &fy);
    to_screen(settled_x, settled_y, &sx, &sy);
    to_screen(release_x, release_y, &rx, &ry);
    const int aim_x = target.x + target.side / 2;
    const int aim_y = target.y + target.side / 2;
    ui_transform_point(shell_transform(), aim_x, aim_y, &aim_panel_x, &aim_panel_y);

    last_hit = touch_probe_record(&stats, target, fx, fy);
    last_x = fx;
    last_y = fy;
    have_last_tap = true;

    ESP_LOGI(TAG,
             "probe %d %s q%d aim %d,%d aim_panel %d,%d first %d,%d settled %d,%d release %d,%d panel_first %d,%d "
             "panel_settled %d,%d panel_release %d,%d held %d idle %d samples %d %s cal %d",
             stats.taps, MODE_NAMES[mode], display_quarter_now(), aim_x, aim_y, aim_panel_x, aim_panel_y, fx, fy, sx,
             sy, rx, ry, first.x, first.y, settled_x, settled_y, release_x, release_y, tap.held_ms,
             tap.idle_before_press, tap.count, last_hit ? "HIT" : "miss", calibration_on());
    advance_target();
}

static void
track_touch(uint32_t dt_ms, const input_t* input) {
    if (input->pressed || input->down) {
        widen_panel_range(input->x, input->y);
    }
    if (!touch_probe_track(&tap, (int)dt_ms, input)) {
        return;
    }
    if (mode == MODE_BEZEL) {
        ESP_LOGI(TAG, "bezel radius %d quarter %d", bezel_radius, display_quarter_now());
    } else {
        finish_tap(input->x, input->y);
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
    const mu_Color ink = ui_rgb(COL_TARGET);
    for (int row = 0; row < bezel_radius; row += ARC_STEP) {
        const int inset = display_panel_corner_inset(bezel_radius, screen_h, row);
        const int before =
            row >= ARC_STEP ? display_panel_corner_inset(bezel_radius, screen_h, row - ARC_STEP) : inset + 1;
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
    ui_text_in(ctx, mu_rect(0, HUD_TOP + row * HUD_LINE_H, screen_w, HUD_LINE_H), text, ui_rgb(COL_HUD),
               mode == MODE_MOTION ? MOTION_HUD_SCALE : HUD_SCALE, UI_ALIGN_CENTRE);
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
    if (panel_max_x >= 0) {
        snprintf(line, sizeof line, "PANEL X %d-%d Y %d-%d", panel_min_x, panel_max_x, panel_min_y, panel_max_y);
        hud_line(ctx, 3, line);
    }
}

static void
draw_target(mu_Context* ctx) {
    if (have_last_tap) {
        const mu_Color mark = last_hit ? ui_rgb(COL_HIT) : ui_rgb(COL_MISS);
        mu_draw_rect(ctx, mu_rect(last_x - MARK_SIDE / 2, last_y - MARK_SIDE / 2, MARK_SIDE, MARK_SIDE), mark);
    }
    mu_draw_rect(ctx, mu_rect(target.x, target.y, target.side, target.side), ui_rgb(COL_TARGET));
}

static void
read_motion(uint32_t dt_ms) {
    motion_log_ms += dt_ms;
    have_motion = input_read_motion(&motion);
    if (!have_motion) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        return;
    }
    tilt_update(&tilt, motion.acceleration, motion.turn_speed, dt_ms);
    motion_probe = motion_probe_measure(motion.acceleration);
    if (motion_log_ms >= MOTION_LOG_PERIOD_MS) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        ESP_LOGI(TAG, "motion raw %d,%d,%d tilt %d bearing %d filt %d,%d str %d shake %d turn %d",
                 motion.acceleration.x, motion.acceleration.y, motion.acceleration.z,
                 (int)lroundf(motion_probe.tilt_deg),
                 (int)lroundf(motion_probe.bearing_deg) % MOTION_PROBE_FULL_TURN_DEG, tilt_x(&tilt), tilt_y(&tilt),
                 tilt_strength(&tilt), tilt_shake(&tilt), motion.turn_speed);
    }
}

static void
format_g(char* out, size_t len, float counts) {
    const int milli = (int)lroundf(counts * MOTION_PROBE_MILLI_SCALE / INPUT_ACCEL_ONE_G);
    snprintf(out, len, "%c%d.%0*d", milli < 0 ? '-' : '+', abs(milli) / MOTION_PROBE_MILLI_SCALE,
             MOTION_PROBE_MILLI_DIGITS, abs(milli) % MOTION_PROBE_MILLI_SCALE);
}

static void
draw_motion_dot(mu_Context* ctx, int x, int y, int radius, int cx, int cy, int side, uint32_t color) {
    const vec2i_t offset = motion_probe_bubble(x, y, radius, display_quarter_now());
    mu_draw_rect(ctx, mu_rect(cx + offset.x - side / 2, cy + offset.y - side / 2, side, side), ui_rgb(color));
}

static void
draw_motion(mu_Context* ctx) {
    if (!have_motion) {
        hud_line(ctx, 0, "NO MOTION SENSOR");
        return;
    }
    char line[MOTION_HUD_TEXT_SIZE];
    char x[MOTION_G_TEXT_SIZE], y[MOTION_G_TEXT_SIZE], z[MOTION_G_TEXT_SIZE];
    char magnitude[MOTION_G_TEXT_SIZE], plane[MOTION_G_TEXT_SIZE];
    format_g(x, sizeof x, motion.acceleration.x);
    format_g(y, sizeof y, motion.acceleration.y);
    format_g(z, sizeof z, motion.acceleration.z);
    format_g(magnitude, sizeof magnitude, motion_probe.magnitude_counts);
    format_g(plane, sizeof plane, motion_probe.plane_counts);
    snprintf(line, sizeof line, "X%s Y%s Z%s |A|%sG", x, y, z, magnitude + 1);
    hud_line(ctx, 0, line);
    snprintf(line, sizeof line, "TILT %d DEG FROM FLAT", (int)lroundf(motion_probe.tilt_deg));
    hud_line(ctx, 1, line);
    snprintf(line, sizeof line, "BEARING %d DEG PLANE %sG",
             (int)lroundf(motion_probe.bearing_deg) % MOTION_PROBE_FULL_TURN_DEG, plane + 1);
    hud_line(ctx, 2, line);
    format_g(x, sizeof x, tilt_x(&tilt));
    format_g(y, sizeof y, tilt_y(&tilt));
    snprintf(line, sizeof line, "F %s,%s S%d K%d T%d", x, y, tilt_strength(&tilt), tilt_shake(&tilt),
             motion.turn_speed);
    hud_line(ctx, 3, line);

    const int top = HUD_TOP + MOTION_HUD_ROWS * HUD_LINE_H + MOTION_MARGIN;
    const int bottom = screen_h - MOTION_MARGIN - 2 * HUD_LINE_H;
    int radius = (bottom - top) / 2;
    radius = radius < MOTION_RING_RADIUS ? radius : MOTION_RING_RADIUS;
    const int width_radius = screen_w / 2 - MOTION_MARGIN;
    radius = radius < width_radius ? radius : width_radius;
    const int cx = screen_w / 2;
    const int cy = (top + bottom) / 2;
    for (int row = -radius; row <= radius; row += ARC_STEP) {
        const int extent = motion_probe_ring_extent(radius, row);
        const int previous = row > -radius ? motion_probe_ring_extent(radius, row - ARC_STEP) : 0;
        const int left = extent > previous ? extent : previous;
        const int width = abs(extent - previous) + 1;
        mu_draw_rect(ctx, mu_rect(cx - left, cy + row, width, ARC_STEP), ui_rgb(COL_MOTION_RING));
        mu_draw_rect(ctx, mu_rect(cx + left - width + 1, cy + row, width, ARC_STEP), ui_rgb(COL_MOTION_RING));
    }
    draw_motion_dot(ctx, motion.acceleration.x, motion.acceleration.y, radius, cx, cy, MOTION_DOT_SIDE, COL_MISS);
    draw_motion_dot(ctx, tilt_x(&tilt), tilt_y(&tilt), radius, cx, cy, MOTION_FILTER_SIDE, COL_MOTION_FILTER);
    ui_text_in(ctx, mu_rect(0, bottom, screen_w / 2, HUD_LINE_H), "RAW", ui_rgb(COL_MISS), MOTION_HUD_SCALE,
               UI_ALIGN_CENTRE);
    ui_text_in(ctx, mu_rect(screen_w / 2, bottom, screen_w / 2, HUD_LINE_H), "FILTER", ui_rgb(COL_MOTION_FILTER),
               MOTION_HUD_SCALE, UI_ALIGN_CENTRE);
    ui_text_in(ctx, mu_rect(0, bottom + HUD_LINE_H, screen_w, HUD_LINE_H), "RING 1G  BEARING 0=+X, +TOWARD +Y",
               ui_rgb(COL_MOTION_RING), MOTION_HUD_SCALE, UI_ALIGN_CENTRE);
}

static void
resize_page(void) {
    if (ui_width() == screen_w && ui_height() == screen_h) {
        return;
    }
    if (mode == MODE_MOTION) {
        screen_w = ui_width();
        screen_h = ui_height();
    } else {
        place_target();
    }
}

static void
update_page(uint32_t dt_ms, const input_t* input) {
    if (mode == MODE_MOTION) {
        read_motion(dt_ms);
        return;
    }
    track_touch(dt_ms, input);
    if (mode == MODE_BEZEL) {
        drag_bezel(input);
    }
}

static void
draw_page(mu_Context* ctx) {
    if (mode == MODE_MOTION) {
        draw_motion(ctx);
        return;
    }
    draw_hud(ctx);
    if (mode == MODE_BEZEL) {
        draw_corner_arcs(ctx);
    } else {
        draw_target(ctx);
    }
}

static void
input_lab_frame(uint32_t dt_ms, const input_t* input) {
    if (input->boot.pressed) {
        mode = (lab_mode_t)((mode + 1) % MODE_COUNT);
        start_round();
    }
    resize_page();
    update_page(dt_ms, input);

    mu_Context* ctx = ui_context();
    ui_begin(input);
    if (ui_begin_screen(ctx, "Input Lab", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        draw_page(ctx);
        mu_end_window(ctx);
    }
    ui_end(COL_BACKGROUND);
}

static void
input_lab_exit(void) {
    log_panel_range();
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

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
 * the end of every round. A ruler in the middle of each edge draws a short
 * 1 px line at every depth 0..RULER_DEPTH from that edge, labelled every
 * RULER_LABEL_EVERY: the first line that shows is how many pixels the glass
 * hides there.
 *
 * The motion page shows acceleration, gravity angles, a bubble level and
 * the tilt filter, with a 10 Hz "motion ..." console line. Numbers stay in
 * panel axes; the whole page stays fixed in the default landscape orientation.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

#include "app/app.h"
#include "apps/input_lab/imu_probe.h"
#include "apps/input_lab/motion_probe.h"
#include "apps/input_lab/touch_probe.h"
#include "core/timing.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "input/gravity.h"
#include "input/input_shell.h"
#include "input/tilt.h"
#include "math/scalar/mathi.h"
#include "services/tune.h"
#include "ui/controls/ui_style.h"
#include "ui/controls/ui_widgets.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

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
#define RULER_DEPTH          20
#define RULER_SLOT           12
#define RULER_TICK           8
#define RULER_LABEL_EVERY    5
#define RULER_LABEL_DEPTH    32
#define RULER_LABEL_SIDE     20
#define COL_RULER            0xFFFFFF
#define COL_RULER_MAJOR      0xFF3030
#define MOTION_G_TEXT_SIZE   16
#define MOTION_MILLI_DIGITS  3
#define OFFSET_DIGITS        1
#define OFFSET_SCALE         10
#define MOTION_HUD_TEXT_SIZE 96
#define MOTION_US_PER_MS     1000
#define MOTION_LOG_PERIOD_MS 100
#define MOTION_RING_RADIUS   88
#define MOTION_DOT_SIDE      7
#define MOTION_FILTER_SIDE   11
#define MOTION_MARGIN        16
#define MOTION_HUD_ROWS      5
#define MOTION_HUD_SCALE     1
#define COL_HIT              0x40E060
#define COL_MISS             0xFFB020
#define COL_TARGET           0xFF00FF
#define COL_HUD              0xE0E0E0
#define COL_MOTION_FILTER    0x40E0FF
#define COL_MOTION_RING      0x607080

typedef enum { MODE_RANDOM, MODE_GRID, MODE_BEZEL, MODE_MOTION, MODE_COUNT } lab_mode_t;

typedef struct {
    int cx, cy, radius;
    mu_Rect bounds;
} motion_ring_t;

static const char* const MODE_NAMES[MODE_COUNT] = {"random", "grid", "bezel", "motion"};

static lab_mode_t mode;
static tilt_t tilt;
static gravity_t gravity;
static input_motion_t motion;
static vec3i_t raw_acceleration;
static imu_probe_round_t imu_round;
static motion_probe_t motion_probe;
static bool have_motion;
static uint32_t motion_log_ms;
static int64_t motion_read_us;
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
        gravity_reset(&gravity);
        have_motion = false;
        motion_log_ms = 0;
        motion_read_us = timing_now_us();
        imu_round = (imu_probe_round_t){0};
        const bool sideways = (DISPLAY_DEFAULT_QUARTER % 2) != 0;
        screen_w = sideways ? GFX_HEIGHT : GFX_WIDTH;
        screen_h = sideways ? GFX_WIDTH : GFX_HEIGHT;
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
    const int aim_x = target.x + (target.side / 2);
    const int aim_y = target.y + (target.side / 2);
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

/* One tick per depth along each edge's middle; the top and bottom rulers
 * step along x, the side rulers along y. Every RULER_LABEL_EVERY-th tick is
 * red and labelled further in. */
static void
draw_edge_rulers(mu_Context* ctx) {
    const int span = (RULER_DEPTH + 1) * RULER_SLOT;
    const int left = (screen_w - span) / 2;
    const int top = (screen_h - span) / 2;
    for (int depth = 0; depth <= RULER_DEPTH; depth++) {
        const bool major = depth % RULER_LABEL_EVERY == 0;
        const mu_Color ink = ui_rgb(major ? COL_RULER_MAJOR : COL_RULER);
        const int x = left + (depth * RULER_SLOT);
        const int y = top + (depth * RULER_SLOT);
        mu_draw_rect(ctx, mu_rect(x, depth, RULER_TICK, 1), ink);
        mu_draw_rect(ctx, mu_rect(x, screen_h - 1 - depth, RULER_TICK, 1), ink);
        mu_draw_rect(ctx, mu_rect(depth, y, 1, RULER_TICK), ink);
        mu_draw_rect(ctx, mu_rect(screen_w - 1 - depth, y, 1, RULER_TICK), ink);
        if (!major) {
            continue;
        }
        char label[4];
        if (snprintf(label, sizeof label, "%d", depth) < 0) {
            continue;
        }
        const int lx = x + (RULER_TICK / 2) - (RULER_LABEL_SIDE / 2);
        const int ly = y + (RULER_TICK / 2) - (RULER_LABEL_SIDE / 2);
        const int far = RULER_LABEL_DEPTH + RULER_LABEL_SIDE;
        ui_text_in(ctx, mu_rect(lx, RULER_LABEL_DEPTH, RULER_LABEL_SIDE, RULER_LABEL_SIDE), label, ink, 1,
                   UI_ALIGN_CENTRE);
        ui_text_in(ctx, mu_rect(lx, screen_h - far, RULER_LABEL_SIDE, RULER_LABEL_SIDE), label, ink, 1,
                   UI_ALIGN_CENTRE);
        ui_text_in(ctx, mu_rect(RULER_LABEL_DEPTH, ly, RULER_LABEL_SIDE, RULER_LABEL_SIDE), label, ink, 1,
                   UI_ALIGN_CENTRE);
        ui_text_in(ctx, mu_rect(screen_w - far, ly, RULER_LABEL_SIDE, RULER_LABEL_SIDE), label, ink, 1,
                   UI_ALIGN_CENTRE);
    }
}

/* The bezel page's rulers own the top edge, so its HUD sits mid-screen. */
static void
hud_line(mu_Context* ctx, int row, const char* text) {
    const int hud_top = mode == MODE_BEZEL ? (screen_h / 2) - (2 * HUD_LINE_H) : HUD_TOP;
    ui_text_in(ctx, mu_rect(0, hud_top + (row * HUD_LINE_H), screen_w, HUD_LINE_H), text, ui_rgb(COL_HUD),
               mode == MODE_MOTION ? MOTION_HUD_SCALE : HUD_SCALE, UI_ALIGN_CENTRE);
}

static void
hud_offset_line(mu_Context* ctx, int row, const char* label, float mean, float spread) {
    char line[64];
    char mean_text[24], spread_text[24];
    motion_probe_format_fixed(mean_text, sizeof mean_text, (int)lroundf(mean * OFFSET_SCALE), OFFSET_DIGITS, true);
    motion_probe_format_fixed(spread_text, sizeof spread_text, (int)lroundf(spread * OFFSET_SCALE), OFFSET_DIGITS,
                              false);
    if (snprintf(line, sizeof line, "%s %s SD %s", label, mean_text, spread_text) < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, row, line);
}

static void
draw_offsets(mu_Context* ctx) {
    char line[64];
    const int hit_pct = stats.taps > 0 ? (100 * stats.hits + stats.taps / 2) / stats.taps : 0;

    if (snprintf(line, sizeof line, "%s %d  HITS %d%%", mode == MODE_GRID ? "GRID" : "RANDOM", stats.taps, hit_pct)
        < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, 0, line);
    hud_offset_line(ctx, 1, "DX", touch_probe_mean_dx(&stats), touch_probe_spread_dx(&stats));
    hud_offset_line(ctx, 2, "DY", touch_probe_mean_dy(&stats), touch_probe_spread_dy(&stats));
}

static void
draw_hud(mu_Context* ctx) {
    char line[64];
    if (mode == MODE_BEZEL) {
        if (snprintf(line, sizeof line, "BEZEL R %d", bezel_radius) < 0) {
            line[0] = '\0';
        }
        hud_line(ctx, 0, line);
        hud_line(ctx, 1, "DRAG UP OR DOWN");
    } else {
        draw_offsets(ctx);
    }
    if (panel_max_x >= 0) {
        if (snprintf(line, sizeof line, "PANEL X %d-%d Y %d-%d", panel_min_x, panel_max_x, panel_min_y, panel_max_y)
            < 0) {
            line[0] = '\0';
        }
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
    raw_acceleration = vec3i_add(motion.acceleration, input_accel_offset());
    tilt_update(&tilt, motion.acceleration, motion.turn_speed, dt_ms);
    gravity_update(&gravity, &motion, dt_ms);
    motion_probe = motion_probe_measure(motion.acceleration);
    if (motion_log_ms >= MOTION_LOG_PERIOD_MS) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        ESP_LOGI(TAG,
                 "motion raw %d,%d,%d tilt %d bearing %d filt %d,%d str %d shake %d turn %d rot %d,%d,%d quarter %d "
                 "grav_mg %d,%d,%d bias_dps10 %d,%d,%d",
                 raw_acceleration.x, raw_acceleration.y, raw_acceleration.z, (int)lroundf(motion_probe.tilt_deg),
                 motion_probe.bearing_deg, tilt_x(&tilt), tilt_y(&tilt), tilt_strength(&tilt), tilt_shake(&tilt),
                 motion.turn_speed, motion.rotation.x, motion.rotation.y, motion.rotation.z, display_quarter_now(),
                 (int)lroundf(gravity.down.x * IMU_PROBE_MILLI), (int)lroundf(gravity.down.y * IMU_PROBE_MILLI),
                 (int)lroundf(gravity.down.z * IMU_PROBE_MILLI), (int)lroundf(gravity.gyro_bias.x * OFFSET_SCALE),
                 (int)lroundf(gravity.gyro_bias.y * OFFSET_SCALE), (int)lroundf(gravity.gyro_bias.z * OFFSET_SCALE));
    }
}

static void
log_imu_pose(void) {
    const int pose = imu_round.pose - 1;
    const imu_probe_average_t* average = &imu_round.poses[pose];
    const input_motion_t mean = imu_probe_average_read(average);
    ESP_LOGI(TAG, "pose %s %d %d %d %d %d %d %d", imu_probe_pose_name(pose), mean.acceleration.x, mean.acceleration.y,
             mean.acceleration.z, mean.rotation.x, mean.rotation.y, mean.rotation.z, average->n);
    if (imu_round.active) {
        return;
    }
    const imu_probe_fit_t f = imu_round.fit;
    const vec3i_t offset_mg = imu_probe_mg(imu_probe_saved_offset(input_accel_offset(), &f));
    ESP_LOGI(TAG, "calib x off %d scale %d y off %d scale %d z off %d scale %d gyro %d,%d,%d", offset_mg.x,
             f.scale_ppm.x, offset_mg.y, f.scale_ppm.y, offset_mg.z, f.scale_ppm.z, f.gyro_mdps.x, f.gyro_mdps.y,
             f.gyro_mdps.z);
}

static void
update_imu_round(uint32_t dt_ms, const input_t* input) {
    if (input->pressed && !imu_round.active && imu_round.pose != IMU_PROBE_POSE_COUNT) {
        imu_probe_start(&imu_round, DISPLAY_DEFAULT_QUARTER);
        ESP_LOGI(TAG, "round imu quarter %d", DISPLAY_DEFAULT_QUARTER);
    }
    read_motion(dt_ms);
    const int64_t now_us = timing_now_us();
    const int64_t elapsed_ms = (now_us - motion_read_us) / MOTION_US_PER_MS;
    motion_read_us = now_us;
    /* The probe needs the real reading gap: the shell clamps frame time, hiding stalls
     * that must discard settling and measurement windows. */
    const uint32_t read_dt_ms = elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
    if (imu_probe_update(&imu_round, have_motion ? &motion : NULL, read_dt_ms)) {
        log_imu_pose();
    }
}

typedef enum { IMU_ACTION_NONE, IMU_ACTION_SAVE, IMU_ACTION_CLEAR } imu_action_t;

static imu_action_t
draw_imu_round(mu_Context* ctx) {
    char line[MOTION_HUD_TEXT_SIZE];
    if (imu_round.active) {
        char label[IMU_PROBE_LABEL_SIZE];
        imu_probe_pose_label(imu_round.pose, label, sizeof label);
        snprintf(line, sizeof line, "%s  POSE %d/%d", label, imu_round.pose + 1, IMU_PROBE_POSE_COUNT);
        hud_line(ctx, 0, line);
        if (imu_round.settle_ms < IMU_PROBE_SETTLE_MS) {
            hud_line(ctx, 1, "HOLD STILL");
        } else {
            snprintf(line, sizeof line, "MEASURING %u%%",
                     (unsigned)(imu_round.window_ms * IMU_PROBE_PERCENT / IMU_PROBE_WINDOW_MS));
            hud_line(ctx, 1, line);
        }
        hud_line(ctx, 3, "BOOT TO LEAVE ROUND");
        return IMU_ACTION_NONE;
    }
    const imu_probe_fit_t f = imu_round.fit;
    const vec3i_t offset_mg = imu_probe_mg(f.calib.offset);
    const int off[] = {offset_mg.x, offset_mg.y, offset_mg.z};
    const int scale[] = {f.scale_ppm.x, f.scale_ppm.y, f.scale_ppm.z};
    const int gyro[] = {f.gyro_mdps.x, f.gyro_mdps.y, f.gyro_mdps.z};
    static const char* const axes[] = {"X", "Y", "Z"};
    for (int axis = 0; axis < 3; axis++) {
        snprintf(line, sizeof line, "%s RESID %dMG SCALE %dPPM GYRO %dMDPS", axes[axis], off[axis], scale[axis],
                 gyro[axis]);
        hud_line(ctx, axis, line);
    }
    const ui_theme_t theme = ui_theme_from_style(ctx->style);
    const int top = HUD_TOP + (MOTION_HUD_ROWS * HUD_LINE_H) + MOTION_MARGIN;
    const int width = (screen_w - 3 * MOTION_MARGIN) / 2;
    const ui_widget_button_t save = {.label = "SAVE", .enabled = true};
    const ui_widget_button_t clear = {.label = "CLEAR", .enabled = true};
    const bool saving =
        ui_icon_button(ctx, "imu-save", mu_rect(MOTION_MARGIN, top, width, 2 * HUD_LINE_H), &save, &theme);
    const bool clearing = ui_icon_button(
        ctx, "imu-clear", mu_rect(2 * MOTION_MARGIN + width, top, width, 2 * HUD_LINE_H), &clear, &theme);
    hud_line(ctx, 3, "BOOT TO LEAVE RESULTS");
    return clearing ? IMU_ACTION_CLEAR : saving ? IMU_ACTION_SAVE : IMU_ACTION_NONE;
}

static void
store_imu_result(imu_action_t action) {
    if (action == IMU_ACTION_NONE) {
        return;
    }
    const bool clear = action == IMU_ACTION_CLEAR;
    const vec3i_t offset = clear ? (vec3i_t){0} : imu_probe_saved_offset(input_accel_offset(), &imu_round.fit);
    if (!input_set_accel_offset(offset)) {
        ESP_LOGW(TAG, "imu_calib could not save offset");
        return;
    }
    if (clear) {
        ESP_LOGI(TAG, "imu_calib cleared");
    } else {
        ESP_LOGI(TAG, "imu_calib saved %d %d %d", (int)offset.x, (int)offset.y, (int)offset.z);
    }
    imu_round = (imu_probe_round_t){0};
}

static mu_Rect
motion_dot_rect(int x, int y, motion_ring_t ring, int side) {
    const vec2i_t offset = motion_probe_bubble(x, y, ring.radius, DISPLAY_DEFAULT_QUARTER);
    const mu_Rect bounds = ring.bounds;
    return mu_rect(mathi_clamp(ring.cx + offset.x - (side / 2), bounds.x, bounds.x + bounds.w - side),
                   mathi_clamp(ring.cy + offset.y - (side / 2), bounds.y, bounds.y + bounds.h - side), side, side);
}

static void
draw_gravity_line(mu_Context* ctx, int row) {
    char line[MOTION_HUD_TEXT_SIZE];

    enum { GRAV_X, GRAV_Y, GRAV_Z, GRAV_STRENGTH, GRAV_BIAS_X, GRAV_BIAS_Y, GRAV_BIAS_Z, GRAV_COUNT };

    const float gravity_values[GRAV_COUNT] = {
        gravity.down.x,      gravity.down.y,      gravity.down.z,     gravity_strength(&gravity),
        gravity.gyro_bias.x, gravity.gyro_bias.y, gravity.gyro_bias.z};
    char gravity_text[GRAV_COUNT][MOTION_G_TEXT_SIZE];
    for (size_t i = 0; i < GRAV_COUNT; i++) {
        const bool bias = i >= GRAV_BIAS_X;
        motion_probe_format_fixed(gravity_text[i], sizeof gravity_text[i],
                                  (int)lroundf(gravity_values[i] * (bias ? OFFSET_SCALE : IMU_PROBE_MILLI)),
                                  bias ? OFFSET_DIGITS : MOTION_MILLI_DIGITS, i != GRAV_STRENGTH);
    }
    if (snprintf(line, sizeof line, "GRAV %s,%s,%s S%s B%s,%s,%s", gravity_text[GRAV_X], gravity_text[GRAV_Y],
                 gravity_text[GRAV_Z], gravity_text[GRAV_STRENGTH], gravity_text[GRAV_BIAS_X],
                 gravity_text[GRAV_BIAS_Y], gravity_text[GRAV_BIAS_Z])
        < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, row, line);
}

static void
draw_motion(mu_Context* ctx) {
    if (!have_motion) {
        hud_line(ctx, 0, "NO MOTION SENSOR");
        return;
    }
    if (imu_round.active || imu_round.pose == IMU_PROBE_POSE_COUNT) {
        store_imu_result(draw_imu_round(ctx));
        return;
    }
    char line[MOTION_HUD_TEXT_SIZE];

    enum { VALUE_X, VALUE_Y, VALUE_Z, VALUE_MAGNITUDE, VALUE_PLANE, VALUE_FILTER_X, VALUE_FILTER_Y, VALUE_COUNT };

    const vec3i_t raw_mg = imu_probe_mg(raw_acceleration);
    const vec3i_t filtered_mg = imu_probe_mg((vec3i_t){tilt_x(&tilt), tilt_y(&tilt), 0});
    const int mg[VALUE_COUNT] = {raw_mg.x,
                                 raw_mg.y,
                                 raw_mg.z,
                                 (int)lroundf(motion_probe.magnitude_counts * IMU_PROBE_MILLI / INPUT_ACCEL_ONE_G),
                                 (int)lroundf(motion_probe.plane_counts * IMU_PROBE_MILLI / INPUT_ACCEL_ONE_G),
                                 filtered_mg.x,
                                 filtered_mg.y};
    char values[VALUE_COUNT][MOTION_G_TEXT_SIZE];
    for (size_t i = 0; i < sizeof mg / sizeof mg[0]; i++) {
        motion_probe_format_fixed(values[i], sizeof values[i], mg[i], MOTION_MILLI_DIGITS,
                                  i != VALUE_MAGNITUDE && i != VALUE_PLANE);
    }
    if (snprintf(line, sizeof line, "X%s Y%s Z%s MAG%sG", values[VALUE_X], values[VALUE_Y], values[VALUE_Z],
                 values[VALUE_MAGNITUDE])
        < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, 0, line);
    if (snprintf(line, sizeof line, "TILT %d DEG FROM FLAT", (int)lroundf(motion_probe.tilt_deg)) < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, 1, line);
    if (snprintf(line, sizeof line, "BEARING %d DEG PLANE %sG", motion_probe.bearing_deg, values[VALUE_PLANE]) < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, 2, line);
    if (snprintf(line, sizeof line, "F %s,%s S%d K%d T%d", values[VALUE_FILTER_X], values[VALUE_FILTER_Y],
                 tilt_strength(&tilt), tilt_shake(&tilt), motion.turn_speed)
        < 0) {
        line[0] = '\0';
    }
    hud_line(ctx, 3, line);

    draw_gravity_line(ctx, MOTION_HUD_ROWS - 1);

    const vec3i_t offset = input_accel_offset();
    snprintf(line, sizeof line, "OFFSET %d %d %d Q12", (int)offset.x, (int)offset.y, (int)offset.z);
    hud_line(ctx, MOTION_HUD_ROWS, line);
    const int top = HUD_TOP + ((MOTION_HUD_ROWS + 1) * HUD_LINE_H) + MOTION_MARGIN;
    const int bottom = screen_h - (2 * MOTION_MARGIN) - (2 * HUD_LINE_H);
    const int cx = screen_w / 2;
    const int cy = screen_h / 2;
    const int vertical_radius = mathi_min(cy - top, bottom - cy);
    const int radius = mathi_min(MOTION_RING_RADIUS, mathi_min(vertical_radius, cx - MOTION_MARGIN));
    const motion_ring_t ring = {cx, cy, radius,
                                mu_rect(MOTION_MARGIN, top, screen_w - (2 * MOTION_MARGIN), bottom - top)};
    for (int row = -radius; row <= radius; row += ARC_STEP) {
        const int extent = motion_probe_ring_extent(radius, row);
        const int previous = row > -radius ? motion_probe_ring_extent(radius, row - ARC_STEP) : 0;
        const int left = extent > previous ? extent : previous;
        const int width = abs(extent - previous) + 1;
        mu_draw_rect(ctx, mu_rect(cx - left, cy + row, width, ARC_STEP), ui_rgb(COL_MOTION_RING));
        mu_draw_rect(ctx, mu_rect(cx + left - width + 1, cy + row, width, ARC_STEP), ui_rgb(COL_MOTION_RING));
    }
    mu_draw_rect(ctx, motion_dot_rect(raw_acceleration.x, raw_acceleration.y, ring, MOTION_DOT_SIDE), ui_rgb(COL_MISS));
    mu_draw_box(ctx, motion_dot_rect(tilt_x(&tilt), tilt_y(&tilt), ring, MOTION_FILTER_SIDE),
                ui_rgb(COL_MOTION_FILTER));
    ui_text_in(ctx, mu_rect(0, bottom, screen_w / 2, HUD_LINE_H), "RAW", ui_rgb(COL_MISS), MOTION_HUD_SCALE,
               UI_ALIGN_CENTRE);
    ui_text_in(ctx, mu_rect(screen_w / 2, bottom, screen_w / 2, HUD_LINE_H), "FILTER", ui_rgb(COL_MOTION_FILTER),
               MOTION_HUD_SCALE, UI_ALIGN_CENTRE);
    ui_text_in(ctx, mu_rect(0, bottom + HUD_LINE_H, screen_w, HUD_LINE_H), "TAP TO CALIBRATE", ui_rgb(COL_MOTION_RING),
               MOTION_HUD_SCALE, UI_ALIGN_CENTRE);
}

static void
resize_page(void) {
    if (mode == MODE_MOTION || (ui_width() == screen_w && ui_height() == screen_h)) {
        return;
    }
    place_target();
}

static void
update_page(uint32_t dt_ms, const input_t* input) {
    if (mode == MODE_MOTION) {
        update_imu_round(dt_ms, input);
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
        draw_edge_rulers(ctx);
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

    if (mode == MODE_MOTION) {
        ui_set_transform(ui_transform_quarter_turn(DISPLAY_DEFAULT_QUARTER, GFX_WIDTH, GFX_HEIGHT));
    }
    mu_Context* ctx = ui_context();
    ui_begin(input);
    if (ui_begin_screen(ctx, "Input Lab", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        draw_page(ctx);
        mu_end_window(ctx);
    }
    ui_end(COL_BACKGROUND);
    if (mode == MODE_MOTION) {
        ui_set_transform(shell_transform());
    }
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

/* BOOT cycles touch targets, bezel edges, motion calibration, shake and orientation.
 * Motion pages stay in default landscape with panel axes. Shake compares
 * sample-rate linear acceleration candidates against shared frame-rate shake
 * and retains a guided round in PSRAM for labdump. */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "app/app.h"
#include "apps/input_lab/guided_step.h"
#include "apps/input_lab/imu_probe.h"
#include "apps/input_lab/imu_record.h"
#include "apps/input_lab/motion_feedback.h"
#include "apps/input_lab/motion_probe.h"
#include "apps/input_lab/shake_probe.h"
#include "apps/input_lab/touch_probe.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "display/display_judge.h"
#include "gfx/gfx.h"
#include "input/input_shell.h"
#include "input/motion/gravity.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathi.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

static const char* TAG = "input_lab";

#define TARGET_SIDE           12
#define TARGET_MARGIN         4
#define GRID_COLS             5
#define GRID_ROWS             5
#define GRID_COUNT            (GRID_COLS * GRID_ROWS)
#define SETTLE_FROM_MS        30
#define SETTLE_TO_MS          80
#define HUD_TOP               24
#define HUD_LINE_H            20
#define HUD_SCALE             2
#define MARK_SIDE             4
#define BEZEL_MAX             80
#define BEZEL_DRAG_PX         4
#define ARC_STEP              2
#define RULER_DEPTH           20
#define RULER_SLOT            12
#define RULER_TICK            8
#define RULER_LABEL_EVERY     5
#define RULER_LABEL_DEPTH     32
#define RULER_LABEL_SIDE      20
#define COL_RULER             0xFFFFFF
#define COL_RULER_MAJOR       0xFF3030
#define MOTION_G_TEXT_SIZE    16
#define MOTION_MILLI_DIGITS   3
#define OFFSET_DIGITS         1
#define OFFSET_SCALE          10
#define MOTION_HUD_TEXT_SIZE  96
#define MOTION_US_PER_MS      1000
#define MOTION_LOG_PERIOD_MS  100

#define SHAKE_BAR_X           200
#define SHAKE_BAR_HEIGHT      8
#define SHAKE_BAR_ROW         6
#define SHAKE_BAR_ROW_SPACING 2
/* Log/display scale for the shared shake level. */
#define SHAKE_SHARED_FULL     255
/* Log/display scale for in-plane strength. */
#define MOTION_STRENGTH_ONE   256

#define MOTION_RING_RADIUS    88
#define MOTION_DOT_SIDE       7
#define MOTION_FILTER_SIDE    11
#define MOTION_MARGIN         16
#define MOTION_HUD_ROWS       5
#define MOTION_HUD_SCALE      1
#define COL_HIT               0x40E060
#define COL_MISS              0xFFB020
#define COL_TARGET            0xFF00FF
#define COL_HUD               0xE0E0E0
#define COL_MOTION_FILTER     0x40E0FF
#define COL_MOTION_RING       0x607080

/* White acknowledges a reading; longer cyan ends the round. Both are readable
 * at arm's length while unplugged. */
#define IMU_POSE_FLASH_MS     MOTION_STEP_FLASH_MS
#define IMU_DONE_FLASH_MS     MOTION_DONE_FLASH_MS
#define COL_IMU_POSE_FLASH    0xFFFFFF
#define COL_IMU_DONE_FLASH    0x00FFFF
/* Sub-mg resolution keeps small changes visible in the turn trace. */
#define IMU_GRAVITY_DIGITS    6

typedef enum { MODE_RANDOM, MODE_GRID, MODE_BEZEL, MODE_MOTION, MODE_SHAKE, MODE_ORIENTATION, MODE_COUNT } lab_mode_t;

typedef struct {
    int cx, cy, radius;
    recti_t bounds;
} motion_ring_t;

typedef struct {
    shake_probe_round_t round;
    shake_probe_levels_t levels;
    int64_t start_us;
    uint32_t clock_ms;
} shake_capture_t;

typedef struct {
    bool running;
    orientation_probe_step_t step;
    guided_step_t guide;
    int64_t start_us;
} orientation_capture_t;

static const char* const MODE_NAMES[MODE_COUNT] = {"random", "grid", "bezel", "motion", "shake", "orientation"};

static lab_mode_t mode;
static gravity_t gravity;
static input_motion_t motion;
static input_motion_batch_t* motion_batch;
static input_motion_batch_t* motion_slice;
static shake_capture_t shake_capture;
static orientation_capture_t orientation_capture;
static int motion_samples;
static int motion_lost;
static int64_t motion_drain_us;
/* Bias-corrected gyro rotation since the turn step armed, in degrees: proves the gyro scale on its own. */
static vec3f_t turn_gyro_deg;
static vec3f_t turn_gyro_rate;
static vec3i_t raw_acceleration;
static imu_probe_round_t imu_round;
static imu_record_t imu_record;
static uint32_t imu_flash_ms;
static bool imu_record_failed;
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

static bool
motion_page(void) {
    return mode == MODE_MOTION || mode == MODE_SHAKE || mode == MODE_ORIENTATION;
}

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
    imu_record_free(&imu_record);
    imu_flash_ms = 0;
    imu_record_failed = false;
    orientation_capture.running = false;
    orientation_capture.step = ORIENTATION_PROBE_READ_0;
    stats = (touch_probe_stats_t){0};
    have_last_tap = false;
    grid_next = 0;
    panel_min_x = panel_min_y = 1 << 20;
    panel_max_x = panel_max_y = -1;
    if (motion_page()) {
        gravity_reset(&gravity);
        have_motion = false;
        motion_log_ms = 0;
        motion_samples = 0;
        motion_lost = 0;
        motion_drain_us = 0;
        motion_read_us = timing_now_us();
        imu_round = (imu_probe_round_t){0};
        shake_capture.round = (shake_probe_round_t){0};
        shake_capture.levels = (shake_probe_levels_t){0};
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
    motion_batch = memory_alloc(sizeof *motion_batch, MEMORY_PSRAM);
    if (motion_batch == NULL) {
        ESP_LOGE(TAG, "Cannot allocate motion batch in PSRAM");
    }
    motion_slice = memory_alloc(sizeof *motion_slice, MEMORY_PSRAM);
    rng_seed(&rng, (uint32_t)timing_now_us());
    panel_max_x = -1;
    mode = MODE_RANDOM;
    tap = (touch_probe_tap_t){0};
    start_round();
    ui_invalidate();
}

static affine2x_t
shell_transform(void) {
    return ui_transform_quarter_turn(display_quarter_now(), GFX_WIDTH, GFX_HEIGHT);
}

static vec2i_t
to_screen(vec2i_t point) {
    return ui_transform_unmap_pixel(shell_transform(), point);
}

static void
widen_panel_range(vec2i_t point) {
    panel_min_x = point.x < panel_min_x ? point.x : panel_min_x;
    panel_max_x = point.x > panel_max_x ? point.x : panel_max_x;
    panel_min_y = point.y < panel_min_y ? point.y : panel_min_y;
    panel_max_y = point.y > panel_max_y ? point.y : panel_max_y;
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
finish_tap(vec2i_t release) {
    const touch_probe_sample_t first = tap.samples[0];
    const vec2i_t settled = touch_probe_settled(tap.samples, tap.count, SETTLE_FROM_MS, SETTLE_TO_MS);

    int aim_panel_x;
    int aim_panel_y;
    const vec2i_t first_screen = to_screen((vec2i_t){first.x, first.y});
    const vec2i_t settled_screen = to_screen(settled);
    const vec2i_t release_screen = to_screen(release);
    const int aim_x = target.x + (target.side / 2);
    const int aim_y = target.y + (target.side / 2);
    const affine2x_t transform = shell_transform();
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){aim_x, aim_y});
    aim_panel_x = mapped_pixel.x;
    aim_panel_y = mapped_pixel.y;

    last_hit = touch_probe_record(&stats, target, first_screen);
    last_x = first_screen.x;
    last_y = first_screen.y;
    have_last_tap = true;

    ESP_LOGI(TAG,
             "probe %d %s q%d aim %d,%d aim_panel %d,%d first %d,%d settled %d,%d release %d,%d panel_first %d,%d "
             "panel_settled %d,%d panel_release %d,%d held %d idle %d samples %d %s cal %d",
             stats.taps, MODE_NAMES[mode], display_quarter_now(), aim_x, aim_y, aim_panel_x, aim_panel_y,
             (int)first_screen.x, (int)first_screen.y, (int)settled_screen.x, (int)settled_screen.y,
             (int)release_screen.x, (int)release_screen.y, first.x, first.y, (int)settled.x, (int)settled.y,
             (int)release.x, (int)release.y, tap.held_ms, tap.idle_before_press, tap.count, last_hit ? "HIT" : "miss",
             calibration_on());
    advance_target();
}

static void
track_touch(uint32_t dt_ms, const input_t* input) {
    if (input->pressed || input->down) {
        widen_panel_range(input->point);
    }
    if (!touch_probe_track(&tap, (int)dt_ms, input)) {
        return;
    }
    if (mode == MODE_BEZEL) {
        ESP_LOGI(TAG, "bezel radius %d quarter %d", bezel_radius, display_quarter_now());
    } else {
        finish_tap(input->point);
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
    const vec2i_t press = to_screen(input->press);
    const vec2i_t point = to_screen(input->point);
    const int radius = radius_at_press + ((press.y - point.y) / BEZEL_DRAG_PX);
    bezel_radius = radius < 0 ? 0 : radius > BEZEL_MAX ? BEZEL_MAX : radius;
}

/* Every ARC_STEP-th row of each corner, run out to where the row above
 * began so the steep end of the curve has no gaps. */
static void
draw_corner_arcs(void) {
    for (int row = 0; row < bezel_radius; row += ARC_STEP) {
        const int inset = display_panel_corner_inset(bezel_radius, screen_h, row);
        const int before =
            row >= ARC_STEP ? display_panel_corner_inset(bezel_radius, screen_h, row - ARC_STEP) : inset + 1;
        const int w = before - inset > 1 ? before - inset : 1;
        const int bottom = screen_h - row - ARC_STEP;
        ui_fill((recti_t){inset, row, w, ARC_STEP}, COL_TARGET);
        ui_fill((recti_t){screen_w - inset - w, row, w, ARC_STEP}, COL_TARGET);
        ui_fill((recti_t){inset, bottom, w, ARC_STEP}, COL_TARGET);
        ui_fill((recti_t){screen_w - inset - w, bottom, w, ARC_STEP}, COL_TARGET);
    }
}

/* One tick per depth along each edge's middle; the top and bottom rulers
 * step along x, the side rulers along y. Every RULER_LABEL_EVERY-th tick is
 * red and labelled further in. */
static void
draw_edge_rulers(void) {
    const int span = (RULER_DEPTH + 1) * RULER_SLOT;
    const int left = (screen_w - span) / 2;
    const int top = (screen_h - span) / 2;
    for (int depth = 0; depth <= RULER_DEPTH; depth++) {
        const bool major = depth % RULER_LABEL_EVERY == 0;
        const uint32_t ink = major ? COL_RULER_MAJOR : COL_RULER;
        const int x = left + (depth * RULER_SLOT);
        const int y = top + (depth * RULER_SLOT);
        ui_fill((recti_t){x, depth, RULER_TICK, 1}, ink);
        ui_fill((recti_t){x, screen_h - 1 - depth, RULER_TICK, 1}, ink);
        ui_fill((recti_t){depth, y, 1, RULER_TICK}, ink);
        ui_fill((recti_t){screen_w - 1 - depth, y, 1, RULER_TICK}, ink);
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
        ui_color_begin(UI_COLOR_TEXT, ink);
        ui_scale_begin(1);
        ui_text_centre((recti_t){lx, RULER_LABEL_DEPTH, RULER_LABEL_SIDE, RULER_LABEL_SIDE}, label);
        ui_text_centre((recti_t){lx, screen_h - far, RULER_LABEL_SIDE, RULER_LABEL_SIDE}, label);
        ui_text_centre((recti_t){RULER_LABEL_DEPTH, ly, RULER_LABEL_SIDE, RULER_LABEL_SIDE}, label);
        ui_text_centre((recti_t){screen_w - far, ly, RULER_LABEL_SIDE, RULER_LABEL_SIDE}, label);
        ui_scale_end();
        ui_color_end();
    }
}

static void
hud_text(recti_t r, const char* text, uint32_t ink) {
    ui_color_begin(UI_COLOR_TEXT, ink);
    ui_scale_begin(motion_page() ? MOTION_HUD_SCALE : HUD_SCALE);
    ui_text_centre(r, text);
    ui_scale_end();
    ui_color_end();
}

/* The bezel page's rulers own the top edge, so its HUD sits mid-screen. */
static void
hud_line(int row, const char* text) {
    const int hud_top = mode == MODE_BEZEL ? (screen_h / 2) - (2 * HUD_LINE_H) : HUD_TOP;
    hud_text((recti_t){0, hud_top + (row * HUD_LINE_H), screen_w, HUD_LINE_H}, text, COL_HUD);
}

static void
hud_offset_line(int row, const char* label, float mean, float spread) {
    char line[64];
    char mean_text[24];
    char spread_text[24];
    motion_probe_format_fixed(mean_text, sizeof mean_text, (int)lroundf(mean * OFFSET_SCALE), OFFSET_DIGITS, true);
    motion_probe_format_fixed(spread_text, sizeof spread_text, (int)lroundf(spread * OFFSET_SCALE), OFFSET_DIGITS,
                              false);
    if (snprintf(line, sizeof line, "%s %s SD %s", label, mean_text, spread_text) < 0) {
        line[0] = '\0';
    }
    hud_line(row, line);
}

static void
draw_offsets(void) {
    char line[64];
    const int hit_pct = stats.taps > 0 ? (100 * stats.hits + stats.taps / 2) / stats.taps : 0;

    if (snprintf(line, sizeof line, "%s %d  HITS %d%%", mode == MODE_GRID ? "GRID" : "RANDOM", stats.taps, hit_pct)
        < 0) {
        line[0] = '\0';
    }
    hud_line(0, line);
    hud_offset_line(1, "DX", touch_probe_mean_dx(&stats), touch_probe_spread_dx(&stats));
    hud_offset_line(2, "DY", touch_probe_mean_dy(&stats), touch_probe_spread_dy(&stats));
}

static void
draw_hud(void) {
    char line[64];
    if (mode == MODE_BEZEL) {
        if (snprintf(line, sizeof line, "BEZEL R %d", bezel_radius) < 0) {
            line[0] = '\0';
        }
        hud_line(0, line);
        hud_line(1, "DRAG UP OR DOWN");
    } else {
        draw_offsets();
    }
    if (panel_max_x >= 0) {
        if (snprintf(line, sizeof line, "PANEL X %d-%d Y %d-%d", panel_min_x, panel_max_x, panel_min_y, panel_max_y)
            < 0) {
            line[0] = '\0';
        }
        hud_line(3, line);
    }
}

static void
draw_target(void) {
    if (have_last_tap) {
        const uint32_t mark = last_hit ? COL_HIT : COL_MISS;
        ui_fill((recti_t){last_x - (MARK_SIDE / 2), last_y - (MARK_SIDE / 2), MARK_SIDE, MARK_SIDE}, mark);
    }
    ui_fill((recti_t){target.x, target.y, target.side, target.side}, COL_TARGET);
}

static vec3i_t
motion_log_vector(vec3f_t value, int scale) {
    return (vec3i_t){(int)lroundf(value.x * (float)scale), (int)lroundf(value.y * (float)scale),
                     (int)lroundf(value.z * (float)scale)};
}

static void
integrate_turn_gyro(const input_motion_batch_t* batch) {
    if (imu_round.step != IMU_PROBE_TURN && imu_round.step != IMU_PROBE_TRACE) {
        turn_gyro_deg = (vec3f_t){0};
        turn_gyro_rate = (vec3f_t){0};
        return;
    }
    const float dt = (float)INPUT_MOTION_SAMPLE_US / (float)MATH_US_PER_S;
    for (int i = 0; i < batch->count; i++) {
        const uint32_t held_us = gravity_held_us(batch->lost_before[i]);
        turn_gyro_deg = vec3f_add(turn_gyro_deg, vec3f_scale(turn_gyro_rate, (float)held_us / MATH_US_PER_S));
        turn_gyro_rate =
            vec3f_sub(vec3f_scale(vec3f_from_vec3i(batch->samples[i].rotation), 1.0F / INPUT_ROTATION_ONE_DPS),
                      gravity_gyro_bias(&gravity));
        turn_gyro_deg = vec3f_add(turn_gyro_deg, vec3f_scale(turn_gyro_rate, dt));
    }
}

static void
advance_shake_clock(uint32_t ms) {
    while (ms > shake_capture.clock_ms) {
        const uint32_t remaining = SHAKE_PROBE_STEP_MS - shake_capture.round.ms;
        const uint32_t delta = ms - shake_capture.clock_ms;
        const uint32_t dt = delta < remaining ? delta : remaining;
        shake_capture.clock_ms += dt;
        if (shake_probe_advance(&shake_capture.round, dt)) {
            (void)imu_record_appendf(&imu_record, "shake_step %d %u", shake_capture.round.step,
                                     (unsigned)shake_capture.clock_ms);
            ESP_LOGI(TAG, "shake_step %d %u", shake_capture.round.step, (unsigned)shake_capture.clock_ms);
        }
    }
}

static void
record_orientation_sample(const input_motion_t* sample, int64_t elapsed_us) {
    if (!orientation_capture.running || elapsed_us < 0) {
        return;
    }
    const orientation_probe_step_t step = orientation_capture.step;
    const vec3i_t a = sample->acceleration;
    const vec3i_t r = sample->rotation;
    (void)imu_record_appendf(&imu_record, ORIENTATION_PROBE_SAMPLE_FORMAT, (int)step, (long long)elapsed_us, (int)a.x,
                             (int)a.y, (int)a.z, (int)r.x, (int)r.y, (int)r.z);
}

static void
record_shake_sample(const input_motion_t* sample, int64_t elapsed_us) {
    const vec3f_t linear = gravity_linear(&gravity);
    shake_probe_feed(&shake_capture.levels, linear);
    if (shake_capture.round.step == SHAKE_PROBE_IDLE || elapsed_us < 0) {
        return;
    }
    const uint32_t ms = elapsed_us / MATH_US_PER_MS > SHAKE_PROBE_ROUND_MS ? SHAKE_PROBE_ROUND_MS
                                                                           : (uint32_t)(elapsed_us / MATH_US_PER_MS);
    advance_shake_clock(ms);
    if (ms >= SHAKE_PROBE_ROUND_MS) {
        return;
    }
    const vec3i_t acc = imu_probe_mg(sample->acceleration);
    const vec3i_t lin = motion_log_vector(linear, IMU_PROBE_MILLI);
    (void)imu_record_appendf(
        &imu_record, SHAKE_PROBE_SAMPLE_FORMAT, (int)SHAKE_PROBE_STILL + (int)(ms / SHAKE_PROBE_STEP_MS), (unsigned)ms,
        (int)acc.x, (int)acc.y, (int)acc.z, (int)lin.x, (int)lin.y, (int)lin.z, (int)lroundf(shake_capture.levels.size),
        (int)lroundf(shake_capture.levels.shake.smooth_g * IMU_PROBE_MILLI),
        (int)lroundf(input_shake() * SHAKE_SHARED_FULL));
}

/* Only this adapter depends on batch loss placement. The scratch batch lives in PSRAM. */
static void
read_round_samples(const input_motion_batch_t* batch, int64_t start_us, void (*sink)(const input_motion_t*, int64_t)) {
    if (motion_slice == NULL) {
        return;
    }
    motion_slice->gap = batch->gap;
    motion_slice->count = batch->count > 0 ? 1 : 0;
    if (batch->count == 0) {
        gravity_update_batch(&gravity, motion_slice);
    }
    const int64_t end_us = timing_now_us();
    for (int i = 0; i < batch->count; i++) {
        motion_slice->samples[0] = batch->samples[i];
        motion_slice->lost_before[0] = batch->lost_before[i];
        gravity_update_batch(&gravity, motion_slice);
        motion_slice->gap = false;
        const int64_t elapsed_us = (end_us - start_us) - ((int64_t)((batch->count - i) - 1) * INPUT_MOTION_SAMPLE_US);
        sink(&batch->samples[i], elapsed_us);
    }
}

static void
read_motion(uint32_t dt_ms) {
    motion_log_ms += dt_ms;
    const int64_t drain_start_us = timing_now_us();
    const bool read = motion_batch != NULL && input_read_motion_batch(motion_batch);
    motion_drain_us += timing_now_us() - drain_start_us;
    if (!read) {
        have_motion = false;
    } else if (read && motion_batch->count > 0) {
        motion = motion_batch->samples[motion_batch->count - 1];
        have_motion = true;
        motion_samples += motion_batch->count;
    }
    if (read) {
        for (int i = 0; i < motion_batch->count; i++) {
            motion_lost += motion_batch->lost_before[i];
        }
        if (mode == MODE_MOTION) {
            gravity_update_batch(&gravity, motion_batch);
            integrate_turn_gyro(motion_batch);
        }
    }
    if (read) {
        switch (mode) {
            case MODE_SHAKE: read_round_samples(motion_batch, shake_capture.start_us, record_shake_sample); break;
            case MODE_ORIENTATION:
                read_round_samples(motion_batch, orientation_capture.start_us, record_orientation_sample);
                break;
            default: break;
        }
    }
    if (!have_motion) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        return;
    }
    raw_acceleration = vec3i_add(motion.acceleration, input_accel_offset());
    motion_probe = motion_probe_measure(motion.acceleration);
    if (motion_log_ms >= MOTION_LOG_PERIOD_MS) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        const vec3f_t g = input_gravity();
        const vec3i_t down_mg = motion_log_vector(gravity_down(&gravity), IMU_PROBE_MILLI);
        const vec3i_t bias_dps10 = motion_log_vector(gravity_gyro_bias(&gravity), OFFSET_SCALE);
        ESP_LOGI(TAG,
                 "motion raw %d,%d,%d tilt %d bearing %d filt %d,%d str %d shake %d turn %d rot %d,%d,%d quarter %d "
                 "grav_mg %d,%d,%d bias_dps10 %d,%d,%d samples %d lost %d drain_us %lld",
                 raw_acceleration.x, raw_acceleration.y, raw_acceleration.z, (int)lroundf(motion_probe.tilt_deg),
                 motion_probe.bearing_deg, (int)lroundf(g.x * INPUT_ACCEL_ONE_G), (int)lroundf(g.y * INPUT_ACCEL_ONE_G),
                 (int)lroundf(hypotf(g.x, g.y) * MOTION_STRENGTH_ONE), (int)lroundf(input_shake() * SHAKE_SHARED_FULL),
                 motion.turn_speed, motion.rotation.x, motion.rotation.y, motion.rotation.z, display_quarter_now(),
                 (int)down_mg.x, (int)down_mg.y, (int)down_mg.z, (int)bias_dps10.x, (int)bias_dps10.y,
                 (int)bias_dps10.z, motion_samples, motion_lost, (long long)motion_drain_us);
        motion_samples = 0;
        motion_lost = 0;
        motion_drain_us = 0;
    }
}

#define LOG_IMU_LINE(...)                                                                                              \
    do {                                                                                                               \
        (void)imu_record_appendf(&imu_record, __VA_ARGS__);                                                            \
        ESP_LOGI(TAG, __VA_ARGS__);                                                                                    \
    } while (0)

static void
emit_imu_record(const char* text, size_t length) {
    printf("%.*s", (int)length, text);
    timing_yield();
}

static bool
dump_imu_record(const char* args) {
    if (*args != '\0') {
        return false;
    }
    const bool running = mode == MODE_ORIENTATION ? orientation_capture.running
                         : mode == MODE_SHAKE     ? shake_probe_running(&shake_capture.round)
                                              : imu_round.step != IMU_PROBE_IDLE && imu_round.step != IMU_PROBE_DONE;
    const char* error = imu_record_dump_error(&imu_record, running);
    if (error != NULL) {
        printf("%s", error);
    } else {
        printf(IMU_RECORD_PREFIX "_BEGIN\n");
        imu_record_replay(&imu_record, emit_imu_record);
        printf(IMU_RECORD_PREFIX "_END\n");
    }
    if (fflush(stdout) != 0) {
        return false;
    }
    return true;
}

APP_CONSOLE(IMU_RECORD_COMMAND, dump_imu_record);

static void
log_imu_gravity(int pose) {
    LOG_IMU_LINE("GRAV %s down %.*f %.*f %.*f bias_dps %.*f %.*f %.*f", imu_probe_pose_name(pose), IMU_GRAVITY_DIGITS,
                 (double)gravity_down(&gravity).x, IMU_GRAVITY_DIGITS, (double)gravity_down(&gravity).y,
                 IMU_GRAVITY_DIGITS, (double)gravity_down(&gravity).z, IMU_GRAVITY_DIGITS,
                 (double)gravity_gyro_bias(&gravity).x, IMU_GRAVITY_DIGITS, (double)gravity_gyro_bias(&gravity).y,
                 IMU_GRAVITY_DIGITS, (double)gravity_gyro_bias(&gravity).z);
}

static void
log_imu_pose(void) {
    const int pose = imu_round.pose - 1;
    const imu_rest_t* average = &imu_round.poses[pose];
    const input_motion_t mean = imu_rest_mean(average);
    LOG_IMU_LINE("pose %s %d %d %d %d %d %d %d", imu_probe_pose_name(pose), mean.acceleration.x, mean.acceleration.y,
                 mean.acceleration.z, mean.rotation.x, mean.rotation.y, mean.rotation.z, average->n);
    log_imu_gravity(pose);
    imu_flash_ms = IMU_POSE_FLASH_MS;
    if (imu_round.step == IMU_PROBE_POSES) {
        return;
    }
    const imu_probe_fit_t f = imu_round.fit;
    const vec3i_t offset_mg = imu_probe_mg(imu_probe_saved_offset(input_accel_offset(), &f));
    LOG_IMU_LINE("calib x off %d scale %d y off %d scale %d z off %d scale %d gyro %d,%d,%d", offset_mg.x,
                 f.scale_ppm.x, offset_mg.y, f.scale_ppm.y, offset_mg.z, f.scale_ppm.z, f.gyro_mdps.x, f.gyro_mdps.y,
                 f.gyro_mdps.z);
}

static void
update_imu_round(uint32_t dt_ms, const input_t* input) {
    imu_flash_ms = dt_ms < imu_flash_ms ? imu_flash_ms - dt_ms : 0;
    if (input->pressed && imu_round.step == IMU_PROBE_IDLE) {
        imu_record_failed = !imu_record_start(&imu_record, IMU_RECORD_CAPACITY);
        if (!imu_record_failed) {
            imu_probe_start(&imu_round, DISPLAY_DEFAULT_QUARTER);
            LOG_IMU_LINE("round imu quarter %d", DISPLAY_DEFAULT_QUARTER);
        }
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
    const imu_probe_step_t previous = imu_round.step;
    if (imu_probe_turn_update(&imu_round, have_motion ? &motion : NULL, gravity_down(&gravity), read_dt_ms)) {
        const vec3f_t down = imu_round.trace_down;
        const vec3f_t acceleration = vec3f_from_vec3i(motion.acceleration);
        const vec3f_t direction =
            vec3f_dot(acceleration, acceleration) > 0.0F ? vec3f_normalize(acceleration) : (vec3f_t){0};
        LOG_IMU_LINE("turn ms %u down %.*f %.*f %.*f accel %.*f %.*f %.*f gyro_deg %.1f", (unsigned)imu_round.trace_ms,
                     IMU_GRAVITY_DIGITS, (double)down.x, IMU_GRAVITY_DIGITS, (double)down.y, IMU_GRAVITY_DIGITS,
                     (double)down.z, IMU_GRAVITY_DIGITS, (double)direction.x, IMU_GRAVITY_DIGITS, (double)direction.y,
                     IMU_GRAVITY_DIGITS, (double)direction.z, (double)sqrtf(vec3f_dot(turn_gyro_deg, turn_gyro_deg)));
    }
    if (previous != IMU_PROBE_DONE && imu_round.step == IMU_PROBE_DONE) {
        imu_flash_ms = IMU_DONE_FLASH_MS;
    }
}

static void
update_shake_round(uint32_t dt_ms, const input_t* input) {
    if (input->pressed && !shake_probe_running(&shake_capture.round)) {
        imu_record_failed =
            motion_batch == NULL || motion_slice == NULL || !imu_record_start(&imu_record, IMU_RECORD_SHAKE_CAPACITY);
        if (!imu_record_failed) {
            shake_probe_start(&shake_capture.round);
            shake_capture.start_us = timing_now_us();
            shake_capture.clock_ms = 0;
            LOG_IMU_LINE("round shake");
            LOG_IMU_LINE("shake_step %d 0", shake_capture.round.step);
        }
    }
    const bool was_done = shake_capture.round.step == SHAKE_PROBE_DONE;
    read_motion(dt_ms);
    if (shake_probe_running(&shake_capture.round)) {
        const int64_t elapsed_ms = (timing_now_us() - shake_capture.start_us) / MATH_US_PER_MS;
        advance_shake_clock(elapsed_ms > SHAKE_PROBE_ROUND_MS ? SHAKE_PROBE_ROUND_MS : (uint32_t)elapsed_ms);
    } else if (was_done) {
        (void)shake_probe_advance(&shake_capture.round, dt_ms);
    }
}

static bool
start_orientation_round(void) {
    imu_record_failed =
        motion_batch == NULL || motion_slice == NULL || !imu_record_start(&imu_record, IMU_RECORD_ORIENTATION_CAPACITY);
    if (!imu_record_failed) {
        orientation_capture.running = true;
        orientation_capture.step = ORIENTATION_PROBE_READ_0;
        orientation_capture.start_us = timing_now_us();
        guided_step_begin(&orientation_capture.guide, orientation_capture.step, orientation_capture.start_us);
        LOG_IMU_LINE("round orientation");
        LOG_IMU_LINE("orientation_step %d 0 %d", (int)orientation_capture.step,
                     orientation_probe_target(orientation_capture.step));
        LOG_IMU_LINE("orientation_gate %d 0 begin", (int)orientation_capture.step);
    }
    return !imu_record_failed;
}

static void
update_orientation_round(uint32_t dt_ms, const input_t* input) {
    imu_flash_ms = dt_ms < imu_flash_ms ? imu_flash_ms - dt_ms : 0;
    bool started = false;
    if (input->pressed && !orientation_capture.running) {
        started = start_orientation_round();
    }
    read_motion(dt_ms);
    if (orientation_capture.running && !started) {
        const int64_t now_us = timing_now_us();
        const int64_t elapsed_us = now_us - orientation_capture.start_us;
        const guided_step_event_t event = guided_step_update(&orientation_capture.guide, input->pressed, now_us,
                                                             (int64_t)ORIENTATION_PROBE_STEP_MS * MATH_US_PER_MS);
        if (event == GUIDED_STEP_ARMED) {
            LOG_IMU_LINE("orientation_gate %d %lld end", (int)orientation_capture.step, (long long)elapsed_us);
        } else if (event == GUIDED_STEP_NEXT) {
            orientation_capture.step = (orientation_probe_step_t)orientation_capture.guide.step;
            imu_flash_ms = orientation_capture.step == ORIENTATION_PROBE_DONE ? IMU_DONE_FLASH_MS : IMU_POSE_FLASH_MS;
            LOG_IMU_LINE("orientation_step %d %lld %d", (int)orientation_capture.step, (long long)elapsed_us,
                         orientation_probe_target(orientation_capture.step));
            if (orientation_capture.step == ORIENTATION_PROBE_DONE) {
                orientation_capture.running = false;
            } else {
                LOG_IMU_LINE("orientation_gate %d %lld begin", (int)orientation_capture.step, (long long)elapsed_us);
            }
        }
    }
}

static void
draw_orientation_guide(void) {
    const int target = orientation_probe_target(orientation_capture.step);
    const motion_probe_t reading = motion_probe_measure(raw_acceleration);
    const guided_step_view_t view = {
        .action = orientation_probe_action(orientation_capture.step),
        .target = target == DISPLAY_QUARTER_NONE ? NULL : display_quarter_name(target),
        .current_bearing_deg = reading.bearing_deg,
        .committed_bearing_deg = orientation_probe_bearing(display_quarter_now()),
        .target_bearing_deg = orientation_probe_bearing(target),
        .tilt_deg = (int)lroundf(reading.tilt_deg),
        .target_tilt_deg = target == DISPLAY_QUARTER_NONE ? 0 : ORIENTATION_PROBE_READING_TILT_DEG,
        .width = screen_w,
        .height = screen_h,
    };
    guided_step_draw(&orientation_capture.guide, &view);
}

static void
draw_orientation(void) {
    char line[MOTION_HUD_TEXT_SIZE];
    hud_line(0, "ORIENTATION / BOOT TO CHANGE PAGE");
    orientation_probe_prompt(orientation_capture.running || orientation_capture.step == ORIENTATION_PROBE_DONE
                                 ? orientation_capture.step
                                 : ORIENTATION_PROBE_READ_0,
                             line, sizeof line);
    hud_line(1, line);
    if (orientation_capture.running) {
        const int64_t left_us = guided_step_remaining_us(&orientation_capture.guide, timing_now_us(),
                                                         (int64_t)ORIENTATION_PROBE_STEP_MS * MATH_US_PER_MS);
        (void)snprintf(line, sizeof line, "STEP %d/%d  %lld S LEFT", orientation_capture.step + 1,
                       ORIENTATION_PROBE_DONE, (long long)((left_us + MATH_US_PER_S - 1) / MATH_US_PER_S));
        hud_line(2, line);
        if (imu_record.truncated) {
            hud_line(4, "TRANSCRIPT FULL - RESTART");
        }
        draw_orientation_guide();
    }
    (void)snprintf(line, sizeof line, "COMMITTED %s", display_quarter_name(display_quarter_now()));
    hud_line(3, line);
    if (!orientation_capture.running) {
        hud_line(5, imu_record_failed ? "NO MEMORY FOR ROUND" : "TAP TO START ROUND");
    }
}

static void
draw_shake(void) {
    char line[MOTION_HUD_TEXT_SIZE];
    hud_line(0, "SHAKE  /  BOOT TO CHANGE PAGE");
    hud_line(1, shake_probe_prompt(shake_capture.round.step));
    if (shake_probe_running(&shake_capture.round)) {
        (void)snprintf(line, sizeof line, "STEP %d/%d  %u S LEFT", shake_capture.round.step - SHAKE_PROBE_STILL + 1,
                       SHAKE_PROBE_STEP_COUNT,
                       (unsigned)((SHAKE_PROBE_STEP_MS - shake_capture.round.ms + MATH_MS_PER_S - 1) / MATH_MS_PER_S));
        hud_line(2, line);
    } else {
        hud_line(2, imu_record_failed ? "NO MEMORY FOR ROUND" : "TAP TO START ROUND");
    }
    if (!have_motion) {
        hud_line(4, "WAITING FOR IMU");
        return;
    }
    const vec3i_t lin = motion_log_vector(gravity_linear(&gravity), IMU_PROBE_MILLI);
    (void)snprintf(line, sizeof line, "LINEAR %d %d %d MG", (int)lin.x, (int)lin.y, (int)lin.z);
    hud_line(4, line);
    static const char* const names[] = {"SIZE", "SMOOTH", "SHARED"};
    const int values[] = {(int)lroundf(shake_capture.levels.size),
                          (int)lroundf(shake_capture.levels.shake.smooth_g * IMU_PROBE_MILLI),
                          (int)lroundf(input_shake() * SHAKE_SHARED_FULL)};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        const int row = SHAKE_BAR_ROW + ((int)i * SHAKE_BAR_ROW_SPACING);
        const int full = i == (sizeof values / sizeof values[0]) - 1 ? SHAKE_SHARED_FULL : IMU_PROBE_MILLI;
        const int width = screen_w - SHAKE_BAR_X - MOTION_MARGIN;
        const int value = values[i] < full ? values[i] : full;
        (void)snprintf(line, sizeof line, "%s %d%s", names[i], values[i], full == SHAKE_SHARED_FULL ? "" : " MG");
        hud_text((recti_t){MOTION_MARGIN, HUD_TOP + (row * HUD_LINE_H), SHAKE_BAR_X - MOTION_MARGIN, HUD_LINE_H}, line,
                 COL_HUD);
        const recti_t bar = {SHAKE_BAR_X, HUD_TOP + (row * HUD_LINE_H), width, SHAKE_BAR_HEIGHT};
        ui_outline(bar, COL_MOTION_RING);
        ui_fill((recti_t){bar.x, bar.y, width * value / full, bar.h}, COL_MOTION_FILTER);
    }
}

typedef enum { IMU_ACTION_NONE, IMU_ACTION_SAVE, IMU_ACTION_CLEAR } imu_action_t;

static bool
draw_imu_turn(void) {
    if (imu_round.step == IMU_PROBE_IDLE || imu_round.step == IMU_PROBE_POSES || imu_round.step == IMU_PROBE_DONE) {
        return false;
    }
    hud_line(0, imu_round.step == IMU_PROBE_UPRIGHT ? "STAND IT UPRIGHT, SCREEN FACING YOU" : "TURN 90 CW QUICKLY");
    hud_line(1, imu_round.step == IMU_PROBE_UPRIGHT ? "HOLD WITHOUT TURNING TO ARM"
                : imu_round.step == IMU_PROBE_TURN  ? "ARMED: TURN NOW"
                                                    : "RECORDING TURN");
    hud_line(3, "BOOT TO LEAVE ROUND");
    return true;
}

static imu_action_t
draw_imu_round(void) {
    if (draw_imu_turn()) {
        return IMU_ACTION_NONE;
    }
    char line[MOTION_HUD_TEXT_SIZE];
    if (imu_round.step == IMU_PROBE_POSES) {
        char label[IMU_PROBE_LABEL_SIZE];
        imu_probe_pose_label(imu_round.pose, label, sizeof label);
        (void)snprintf(line, sizeof line, "%s  POSE %d/%d", label, imu_round.pose + 1, IMU_PROBE_POSE_COUNT);
        hud_line(0, line);
        if ((imu_round.rest.settle_us / MATH_US_PER_MS) < IMU_REST_SETTLE_MS) {
            hud_line(1, "HOLD STILL");
        } else {
            (void)snprintf(
                line, sizeof line, "MEASURING %u%%",
                (unsigned)((imu_round.rest.window_us / MATH_US_PER_MS) * IMU_PROBE_PERCENT / IMU_REST_WINDOW_MS));
            hud_line(1, line);
        }
        hud_line(3, "BOOT TO LEAVE ROUND");
        return IMU_ACTION_NONE;
    }
    const imu_probe_fit_t f = imu_round.fit;
    const vec3i_t offset_mg = imu_probe_mg(f.calib.offset);
    const int off[] = {offset_mg.x, offset_mg.y, offset_mg.z};
    const int scale[] = {f.scale_ppm.x, f.scale_ppm.y, f.scale_ppm.z};
    const int gyro[] = {f.gyro_mdps.x, f.gyro_mdps.y, f.gyro_mdps.z};
    static const char* const axes[] = {"X", "Y", "Z"};
    for (int axis = 0; axis < 3; axis++) {
        (void)snprintf(line, sizeof line, "%s RESID %dMG SCALE %dPPM GYRO %dMDPS", axes[axis], off[axis], scale[axis],
                       gyro[axis]);
        hud_line(axis, line);
    }
    const int top = HUD_TOP + (MOTION_HUD_ROWS * HUD_LINE_H) + MOTION_MARGIN;
    const int width = (screen_w - 3 * MOTION_MARGIN) / 2;
    const bool saving = ui_button((recti_t){MOTION_MARGIN, top, width, 2 * HUD_LINE_H}, "SAVE");
    const bool clearing = ui_button((recti_t){(2 * MOTION_MARGIN) + width, top, width, 2 * HUD_LINE_H}, "CLEAR");
    hud_line(3, "BOOT TO LEAVE RESULTS");
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

static recti_t
motion_dot_rect(int x, int y, motion_ring_t ring, int side) {
    const vec2i_t offset = motion_probe_bubble(x, y, ring.radius, DISPLAY_DEFAULT_QUARTER);
    const recti_t bounds = ring.bounds;
    return (recti_t){mathi_clamp(ring.cx + offset.x - (side / 2), bounds.x, bounds.x + bounds.w - side),
                     mathi_clamp(ring.cy + offset.y - (side / 2), bounds.y, bounds.y + bounds.h - side), side, side};
}

static void
draw_gravity_line(int row) {
    char line[MOTION_HUD_TEXT_SIZE];

    enum { GRAV_X, GRAV_Y, GRAV_Z, GRAV_STRENGTH, GRAV_BIAS_X, GRAV_BIAS_Y, GRAV_BIAS_Z, GRAV_COUNT };

    const float gravity_values[GRAV_COUNT] = {gravity_down(&gravity).x,      gravity_down(&gravity).y,
                                              gravity_down(&gravity).z,      gravity_strength(&gravity),
                                              gravity_gyro_bias(&gravity).x, gravity_gyro_bias(&gravity).y,
                                              gravity_gyro_bias(&gravity).z};
    char gravity_text[GRAV_COUNT][MOTION_G_TEXT_SIZE];
    for (size_t i = 0; i < GRAV_COUNT; i++) {
        const bool bias = i >= GRAV_BIAS_X;
        motion_probe_format_fixed(gravity_text[i], sizeof gravity_text[i],
                                  (int)lroundf(gravity_values[i] * (float)(bias ? OFFSET_SCALE : IMU_PROBE_MILLI)),
                                  bias ? OFFSET_DIGITS : MOTION_MILLI_DIGITS, i != GRAV_STRENGTH);
    }
    if (snprintf(line, sizeof line, "GRAV %s,%s,%s S%s B%s,%s,%s", gravity_text[GRAV_X], gravity_text[GRAV_Y],
                 gravity_text[GRAV_Z], gravity_text[GRAV_STRENGTH], gravity_text[GRAV_BIAS_X],
                 gravity_text[GRAV_BIAS_Y], gravity_text[GRAV_BIAS_Z])
        < 0) {
        line[0] = '\0';
    }
    hud_line(row, line);
}

static void
draw_motion(void) {
    if (!have_motion) {
        hud_line(0, "NO MOTION SENSOR");
        return;
    }
    if (imu_round.step != IMU_PROBE_IDLE) {
        store_imu_result(draw_imu_round());
        return;
    }
    char line[MOTION_HUD_TEXT_SIZE];

    enum { VALUE_X, VALUE_Y, VALUE_Z, VALUE_MAGNITUDE, VALUE_PLANE, VALUE_FILTER_X, VALUE_FILTER_Y, VALUE_COUNT };

    const vec3i_t raw_mg = imu_probe_mg(raw_acceleration);
    const vec3f_t g = input_gravity();
    const vec3i_t filtered_mg = motion_log_vector(g, IMU_PROBE_MILLI);
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
    hud_line(0, line);
    if (snprintf(line, sizeof line, "TILT %d DEG FROM FLAT", (int)lroundf(motion_probe.tilt_deg)) < 0) {
        line[0] = '\0';
    }
    hud_line(1, line);
    if (snprintf(line, sizeof line, "BEARING %d DEG PLANE %sG", motion_probe.bearing_deg, values[VALUE_PLANE]) < 0) {
        line[0] = '\0';
    }
    hud_line(2, line);
    if (snprintf(line, sizeof line, "F %s,%s S%d K%d T%d", values[VALUE_FILTER_X], values[VALUE_FILTER_Y],
                 (int)lroundf(hypotf(g.x, g.y) * MOTION_STRENGTH_ONE), (int)lroundf(input_shake() * SHAKE_SHARED_FULL),
                 motion.turn_speed)
        < 0) {
        line[0] = '\0';
    }
    hud_line(3, line);

    draw_gravity_line(MOTION_HUD_ROWS - 1);

    const vec3i_t offset = input_accel_offset();
    (void)snprintf(line, sizeof line, "OFFSET %d %d %d Q12", (int)offset.x, (int)offset.y, (int)offset.z);
    hud_line(MOTION_HUD_ROWS, line);
    const vec3i_t gyro = input_gyro_offset();
    (void)snprintf(line, sizeof line, "GYRO %d %d %d Q6", (int)gyro.x, (int)gyro.y, (int)gyro.z);
    hud_line(MOTION_HUD_ROWS + 1, line);
    const int top = HUD_TOP + ((MOTION_HUD_ROWS + 2) * HUD_LINE_H) + MOTION_MARGIN;
    const int bottom = screen_h - (2 * MOTION_MARGIN) - (2 * HUD_LINE_H);
    const int cx = screen_w / 2;
    const int cy = (top + bottom) / 2;
    const int vertical_radius = mathi_min(cy - top, bottom - cy);
    const int radius = mathi_min(MOTION_RING_RADIUS, mathi_min(vertical_radius, cx - MOTION_MARGIN));
    const motion_ring_t ring = {cx, cy, radius,
                                (recti_t){MOTION_MARGIN, top, screen_w - (2 * MOTION_MARGIN), bottom - top}};
    for (int row = -radius; row <= radius; row += ARC_STEP) {
        const int extent = mathi_half_chord_round(radius, row);
        const int previous = row > -radius ? mathi_half_chord_round(radius, row - ARC_STEP) : 0;
        const int left = extent > previous ? extent : previous;
        const int width = abs(extent - previous) + 1;
        ui_fill((recti_t){cx - left, cy + row, width, ARC_STEP}, COL_MOTION_RING);
        ui_fill((recti_t){cx + left - width + 1, cy + row, width, ARC_STEP}, COL_MOTION_RING);
    }
    ui_fill(motion_dot_rect(raw_acceleration.x, raw_acceleration.y, ring, MOTION_DOT_SIDE), COL_MISS);
    ui_outline(motion_dot_rect((int)lroundf(g.x * INPUT_ACCEL_ONE_G), (int)lroundf(g.y * INPUT_ACCEL_ONE_G), ring,
                               MOTION_FILTER_SIDE),
               COL_MOTION_FILTER);
    hud_text((recti_t){0, bottom, screen_w / 2, HUD_LINE_H}, "RAW", COL_MISS);
    hud_text((recti_t){screen_w / 2, bottom, screen_w / 2, HUD_LINE_H}, "FILTER", COL_MOTION_FILTER);
    hud_text((recti_t){0, bottom + HUD_LINE_H, screen_w, HUD_LINE_H},
             imu_record_failed ? "NO MEMORY FOR ROUND" : "TAP TO CALIBRATE", COL_MOTION_RING);
}

static void
resize_page(void) {
    if (motion_page() || (ui_width() == screen_w && ui_height() == screen_h)) {
        return;
    }
    place_target();
}

static void
update_page(uint32_t dt_ms, const input_t* input) {
    if (mode == MODE_ORIENTATION) {
        update_orientation_round(dt_ms, input);
        return;
    }
    if (mode == MODE_SHAKE) {
        update_shake_round(dt_ms, input);
        return;
    }
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
draw_page(void) {
    if (mode == MODE_ORIENTATION) {
        draw_orientation();
        return;
    }
    if (mode == MODE_SHAKE) {
        draw_shake();
        return;
    }
    if (mode == MODE_MOTION) {
        draw_motion();
        return;
    }
    draw_hud();
    if (mode == MODE_BEZEL) {
        draw_corner_arcs();
        draw_edge_rulers();
    } else {
        draw_target();
    }
}

static void
draw_imu_flash(void) {
    const uint32_t flash_ms = mode == MODE_SHAKE ? shake_capture.round.flash_ms : imu_flash_ms;
    const bool done =
        mode == MODE_SHAKE ? shake_capture.round.step == SHAKE_PROBE_DONE : imu_round.step == IMU_PROBE_DONE;
    if ((mode != MODE_MOTION && mode != MODE_SHAKE && mode != MODE_ORIENTATION) || flash_ms == 0) {
        return;
    }
    ui_fill((recti_t){0, 0, screen_w, screen_h},
            (done || (mode == MODE_ORIENTATION && orientation_capture.step == ORIENTATION_PROBE_DONE))
                ? COL_IMU_DONE_FLASH
                : COL_IMU_POSE_FLASH);
}

static void
input_lab_frame(uint32_t dt_ms, const input_t* input) {
    if (input->boot.pressed) {
        mode = (lab_mode_t)((mode + 1) % MODE_COUNT);
        start_round();
    }
    resize_page();
    update_page(dt_ms, input);

    if (motion_page()) {
        ui_set_transform(ui_transform_quarter_turn(DISPLAY_DEFAULT_QUARTER, GFX_WIDTH, GFX_HEIGHT));
    }
    ui_begin(input);
    ui_screen();
    draw_page();
    draw_imu_flash();
    ui_end(UI_BACKGROUND);
    if (motion_page()) {
        ui_set_transform(shell_transform());
    }
}

static void
input_lab_exit(void) {
    memory_free(motion_slice);
    motion_slice = NULL;
    memory_free(motion_batch);
    motion_batch = NULL;
    imu_record_free(&imu_record);
    log_panel_range();
}

app_t app_input_lab = {
    .name = "Input Lab",
    .summary = "Touch precision: tap the magenta squares",
    .enter = input_lab_enter,
    .frame = input_lab_frame,
    .exit = input_lab_exit,
    .console = APP_CONSOLE_PTR(dump_imu_record),
    .invalidate = ui_invalidate,
    .home_gesture = true,
};

APP_REGISTER(app_input_lab);

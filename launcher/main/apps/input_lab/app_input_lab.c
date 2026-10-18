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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "app/app.h"
#include "apps/input_lab/imu_probe.h"
#include "apps/input_lab/imu_record.h"
#include "apps/input_lab/motion_probe.h"
#include "apps/input_lab/touch_probe.h"
#include "core/memory.h"
#include "core/timing.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "input/gravity.h"
#include "input/input_shell.h"
#include "input/tilt.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathi.h"
#include "services/tune.h"
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
#define MOTION_DRAIN_MAX_MS  (MATH_MS_PER_S / 2U)

TUNE_OWNER(input_lab);
TUNE(input_lab, imu_drain_ms, 0, 0, MOTION_DRAIN_MAX_MS);
#define MOTION_RING_RADIUS 88
#define MOTION_DOT_SIDE    7
#define MOTION_FILTER_SIDE 11
#define MOTION_MARGIN      16
#define MOTION_HUD_ROWS    5
#define MOTION_HUD_SCALE   1
#define COL_HIT            0x40E060
#define COL_MISS           0xFFB020
#define COL_TARGET         0xFF00FF
#define COL_HUD            0xE0E0E0
#define COL_MOTION_FILTER  0x40E0FF
#define COL_MOTION_RING    0x607080

/* White acknowledges a reading; longer cyan ends the round. Both are readable
 * at arm's length while unplugged. */
#define IMU_POSE_FLASH_MS  160U
#define IMU_DONE_FLASH_MS  600U
#define COL_IMU_POSE_FLASH 0xFFFFFF
#define COL_IMU_DONE_FLASH 0x00FFFF
/* Sub-mg resolution keeps small changes visible in the turn trace. */
#define IMU_GRAVITY_DIGITS 6

typedef enum { MODE_RANDOM, MODE_GRID, MODE_BEZEL, MODE_MOTION, MODE_COUNT } lab_mode_t;

typedef struct {
    int cx, cy, radius;
    recti_t bounds;
} motion_ring_t;

static const char* const MODE_NAMES[MODE_COUNT] = {"random", "grid", "bezel", "motion"};

static lab_mode_t mode;
static tilt_t tilt;
static gravity_t gravity;
static input_motion_t motion;
static input_motion_batch_t* motion_batch;
static int motion_samples;
static int motion_lost;
static uint32_t motion_since_drain_ms;
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
        motion_samples = 0;
        motion_lost = 0;
        motion_since_drain_ms = 0;
        motion_drain_us = 0;
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
    motion_batch = memory_alloc(sizeof *motion_batch, MEMORY_PSRAM);
    if (motion_batch == NULL) {
        ESP_LOGE(TAG, "Cannot allocate motion batch in PSRAM");
    }
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

static void
to_screen(int px, int py, int* x, int* y) {
    const vec2i_t screen = ui_transform_unmap_pixel(shell_transform(), (vec2i_t){px, py});
    *x = screen.x;
    *y = screen.y;
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
    const affine2x_t transform = shell_transform();
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){aim_x, aim_y});
    aim_panel_x = mapped_pixel.x;
    aim_panel_y = mapped_pixel.y;

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
    const int radius = radius_at_press + ((press_y - y) / BEZEL_DRAG_PX);
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
    ui_scale_begin(mode == MODE_MOTION ? MOTION_HUD_SCALE : HUD_SCALE);
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
    char mean_text[24], spread_text[24];
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
        ui_fill((recti_t){last_x - MARK_SIDE / 2, last_y - MARK_SIDE / 2, MARK_SIDE, MARK_SIDE}, mark);
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
    /* Samples lost to FIFO read mode advance at the last rate, as gravity_update_batch does. */
    turn_gyro_deg = vec3f_add(turn_gyro_deg, vec3f_scale(turn_gyro_rate, dt * (float)batch->lost));
    for (int i = 0; i < batch->count; i++) {
        turn_gyro_rate =
            vec3f_sub(vec3f_scale(vec3f_from_vec3i(batch->samples[i].rotation), 1.0F / INPUT_ROTATION_ONE_DPS),
                      gravity_gyro_bias(&gravity));
        turn_gyro_deg = vec3f_add(turn_gyro_deg, vec3f_scale(turn_gyro_rate, dt));
    }
}

static void
read_motion(uint32_t dt_ms) {
    motion_log_ms += dt_ms;
    const int64_t drain_start_us = timing_now_us();
    motion_since_drain_ms += dt_ms;
    const bool due = motion_since_drain_ms >= (uint32_t)imu_drain_ms;
    const bool read = due && motion_batch != NULL && input_read_motion_batch(motion_batch);
    if (due) {
        motion_since_drain_ms = 0;
        motion_drain_us += timing_now_us() - drain_start_us;
    }
    if (due && !read) {
        have_motion = false;
    } else if (read && motion_batch->count > 0) {
        motion = motion_batch->samples[motion_batch->count - 1];
        have_motion = true;
        motion_samples += motion_batch->count;
    }
    if (read) {
        motion_lost += motion_batch->lost;
        gravity_update_batch(&gravity, motion_batch);
        integrate_turn_gyro(motion_batch);
    }
    if (!have_motion) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        return;
    }
    raw_acceleration = vec3i_add(motion.acceleration, input_accel_offset());
    tilt_update(&tilt, motion.acceleration, motion.turn_speed, dt_ms);
    motion_probe = motion_probe_measure(motion.acceleration);
    if (motion_log_ms >= MOTION_LOG_PERIOD_MS) {
        motion_log_ms %= MOTION_LOG_PERIOD_MS;
        const vec3i_t down_mg = motion_log_vector(gravity_down(&gravity), IMU_PROBE_MILLI);
        const vec3i_t bias_dps10 = motion_log_vector(gravity_gyro_bias(&gravity), OFFSET_SCALE);
        ESP_LOGI(TAG,
                 "motion raw %d,%d,%d tilt %d bearing %d filt %d,%d str %d shake %d turn %d rot %d,%d,%d quarter %d "
                 "grav_mg %d,%d,%d bias_dps10 %d,%d,%d samples %d lost %d drain_us %lld",
                 raw_acceleration.x, raw_acceleration.y, raw_acceleration.z, (int)lroundf(motion_probe.tilt_deg),
                 motion_probe.bearing_deg, tilt_x(&tilt), tilt_y(&tilt), tilt_strength(&tilt), tilt_shake(&tilt),
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
emit_imu_record(const char* text) {
    printf("%s", text);
}

static bool
dump_imu_record(const char* args) {
    if (*args != '\0') {
        return false;
    }
    const char* error = imu_record_dump_error(&imu_record, imu_round.step);
    if (error != NULL) {
        printf("%s", error);
    } else {
        printf(IMU_RECORD_PREFIX "_BEGIN\n");
        imu_record_take(&imu_record, emit_imu_record);
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
    const imu_probe_average_t* average = &imu_round.poses[pose];
    const input_motion_t mean = imu_probe_average_read(average);
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
        snprintf(line, sizeof line, "%s  POSE %d/%d", label, imu_round.pose + 1, IMU_PROBE_POSE_COUNT);
        hud_line(0, line);
        if (imu_round.settle_ms < IMU_PROBE_SETTLE_MS) {
            hud_line(1, "HOLD STILL");
        } else {
            snprintf(line, sizeof line, "MEASURING %u%%",
                     (unsigned)(imu_round.window_ms * IMU_PROBE_PERCENT / IMU_PROBE_WINDOW_MS));
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
        snprintf(line, sizeof line, "%s RESID %dMG SCALE %dPPM GYRO %dMDPS", axes[axis], off[axis], scale[axis],
                 gyro[axis]);
        hud_line(axis, line);
    }
    const int top = HUD_TOP + (MOTION_HUD_ROWS * HUD_LINE_H) + MOTION_MARGIN;
    const int width = (screen_w - 3 * MOTION_MARGIN) / 2;
    const bool saving = ui_button((recti_t){MOTION_MARGIN, top, width, 2 * HUD_LINE_H}, "SAVE");
    const bool clearing = ui_button((recti_t){2 * MOTION_MARGIN + width, top, width, 2 * HUD_LINE_H}, "CLEAR");
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
                 tilt_strength(&tilt), tilt_shake(&tilt), motion.turn_speed)
        < 0) {
        line[0] = '\0';
    }
    hud_line(3, line);

    draw_gravity_line(MOTION_HUD_ROWS - 1);

    const vec3i_t offset = input_accel_offset();
    snprintf(line, sizeof line, "OFFSET %d %d %d Q12", (int)offset.x, (int)offset.y, (int)offset.z);
    hud_line(MOTION_HUD_ROWS, line);
    const int top = HUD_TOP + ((MOTION_HUD_ROWS + 1) * HUD_LINE_H) + MOTION_MARGIN;
    const int bottom = screen_h - (2 * MOTION_MARGIN) - (2 * HUD_LINE_H);
    const int cx = screen_w / 2;
    const int cy = screen_h / 2;
    const int vertical_radius = mathi_min(cy - top, bottom - cy);
    const int radius = mathi_min(MOTION_RING_RADIUS, mathi_min(vertical_radius, cx - MOTION_MARGIN));
    const motion_ring_t ring = {cx, cy, radius,
                                (recti_t){MOTION_MARGIN, top, screen_w - (2 * MOTION_MARGIN), bottom - top}};
    for (int row = -radius; row <= radius; row += ARC_STEP) {
        const int extent = motion_probe_ring_extent(radius, row);
        const int previous = row > -radius ? motion_probe_ring_extent(radius, row - ARC_STEP) : 0;
        const int left = extent > previous ? extent : previous;
        const int width = abs(extent - previous) + 1;
        ui_fill((recti_t){cx - left, cy + row, width, ARC_STEP}, COL_MOTION_RING);
        ui_fill((recti_t){cx + left - width + 1, cy + row, width, ARC_STEP}, COL_MOTION_RING);
    }
    ui_fill(motion_dot_rect(raw_acceleration.x, raw_acceleration.y, ring, MOTION_DOT_SIDE), COL_MISS);
    ui_outline(motion_dot_rect(tilt_x(&tilt), tilt_y(&tilt), ring, MOTION_FILTER_SIDE), COL_MOTION_FILTER);
    hud_text((recti_t){0, bottom, screen_w / 2, HUD_LINE_H}, "RAW", COL_MISS);
    hud_text((recti_t){screen_w / 2, bottom, screen_w / 2, HUD_LINE_H}, "FILTER", COL_MOTION_FILTER);
    hud_text((recti_t){0, bottom + HUD_LINE_H, screen_w, HUD_LINE_H},
             imu_record_failed ? "NO MEMORY FOR ROUND" : "TAP TO CALIBRATE", COL_MOTION_RING);
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
draw_page(void) {
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
    if (mode != MODE_MOTION || imu_flash_ms == 0) {
        return;
    }
    ui_fill((recti_t){0, 0, screen_w, screen_h},
            imu_round.step == IMU_PROBE_DONE ? COL_IMU_DONE_FLASH : COL_IMU_POSE_FLASH);
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
    ui_begin(input);
    ui_screen();
    draw_page();
    draw_imu_flash();
    ui_end(COL_BACKGROUND);
    if (mode == MODE_MOTION) {
        ui_set_transform(shell_transform());
    }
}

static void
input_lab_exit(void) {
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

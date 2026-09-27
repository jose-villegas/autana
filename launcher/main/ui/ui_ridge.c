/* ui_ridge - the launcher's layered, gravity-level backdrop. */

#include "ui/ui_ridge.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "gfx/gfx_dither.h"
#include "ui/ridge_curve_generated.h"
#include "ui/ridge_motion.h"
#include "ui/ridge_pose.h"
#include "ui/ridge_theme.h"
#include "util/frame_cost.h"
#include "util/spring_line.h"
#include "util/tune.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define RIDGE_ALLOC(bytes) heap_caps_malloc((bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define RIDGE_ALLOC(bytes) malloc(bytes)
#endif

TUNE_OWNER(ridge);
TUNE(ridge, theme_rgb, 0x1199C8, 0, 0xffffff);
/* Each role starts as the theme's and can then be tuned on its own. */
#if TUNE_ENABLED
TUNE(ridge, sky_top_rgb, 0, 0, 0xffffff);
TUNE(ridge, sky_bottom_rgb, 0, 0, 0xffffff);
TUNE(ridge, background_rgb, 0, 0, 0xffffff);
TUNE(ridge, back0_rgb, 0, 0, 0xffffff);
TUNE(ridge, back1_rgb, 0, 0, 0xffffff);
#endif
TUNE(ridge, fill_pattern, GFX_DITHER_SCANLINES4, 0, GFX_DITHER_PATTERN_COUNT - 1);
TUNE(ridge, lip_px, 12, 1, 48);
TUNE(ridge, back0_offset, -70, -160, 80);
TUNE(ridge, back0_amplitude, 5, 0, 32);
TUNE(ridge, back0_wavelength, 260, 8, 1000);
TUNE(ridge, back0_period_ms, 6000, 100, 60000);
TUNE(ridge, back0_body_alpha, 204, 0, 255);
TUNE(ridge, back0_lip_alpha, 71, 0, 255);
TUNE(ridge, back1_offset, -30, -160, 80);
TUNE(ridge, back1_amplitude, 8, 0, 32);
TUNE(ridge, back1_wavelength, 200, 8, 1000);
TUNE(ridge, back1_period_ms, 4300, 100, 60000);
TUNE(ridge, back1_body_alpha, 204, 0, 255);
TUNE(ridge, back1_lip_alpha, 71, 0, 255);
TUNE(ridge, front_offset, 0, -160, 80);
TUNE(ridge, front_body_alpha, 217, 0, 255);
TUNE(ridge, front_lip_alpha, 76, 0, 255);
TUNE(ridge, pluck_tap, 4500, 0, 8000);
TUNE(ridge, pluck_strum, 3000, 0, 8000);
TUNE(ridge, pluck_width, 40, 2, 80);
TUNE(ridge, spring_tension, 40, 0, 250);
TUNE(ridge, spring_stiffness, 4, 1, 64);
TUNE(ridge, spring_damping, 8, 0, 64);
TUNE(ridge, breath_ms, 5000, 500, 60000);
TUNE(ridge, breath_depth, 164, 0, 256);
TUNE(ridge, breath_smooth, 20, 0, 60);
TUNE(ridge, front_amplitude, 6, 0, 32);
TUNE(ridge, front_wavelength, 164, 8, 1000);
TUNE(ridge, front_period_ms, 2600, 100, 60000);
TUNE(ridge, tilt_push, 350, 0, 1000);
TUNE(ridge, tilt_coast_ms, 2000, 50, 10000);
TUNE(ridge, level_tau_ms, 700, 10, 5000);
TUNE(ridge, gradient_steps, 3, 1, 90);
TUNE(ridge, boot_hold_ms, 700, 0, 10000);
TUNE(ridge, ambient_ease_ms, 4000, 0, 30000);

#define RIDGE_EXTRA        88
#define RIDGE_COLUMNS      (RIDGE_CURVE_POINTS + (2 * RIDGE_EXTRA))
#define RIDGE_LAYER_COUNT  3
#define RIDGE_MAX_LIP_PX   48
#define MIN_TILT_STRENGTH  64
/* Once the board holds still, a pose within 1.5 degrees of its target jumps
 * the rest of the way instead of creeping a half-degree step at a time. */
#define POSE_ARRIVE_STEP   430
#define POSE_STEPS         720
#define GRADIENT_SETTLE_MS 200
/* Enough entries for the panel's diagonal, the longest a gradient along
 * gravity can span. */
#define RIDGE_SKY_ENTRIES  600
#define RIDGE_SKY_EDGES    96
#define RIDGE_PI           3.14159265f
#define LEVEL_STEADY_STEP  29
#define LEVEL_STEADY_MS    300
#define STRUM_STEP_PX      12
#define SHAKE_THRESHOLD    48
#define SHAKE_HALF_WIDTH   16
#define POSE_LANDSCAPE     ((ridge_vector_t){-RIDGE_POSE_ONE, 0})

/* Where each shade edge crosses a row for one pose, in 1/65536 columns:
 * `at_row0[edge] + (y * per_row)`. A pose with no sideways part crosses no
 * row, and its edges lie wholly to one side of each. */
typedef struct {
    ridge_vector_t pose;
    int64_t per_row;
    int64_t at_row0[RIDGE_SKY_EDGES];
} edge_lines_t;

typedef struct {
    spring_line_t line;
    int32_t offset[RIDGE_COLUMNS], velocity[RIDGE_COLUMNS];
    int16_t rigid[RIDGE_COLUMNS], heights[RIDGE_COLUMNS], smooth[RIDGE_COLUMNS], shape[RIDGE_COLUMNS];
    int16_t layers[RIDGE_LAYER_COUNT][RIDGE_COLUMNS];
    int16_t shown_layers[RIDGE_LAYER_COUNT][RIDGE_COLUMNS];
    ridge_motion_t motion;
    ridge_pose_t attitude;
    ridge_vector_t pose_on_screen, shown_pose;
    int pose_step;
    ridge_theme_t theme;
    gfx_color_t background_color, back0_color, back1_color;
    gfx_color_t sky[RIDGE_SKY_ENTRIES];
    int16_t sky_edge[RIDGE_SKY_EDGES];
    int sky_half, sky_edge_count;
    ridge_vector_t gradient_pose;
    int gradient_step;
    uint32_t pose_still_ms;
    edge_lines_t was_lines, now_lines;
    gfx_color_t scratch_row[GFX_WIDTH];
    uint8_t lip_alpha[RIDGE_LAYER_COUNT][RIDGE_MAX_LIP_PX];
    uint8_t body_alpha[RIDGE_LAYER_COUNT];
    uint32_t theme_seed, tuned_at, alive_ms, wave_ms, shake_seed;
    int shake, last_pluck_x, shown_lip;
    bool ambient, painted, scanline_dither;
} ridge_t;

static ridge_t* ridge;
static bool allocation_tried;

static inline __attribute__((always_inline)) int
clamp_int(int value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

static uint32_t
rgb_mix(uint32_t a, uint32_t b, int amount) {
    uint32_t mixed = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int from = (int)((a >> shift) & 0xffU);
        mixed |= (uint32_t)(from + (((int)(b >> shift & 0xff) - from) * amount / 100)) << shift;
    }
    return mixed;
}

/* The fill's gradient runs along gravity, top colour up and bottom colour
 * down, over a fixed span centred on the panel, so a turn only rotates it:
 * the pixels that change are the ones a shade edge swept across. */
#define RIDGE_SKY_HALF ((GFX_WIDTH + GFX_HEIGHT) / 4)

static void
build_sky_gradient(void) {
    ridge->sky_half = RIDGE_SKY_HALF;
    ridge->sky_edge_count = 0;
    for (int along = 0; along <= 2 * RIDGE_SKY_HALF; along++) {
        ridge->sky[along] =
            gfx_rgb(rgb_mix(ridge->theme.sky_top_rgb, ridge->theme.sky_bottom_rgb, along * 100 / (2 * RIDGE_SKY_HALF)));
        if (along > 0 && ridge->sky[along] != ridge->sky[along - 1] && ridge->sky_edge_count < RIDGE_SKY_EDGES) {
            ridge->sky_edge[ridge->sky_edge_count++] = (int16_t)along;
        }
    }
}

/* How far down, in panel pixels from the centre, `(x, y)` lies, twice over:
 * doubled coordinates keep the centre of an even-sized panel whole. */
static inline __attribute__((always_inline)) int32_t
sky_depth2(int x, int y) {
    return ((2 * x - (GFX_WIDTH - 1)) * ridge->gradient_pose.down_x)
           + ((2 * y - (GFX_HEIGHT - 1)) * ridge->gradient_pose.down_y);
}

static inline __attribute__((always_inline)) gfx_color_t
sky_from_depth2(int32_t depth2) {
    int along = ridge->sky_half + (depth2 >> 15);
    along = along < 0 ? 0 : along > 2 * ridge->sky_half ? 2 * ridge->sky_half : along;
    return ridge->sky[along];
}

static inline __attribute__((always_inline)) gfx_color_t
sky_at(int x, int y) {
    return sky_from_depth2(sky_depth2(x, y));
}

static void
apply_theme(void) {
    ridge->theme = ridge_theme_from_rgb((uint32_t)theme_rgb);
#if TUNE_ENABLED
    background_rgb = (int32_t)ridge->theme.background_rgb;
    sky_top_rgb = (int32_t)ridge->theme.sky_top_rgb;
    sky_bottom_rgb = (int32_t)ridge->theme.sky_bottom_rgb;
    back0_rgb = (int32_t)ridge->theme.back0_rgb;
    back1_rgb = (int32_t)ridge->theme.back1_rgb;
#endif
    ridge->theme_seed = (uint32_t)theme_rgb;
}

static void
build_layers(void) {
    const int amplitude[2] = {back0_amplitude, back1_amplitude};
    const int wavelength[2] = {back0_wavelength, back1_wavelength};
    const int period[2] = {back0_period_ms, back1_period_ms};
    const int offset[2] = {back0_offset, back1_offset};
    /* The waves grow in and go out with the rest of the ambient motion, so
     * a ridge whose motion is switched off rests on its rigid shape. */
    const int gain = ridge->ambient && ridge->alive_ms > (uint32_t)boot_hold_ms
                         ? ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms)
                         : 0;
    uint32_t travelled[2];
    for (int layer = 0; layer < 2; layer++) {
        travelled[layer] = (uint32_t)((uint64_t)ridge->wave_ms * 65536U / (uint32_t)period[layer]);
    }
    for (int x = 0; x < RIDGE_COLUMNS; x++) {
        for (int layer = 0; layer < 2; layer++) {
            const uint32_t phase = ((uint32_t)x * 65536U / (uint32_t)wavelength[layer]) - travelled[layer];
            const int echo = spring_line_scale(ridge->line.offset[x], layer == 0 ? 64 : 128) / (SPRING_LINE_ONE / 16);
            const int wave = amplitude[layer] * trig_sin((uint16_t)phase) / 2048 * gain / 256;
            ridge->layers[layer][x] = (int16_t)(ridge->rigid[x] + (offset[layer] * 16) + wave + echo);
        }
        ridge->layers[2][x] = (int16_t)(ridge->heights[x] + (front_offset * 16));
    }
}

/* `value / RIDGE_POSE_ONE` exactly, truncating toward zero, without the
 * 64-bit division: a library call on this chip, four per curve point. */
static inline __attribute__((always_inline)) int
pose_scale(int32_t value) {
    return value >= 0 ? value >> 14 : -((-value) >> 14);
}

/* The pose the ridge is drawn at: the eased pose snapped to 0.5 degree
 * steps, moving to a new step only once the eased pose is three quarters of
 * a step past the one on screen, so a pose resting between two steps
 * settles on one of them instead of flickering. */
static bool
snap_pose_on_screen(void) {
    const float step = 2.0F * RIDGE_PI / POSE_STEPS;
    const float angle = atan2f((float)ridge->attitude.pose.down_x, (float)ridge->attitude.pose.down_y);
    float off = angle - ((float)ridge->pose_step * step);
    off -= 2.0F * RIDGE_PI * floorf((off / (2.0F * RIDGE_PI)) + 0.5F);
    if (ridge->painted && fabsf(off) < 0.75F * step) {
        return false;
    }
    const int snapped = (int)lroundf(angle / step);
    ridge->pose_step = ((snapped % POSE_STEPS) + POSE_STEPS) % POSE_STEPS;
    const float drawn = (float)ridge->pose_step * step;
    const ridge_vector_t pose = {(int32_t)lroundf(sinf(drawn) * RIDGE_POSE_ONE),
                                 (int32_t)lroundf(cosf(drawn) * RIDGE_POSE_ONE)};
    const bool moved = pose.down_x != ridge->pose_on_screen.down_x || pose.down_y != ridge->pose_on_screen.down_y;
    ridge->pose_on_screen = pose;
    return moved;
}

/* Where a pixel lies in the ridge's own frame, in 1/32768 px from the
 * panel's centre: `along` the curve and `down` along gravity. Both move by
 * a fixed step per pixel along a row. */
typedef struct {
    int32_t along, down;
} ridge_point_t;

static inline __attribute__((always_inline)) ridge_point_t
pixel_point(ridge_vector_t pose, int x, int y) {
    const int32_t x2 = (2 * x) - (GFX_WIDTH - 1);
    const int32_t y2 = (2 * y) - (GFX_HEIGHT - 1);
    return (ridge_point_t){(x2 * pose.down_y) - (y2 * pose.down_x), (x2 * pose.down_x) + (y2 * pose.down_y)};
}

/* The curve column under `along`, and how far past it, out of 256. */
typedef struct {
    int column, frac;
} curve_at_t;

static inline __attribute__((always_inline)) curve_at_t
curve_at(int32_t along) {
    const int32_t at = along + ((RIDGE_COLUMNS - 1) * 16384);
    const int column = at >> 15;
    if (column < 0) {
        return (curve_at_t){0, 0};
    }
    if (column >= RIDGE_COLUMNS - 1) {
        return (curve_at_t){RIDGE_COLUMNS - 2, 256};
    }
    return (curve_at_t){column, (at >> 7) & 0xff};
}

/* How far below `layer`'s curve, in whole pixels along gravity, a point
 * `down` (1/32768 px) lies; negative above it. */
static inline __attribute__((always_inline)) int
layer_depth(int layer, curve_at_t at, int32_t down) {
    const int16_t* const heights = ridge->layers[layer];
    const int curve_q4 = heights[at.column] + (((heights[at.column + 1] - heights[at.column]) * at.frac) >> 8);
    return ((down >> 11) + (RIDGE_CURVE_VIEW_H * 8) - curve_q4) >> 4;
}

static inline __attribute__((always_inline)) uint8_t
layer_alpha(int layer, int depth) {
    if (depth < 0) {
        return 0;
    }
    return depth < lip_px ? ridge->lip_alpha[layer][depth] : ridge->body_alpha[layer];
}

static inline __attribute__((always_inline)) bool
backdrop_dither_pick(int x, int y, uint8_t alpha) {
    if (ridge->scanline_dither) {
        static const uint8_t cutoff[4] = {64, 192, 128, 255};
        return alpha == 255 || alpha >= cutoff[y & 3];
    }
    return gfx_dither_alpha_pick((gfx_dither_pattern_id_t)fill_pattern, x, y, alpha);
}

/* One pixel of the backdrop, the front layer first since it covers the
 * others wherever it shows. */
static inline __attribute__((always_inline)) gfx_color_t
backdrop_pixel(ridge_point_t point, int32_t sky_depth, int x, int y) {
    const curve_at_t at = curve_at(point.along);
    if (backdrop_dither_pick(x, y, layer_alpha(2, layer_depth(2, at, point.down)))) {
        return sky_from_depth2(sky_depth);
    }
    if (backdrop_dither_pick(x, y, layer_alpha(1, layer_depth(1, at, point.down)))) {
        return ridge->back1_color;
    }
    if (backdrop_dither_pick(x, y, layer_alpha(0, layer_depth(0, at, point.down)))) {
        return ridge->back0_color;
    }
    return ridge->background_color;
}

static void
paint_row_into(gfx_color_t* row, int y, int lo, int hi) {
    const ridge_vector_t pose = ridge->pose_on_screen;
    ridge_point_t point = pixel_point(pose, lo, y);
    int32_t sky_depth = sky_depth2(lo, y);
    const int32_t sky_step = 2 * ridge->gradient_pose.down_x;
    for (int x = lo; x < hi; x++) {
        row[x] = backdrop_pixel(point, sky_depth, x, y);
        point.along += 2 * pose.down_y;
        point.down += 2 * pose.down_x;
        sky_depth += sky_step;
    }
}

/* Most of a repainted span comes out as it was, and a pixel sent costs far
 * more than one compared: copy `from[lo, hi)` over row `y` and mark dirty
 * only the stretch that differed. */
static void
commit_row(const gfx_color_t* from, int y, int lo, int hi) {
    gfx_color_t* const row = gfx_framebuffer() + (y * GFX_WIDTH);
    int first = hi;
    int last = lo;
    for (int x = lo; x < hi; x++) {
        if (row[x] != from[x]) {
            row[x] = from[x];
            first = x < first ? x : first;
            last = x + 1;
        }
    }
    if (first < last) {
        gfx_mark_dirty(first, y, last - first, 1);
    }
}

static void
repaint_span(int y, int x0, int x1) {
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > GFX_WIDTH ? GFX_WIDTH : x1;
    if (x0 < x1) {
        paint_row_into(ridge->scratch_row, y, x0, x1);
        commit_row(ridge->scratch_row, y, x0, x1);
    }
}

typedef struct {
    int x0, y0, x1, y1;
} ridge_box_t;

static inline __attribute__((always_inline)) void
box_add(ridge_box_t* box, int32_t x_q4, int32_t y_q4) {
    const int x = x_q4 >> 4;
    const int y = y_q4 >> 4;
    box->x0 = x < box->x0 ? x : box->x0;
    box->x1 = x + 1 > box->x1 ? x + 1 : box->x1;
    box->y0 = y < box->y0 ? y : box->y0;
    box->y1 = y + 1 > box->y1 ? y + 1 : box->y1;
}

/* Adds to `box` where columns [c0, c1] of a curve stand at `pose`, from the
 * curve down to the end of its lip, in panel pixels. */
static void
box_add_columns(ridge_box_t* box, ridge_vector_t pose, const int16_t* heights, int lip, int c0, int c1) {
    for (int column = c0; column <= c1; column++) {
        const int32_t along = (16 * column) - ((RIDGE_COLUMNS - 1) * 8);
        const int32_t down = heights[column] - (RIDGE_CURVE_VIEW_H * 8);
        for (int reach = 0; reach <= 1; reach++) {
            const int32_t h = down + (reach * lip * 16);
            box_add(box, ((GFX_WIDTH - 1) * 8) + pose_scale((along * pose.down_y) + (h * pose.down_x)),
                    ((GFX_HEIGHT - 1) * 8) + pose_scale((h * pose.down_y) - (along * pose.down_x)));
        }
    }
}

/* Columns 8 at a time: a group's box stays small, and there are few enough
 * to find a moved one cheaply. */
#define RIDGE_GROUP 8

static bool
group_moved(int layer, int c0, int c1) {
    for (int column = c0; column <= c1; column++) {
        if (ridge->layers[layer][column] != ridge->shown_layers[layer][column]) {
            return true;
        }
    }
    return false;
}

/* The pixels a moved group of columns may have changed: everything from
 * where it stood to where it stands, lips and all. */
static void
repaint_group(int layer, int c0, int c1, bool everything_moved) {
    if (!everything_moved && !group_moved(layer, c0, c1)) {
        return;
    }
    ridge_box_t box = {GFX_WIDTH, GFX_HEIGHT, 0, 0};
    box_add_columns(&box, ridge->shown_pose, ridge->shown_layers[layer], ridge->shown_lip, c0, c1);
    box_add_columns(&box, ridge->pose_on_screen, ridge->layers[layer], lip_px, c0, c1);
    const int y0 = clamp_int(box.y0 - 1, 0, GFX_HEIGHT);
    const int y1 = clamp_int(box.y1 + 1, 0, GFX_HEIGHT);
    for (int y = y0; y < y1; y++) {
        repaint_span(y, box.x0 - 1, box.x1 + 1);
    }
}

static void
repaint_changed(void) {
    const bool everything_moved = ridge->shown_pose.down_x != ridge->pose_on_screen.down_x
                                  || ridge->shown_pose.down_y != ridge->pose_on_screen.down_y
                                  || ridge->shown_lip != lip_px;
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        for (int c0 = 0; c0 < RIDGE_COLUMNS - 1; c0 += RIDGE_GROUP) {
            const int c1 = c0 + RIDGE_GROUP < RIDGE_COLUMNS - 1 ? c0 + RIDGE_GROUP : RIDGE_COLUMNS - 1;
            repaint_group(layer, c0, c1, everything_moved);
        }
    }
    memcpy(ridge->shown_layers, ridge->layers, sizeof ridge->shown_layers);
    ridge->shown_pose = ridge->pose_on_screen;
    ridge->shown_lip = lip_px;
}

#define EDGE_FAR (INT64_C(1) << 40)

static inline __attribute__((always_inline)) int32_t
sky_edge_depth2(int edge) {
    return (ridge->sky_edge[edge] - ridge->sky_half) * 32768;
}

static void
edge_lines(edge_lines_t* lines, ridge_vector_t pose) {
    lines->pose = pose;
    if (pose.down_x == 0) {
        return;
    }
    const int64_t per_column = 2 * (int64_t)pose.down_x;
    const int64_t origin = ((int64_t)(GFX_WIDTH - 1) * pose.down_x) + ((int64_t)(GFX_HEIGHT - 1) * pose.down_y);
    lines->per_row = (-2 * (int64_t)pose.down_y * 65536) / per_column;
    for (int edge = 0; edge < ridge->sky_edge_count; edge++) {
        lines->at_row0[edge] = ((sky_edge_depth2(edge) + origin) * 65536) / per_column;
    }
}

static inline __attribute__((always_inline)) int64_t
edge_column_q16(const edge_lines_t* lines, int edge, int y, int64_t row_offset) {
    if (lines->pose.down_x == 0) {
        return ((2 * y - (GFX_HEIGHT - 1)) * lines->pose.down_y) >= sky_edge_depth2(edge) ? -EDGE_FAR : EDGE_FAR;
    }
    return lines->at_row0[edge] + row_offset;
}

/* The columns of row `y` between where `edge` crossed it at the old pose
 * and where it crosses it now, with a pixel of slack each side. */
static inline __attribute__((always_inline)) void
edge_swept_columns(int edge, int y, int64_t was_offset, int64_t now_offset, int* x0, int* x1) {
    const int64_t a = edge_column_q16(&ridge->was_lines, edge, y, was_offset);
    const int64_t b = edge_column_q16(&ridge->now_lines, edge, y, now_offset);
    int64_t lo = a < b ? a : b;
    int64_t hi = a > b ? a : b;
    lo = lo < -2 * 65536 ? -2 * 65536 : lo;
    hi = hi > (int64_t)(GFX_WIDTH + 2) * 65536 ? (int64_t)(GFX_WIDTH + 2) * 65536 : hi;
    *x0 = clamp_int((int)(lo >> 16) - 1, 0, GFX_WIDTH);
    *x1 = clamp_int((int)((hi + 65535) >> 16) + 2, 0, GFX_WIDTH);
}

/* Along a row the edges come in depth order, so neighbouring edges' sweeps
 * merge into one span as they are met. */
static void
repaint_swept_row(int y) {
    const int64_t was_offset = y * ridge->was_lines.per_row;
    const int64_t now_offset = y * ridge->now_lines.per_row;
    int run_x0 = 0;
    int run_x1 = 0;
    for (int edge = 0; edge < ridge->sky_edge_count; edge++) {
        int x0;
        int x1;
        edge_swept_columns(edge, y, was_offset, now_offset, &x0, &x1);
        if (x0 >= x1) {
            continue;
        }
        if (run_x0 < run_x1 && x0 <= run_x1 && x1 >= run_x0) {
            run_x0 = x0 < run_x0 ? x0 : run_x0;
            run_x1 = x1 > run_x1 ? x1 : run_x1;
            continue;
        }
        if (run_x0 < run_x1) {
            repaint_span(y, run_x0, run_x1);
        }
        run_x0 = x0;
        run_x1 = x1;
    }
    if (run_x0 < run_x1) {
        repaint_span(y, run_x0, run_x1);
    }
}

/* The gradient turned from `was` to its pose now: repaint the pixels
 * between each shade edge's old and new line. */
static void
repaint_gradient_turn(ridge_vector_t was) {
    edge_lines(&ridge->was_lines, was);
    edge_lines(&ridge->now_lines, ridge->gradient_pose);
    for (int y = 0; y < GFX_HEIGHT; y++) {
        repaint_swept_row(y);
    }
}

static void
catch_up_gradient(void) {
    ridge->gradient_pose = ridge->pose_on_screen;
    ridge->gradient_step = ridge->pose_step;
}

/* The gradient follows the ridge in steps of `gradient_steps` poses rather
 * than every pose: a turned gradient moves pixels across every band, which
 * then goes whole to the panel. Once the ridge holds still it catches up. */
static bool
gradient_due(void) {
    int drift = abs(ridge->pose_step - ridge->gradient_step);
    drift = drift > POSE_STEPS / 2 ? POSE_STEPS - drift : drift;
    return drift >= gradient_steps || (drift > 0 && ridge->pose_still_ms >= GRADIENT_SETTLE_MS);
}

static void
paint_all(void) {
    if (!ridge->painted) {
        catch_up_gradient();
    }
    gfx_color_t* const framebuffer = gfx_framebuffer();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        paint_row_into(framebuffer + (y * GFX_WIDTH), y, 0, GFX_WIDTH);
    }
    gfx_mark_all_dirty();
    memcpy(ridge->shown_layers, ridge->layers, sizeof ridge->shown_layers);
    ridge->shown_pose = ridge->pose_on_screen;
    ridge->shown_lip = lip_px;
    ridge->painted = true;
}

static void
bake_what_is_tuned(void) {
    if ((uint32_t)theme_rgb != ridge->theme_seed) {
        apply_theme();
    }
    ridge_motion_smooth(ridge->rigid, ridge->smooth, ridge->shape, RIDGE_COLUMNS, breath_smooth);
#if TUNE_ENABLED
    ridge->theme.background_rgb = (uint32_t)background_rgb;
    ridge->theme.sky_top_rgb = (uint32_t)sky_top_rgb;
    ridge->theme.sky_bottom_rgb = (uint32_t)sky_bottom_rgb;
    ridge->theme.back0_rgb = (uint32_t)back0_rgb;
    ridge->theme.back1_rgb = (uint32_t)back1_rgb;
#endif
    ridge->background_color = gfx_rgb(ridge->theme.background_rgb);
    ridge->back0_color = gfx_rgb(ridge->theme.back0_rgb);
    ridge->back1_color = gfx_rgb(ridge->theme.back1_rgb);
    const int lip_start[RIDGE_LAYER_COUNT] = {back0_lip_alpha, back1_lip_alpha, front_lip_alpha};
    const int lip_end[RIDGE_LAYER_COUNT] = {back0_body_alpha, back1_body_alpha, front_body_alpha};
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        ridge->body_alpha[layer] = (uint8_t)lip_end[layer];
        for (int distance = 0; distance < lip_px; distance++) {
            ridge->lip_alpha[layer][distance] =
                (uint8_t)(lip_start[layer] + ((lip_end[layer] - lip_start[layer]) * distance / lip_px));
        }
    }
    ridge->scanline_dither = (int)fill_pattern == (int)GFX_DITHER_SCANLINES4;
    build_sky_gradient();
    ridge->tuned_at = TUNE_GENERATION(ridge);
}

static void
allocate_once(void) {
    if (allocation_tried) {
        return;
    }
    allocation_tried = true;
    ridge = RIDGE_ALLOC(sizeof *ridge);
    if (ridge == NULL) {
        return;
    }
    memset(ridge, 0, sizeof *ridge);
    spring_line_init(&ridge->line, ridge->offset, ridge->velocity, RIDGE_COLUMNS, spring_tension, spring_stiffness,
                     spring_damping);
    ridge_motion_extend(ridge_curve_y, RIDGE_CURVE_POINTS, ridge->rigid, RIDGE_EXTRA);
    memcpy(ridge->heights, ridge->rigid, sizeof ridge->heights);
    apply_theme();
    bake_what_is_tuned();
    ridge->attitude.pose = POSE_LANDSCAPE;
    ridge->attitude.level = POSE_LANDSCAPE;
    ridge->attitude.steady_level = POSE_LANDSCAPE;
    ridge->pose_on_screen = POSE_LANDSCAPE;
    ridge->shake_seed = 0x9e3779b9U;
    ridge->ambient = true;
}

void
ui_ridge_set_gravity(int gx, int gy, int strength, int shake) {
    allocate_once();
    if (ridge != NULL) {
        ridge->shake = shake;
        ridge->attitude.level =
            ridge_pose_level_from_gravity(ridge->attitude.level, gx, gy, strength, MIN_TILT_STRENGTH);
    }
}

void
ui_ridge_set_ambient(bool on) {
    allocate_once();
    if (ridge != NULL) {
        ridge->ambient = on;
    }
}

void
ui_ridge_settle(void) {
    allocate_once();
    if (ridge != NULL) {
        ridge->alive_ms = (uint32_t)boot_hold_ms + (uint32_t)ambient_ease_ms;
        ridge->attitude.pose = ridge->attitude.level;
        ridge->attitude.steady_level = ridge->attitude.level;
        ridge->attitude.steady_ms = LEVEL_STEADY_MS;
    }
}

#if CONFIG_LAUNCHER_SELFTEST
void
ui_ridge_reset_for_test(void) {
    allocate_once();
    if (ridge == NULL) {
        return;
    }
    spring_line_init(&ridge->line, ridge->offset, ridge->velocity, RIDGE_COLUMNS, spring_tension, spring_stiffness,
                     spring_damping);
    memcpy(ridge->heights, ridge->rigid, sizeof ridge->heights);
    memset(&ridge->motion, 0, sizeof ridge->motion);
    ridge->attitude.pose = POSE_LANDSCAPE;
    ridge->attitude.level = POSE_LANDSCAPE;
    ridge->attitude.steady_level = POSE_LANDSCAPE;
    ridge->attitude.steady_ms = LEVEL_STEADY_MS;
    ridge->pose_on_screen = POSE_LANDSCAPE;
    ridge->alive_ms = (uint32_t)boot_hold_ms + (uint32_t)ambient_ease_ms;
    ridge->shake = 0;
    ridge->last_pluck_x = 0;
    ridge->ambient = false;
    ridge->painted = false;
}
#endif

void
ui_ridge_paint(void) {
    allocate_once();
    if (ridge == NULL) {
        gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, GFX_RGB(0));
        return;
    }
    FRAME_COST_BEGIN(painted_from);
    paint_all();
    FRAME_COST_END(painted_from, "ridge.paint");
}

static void
pluck_from_touch(const input_t* input) {
    if (!input->down) {
        return;
    }
    const int x =
        ridge_pose_column_under(ridge->attitude.pose, GFX_WIDTH, GFX_HEIGHT, RIDGE_COLUMNS, input->x, input->y);
    if (input->pressed || abs(x - ridge->last_pluck_x) >= STRUM_STEP_PX) {
        spring_line_poke(&ridge->line, x, pluck_width,
                         -(int32_t)((int64_t)SPRING_LINE_ONE * (input->pressed ? pluck_tap : pluck_strum) / 1000));
        ridge->last_pluck_x = x;
    }
}

static void
shape_this_frame(uint32_t dt_ms) {
    if (!ridge->ambient || ridge->alive_ms < (uint32_t)boot_hold_ms) {
        memcpy(ridge->shape, ridge->rigid, sizeof ridge->shape);
        return;
    }
    const ridge_motion_params_t params = {.breath_ms = breath_ms,
                                          .breath_depth = breath_depth,
                                          .wave_height_q4 = front_amplitude * 16,
                                          .wave_length = front_wavelength,
                                          .wave_passes_in_ms = front_period_ms,
                                          .push = tilt_push,
                                          .coast_ms = tilt_coast_ms};
    ridge_motion_advance(&ridge->motion, &params, dt_ms, ridge_pose_slope(&ridge->attitude));
    const int gain = ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms);
    for (int x = 0; x < RIDGE_COLUMNS; x++) {
        ridge->shape[x] =
            (int16_t)(ridge->rigid[x]
                      + ((ridge_motion_height(&ridge->motion, &params, ridge->rigid[x], ridge->smooth[x], x)
                          - ridge->rigid[x])
                         * gain / 256));
    }
}

static void
pluck_from_shaking(void) {
    if (ridge->shake < SHAKE_THRESHOLD) {
        return;
    }
    ridge->shake_seed = ridge->shake_seed * 1664525U + 1013904223U;
    spring_line_poke(&ridge->line, (int)((ridge->shake_seed >> 8) % RIDGE_COLUMNS), SHAKE_HALF_WIDTH,
                     (ridge->shake_seed & 0x80U ? 1 : -1) * (SPRING_LINE_ONE / 128) * ridge->shake);
}

void
ui_ridge_step(const input_t* input, uint32_t dt_ms) {
    allocate_once();
    if (ridge == NULL) {
        return;
    }
    const bool retuned = TUNE_GENERATION(ridge) != ridge->tuned_at;
    if (retuned) {
        bake_what_is_tuned();
    }
    ridge->line.tension = spring_tension;
    ridge->line.stiffness = spring_stiffness;
    ridge->line.damping = spring_damping;
    ridge->alive_ms += dt_ms;
    /* The back waves move with the rest of the ambient motion, or not at all:
     * nothing repaints a wave that moves while the ridge is quiet. */
    if (ridge->ambient) {
        ridge->wave_ms += dt_ms;
    }
    const ridge_pose_params_t pose_params = {.boot_pose = POSE_LANDSCAPE,
                                             .hold_ms = (uint32_t)boot_hold_ms,
                                             .tau_ms = level_tau_ms,
                                             .steady_step = LEVEL_STEADY_STEP,
                                             .steady_hold_ms = LEVEL_STEADY_MS,
                                             .redraw_step = POSE_ARRIVE_STEP};
    ridge_pose_advance(&ridge->attitude, &pose_params, dt_ms, ridge->alive_ms);
    pluck_from_touch(input);
    if (ridge->ambient) {
        pluck_from_shaking();
    }
    shape_this_frame(dt_ms);
    int lo;
    int hi;
    spring_line_advance(&ridge->line, dt_ms);
    spring_line_apply(&ridge->line, ridge->shape, ridge->heights, &lo, &hi);
    const bool pose_moved = snap_pose_on_screen();
    ridge->pose_still_ms = pose_moved ? 0 : ridge->pose_still_ms + dt_ms;
    FRAME_COST_BEGIN(layers_from);
    build_layers();
    FRAME_COST_END(layers_from, "ridge.layers");
    FRAME_COST_BEGIN(painted_from);
    if (!ridge->painted || retuned) {
        catch_up_gradient();
        paint_all();
    } else {
        /* Every frame, not only when the spring reports motion: a spring put
         * to rest takes its last fraction of a pixel without reporting it. */
        if (ridge->scanline_dither && gradient_due()) {
            const ridge_vector_t was = ridge->gradient_pose;
            catch_up_gradient();
            repaint_gradient_turn(was);
        }
        repaint_changed();
    }
    FRAME_COST_END(painted_from, "ridge.paint");
}

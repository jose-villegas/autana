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
TUNE(ridge, sky_top_rgb, 0x1199C8, 0, 0xffffff);
TUNE(ridge, sky_bottom_rgb, 0x91C6D1, 0, 0xffffff);
TUNE(ridge, background_rgb, 0x0B6382, 0, 0xffffff);
TUNE(ridge, back0_rgb, 0x07638C, 0, 0xffffff);
TUNE(ridge, back1_rgb, 0x10606B, 0, 0xffffff);
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
TUNE(ridge, boot_hold_ms, 700, 0, 10000);
TUNE(ridge, ambient_ease_ms, 4000, 0, 30000);

#define RIDGE_EXTRA       88
#define RIDGE_COLUMNS     (RIDGE_CURVE_POINTS + 2 * RIDGE_EXTRA)
#define RIDGE_LAYER_COUNT 3
#define RIDGE_MAX_LIP_PX  48
#define MIN_TILT_STRENGTH 64
#define POSE_REDRAW_STEP  143
#define POSE_STEPS        720
/* Enough entries for the panel's diagonal, the longest a gradient along
 * gravity can span. */
#define RIDGE_SKY_ENTRIES 600
#define RIDGE_PI          3.14159265f
#define LEVEL_STEADY_STEP 29
#define LEVEL_STEADY_MS   300
#define STRUM_STEP_PX     12
#define SHAKE_THRESHOLD   48
#define SHAKE_HALF_WIDTH  16
#define POSE_LANDSCAPE    ((ridge_vector_t){-RIDGE_POSE_ONE, 0})

typedef struct {
    spring_line_t line;
    int32_t offset[RIDGE_COLUMNS], velocity[RIDGE_COLUMNS];
    int16_t rigid[RIDGE_COLUMNS], heights[RIDGE_COLUMNS], smooth[RIDGE_COLUMNS], shape[RIDGE_COLUMNS];
    int16_t layers[RIDGE_LAYER_COUNT][RIDGE_COLUMNS];
    int16_t boundary[RIDGE_LAYER_COUNT][GFX_HEIGHT], shown[RIDGE_LAYER_COUNT][GFX_HEIGHT];
    ridge_motion_t motion;
    ridge_pose_t attitude;
    ridge_vector_t pose_on_screen;
    int pose_step;
    ridge_theme_t theme;
    gfx_color_t background_color, back0_color, back1_color;
    gfx_color_t sky[RIDGE_SKY_ENTRIES];
    int sky_half;
    uint8_t lip_alpha[RIDGE_LAYER_COUNT][RIDGE_MAX_LIP_PX];
    uint32_t theme_seed, tuned_at, alive_ms, wave_ms, shake_seed;
    int shake, last_pluck_x, strips, down_sign;
    bool by_column, axis_on_screen, ambient, painted, scanline_dither;
} ridge_t;

static ridge_t* ridge;
static bool allocation_tried;

static int
round_q4(int value) {
    return value >= 0 ? (value + 8) / 16 : (value - 8) / 16;
}

static uint32_t
rgb_mix(uint32_t a, uint32_t b, int amount) {
    uint32_t mixed = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int from = (a >> shift) & 0xff;
        mixed |= (uint32_t)(from + (((int)(b >> shift & 0xff) - from) * amount / 100)) << shift;
    }
    return mixed;
}

/* The fill's gradient runs along gravity, top colour up and bottom colour
 * down, across the screen's own extent in that direction, so it turns with
 * the ridge instead of flipping from panel rows to columns at 45 degrees. */
static void
build_sky_gradient(void) {
    const ridge_vector_t down = ridge->pose_on_screen;
    const int extent = (GFX_WIDTH * abs(down.down_x) + GFX_HEIGHT * abs(down.down_y)) / RIDGE_POSE_ONE;
    ridge->sky_half = extent / 2;
    for (int along = 0; along <= 2 * ridge->sky_half && along < RIDGE_SKY_ENTRIES; along++) {
        ridge->sky[along] =
            gfx_rgb(rgb_mix((uint32_t)sky_top_rgb, (uint32_t)sky_bottom_rgb, along * 100 / (2 * ridge->sky_half)));
    }
}

/* How far down, in panel pixels from the centre, `(x, y)` lies, twice over:
 * doubled coordinates keep the centre of an even-sized panel whole. */
static inline __attribute__((always_inline)) int32_t
sky_depth2(int x, int y) {
    return (2 * x - (GFX_WIDTH - 1)) * ridge->pose_on_screen.down_x
           + (2 * y - (GFX_HEIGHT - 1)) * ridge->pose_on_screen.down_y;
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

static inline __attribute__((always_inline)) int
ridge_layer_alpha(int layer, int distance) {
    if (distance < 0) {
        return 0;
    }
    if (distance >= lip_px) {
        return layer == 0 ? back0_body_alpha : layer == 1 ? back1_body_alpha : front_body_alpha;
    }
    return ridge->lip_alpha[layer][distance];
}

static inline __attribute__((always_inline)) int
boundary_distance(int layer, int x, int y) {
    const int strip = ridge->by_column ? x : y;
    const int boundary = ridge->boundary[layer][strip];
    return boundary == INT16_MAX ? -1 : ridge->down_sign * ((ridge->by_column ? y : x) - boundary);
}

static inline __attribute__((always_inline)) bool
backdrop_dither_pick(int x, int y, uint8_t alpha) {
    if (ridge->scanline_dither) {
        static const uint8_t cutoff[4] = {64, 192, 128, 255};
        return alpha == 255 || alpha >= cutoff[y & 3];
    }
    return gfx_dither_alpha_pick((gfx_dither_pattern_id_t)fill_pattern, x, y, alpha);
}

static inline __attribute__((always_inline)) gfx_color_t
backdrop_pixel(int x, int y, uint8_t reveal_alpha) {
    if (!backdrop_dither_pick(x, y, reveal_alpha)) {
        return GFX_RGB(0);
    }
    gfx_color_t color = ridge->background_color;
    const int back0 = boundary_distance(0, x, y);
    const int back1 = boundary_distance(1, x, y);
    const int front = boundary_distance(2, x, y);
    if (backdrop_dither_pick(x, y, (uint8_t)ridge_layer_alpha(0, back0))) {
        color = ridge->back0_color;
    }
    if (backdrop_dither_pick(x, y, (uint8_t)ridge_layer_alpha(1, back1))) {
        color = ridge->back1_color;
    }
    if (backdrop_dither_pick(x, y, (uint8_t)ridge_layer_alpha(2, front))) {
        color = sky_at(x, y);
    }
    return color;
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
    for (int x = 0; x < RIDGE_COLUMNS; x++) {
        for (int layer = 0; layer < 2; layer++) {
            const uint32_t phase = (uint32_t)x * 65536u / (uint32_t)wavelength[layer]
                                   - (uint32_t)((uint64_t)ridge->wave_ms * 65536u / (uint32_t)period[layer]);
            const int echo = spring_line_scale(ridge->line.offset[x], layer == 0 ? 64 : 128) / (SPRING_LINE_ONE / 16);
            ridge->layers[layer][x] = (int16_t)(ridge->rigid[x] + offset[layer] * 16
                                                + amplitude[layer] * trig_sin((uint16_t)phase) / 2048 + echo);
        }
        ridge->layers[2][x] = (int16_t)(ridge->heights[x] + front_offset * 16);
    }
}

static void
put_boundary(int layer, int strip, int crossing) {
    if (strip < 0 || strip >= ridge->strips) {
        return;
    }
    int16_t* const at = &ridge->boundary[layer][strip];
    if (*at == INT16_MAX || (ridge->down_sign > 0 ? crossing < *at : crossing > *at)) {
        *at = (int16_t)crossing;
    }
}

static void
raster_segment(int layer, int x0, int y0, int x1, int y1) {
    const int a0 = ridge->by_column ? x0 : y0, a1 = ridge->by_column ? x1 : y1;
    const int b0 = ridge->by_column ? y0 : x0, b1 = ridge->by_column ? y1 : x1;
    if (a0 == a1) {
        put_boundary(layer, a0, b0);
        put_boundary(layer, a0, b1);
        return;
    }
    const int low = a0 < a1 ? a0 : a1, high = a0 > a1 ? a0 : a1;
    for (int a = low; a <= high; a++) {
        put_boundary(layer, a, b0 + (b1 - b0) * (a - a0) / (a1 - a0));
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
    const float step = 2.0f * RIDGE_PI / POSE_STEPS;
    const float angle = atan2f((float)ridge->attitude.pose.down_x, (float)ridge->attitude.pose.down_y);
    float off = angle - ridge->pose_step * step;
    off -= 2.0f * RIDGE_PI * floorf(off / (2.0f * RIDGE_PI) + 0.5f);
    if (ridge->painted && fabsf(off) < 0.75f * step) {
        return false;
    }
    const int snapped = (int)lroundf(angle / step);
    ridge->pose_step = ((snapped % POSE_STEPS) + POSE_STEPS) % POSE_STEPS;
    const float drawn = ridge->pose_step * step;
    const ridge_vector_t pose = {(int32_t)lroundf(sinf(drawn) * RIDGE_POSE_ONE),
                                 (int32_t)lroundf(cosf(drawn) * RIDGE_POSE_ONE)};
    const bool moved = pose.down_x != ridge->pose_on_screen.down_x || pose.down_y != ridge->pose_on_screen.down_y;
    ridge->pose_on_screen = pose;
    return moved;
}

static void
raster_boundaries(void) {
    const bool was_by_column = ridge->by_column;
    const ridge_vector_t pose = ridge->pose_on_screen;
    ridge->by_column = abs(pose.down_y) >= abs(pose.down_x);
    ridge->axis_on_screen = ridge->painted && was_by_column != ridge->by_column;
    ridge->strips = ridge->by_column ? GFX_WIDTH : GFX_HEIGHT;
    ridge->down_sign = (ridge->by_column ? pose.down_y : pose.down_x) >= 0 ? 1 : -1;
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        for (int strip = 0; strip < GFX_HEIGHT; strip++) {
            ridge->boundary[layer][strip] = INT16_MAX;
        }
        const int32_t rx = pose.down_y, ry = -pose.down_x, dx = pose.down_x, dy = pose.down_y;
        const int32_t u0 = -(RIDGE_COLUMNS - 1) * 8;
        int32_t along_x = u0 * rx, along_y = u0 * ry;
        int x0 = 0, y0 = 0;
        for (int point = 0; point < RIDGE_COLUMNS; point++) {
            const int32_t h = ridge->layers[layer][point] - RIDGE_CURVE_VIEW_H * 8;
            const int x1 = round_q4((GFX_WIDTH - 1) * 8 + pose_scale(along_x + h * dx));
            const int y1 = round_q4((GFX_HEIGHT - 1) * 8 + pose_scale(along_y + h * dy));
            if (point > 0) {
                raster_segment(layer, x0, y0, x1, y1);
            }
            x0 = x1;
            y0 = y1;
            along_x += 16 * rx;
            along_y += 16 * ry;
        }
    }
    build_sky_gradient();
}

static void
fill_row(gfx_color_t* dst, int x0, int x1, gfx_color_t color) {
    for (int x = x0; x < x1; x++) {
        dst[x] = color;
    }
}

static void
add_row_break(int* points, int* count, int x, int lo, int hi) {
    if (x <= lo || x >= hi) {
        return;
    }
    int at = *count;
    while (at > 0 && points[at - 1] > x) {
        points[at] = points[at - 1];
        at--;
    }
    if (at == 0 || points[at - 1] != x) {
        points[at] = x;
        (*count)++;
    }
}

static inline __attribute__((always_inline)) bool
row_lip_at(int y, int x) {
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        const int distance = boundary_distance(layer, x, y);
        if (distance >= 0 && distance < lip_px) {
            return true;
        }
    }
    return false;
}

static inline __attribute__((always_inline)) int
collect_row_breaks(int* points, int y, int lo, int hi) {
    int count = 1;
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        const int boundary = ridge->boundary[layer][y];
        if (boundary == INT16_MAX) {
            continue;
        }
        if (ridge->down_sign > 0) {
            add_row_break(points, &count, boundary, lo, hi);
            add_row_break(points, &count, boundary + lip_px, lo, hi);
        } else {
            add_row_break(points, &count, boundary - lip_px + 1, lo, hi);
            add_row_break(points, &count, boundary + 1, lo, hi);
        }
    }
    points[count++] = hi;
    return count;
}

static inline __attribute__((always_inline)) gfx_color_t
row_body_color(int y, int x) {
    gfx_color_t color = ridge->background_color;
    if (backdrop_dither_pick(0, y, (uint8_t)ridge_layer_alpha(0, boundary_distance(0, x, y)))) {
        color = ridge->back0_color;
    }
    if (backdrop_dither_pick(0, y, (uint8_t)ridge_layer_alpha(1, boundary_distance(1, x, y)))) {
        color = ridge->back1_color;
    }
    return color;
}

static inline __attribute__((always_inline)) bool
scanline_pick(uint8_t alpha, int phase) {
    static const uint8_t cutoff[4] = {64, 192, 128, 255};
    return alpha == 255 || alpha >= cutoff[phase];
}

static inline __attribute__((always_inline)) uint8_t
layer_alpha_at(int layer, int distance) {
    return (uint8_t)ridge_layer_alpha(layer, distance);
}

/* A segment with a lip in it, along its strip from `a0` to `a1`, scanline
 * dither only. The same picture as backdrop_pixel(), with each layer's
 * distance stepped instead of looked up and no orientation test per pixel. */
static void
paint_lip_segment(gfx_color_t* out, int strip, int a0, int a1, uint8_t reveal_alpha) {
    const bool by_column = ridge->by_column;
    const int step = by_column ? GFX_WIDTH : 1;
    int distance[RIDGE_LAYER_COUNT];
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        const int boundary = ridge->boundary[layer][strip];
        distance[layer] = boundary == INT16_MAX ? INT_MIN / 2 : ridge->down_sign * (a0 - boundary);
    }
    const gfx_color_t back0 = ridge->back0_color, back1 = ridge->back1_color, black = GFX_RGB(0);
    for (int a = a0; a < a1; a++, out += step) {
        const int phase = (by_column ? a : strip) & 3;
        gfx_color_t color = ridge->background_color;
        if (!scanline_pick(reveal_alpha, phase)) {
            color = black;
        } else {
            if (scanline_pick(layer_alpha_at(0, distance[0]), phase)) {
                color = back0;
            }
            if (scanline_pick(layer_alpha_at(1, distance[1]), phase)) {
                color = back1;
            }
            if (scanline_pick(layer_alpha_at(2, distance[2]), phase)) {
                color = sky_at(by_column ? strip : a, by_column ? a : strip);
            }
        }
        *out = color;
        distance[0] += ridge->down_sign;
        distance[1] += ridge->down_sign;
        distance[2] += ridge->down_sign;
    }
}

static inline __attribute__((always_inline)) void
paint_row_part(gfx_color_t* row, int y, int x0, int x1, uint8_t reveal_alpha) {
    if (row_lip_at(y, x0)) {
        paint_lip_segment(row + x0, y, x0, x1, reveal_alpha);
        return;
    }
    if (backdrop_dither_pick(0, y, (uint8_t)ridge_layer_alpha(2, boundary_distance(2, x0, y)))) {
        int32_t depth2 = sky_depth2(x0, y);
        const int32_t step = 2 * ridge->pose_on_screen.down_x;
        for (int x = x0; x < x1; x++, depth2 += step) {
            row[x] = sky_from_depth2(depth2);
        }
        return;
    }
    fill_row(row, x0, x1, row_body_color(y, x0));
}

static void
paint_row_into(gfx_color_t* row, int y, int lo, int hi, uint8_t reveal_alpha) {
    if (!backdrop_dither_pick(0, y, reveal_alpha)) {
        fill_row(row, lo, hi, GFX_RGB(0));
        return;
    }

    int points[2 * RIDGE_LAYER_COUNT + 2] = {lo};
    const int count = collect_row_breaks(points, y, lo, hi);
    for (int part = 0; part + 1 < count; part++) {
        paint_row_part(row, y, points[part], points[part + 1], reveal_alpha);
    }
}

static void
paint_row_span(gfx_color_t* framebuffer, int y, int lo, int hi, uint8_t reveal_alpha) {
    paint_row_into(framebuffer + y * GFX_WIDTH, y, lo, hi, reveal_alpha);
}

/* A column segment with no lip in it: every layer there is either absent or
 * at its body alpha, so a pixel's colour depends only on its scanline phase,
 * and on the sky gradient where the front layer shows. */
static inline __attribute__((always_inline)) void
paint_column_body(gfx_color_t* framebuffer, int x, int y0, int y1, uint8_t reveal_alpha) {
    gfx_color_t phase_color[4];
    bool phase_sky[4];
    for (int phase = 0; phase < 4; phase++) {
        const int y = y0 + ((phase - y0) & 3);
        phase_sky[phase] = false;
        if (!backdrop_dither_pick(x, y, reveal_alpha)) {
            phase_color[phase] = GFX_RGB(0);
            continue;
        }
        phase_sky[phase] = backdrop_dither_pick(x, y, (uint8_t)ridge_layer_alpha(2, boundary_distance(2, x, y0)));
        phase_color[phase] = row_body_color(y, x);
    }
    for (int y = y0; y < y1; y++) {
        const int phase = y & 3;
        framebuffer[y * GFX_WIDTH + x] = phase_sky[phase] ? sky_at(x, y) : phase_color[phase];
    }
}

static void
paint_column_span(gfx_color_t* framebuffer, int x, int lo, int hi, uint8_t reveal_alpha) {
    int points[2 * RIDGE_LAYER_COUNT + 2] = {lo};
    const int count = collect_row_breaks(points, x, lo, hi);
    for (int part = 0; part + 1 < count; part++) {
        const int y0 = points[part], y1 = points[part + 1];
        if (row_lip_at(y0, x)) {
            paint_lip_segment(framebuffer + y0 * GFX_WIDTH + x, x, y0, y1, reveal_alpha);
        } else {
            paint_column_body(framebuffer, x, y0, y1, reveal_alpha);
        }
    }
}

static void
repaint_strip(int strip, int lo, int hi) {
    const int limit = ridge->by_column ? GFX_HEIGHT : GFX_WIDTH;
    lo = lo < 0 ? 0 : lo;
    hi = hi > limit ? limit : hi;
    if (lo >= hi) {
        return;
    }
    const int reveal = ridge->alive_ms <= (uint32_t)boot_hold_ms
                           ? 0
                           : ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms);
    const uint8_t reveal_alpha = (uint8_t)(reveal * 255 / 256);
    gfx_color_t* const framebuffer = gfx_framebuffer();
    if (ridge->by_column) {
        if (ridge->scanline_dither) {
            paint_column_span(framebuffer, strip, lo, hi, reveal_alpha);
        } else {
            for (int y = lo; y < hi; y++) {
                framebuffer[y * GFX_WIDTH + strip] = backdrop_pixel(strip, y, reveal_alpha);
            }
        }
        gfx_mark_dirty(strip, lo, 1, hi - lo);
    } else {
        if (ridge->scanline_dither) {
            paint_row_span(framebuffer, strip, lo, hi, reveal_alpha);
        } else {
            for (int x = lo; x < hi; x++) {
                framebuffer[strip * GFX_WIDTH + x] = backdrop_pixel(x, strip, reveal_alpha);
            }
        }
        gfx_mark_dirty(lo, strip, hi - lo, 1);
    }
}

static void
extend_repaint_range(int was, int now, int* lo, int* hi) {
    if (was == INT16_MAX && now == INT16_MAX) {
        return;
    }
    int first = was == INT16_MAX ? now : was;
    int last = now == INT16_MAX ? was : now;
    if (first > last) {
        const int swap = first;
        first = last;
        last = swap;
    }
    /* The lip lies on the down side of the boundary only. */
    if (ridge->down_sign > 0) {
        last += lip_px + 1;
    } else {
        first -= lip_px;
        last += 1;
    }
    if (first < *lo) {
        *lo = first;
    }
    if (last > *hi) {
        *hi = last;
    }
}

/* Columns 16 wide: one PSRAM cache line of RGB565. */
#define COLUMN_GROUP 16

static inline __attribute__((always_inline)) gfx_color_t
column_pixel(int x, int y, uint8_t reveal_alpha) {
    const int phase = y & 3;
    if (!scanline_pick(reveal_alpha, phase)) {
        return GFX_RGB(0);
    }
    gfx_color_t color = ridge->background_color;
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        const int boundary = ridge->boundary[layer][x];
        if (boundary == INT16_MAX) {
            continue;
        }
        if (scanline_pick(layer_alpha_at(layer, ridge->down_sign * (y - boundary)), phase)) {
            color = layer == 0 ? ridge->back0_color : layer == 1 ? ridge->back1_color : sky_at(x, y);
        }
    }
    return color;
}

static void
repaint_column_group(const int16_t* lo, const int16_t* hi, int x0, uint8_t reveal_alpha) {
    int y0 = GFX_HEIGHT, y1 = 0;
    for (int x = x0; x < x0 + COLUMN_GROUP && x < GFX_WIDTH; x++) {
        if (lo[x] < hi[x]) {
            y0 = lo[x] < y0 ? lo[x] : y0;
            y1 = hi[x] > y1 ? hi[x] : y1;
        }
    }
    if (y0 >= y1) {
        return;
    }
    const int x1 = x0 + COLUMN_GROUP < GFX_WIDTH ? x0 + COLUMN_GROUP : GFX_WIDTH;
    gfx_color_t* const framebuffer = gfx_framebuffer();
    for (int y = y0; y < y1; y++) {
        gfx_color_t* const row = framebuffer + y * GFX_WIDTH;
        for (int x = x0; x < x1; x++) {
            if (y >= lo[x] && y < hi[x]) {
                row[x] = column_pixel(x, y, reveal_alpha);
            }
        }
    }
    gfx_mark_dirty(x0, y0, x1 - x0, y1 - y0);
}

/* Portrait: a column's pixels are a row apart in the framebuffer, so paint
 * a group of columns row by row rather than each column top to bottom. */
static void
repaint_changed_columns(uint8_t reveal_alpha) {
    int16_t lo[GFX_WIDTH], hi[GFX_WIDTH];
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            const int was = ridge->shown[layer][x], now = ridge->boundary[layer][x];
            int first = INT_MAX, last = INT_MIN;
            if (was != now) {
                extend_repaint_range(was, now, &first, &last);
            }
            lo[x] = (int16_t)(first < 0 ? 0 : first > GFX_HEIGHT ? GFX_HEIGHT : first);
            hi[x] = (int16_t)(last > GFX_HEIGHT ? GFX_HEIGHT : last < 0 ? 0 : last);
            ridge->shown[layer][x] = (int16_t)now;
        }
        for (int x0 = 0; x0 < GFX_WIDTH; x0 += COLUMN_GROUP) {
            repaint_column_group(lo, hi, x0, reveal_alpha);
        }
    }
}

static void
repaint_changed(void) {
    if (ridge->by_column && ridge->scanline_dither) {
        const int reveal =
            ridge->alive_ms <= (uint32_t)boot_hold_ms
                ? 0
                : ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms);
        repaint_changed_columns((uint8_t)(reveal * 255 / 256));
        return;
    }
    for (int strip = 0; strip < ridge->strips; strip++) {
        for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
            const int was = ridge->shown[layer][strip];
            const int now = ridge->boundary[layer][strip];
            if (was == now) {
                continue;
            }
            int lo = INT_MAX, hi = INT_MIN;
            extend_repaint_range(was, now, &lo, &hi);
            ridge->shown[layer][strip] = now;
            repaint_strip(strip, lo, hi);
        }
    }
}

static uint8_t
current_reveal_alpha(void) {
    const int reveal = ridge->alive_ms <= (uint32_t)boot_hold_ms
                           ? 0
                           : ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms);
    return (uint8_t)(reveal * 255 / 256);
}

/* A turn of the pose moves the fill's gradient under every pixel, not just
 * the boundaries. Recompute each row, write and mark only the stretch of it
 * that differs from what the framebuffer holds, so a small turn sends the
 * shade edges that moved rather than the whole screen. */
static void
repaint_turned(void) {
    static gfx_color_t line[GFX_WIDTH];
    const uint8_t reveal_alpha = current_reveal_alpha();
    gfx_color_t* const framebuffer = gfx_framebuffer();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        if (ridge->by_column) {
            for (int x = 0; x < GFX_WIDTH; x++) {
                line[x] = column_pixel(x, y, reveal_alpha);
            }
        } else {
            paint_row_into(line, y, 0, GFX_WIDTH, reveal_alpha);
        }
        gfx_color_t* const row = framebuffer + y * GFX_WIDTH;
        int first = 0, last = GFX_WIDTH - 1;
        while (first <= last && line[first] == row[first]) {
            first++;
        }
        while (last >= first && line[last] == row[last]) {
            last--;
        }
        if (first <= last) {
            memcpy(row + first, line + first, (size_t)(last - first + 1) * sizeof(*row));
            gfx_mark_dirty(first, y, last - first + 1, 1);
        }
    }
    memcpy(ridge->shown, ridge->boundary, sizeof ridge->shown);
}

static void
paint_all(void) {
    const int reveal = ridge->alive_ms <= (uint32_t)boot_hold_ms
                           ? 0
                           : ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms);
    const uint8_t reveal_alpha = (uint8_t)(reveal * 255 / 256);
    gfx_color_t* const framebuffer = gfx_framebuffer();
    if (!ridge->by_column && ridge->scanline_dither) {
        for (int y = 0; y < GFX_HEIGHT; y++) {
            paint_row_span(framebuffer, y, 0, GFX_WIDTH, reveal_alpha);
        }
    } else if (ridge->scanline_dither) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            paint_column_span(framebuffer, x, 0, GFX_HEIGHT, reveal_alpha);
        }
    } else {
        for (int y = 0; y < GFX_HEIGHT; y++) {
            for (int x = 0; x < GFX_WIDTH; x++) {
                framebuffer[y * GFX_WIDTH + x] = backdrop_pixel(x, y, reveal_alpha);
            }
        }
    }
    gfx_mark_all_dirty();
    memcpy(ridge->shown, ridge->boundary, sizeof ridge->shown);
    ridge->painted = true;
}

static void
bake_what_is_tuned(void) {
    if ((uint32_t)theme_rgb != ridge->theme_seed) {
        apply_theme();
    }
    ridge_motion_smooth(ridge->rigid, ridge->smooth, ridge->shape, RIDGE_COLUMNS, breath_smooth);
    ridge->background_color = gfx_rgb((uint32_t)background_rgb);
    ridge->back0_color = gfx_rgb((uint32_t)back0_rgb);
    ridge->back1_color = gfx_rgb((uint32_t)back1_rgb);
    const int lip_start[RIDGE_LAYER_COUNT] = {back0_lip_alpha, back1_lip_alpha, front_lip_alpha};
    const int lip_end[RIDGE_LAYER_COUNT] = {back0_body_alpha, back1_body_alpha, front_body_alpha};
    for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
        for (int distance = 0; distance < lip_px; distance++) {
            ridge->lip_alpha[layer][distance] =
                (uint8_t)(lip_start[layer] + (lip_end[layer] - lip_start[layer]) * distance / lip_px);
        }
    }
    ridge->scanline_dither = fill_pattern == GFX_DITHER_SCANLINES4;
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
    ridge->shake_seed = 0x9e3779b9u;
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
                      + (ridge_motion_height(&ridge->motion, &params, ridge->rigid[x], ridge->smooth[x], x)
                         - ridge->rigid[x])
                            * gain / 256);
    }
}

static void
pluck_from_shaking(void) {
    if (ridge->shake < SHAKE_THRESHOLD) {
        return;
    }
    ridge->shake_seed = ridge->shake_seed * 1664525u + 1013904223u;
    spring_line_poke(&ridge->line, (int)((ridge->shake_seed >> 8) % RIDGE_COLUMNS), SHAKE_HALF_WIDTH,
                     (ridge->shake_seed & 0x80u ? 1 : -1) * (SPRING_LINE_ONE / 128) * ridge->shake);
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
                                             .redraw_step = POSE_REDRAW_STEP};
    ridge_pose_advance(&ridge->attitude, &pose_params, dt_ms, ridge->alive_ms);
    pluck_from_touch(input);
    if (ridge->ambient) {
        pluck_from_shaking();
    }
    shape_this_frame(dt_ms);
    int lo, hi;
    spring_line_advance(&ridge->line, dt_ms);
    spring_line_apply(&ridge->line, ridge->shape, ridge->heights, &lo, &hi);
    const bool pose_moved = snap_pose_on_screen();
    FRAME_COST_BEGIN(layers_from);
    build_layers();
    raster_boundaries();
    FRAME_COST_END(layers_from, "ridge.layers");
    const bool revealing = ridge->alive_ms < (uint32_t)boot_hold_ms + (uint32_t)ambient_ease_ms;
    FRAME_COST_BEGIN(painted_from);
    if (!ridge->painted || retuned || ridge->axis_on_screen || revealing) {
        paint_all();
    } else if (pose_moved && ridge->scanline_dither) {
        repaint_turned();
    } else if (hi > lo || pose_moved || ridge->ambient) {
        repaint_changed();
    }
    FRAME_COST_END(painted_from, "ridge.paint");
}

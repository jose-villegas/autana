/*
 * ui_ridge - the launcher's layered backdrop: two dithered back ridges,
 * a dithered sky fill, and Cerro Autana's glowing ridge in front.
 *
 * The ridge is the one the boot animation's photograph ends on, in the same
 * frame, and the launcher's first frames hold it in boot's landscape pose, so
 * the hand-over leaves the outline where the mountain was. Boot knows no
 * orientation; only after boot_hold_ms does the line give in to gravity and
 * ease round to true level, which it then keeps at any angle while the app
 * rows turn in quarters.
 *
 * The boot hand-off stays black until the front ridge has held its photograph
 * pose. The layers then ease in behind it without changing the dissolve.
 */

#include "ui/ui_ridge.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "ui/ridge_curve_generated.h"
#include "ui/ridge_layers.h"
#include "ui/ridge_motion.h"
#include "ui/ridge_pose.h"
#include "util/frame_cost.h"
#include "util/spring_line.h"
#include "util/tune.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define RIDGE_ALLOC(bytes) heap_caps_malloc((bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define RIDGE_ALLOC(bytes) malloc(bytes)
#endif

/* What is judged by eye is a tunable: a development build changes these on
 * the running device ("SET ridge.trail 200"), a release build compiles them
 * in. The values here are the ones that ship. */
TUNE_OWNER(ridge);
TUNE(ridge, glow_radius, 13, 1, GFX_GLOW_MAX_RADIUS);
TUNE(ridge, glow_core, 3, 1, GFX_GLOW_MAX_RADIUS);
TUNE(ridge, glow_core_rgb, 0xFFFFFF, 0, 0xFFFFFF);
TUNE(ridge, glow_halo_rgb, 0x38D6E8, 0, 0xFFFFFF);
TUNE(ridge, glow_pattern, GFX_DITHER_BAYER4, 0, GFX_DITHER_PATTERN_COUNT - 1);
/* Levels of stippled light in the halo; none is a smooth one. */
TUNE(ridge, glow_steps, 0, 0, 16);

TUNE(ridge, sky_top_rgb, 0x1199C8, 0, 0xFFFFFF);
TUNE(ridge, sky_bottom_rgb, 0x91C6D1, 0, 0xFFFFFF);
TUNE(ridge, background_rgb, 0x0B6382, 0, 0xFFFFFF);
TUNE(ridge, colour_balance, 45, 0, 100);
TUNE(ridge, fill_pattern, GFX_DITHER_SCANLINES4, 0, GFX_DITHER_PATTERN_COUNT - 1);
TUNE(ridge, fill_levels, 6, 1, 16);
TUNE(ridge, lip_px, 12, 1, 48);
TUNE(ridge, back0_offset, -70, -160, 80);
TUNE(ridge, back0_amplitude, 5, 0, 32);
TUNE(ridge, back0_wavelength, 260, 8, 1000);
TUNE(ridge, back0_period_ms, 6000, 100, 60000);
TUNE(ridge, back0_rgb, 0x07638C, 0, 0xFFFFFF);
TUNE(ridge, back0_body_alpha, 204, 0, 255);
TUNE(ridge, back0_lip_alpha, 71, 0, 255);
TUNE(ridge, back1_offset, -30, -160, 80);
TUNE(ridge, back1_amplitude, 8, 0, 32);
TUNE(ridge, back1_wavelength, 200, 8, 1000);
TUNE(ridge, back1_period_ms, 4300, 100, 60000);
TUNE(ridge, back1_rgb, 0x10606B, 0, 0xFFFFFF);
TUNE(ridge, back1_body_alpha, 204, 0, 255);
TUNE(ridge, back1_lip_alpha, 71, 0, 255);
TUNE(ridge, front_offset, 0, -160, 80);
TUNE(ridge, front_body_alpha, 217, 0, 255);
TUNE(ridge, front_lip_alpha, 76, 0, 255);

/* What becomes of the light the line leaves behind as it moves, out of 256
 * per redraw: 0 wipes it, 255 never does, between is a trail that fades. */
TUNE(ridge, trail, 226, 0, 255);

/* A tap flicks the line up; a finger drawn along it plucks it again every
 * STRUM_STEP_PX. Thousandths of a pixel per spring tick, over pluck_width
 * columns to either side. */
TUNE(ridge, pluck_tap, 8000, 0, 8000);
TUNE(ridge, pluck_strum, 8000, 0, 8000);
TUNE(ridge, pluck_width, 32, 2, 80);
#define STRUM_STEP_PX 12

/* The spring's own three, out of 256 - see spring_line.h. */
TUNE(ridge, spring_tension, 64, 0, 250);
TUNE(ridge, spring_stiffness, 4, 1, 64);
TUNE(ridge, spring_damping, 2, 0, 64);

/* ridge_motion.h's, by the names a developer types. */
TUNE(ridge, breath_ms, 5000, 500, 60000);
TUNE(ridge, breath_depth, 164, 0, 256);
TUNE(ridge, breath_smooth, 20, 0, 60);
TUNE(ridge, front_amplitude, 6, 0, 32);
TUNE(ridge, front_wavelength, 164, 8, 1000);
TUNE(ridge, front_period_ms, 2600, 100, 60000);
TUNE(ridge, tilt_push, 350, 0, 1000);
TUNE(ridge, tilt_coast_ms, 2000, 50, 10000);

/* How long the line takes to cover about two thirds of a turn toward level. */
TUNE(ridge, level_tau_ms, 700, 10, 5000);

/* Shaking plucks the line at random, harder the harder it is shaken. */
#define SHAKE_THRESHOLD  48
#define SHAKE_HALF_WIDTH 16

/* How long the line keeps boot's pose before gravity gets it, and how long
 * breathing and the wave then take to come in. Until released it is rigid:
 * at the hand-over it has to lie on the photograph. */
TUNE(ridge, boot_hold_ms, 700, 0, 10000);
TUNE(ridge, ambient_ease_ms, 4000, 0, 30000);

/* Below this share of a g in the screen plane the device is lying too flat
 * for "down" to mean anything, and the line keeps the level it had. Out of
 * 256. */
#define MIN_TILT_STRENGTH 64

/* A pose is redrawn once it is this far, in Q14, from the one on screen:
 * about half a degree. A hand is never still; this is what keeps a held
 * device from redrawing every frame for a change nobody could see. */
#define POSE_REDRAW_STEP  143

/* Easing never quite arrives, and the step above would let the line rest
 * half a degree off level. Once down has held within LEVEL_STEADY_STEP (a
 * tenth of a degree) for LEVEL_STEADY_MS - a desk, not a hand - the line is
 * put exactly level and drawn once more. */
#define LEVEL_STEADY_STEP 29
#define LEVEL_STEADY_MS   300

/* The line is longer than the frame it was drawn in: at a diagonal it has to
 * span the panel's diagonal, 580 px, with its glow, or its ends show. */
#define RIDGE_EXTRA       88
#define RIDGE_COLUMNS     (RIDGE_CURVE_POINTS + 2 * RIDGE_EXTRA)

/* The glow is drawn from a map of its light - see gfx_glow.h. */
#define MAP_COLS          ((RIDGE_COLUMNS + GFX_GLOW_MAP_CELL - 1) / GFX_GLOW_MAP_CELL)
#define MAP_ROWS          200

#define POSE_LANDSCAPE    ((gfx_glow_pose_t){-GFX_GLOW_POSE_ONE, 0})

#define RIDGE_LAYER_COUNT 3

typedef struct {
    spring_line_t line;
    gfx_glow_style_t style;
    gfx_glow_field_t field;
    int32_t offset[RIDGE_COLUMNS];
    int32_t velocity[RIDGE_COLUMNS];
    int16_t rigid[RIDGE_COLUMNS];
    int16_t heights[RIDGE_COLUMNS];
    int16_t smooth[RIDGE_COLUMNS];
    int16_t shape[RIDGE_COLUMNS];
    int16_t layers[RIDGE_LAYER_COUNT][RIDGE_COLUMNS];
    int16_t shown_layers[RIDGE_LAYER_COUNT][RIDGE_COLUMNS];
    ridge_motion_t motion;
    uint32_t tuned_at;
    bool ambient;
    int16_t span_lo[RIDGE_COLUMNS];
    int16_t span_hi[RIDGE_COLUMNS];
    int16_t reach_lo[RIDGE_COLUMNS];
    int16_t reach_hi[RIDGE_COLUMNS];
    gfx_glow_map_t map;
    uint16_t map_cells[MAP_COLS * MAP_ROWS];
    int32_t map_row_f[MAP_COLS];
    int32_t map_row_z[MAP_COLS];
    int16_t map_row_v[MAP_COLS];
    int16_t lit_lo[GFX_HEIGHT];
    int16_t lit_hi[GFX_HEIGHT];
    ridge_pose_t attitude;
    gfx_glow_pose_t pose_on_screen;
    uint32_t alive_ms;
    uint32_t shake_seed;
    int shake;
    int last_pluck_x;
    int fade_draws_left;
    bool backdrop_painted;
} ridge_t;

static ridge_t* ridge;
static bool allocation_tried;

static uint32_t
rgb_mix(uint32_t a, uint32_t b, int amount) {
    uint32_t mixed = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int from = (a >> shift) & 0xFF;
        const int to = (b >> shift) & 0xFF;
        mixed |= (uint32_t)(from + (to - from) * amount / 100) << shift;
    }
    return mixed;
}

static uint32_t
sky_colour(int view_y) {
    const uint32_t top = rgb_mix((uint32_t)sky_top_rgb, (uint32_t)background_rgb, colour_balance);
    const uint32_t bottom = rgb_mix((uint32_t)sky_bottom_rgb, (uint32_t)background_rgb, colour_balance);
    const int t = view_y < 0 ? 0 : (view_y >= RIDGE_CURVE_VIEW_H ? 256 : view_y * 256 / RIDGE_CURVE_VIEW_H);
    return rgb_mix(top, bottom, t * 100 / 256);
}

static int
layer_alpha(int delta, int lip_alpha, int body_alpha) {
    if (delta < 0) {
        return 0;
    }
    if (delta >= lip_px) {
        return body_alpha;
    }
    return lip_alpha + (body_alpha - lip_alpha) * delta / lip_px;
}

static bool
layer_covers(int x, int y, int alpha) {
    return gfx_dither_alpha_pick((gfx_dither_pattern_id_t)fill_pattern, x, y, (uint8_t)alpha);
}

static uint32_t
quantise_fill_rgb(uint32_t rgb, int x, int y) {
    const gfx_dither_pattern_t* const pattern = gfx_dither_pattern((gfx_dither_pattern_id_t)fill_pattern);
    const int phase = gfx_dither_threshold((gfx_dither_pattern_id_t)fill_pattern, x, y) * 64 / pattern->levels;
    const int steps = fill_levels * 4;
    uint32_t quantised = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int channel = (rgb >> shift) & 0xFF;
        int level = (channel * steps * 64 / 255 + phase) / 64;
        level = level > steps ? steps : level;
        quantised |= (uint32_t)(level * 255 / steps) << shift;
    }
    return quantised;
}

static gfx_color_t
backdrop_pixel(int panel_x, int panel_y, int ambient) {
    if (ambient == 0) {
        return GFX_RGB(0x000000);
    }
    const int column = panel_y + RIDGE_EXTRA;
    const int view_y = GFX_WIDTH - 1 - panel_x;
    uint32_t rgb = rgb_mix((uint32_t)background_rgb, sky_colour(RIDGE_CURVE_VIEW_H / 2), colour_balance);
    const int alpha0 = layer_alpha(view_y - (ridge->layers[0][column] >> 4), back0_lip_alpha, back0_body_alpha);
    const int alpha1 = layer_alpha(view_y - (ridge->layers[1][column] >> 4), back1_lip_alpha, back1_body_alpha);
    const int alpha2 = layer_alpha(view_y - (ridge->layers[2][column] >> 4), front_lip_alpha, front_body_alpha);
    if (layer_covers(panel_x, panel_y, alpha0)) {
        rgb = (uint32_t)back0_rgb;
    }
    if (layer_covers(panel_x, panel_y, alpha1)) {
        rgb = (uint32_t)back1_rgb;
    }
    if (layer_covers(panel_x, panel_y, alpha2)) {
        rgb = sky_colour(view_y);
    }
    return ambient >= 256 || layer_covers(panel_x, panel_y, ambient) ? gfx_rgb(quantise_fill_rgb(rgb, panel_x, panel_y))
                                                                     : GFX_RGB(0x000000);
}

static void
repaint_backdrop_span(int column, int lo, int hi, int ambient) {
    const int panel_y = column - RIDGE_EXTRA;
    if (panel_y < 0 || panel_y >= GFX_HEIGHT) {
        return;
    }
    int x0 = GFX_WIDTH - hi;
    int x1 = GFX_WIDTH - lo;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > GFX_WIDTH ? GFX_WIDTH : x1;
    if (x1 <= x0) {
        return;
    }
    gfx_color_t* const framebuffer = gfx_framebuffer();
    for (int x = x0; x < x1; x++) {
        framebuffer[panel_y * GFX_WIDTH + x] = backdrop_pixel(x, panel_y, ambient);
    }
    gfx_mark_dirty(x0, panel_y, x1 - x0, 1);
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
                                   - (uint32_t)((uint64_t)ridge->alive_ms * 65536u / (uint32_t)period[layer]);
            const int echo = spring_line_scale(ridge->line.offset[x], layer == 0 ? 64 : 128) / (SPRING_LINE_ONE / 16);
            ridge->layers[layer][x] = (int16_t)(ridge->rigid[x] + offset[layer] * 16
                                                + amplitude[layer] * trig_sin((uint16_t)phase) / 2048 + echo);
        }
        ridge->layers[2][x] = (int16_t)(ridge->heights[x] + front_offset * 16);
    }
}

static void
repaint_moved_backdrop(int ambient) {
    for (int column = RIDGE_EXTRA; column < RIDGE_EXTRA + RIDGE_CURVE_POINTS; column++) {
        int lo = RIDGE_CURVE_VIEW_H;
        int hi = 0;
        for (int layer = 0; layer < RIDGE_LAYER_COUNT; layer++) {
            int a, b;
            ridge_layer_dirty_span(ridge->shown_layers[layer][column], ridge->layers[layer][column], lip_px,
                                   layer == RIDGE_LAYER_COUNT - 1 ? glow_radius : 0, &a, &b);
            lo = a < lo ? a : lo;
            hi = b > hi ? b : hi;
            ridge->shown_layers[layer][column] = ridge->layers[layer][column];
        }
        repaint_backdrop_span(column, lo, hi, ambient);
    }
}

/* What a draw measures distance against, for the line as it now stands. */
static void
prepare_light(void) {
    FRAME_COST_BEGIN(began);
    gfx_glow_field_prepare(&ridge->field, ridge->heights, &ridge->style);
    if (gfx_glow_map_worth_it(glow_radius)) {
        gfx_glow_map_build(&ridge->map, &ridge->field, &ridge->style);
    }
    FRAME_COST_END(began, "ridge.light");
}

/* The glow's ramp and the smoothed shape are tables built from tunables, so
 * they are built again when one changes; everything else is read each frame. */
static void
bake_what_is_tuned(void) {
    gfx_glow_style_set_stepped(&ridge->style, glow_radius, glow_core, (uint32_t)glow_core_rgb, (uint32_t)glow_halo_rgb,
                               glow_steps);
    ridge->style.pattern = (gfx_dither_pattern_id_t)glow_pattern;
    ridge_motion_smooth(ridge->rigid, ridge->smooth, ridge->shape, RIDGE_COLUMNS, breath_smooth);
    memcpy(ridge->shape, ridge->heights, sizeof ridge->shape);
    prepare_light();
    ridge->tuned_at = TUNE_GENERATION(ridge);
}

/* On first use rather than at boot, so the memory is not taken from a boot
 * that never reaches the launcher. Without it the backdrop is plain black. */
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
    ridge->ambient = false;
    ridge->field = (gfx_glow_field_t){
        .span_lo = ridge->span_lo,
        .span_hi = ridge->span_hi,
        .reach_lo = ridge->reach_lo,
        .reach_hi = ridge->reach_hi,
        .count = RIDGE_COLUMNS,
    };
    ridge->map = (gfx_glow_map_t){
        .cells = ridge->map_cells,
        .row_f = ridge->map_row_f,
        .row_z = ridge->map_row_z,
        .row_v = ridge->map_row_v,
        .cols = MAP_COLS,
        .rows = MAP_ROWS,
    };
    bake_what_is_tuned();
    build_layers();
    memcpy(ridge->shown_layers, ridge->layers, sizeof ridge->shown_layers);
    ridge->attitude.pose = POSE_LANDSCAPE;
    ridge->pose_on_screen = POSE_LANDSCAPE;
    ridge->attitude.level = POSE_LANDSCAPE;
    ridge->attitude.steady_level = POSE_LANDSCAPE;
    ridge->shake_seed = 0x9E3779B9u;
}

void
ui_ridge_set_gravity(int gx, int gy, int strength, int shake) {
    allocate_once();
    if (ridge == NULL) {
        return;
    }
    ridge->shake = shake;
    ridge->attitude.level = ridge_pose_level_from_gravity(ridge->attitude.level, gx, gy, strength, MIN_TILT_STRENGTH);
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
        ridge->alive_ms = (uint32_t)boot_hold_ms;
        ridge->attitude.pose = ridge->attitude.level;
        ridge->attitude.steady_level = ridge->attitude.level;
        ridge->attitude.steady_ms = LEVEL_STEADY_MS;
    }
}

static void
draw_ridge(void) {
    FRAME_COST_BEGIN(began);
    gfx_glow_curve_posed(&ridge->field, gfx_glow_map_worth_it(glow_radius) ? &ridge->map : NULL, RIDGE_CURVE_VIEW_H,
                         ridge->attitude.pose, ridge->lit_lo, ridge->lit_hi, trail, &ridge->style);
    ridge->pose_on_screen = ridge->attitude.pose;
    FRAME_COST_END(began, "ridge.draw");
}

void
ui_ridge_paint(void) {
    allocate_once();
    if (ridge == NULL) {
        gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x000000));
        return;
    }
    const int released = ridge->ambient && ridge->alive_ms > (uint32_t)boot_hold_ms
                             ? ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms)
                             : 0;
    build_layers();
    gfx_color_t* const framebuffer = gfx_framebuffer();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            framebuffer[y * GFX_WIDTH + x] = backdrop_pixel(x, y, released);
        }
    }
    gfx_mark_all_dirty();
    memcpy(ridge->shown_layers, ridge->layers, sizeof ridge->shown_layers);
    ridge->backdrop_painted = released == 256;
    memset(ridge->lit_lo, 0, sizeof ridge->lit_lo);
    memset(ridge->lit_hi, 0, sizeof ridge->lit_hi);
    draw_ridge();
}

static void
pluck_from_touch(const input_t* input) {
    if (!input->down) {
        return;
    }
    const int x =
        ridge_pose_column_under(ridge->attitude.pose, GFX_WIDTH, GFX_HEIGHT, RIDGE_COLUMNS, input->x, input->y);
    if (input->pressed) {
        spring_line_poke(&ridge->line, x, pluck_width, -(int32_t)((int64_t)SPRING_LINE_ONE * pluck_tap / 1000));
        ridge->last_pluck_x = x;
    } else if (abs(x - ridge->last_pluck_x) >= STRUM_STEP_PX) {
        spring_line_poke(&ridge->line, x, pluck_width, -(int32_t)((int64_t)SPRING_LINE_ONE * pluck_strum / 1000));
        ridge->last_pluck_x = x;
    }
}

/* The shape the line rests at this frame: the rigid ridge, breathing and
 * carrying its wave once released. */
static void
shape_this_frame(uint32_t dt_ms) {
    if (!ridge->ambient || ridge->alive_ms < (uint32_t)boot_hold_ms) {
        memcpy(ridge->shape, ridge->rigid, sizeof ridge->shape);
        return;
    }
    const ridge_motion_params_t params = {
        .breath_ms = breath_ms,
        .breath_depth = breath_depth,
        .wave_height_q4 = front_amplitude * 16,
        .wave_length = front_wavelength,
        .wave_passes_in_ms = front_period_ms,
        .push = tilt_push,
        .coast_ms = tilt_coast_ms,
    };
    ridge_motion_advance(&ridge->motion, &params, dt_ms, ridge_pose_slope(&ridge->attitude));
    const uint32_t released_for = ridge->alive_ms - (uint32_t)boot_hold_ms;
    const int gain = ridge_motion_ease_in(released_for, (uint32_t)ambient_ease_ms);
    for (int x = 0; x < RIDGE_COLUMNS; x++) {
        const int moved =
            ridge_motion_height(&ridge->motion, &params, ridge->rigid[x], ridge->smooth[x], x) - ridge->rigid[x];
        ridge->shape[x] = (int16_t)(ridge->rigid[x] + moved * gain / 256);
    }
}

static void
pluck_from_shaking(void) {
    if (ridge->shake < SHAKE_THRESHOLD) {
        return;
    }
    ridge->shake_seed = ridge->shake_seed * 1664525u + 1013904223u;
    const int x = (int)((ridge->shake_seed >> 8) % RIDGE_COLUMNS);
    const int32_t up_or_down = (ridge->shake_seed & 0x80u) ? 1 : -1;
    spring_line_poke(&ridge->line, x, SHAKE_HALF_WIDTH, up_or_down * (SPRING_LINE_ONE / 128) * ridge->shake);
}

static bool
pose_moved_enough_to_see(void) {
    return !ridge_pose_within(ridge->attitude.pose, ridge->pose_on_screen, POSE_REDRAW_STEP);
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
    const ridge_pose_params_t pose_params = {
        .boot_pose = POSE_LANDSCAPE,
        .hold_ms = (uint32_t)boot_hold_ms,
        .tau_ms = level_tau_ms,
        .steady_step = LEVEL_STEADY_STEP,
        .steady_hold_ms = LEVEL_STEADY_MS,
        .redraw_step = POSE_REDRAW_STEP,
    };
    const bool arrived = ridge_pose_advance(&ridge->attitude, &pose_params, dt_ms, ridge->alive_ms);

    pluck_from_touch(input);
    if (ridge->ambient) {
        pluck_from_shaking();
    }

    shape_this_frame(dt_ms);
    spring_line_advance(&ridge->line, dt_ms);
    int lo, hi;
    spring_line_apply(&ridge->line, ridge->shape, ridge->heights, &lo, &hi);
    const bool line_moved = hi > lo;
    build_layers();
    if (line_moved) {
        prepare_light();
    }
    const bool settles_now = arrived && !ridge_pose_within(ridge->attitude.pose, ridge->pose_on_screen, 1);
    /* A tail that fades is drawn until it is gone, or it would freeze where
     * the line stopped. */
    const int ambient_gain =
        ridge->ambient && ridge->alive_ms > (uint32_t)boot_hold_ms
            ? ridge_motion_ease_in(ridge->alive_ms - (uint32_t)boot_hold_ms, (uint32_t)ambient_ease_ms)
            : 0;
    const bool backdrop_moved = ambient_gain > 0 && (ambient_gain < 256 || ridge->ambient);
    const bool moved = line_moved || settles_now || retuned || pose_moved_enough_to_see() || backdrop_moved;
    if (moved) {
        ridge->fade_draws_left = gfx_glow_trail_draws(trail);
    } else if (ridge->fade_draws_left > 0) {
        ridge->fade_draws_left--;
    }
    if (moved || ridge->fade_draws_left > 0) {
        if (ambient_gain < 256 || !ridge->backdrop_painted) {
            ui_ridge_paint();
        } else {
            repaint_moved_backdrop(ambient_gain);
            draw_ridge();
        }
    }
}

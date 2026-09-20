/*
 * ui_ridge - the launcher's backdrop: Cerro Autana's ridge as a line of
 * light on black, level with the horizon and set waving by a touch.
 *
 * The ridge is the one the boot animation's photograph ends on, in the same
 * frame, and the launcher's first frames hold it in boot's landscape pose, so
 * the hand-over leaves the outline where the mountain was. Boot knows no
 * orientation; only after boot_hold_ms does the line give in to gravity and
 * ease round to true level, which it then keeps at any angle while the app
 * rows turn in quarters.
 *
 * Black is the point and not a default: an AMOLED pixel at 0 is off, so the
 * line is the only thing lit.
 */

#include "ui/ui_ridge.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "ui/ridge_curve_generated.h"
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
/* Levels of stippled light in the halo; none is a smooth one. */
TUNE(ridge, glow_steps, 0, 0, 16);

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
TUNE(ridge, wave_height, 300, 0, 400);
TUNE(ridge, wave_length, 164, 8, 1000);
TUNE(ridge, wave_period_ms, 2600, 100, 60000);
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
} ridge_t;

static ridge_t* ridge;
static bool allocation_tried;

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
    ridge->ambient = true;
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
    gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x000000));
    if (ridge == NULL) {
        return;
    }
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
        .wave_height_q4 = wave_height,
        .wave_length = wave_length,
        .wave_passes_in_ms = wave_period_ms,
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
    pluck_from_shaking();

    shape_this_frame(dt_ms);
    spring_line_advance(&ridge->line, dt_ms);
    int lo, hi;
    spring_line_apply(&ridge->line, ridge->shape, ridge->heights, &lo, &hi);
    const bool line_moved = hi > lo;
    if (line_moved) {
        prepare_light();
    }
    const bool settles_now = arrived && !ridge_pose_within(ridge->attitude.pose, ridge->pose_on_screen, 1);
    /* A tail that fades is drawn until it is gone, or it would freeze where
     * the line stopped. */
    const bool moved = line_moved || settles_now || retuned || pose_moved_enough_to_see();
    if (moved) {
        ridge->fade_draws_left = gfx_glow_trail_draws(trail);
    } else if (ridge->fade_draws_left > 0) {
        ridge->fade_draws_left--;
    }
    if (moved || ridge->fade_draws_left > 0) {
        draw_ridge();
    }
}

/*
 * ui_ridge - the launcher's backdrop: Cerro Autana's ridge as a line of
 * light on black, level with the horizon and set waving by a touch.
 *
 * The ridge is the one the boot animation's photograph ends on, in the same
 * frame, and the launcher's first frames hold it in boot's landscape pose, so
 * the hand-over leaves the outline where the mountain was. Boot knows no
 * orientation; only after RELEASE_MS does the line give in to gravity and
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
#include "util/spring_line.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define RIDGE_ALLOC(bytes) heap_caps_malloc((bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define RIDGE_ALLOC(bytes) malloc(bytes)
#endif

#define GLOW_RADIUS_PX     13
#define GLOW_CORE_PX       3
#define GLOW_CORE_RGB      0xFFFFFF
#define GLOW_HALO_RGB      0x38D6E8

/* A tap flicks the line up; a finger drawn along it plucks it again every
 * STRUM_STEP_PX. Velocities are pixels per spring tick. */
#define TOUCH_HALF_WIDTH   24
#define TAP_VELOCITY       (-(SPRING_LINE_ONE * 3 / 2))
#define STRUM_VELOCITY     (-(SPRING_LINE_ONE / 2))
#define STRUM_STEP_PX      12

/* Shaking plucks the line at random, harder the harder it is shaken. */
#define SHAKE_THRESHOLD    48
#define SHAKE_HALF_WIDTH   16

/* Breathing and the wave come in over this long once the line is released.
 * Until then it is rigid: at the hand-over it has to lie on the photograph. */
#define AMBIENT_FADE_IN_MS 1500

/* How long the line keeps boot's pose before gravity gets it. */
#define RELEASE_MS         700

/* How long the line takes to cover about two thirds of a turn toward level. */
#define LEVEL_TAU_MS       220

/* Below this share of a g in the screen plane the device is lying too flat
 * for "down" to mean anything, and the line keeps the level it had. Out of
 * 256. */
#define MIN_TILT_STRENGTH  64

/* A pose is redrawn once it is this far, in Q14, from the one on screen:
 * about half a degree. A hand is never still; this is what keeps a held
 * device from redrawing every frame for a change nobody could see. */
#define POSE_REDRAW_STEP   143

/* Easing never quite arrives, and the step above would let the line rest
 * half a degree off level. Once down has held within LEVEL_STEADY_STEP (a
 * tenth of a degree) for LEVEL_STEADY_MS - a desk, not a hand - the line is
 * put exactly level and drawn once more. */
#define LEVEL_STEADY_STEP  29
#define LEVEL_STEADY_MS    300

/* What becomes of the light the line leaves behind as it moves, out of 256
 * per redraw: 0 wipes it, 255 never does, between is a trail that fades. */
#define RIDGE_TRAIL        32

#define POSE_LANDSCAPE     ((gfx_glow_pose_t){-GFX_GLOW_POSE_ONE, 0})

typedef struct {
    spring_line_t line;
    gfx_glow_style_t style;
    gfx_glow_field_t field;
    int32_t offset[RIDGE_CURVE_POINTS];
    int32_t velocity[RIDGE_CURVE_POINTS];
    int16_t heights[RIDGE_CURVE_POINTS];
    int16_t smooth[RIDGE_CURVE_POINTS];
    int16_t shape[RIDGE_CURVE_POINTS];
    ridge_motion_t motion;
    bool ambient;
    int16_t span_lo[RIDGE_CURVE_POINTS];
    int16_t span_hi[RIDGE_CURVE_POINTS];
    int16_t reach_lo[RIDGE_CURVE_POINTS];
    int16_t reach_hi[RIDGE_CURVE_POINTS];
    int16_t lit_lo[GFX_HEIGHT];
    int16_t lit_hi[GFX_HEIGHT];
    gfx_glow_pose_t pose;
    gfx_glow_pose_t pose_on_screen;
    gfx_glow_pose_t level;
    gfx_glow_pose_t steady_level;
    uint32_t steady_ms;
    uint32_t alive_ms;
    uint32_t shake_seed;
    int shake;
    int last_pluck_x;
    int fade_draws_left;
} ridge_t;

static ridge_t* ridge;
static bool allocation_tried;

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
    spring_line_init(&ridge->line, ridge->offset, ridge->velocity, RIDGE_CURVE_POINTS);
    gfx_glow_style_set(&ridge->style, GLOW_RADIUS_PX, GLOW_CORE_PX, GLOW_CORE_RGB, GLOW_HALO_RGB);
    memcpy(ridge->heights, ridge_curve_y, sizeof ridge->heights);
    ridge_motion_smooth(ridge_curve_y, ridge->smooth, ridge->shape, RIDGE_CURVE_POINTS);
    memcpy(ridge->shape, ridge_curve_y, sizeof ridge->shape);
    ridge->ambient = true;
    ridge->field = (gfx_glow_field_t){
        .span_lo = ridge->span_lo,
        .span_hi = ridge->span_hi,
        .reach_lo = ridge->reach_lo,
        .reach_hi = ridge->reach_hi,
        .count = RIDGE_CURVE_POINTS,
    };
    gfx_glow_field_prepare(&ridge->field, ridge->heights, &ridge->style);
    ridge->pose = POSE_LANDSCAPE;
    ridge->pose_on_screen = POSE_LANDSCAPE;
    ridge->level = POSE_LANDSCAPE;
    ridge->steady_level = POSE_LANDSCAPE;
    ridge->shake_seed = 0x9E3779B9u;
}

void
ui_ridge_set_gravity(int gx, int gy, int strength, int shake) {
    allocate_once();
    if (ridge == NULL) {
        return;
    }
    ridge->shake = shake;
    if (strength < MIN_TILT_STRENGTH) {
        return;
    }
    const int64_t length = (int64_t)gfx_glow_isqrt((uint32_t)(gx * gx + gy * gy));
    if (length > 0) {
        ridge->level.down_x = (int32_t)((int64_t)gx * GFX_GLOW_POSE_ONE / length);
        ridge->level.down_y = (int32_t)((int64_t)gy * GFX_GLOW_POSE_ONE / length);
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
        ridge->alive_ms = RELEASE_MS;
        ridge->pose = ridge->level;
        ridge->steady_level = ridge->level;
        ridge->steady_ms = LEVEL_STEADY_MS;
    }
}

static void
draw_ridge(void) {
    gfx_glow_curve_posed(&ridge->field, RIDGE_CURVE_VIEW_H, ridge->pose, ridge->lit_lo, ridge->lit_hi, RIDGE_TRAIL,
                         &ridge->style);
    ridge->pose_on_screen = ridge->pose;
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

/* Which ridge column lies under a point of the panel, at the current pose. */
static int
column_under(int panel_x, int panel_y) {
    const int64_t right_x = ridge->pose.down_y;
    const int64_t right_y = -ridge->pose.down_x;
    const int64_t dx2 = 2 * (int64_t)panel_x - (GFX_WIDTH - 1);
    const int64_t dy2 = 2 * (int64_t)panel_y - (GFX_HEIGHT - 1);
    return (int)((RIDGE_CURVE_POINTS - 1 + (dx2 * right_x + dy2 * right_y) / GFX_GLOW_POSE_ONE) / 2);
}

static void
pluck_from_touch(const input_t* input) {
    if (!input->down) {
        return;
    }
    const int x = column_under(input->x, input->y);
    if (input->pressed) {
        spring_line_poke(&ridge->line, x, TOUCH_HALF_WIDTH, TAP_VELOCITY);
        ridge->last_pluck_x = x;
    } else if (abs(x - ridge->last_pluck_x) >= STRUM_STEP_PX) {
        spring_line_poke(&ridge->line, x, TOUCH_HALF_WIDTH, STRUM_VELOCITY);
        ridge->last_pluck_x = x;
    }
}

/* How steeply the line runs downhill toward its last column, Q14: the part
 * of true down that lies along the line, which is nothing once it is level
 * and most while a turn is still being caught up with. */
static int32_t
slope_along_the_line(void) {
    const int64_t right_x = ridge->pose.down_y;
    const int64_t right_y = -ridge->pose.down_x;
    return (int32_t)((ridge->level.down_x * right_x + ridge->level.down_y * right_y) / GFX_GLOW_POSE_ONE);
}

/* The shape the line rests at this frame: the rigid ridge, breathing and
 * carrying its wave once released. */
static void
shape_this_frame(uint32_t dt_ms) {
    if (!ridge->ambient || ridge->alive_ms < RELEASE_MS) {
        memcpy(ridge->shape, ridge_curve_y, sizeof ridge->shape);
        return;
    }
    ridge_motion_advance(&ridge->motion, dt_ms, slope_along_the_line());
    const uint32_t released_for = ridge->alive_ms - RELEASE_MS;
    const int gain = released_for >= AMBIENT_FADE_IN_MS ? 256 : (int)(released_for * 256 / AMBIENT_FADE_IN_MS);
    for (int x = 0; x < RIDGE_CURVE_POINTS; x++) {
        const int moved = ridge_motion_height(&ridge->motion, ridge_curve_y[x], ridge->smooth[x], x) - ridge_curve_y[x];
        ridge->shape[x] = (int16_t)(ridge_curve_y[x] + moved * gain / 256);
    }
}

static void
pluck_from_shaking(void) {
    if (ridge->shake < SHAKE_THRESHOLD) {
        return;
    }
    ridge->shake_seed = ridge->shake_seed * 1664525u + 1013904223u;
    const int x = (int)((ridge->shake_seed >> 8) % RIDGE_CURVE_POINTS);
    const int32_t up_or_down = (ridge->shake_seed & 0x80u) ? 1 : -1;
    spring_line_poke(&ridge->line, x, SHAKE_HALF_WIDTH, up_or_down * (SPRING_LINE_ONE / 128) * ridge->shake);
}

/* Eases the pose toward `target` and keeps it a unit vector. A blend of two
 * opposed poses lies on the line through both and renormalises straight back
 * to where it started, so it would never turn; it is pushed sideways first. */
static void
ease_pose(gfx_glow_pose_t target, uint32_t dt_ms) {
    const int32_t share = (int32_t)(dt_ms * 256 / (LEVEL_TAU_MS + dt_ms));
    const int64_t facing =
        ((int64_t)ridge->pose.down_x * target.down_x + (int64_t)ridge->pose.down_y * target.down_y) / GFX_GLOW_POSE_ONE;
    int32_t x = ridge->pose.down_x + (target.down_x - ridge->pose.down_x) * share / 256;
    int32_t y = ridge->pose.down_y + (target.down_y - ridge->pose.down_y) * share / 256;
    if (facing < -(GFX_GLOW_POSE_ONE - GFX_GLOW_POSE_ONE / 64)) {
        x += ridge->pose.down_y * share / 256;
        y -= ridge->pose.down_x * share / 256;
    }
    const int64_t length = (int64_t)gfx_glow_isqrt((uint32_t)(x * x + y * y));
    if (length == 0) {
        return;
    }
    ridge->pose.down_x = (int32_t)((int64_t)x * GFX_GLOW_POSE_ONE / length);
    ridge->pose.down_y = (int32_t)((int64_t)y * GFX_GLOW_POSE_ONE / length);
}

static bool
poses_within(gfx_glow_pose_t a, gfx_glow_pose_t b, int step) {
    return abs(a.down_x - b.down_x) < step && abs(a.down_y - b.down_y) < step;
}

/* Where the pose is heading: boot's pose until released, then level - the
 * level down settled on once it has been steady, so that a resting line is
 * not chasing sensor noise. */
static gfx_glow_pose_t
pose_target(uint32_t dt_ms) {
    if (ridge->alive_ms < RELEASE_MS) {
        return POSE_LANDSCAPE;
    }
    if (poses_within(ridge->level, ridge->steady_level, LEVEL_STEADY_STEP)) {
        ridge->steady_ms = ridge->steady_ms < LEVEL_STEADY_MS ? ridge->steady_ms + dt_ms : LEVEL_STEADY_MS;
    } else {
        ridge->steady_level = ridge->level;
        ridge->steady_ms = 0;
    }
    return ridge->steady_ms >= LEVEL_STEADY_MS ? ridge->steady_level : ridge->level;
}

static bool
pose_moved_enough_to_see(void) {
    return !poses_within(ridge->pose, ridge->pose_on_screen, POSE_REDRAW_STEP);
}

void
ui_ridge_step(const input_t* input, uint32_t dt_ms) {
    allocate_once();
    if (ridge == NULL) {
        return;
    }
    ridge->alive_ms += dt_ms;
    const gfx_glow_pose_t target = pose_target(dt_ms);
    ease_pose(target, dt_ms);
    const bool arrived = ridge->steady_ms >= LEVEL_STEADY_MS && poses_within(ridge->pose, target, POSE_REDRAW_STEP);
    if (arrived) {
        ridge->pose = target;
    }

    pluck_from_touch(input);
    pluck_from_shaking();

    shape_this_frame(dt_ms);
    spring_line_advance(&ridge->line, dt_ms);
    int lo, hi;
    spring_line_apply(&ridge->line, ridge->shape, ridge->heights, &lo, &hi);
    const bool line_moved = hi > lo;
    if (line_moved) {
        gfx_glow_field_prepare(&ridge->field, ridge->heights, &ridge->style);
    }
    const bool settles_now = arrived && !poses_within(ridge->pose, ridge->pose_on_screen, 1);
    /* A tail that fades is drawn until it is gone, or it would freeze where
     * the line stopped. */
    const bool moved = line_moved || settles_now || pose_moved_enough_to_see();
    if (moved) {
        ridge->fade_draws_left = gfx_glow_trail_draws(RIDGE_TRAIL);
    } else if (ridge->fade_draws_left > 0) {
        ridge->fade_draws_left--;
    }
    if (moved || ridge->fade_draws_left > 0) {
        draw_ridge();
    }
}

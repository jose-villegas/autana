/*
 * ridge_pose - the launcher ridge's pose: gravity turned into a unit
 * vector, eased and snapped level once it holds still, and where a screen
 * point lies along it.
 *
 * Pure: time, gravity and every threshold are passed in rather than read
 * from a tunable, so the maths runs the same off the device. Poses are Q14
 * unit vectors, gfx_glow_pose_t from gfx_glow.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "gfx/gfx_glow.h"

typedef struct {
    gfx_glow_pose_t pose;
    gfx_glow_pose_t level;
    gfx_glow_pose_t steady_level;
    uint32_t steady_ms;
} ridge_pose_t;

static inline bool
ridge_pose_within(gfx_glow_pose_t a, gfx_glow_pose_t b, int step) {
    return abs(a.down_x - b.down_x) < step && abs(a.down_y - b.down_y) < step;
}

/* The level a gravity reading points to: `gx`, `gy` normalised to a unit
 * vector, or `level` unchanged if the reading is weaker than `min_strength`
 * - too flat a device for "down" to mean anything. */
static inline gfx_glow_pose_t
ridge_pose_level_from_gravity(gfx_glow_pose_t level, int gx, int gy, int strength, int min_strength) {
    if (strength < min_strength) {
        return level;
    }
    const int64_t length = (int64_t)gfx_glow_isqrt((uint32_t)(gx * gx + gy * gy));
    if (length == 0) {
        return level;
    }
    return (gfx_glow_pose_t){
        .down_x = (int32_t)((int64_t)gx * GFX_GLOW_POSE_ONE / length),
        .down_y = (int32_t)((int64_t)gy * GFX_GLOW_POSE_ONE / length),
    };
}

/* Eases `rp->pose` toward `target` over `tau_ms` and keeps it a unit vector.
 * A blend of two opposed poses lies on the line through both and
 * renormalises straight back to where it started, so it would never turn;
 * it is pushed sideways first. */
static inline void
ridge_pose_ease(ridge_pose_t* rp, gfx_glow_pose_t target, uint32_t dt_ms, int tau_ms) {
    const int32_t share = (int32_t)(dt_ms * 256 / ((uint32_t)tau_ms + dt_ms));
    const int64_t facing =
        ((int64_t)rp->pose.down_x * target.down_x + (int64_t)rp->pose.down_y * target.down_y) / GFX_GLOW_POSE_ONE;
    int32_t x = rp->pose.down_x + (target.down_x - rp->pose.down_x) * share / 256;
    int32_t y = rp->pose.down_y + (target.down_y - rp->pose.down_y) * share / 256;
    if (facing < -(GFX_GLOW_POSE_ONE - GFX_GLOW_POSE_ONE / 64)) {
        x += rp->pose.down_y * share / 256;
        y -= rp->pose.down_x * share / 256;
    }
    const int64_t length = (int64_t)gfx_glow_isqrt((uint32_t)(x * x + y * y));
    if (length == 0) {
        return;
    }
    rp->pose.down_x = (int32_t)((int64_t)x * GFX_GLOW_POSE_ONE / length);
    rp->pose.down_y = (int32_t)((int64_t)y * GFX_GLOW_POSE_ONE / length);
}

/* Where the pose is heading: `boot_pose` until `alive_ms` reaches `hold_ms`,
 * then the level down settled on once it has held within `steady_step` for
 * `steady_hold_ms` - so a resting line is not chasing sensor noise. */
static inline gfx_glow_pose_t
ridge_pose_target(ridge_pose_t* rp, uint32_t dt_ms, uint32_t alive_ms, uint32_t hold_ms, gfx_glow_pose_t boot_pose,
                  int steady_step, uint32_t steady_hold_ms) {
    if (alive_ms < hold_ms) {
        return boot_pose;
    }
    if (ridge_pose_within(rp->level, rp->steady_level, steady_step)) {
        rp->steady_ms = rp->steady_ms < steady_hold_ms ? rp->steady_ms + dt_ms : steady_hold_ms;
    } else {
        rp->steady_level = rp->level;
        rp->steady_ms = 0;
    }
    return rp->steady_ms >= steady_hold_ms ? rp->steady_level : rp->level;
}

/* One frame of `ridge_pose_target` then `ridge_pose_ease`, snapped to the
 * target exactly once steady for `steady_hold_ms` and within `redraw_step`
 * of it - easing never quite arriving would otherwise leave the line a
 * fraction of a degree off level forever. Returns whether it snapped. */
static inline bool
ridge_pose_advance(ridge_pose_t* rp, uint32_t dt_ms, uint32_t alive_ms, uint32_t hold_ms, gfx_glow_pose_t boot_pose,
                   int tau_ms, int steady_step, uint32_t steady_hold_ms, int redraw_step) {
    const gfx_glow_pose_t target =
        ridge_pose_target(rp, dt_ms, alive_ms, hold_ms, boot_pose, steady_step, steady_hold_ms);
    ridge_pose_ease(rp, target, dt_ms, tau_ms);
    const bool arrived = rp->steady_ms >= steady_hold_ms && ridge_pose_within(rp->pose, target, redraw_step);
    if (arrived) {
        rp->pose = target;
    }
    return arrived;
}

/* How steeply the line runs downhill toward its last column, Q14: the part
 * of true down that lies along the line, which is nothing once it is level
 * and most while a turn is still being caught up with. */
static inline int32_t
ridge_pose_slope(const ridge_pose_t* rp) {
    const int64_t right_x = rp->pose.down_y;
    const int64_t right_y = -rp->pose.down_x;
    return (int32_t)((rp->level.down_x * right_x + rp->level.down_y * right_y) / GFX_GLOW_POSE_ONE);
}

/* Which column of a curve `columns` long lies under point (`panel_x`,
 * `panel_y`) of a `panel_w` x `panel_h` panel, at `pose`. */
static inline int
ridge_pose_column_under(gfx_glow_pose_t pose, int panel_w, int panel_h, int columns, int panel_x, int panel_y) {
    const int64_t right_x = pose.down_y;
    const int64_t right_y = -pose.down_x;
    const int64_t dx2 = 2 * (int64_t)panel_x - (panel_w - 1);
    const int64_t dy2 = 2 * (int64_t)panel_y - (panel_h - 1);
    return (int)((columns - 1 + (dx2 * right_x + dy2 * right_y) / GFX_GLOW_POSE_ONE) / 2);
}

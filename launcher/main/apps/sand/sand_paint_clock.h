/* sand_paint_clock: the time-driven phases the painter reads, advanced by elapsed milliseconds.
 *
 * Each clock runs on dt_ms rather than frame count, so the look holds at any
 * frame rate. The ones that report "moved" do so because a sparse repaint
 * has to redraw the rows that clock affects; a full repaint can ignore the
 * result. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "material_palette.h"
#include "sand_paint_row.h"
#include "util/scalar/mathx.h"

#define SAND_PAINT_SHINE_STEP_MS       40
#define SAND_PAINT_SHINE_STEP_PX       2
#define SAND_PAINT_FOAM_PHASE_MS       90
#define SAND_PAINT_CULLET_PHASE_MS     250
#define SAND_PAINT_GLASS_PHASE_SHIFT   7

/* A sweep that always travels the same way still reads as one shine, even
 * with gusts dropping out - real wind swings direction. Interval jittered
 * (material_grain_hash of the flip count, not a real RNG) so the swings
 * are not metronomic. */
#define SAND_PAINT_WIND_FLIP_BASE_MS   1200u
#define SAND_PAINT_WIND_FLIP_JITTER_MS 1800u

/* The leaf wave's own time runs continuously for a smooth blend, but its
 * redraw is throttled: dirtying every wood-near-leaf row every frame would
 * defeat dirty rows for a whole tree. Liquid depth is throttled the same way. */
#define SAND_PAINT_WOOD_LEAF_WAKE_MS   40
#define SAND_PAINT_LOCAL_DEPTH_WAKE_MS 120

typedef struct {
    uint32_t shine_ms;
    uint32_t foam_ms;
    uint32_t cullet_ms;
    uint32_t wood_leaf_wake_ms;
    uint32_t local_depth_wake_ms;
    uint32_t wind_flip_ms;
    uint32_t wind_flip_due_ms;
    unsigned wind_flip_count;
} sand_paint_clock_t;

#define SAND_PAINT_CLOCK_INIT {.wind_flip_due_ms = SAND_PAINT_WIND_FLIP_BASE_MS}

/* Whole periods of `period_ms` in `*elapsed_ms` after adding `dt_ms`; the remainder carries. */
static inline uint32_t
sand_paint_clock_periods(uint32_t* elapsed_ms, uint32_t dt_ms, uint32_t period_ms) {
    *elapsed_ms += dt_ms;
    const uint32_t periods = *elapsed_ms / period_ms;
    *elapsed_ms -= periods * period_ms;
    return periods;
}

static inline bool
sand_paint_clock_shine(sand_paint_clock_t* c, sand_paint_frame_t* pf, uint32_t dt_ms) {
    const uint32_t steps = sand_paint_clock_periods(&c->shine_ms, dt_ms, SAND_PAINT_SHINE_STEP_MS);
    if (steps == 0) {
        return false;
    }
    pf->shine_offset =
        (int)(((unsigned)pf->shine_offset + steps * SAND_PAINT_SHINE_STEP_PX) & (SAND_PAINT_SHINE_PERIOD - 1));
    return true;
}

/* Foam reads a phase that only ever grows, so it never reports a move: water rows repaint on their own. */
static inline void
sand_paint_clock_foam(sand_paint_clock_t* c, sand_paint_frame_t* pf, uint32_t dt_ms) {
    c->foam_ms += dt_ms;
    pf->material.foam_phase = c->foam_ms / SAND_PAINT_FOAM_PHASE_MS;
}

static inline bool
sand_paint_clock_cullet(sand_paint_clock_t* c, sand_paint_frame_t* pf, uint32_t dt_ms) {
    const uint32_t steps = sand_paint_clock_periods(&c->cullet_ms, dt_ms, SAND_PAINT_CULLET_PHASE_MS);
    pf->material.cullet_phase += steps;
    return steps != 0;
}

static inline bool
sand_paint_clock_wood_leaf(sand_paint_clock_t* c, sand_paint_frame_t* pf, uint32_t dt_ms) {
    pf->wood_leaf_time_ms += dt_ms;
    return sand_paint_clock_periods(&c->wood_leaf_wake_ms, dt_ms, SAND_PAINT_WOOD_LEAF_WAKE_MS) != 0;
}

static inline void
sand_paint_clock_wind(sand_paint_clock_t* c, sand_paint_frame_t* pf, uint32_t dt_ms) {
    c->wind_flip_ms += dt_ms;
    if (c->wind_flip_ms < c->wind_flip_due_ms) {
        return;
    }
    c->wind_flip_ms -= c->wind_flip_due_ms;
    pf->wood_leaf_wind_sign = -pf->wood_leaf_wind_sign;
    c->wind_flip_count++;
    c->wind_flip_due_ms =
        SAND_PAINT_WIND_FLIP_BASE_MS + material_grain_hash((int)c->wind_flip_count, 0) % SAND_PAINT_WIND_FLIP_JITTER_MS;
}

static inline bool
sand_paint_clock_local_depth(sand_paint_clock_t* c, uint32_t dt_ms) {
    return sand_paint_clock_periods(&c->local_depth_wake_ms, dt_ms, SAND_PAINT_LOCAL_DEPTH_WAKE_MS) != 0;
}

/* A trig-free bearing in Q16 quarter-turns: an L1 pseudo-angle, monotonic
 * around the circle, which is all glass's gradient needs. Flat or free fall
 * has no bearing and reads 0. */
static inline int
sand_paint_gravity_bearing_q16(int gx, int gy) {
    const int64_t ax = gx < 0 ? -(int64_t)gx : (int64_t)gx;
    const int64_t ay = gy < 0 ? -(int64_t)gy : (int64_t)gy;
    const int64_t denom = ax + ay;
    if (denom == 0) {
        return 0;
    }
    const int64_t p_q16 = (int64_t)gx * MATHX_ONE / denom; /* a multiply: gx may be negative */
    return (int)(gy < 0 ? (p_q16 - MATHX_ONE) : (MATHX_ONE - p_q16));
}

/* Not a clock: glass reads gravity's bearing, so a steady tilt leaves it fixed. */
static inline bool
sand_paint_clock_glass(sand_paint_frame_t* pf, int gx, int gy) {
    const int phase = sand_paint_gravity_bearing_q16(gx, gy) >> SAND_PAINT_GLASS_PHASE_SHIFT;
    const bool changed = phase != pf->material.glass_phase;
    pf->material.glass_phase = phase;
    return changed;
}

/*
 * spring_line - a row of points on springs: each is pulled toward rest and
 * toward its neighbours, so a push travels along the row as a wave and dies
 * away. One offset per column, which is what displaces a gfx_glow.h curve.
 *
 * Built to go QUIET. At rest nothing is simulated and nothing is reported as
 * changed, so a screen that owns one costs no draw and no bus time until it
 * is touched. Integer physics does not come to rest by itself - it rounds
 * its way into a small orbit forever - so the line is put to rest once all
 * of it is slow and close enough. All of it, not point by point: zeroing one
 * point beside moving neighbours is a kick at the grid's own wavelength, and
 * near the tension limit those kicks outran the damping and never stopped.
 *
 * Offsets and velocities are Q16 pixels. Q8 in an int16_t would halve the
 * memory and was rejected: damping is a fraction of the velocity, and below
 * a third of a pixel per tick that fraction rounds to nothing, so slow waves
 * would never fade.
 *
 * The caller owns the two arrays; this file allocates nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SPRING_LINE_ONE           (1 << 16)

/* One tick of simulated time. A wave crosses at most one column per tick,
 * which is what bounds both how fast a ripple can look and how far the
 * active range can grow. */
#define SPRING_LINE_TICK_MS       4

/* A long frame runs at most this many ticks and drops the rest: a stall
 * slows the wave down for a moment rather than costing a burst of work. */
#define SPRING_LINE_MAX_TICKS     8

/* Put to rest below an eighth of a pixel and 4 px/s. */
#define SPRING_LINE_REST_OFFSET   (SPRING_LINE_ONE / 8)
#define SPRING_LINE_REST_VELOCITY (SPRING_LINE_ONE / 64)

#define SPRING_LINE_MAX_OFFSET    (120 * SPRING_LINE_ONE)

typedef struct {
    int32_t* offset;
    int32_t* velocity;
    int count;

    /* Columns that may be moving, half open. Empty at rest. */
    int active_lo;
    int active_hi;

    /* Out of 256. Tension is the wave speed squared in columns per tick and
     * must stay under 256. Stiffness pulls toward rest, and at 0 the line
     * has no rest pose to return to. Damping is the share of velocity lost
     * per tick. */
    int tension;
    int stiffness;
    int damping;

    uint32_t carried_ms;
} spring_line_t;

static inline void
spring_line_init(spring_line_t* line, int32_t* offset, int32_t* velocity, int count, int tension, int stiffness,
                 int damping) {
    *line = (spring_line_t){
        .offset = offset,
        .velocity = velocity,
        .count = count,
        .tension = tension,
        .stiffness = stiffness,
        .damping = damping,
    };
    for (int x = 0; x < count; x++) {
        offset[x] = 0;
        velocity[x] = 0;
    }
}

static inline bool
spring_line_at_rest(const spring_line_t* line) {
    return line->active_hi <= line->active_lo;
}

static inline int32_t
spring_line_scale(int32_t value, int out_of_256) {
    return (int32_t)(((int64_t)value * out_of_256) / 256);
}

static inline void
spring_line_wake(spring_line_t* line, int lo, int hi) {
    lo = lo < 0 ? 0 : lo;
    hi = hi > line->count ? line->count : hi;
    if (hi <= lo) {
        return;
    }
    if (spring_line_at_rest(line)) {
        line->active_lo = lo;
        line->active_hi = hi;
        return;
    }
    line->active_lo = lo < line->active_lo ? lo : line->active_lo;
    line->active_hi = hi > line->active_hi ? hi : line->active_hi;
}

/* Adds `amount` at column `x`, falling smoothly to nothing `half_width`
 * columns away as (1 - t^2)^2. A point push would ring at the grid's own
 * wavelength; a smooth one makes a wave. */
static inline void
spring_line_add_bump(spring_line_t* line, int32_t* into, int x, int half_width, int32_t amount) {
    if (half_width < 1) {
        half_width = 1;
    }
    for (int k = -half_width + 1; k < half_width; k++) {
        const int column = x + k;
        if (column < 0 || column >= line->count) {
            continue;
        }
        const int t2 = k * k * 256 / (half_width * half_width);
        const int window = (256 - t2) * (256 - t2) / 256;
        into[column] += spring_line_scale(amount, window);
    }
    spring_line_wake(line, x - half_width, x + half_width + 1);
}

/* A flick: velocity, Q16 pixels per tick. Negative is up the screen. */
static inline void
spring_line_poke(spring_line_t* line, int x, int half_width, int32_t velocity) {
    spring_line_add_bump(line, line->velocity, x, half_width, velocity);
}

/* A shove: the line is moved, Q16 pixels, and let go. */
static inline void
spring_line_nudge(spring_line_t* line, int x, int half_width, int32_t offset) {
    spring_line_add_bump(line, line->offset, x, half_width, offset);
}

static inline int32_t
spring_line_clamp(int32_t value, int32_t limit) {
    return value < -limit ? -limit : (value > limit ? limit : value);
}

static inline bool
spring_line_quiet(const spring_line_t* line, int x) {
    const int32_t away = line->offset[x] < 0 ? -line->offset[x] : line->offset[x];
    const int32_t speed = line->velocity[x] < 0 ? -line->velocity[x] : line->velocity[x];
    return away < SPRING_LINE_REST_OFFSET && speed < SPRING_LINE_REST_VELOCITY;
}

/* Puts to rest the quiet columns at each END of the active range, which is
 * all of them once the whole line is quiet. A quiet column between moving
 * ones is left alone - see the top of this file. */
static inline void
spring_line_trim(spring_line_t* line) {
    while (line->active_lo < line->active_hi && spring_line_quiet(line, line->active_lo)) {
        line->offset[line->active_lo] = 0;
        line->velocity[line->active_lo] = 0;
        line->active_lo++;
    }
    while (line->active_hi > line->active_lo && spring_line_quiet(line, line->active_hi - 1)) {
        line->offset[line->active_hi - 1] = 0;
        line->velocity[line->active_hi - 1] = 0;
        line->active_hi--;
    }
}

/* One tick. Velocities first, from offsets that are all still last tick's,
 * then offsets from the new velocities - which needs no scratch copy and is
 * the order that keeps the energy from creeping up. The ends are free: the
 * missing neighbour mirrors the point itself. */
static inline void
spring_line_tick(spring_line_t* line) {
    if (spring_line_at_rest(line)) {
        return;
    }
    spring_line_wake(line, line->active_lo - 1, line->active_hi + 1);
    const int lo = line->active_lo;
    const int hi = line->active_hi;
    const int last = line->count - 1;

    for (int x = lo; x < hi; x++) {
        const int32_t here = line->offset[x];
        const int32_t left = line->offset[x > 0 ? x - 1 : x];
        const int32_t right = line->offset[x < last ? x + 1 : x];
        int32_t v = line->velocity[x];
        v += spring_line_scale(left - 2 * here + right, line->tension);
        v -= spring_line_scale(here, line->stiffness);
        v -= spring_line_scale(v, line->damping);
        line->velocity[x] = v;
    }

    for (int x = lo; x < hi; x++) {
        line->offset[x] = spring_line_clamp(line->offset[x] + line->velocity[x], SPRING_LINE_MAX_OFFSET);
    }
    spring_line_trim(line);
}

/* Runs the ticks `dt_ms` is worth, carrying the remainder. Returns how many
 * ran. */
static inline int
spring_line_advance(spring_line_t* line, uint32_t dt_ms) {
    if (spring_line_at_rest(line)) {
        line->carried_ms = 0;
        return 0;
    }
    line->carried_ms += dt_ms;
    int ticks = (int)(line->carried_ms / SPRING_LINE_TICK_MS);
    line->carried_ms %= SPRING_LINE_TICK_MS;
    if (ticks > SPRING_LINE_MAX_TICKS) {
        ticks = SPRING_LINE_MAX_TICKS;
    }
    for (int i = 0; i < ticks; i++) {
        spring_line_tick(line);
    }
    return ticks;
}

/* Writes rest + offset into `out`, both Q4, and reports the columns whose
 * value changed, half open and empty when none did - measured against what
 * `out` holds, so it has to be the array last drawn. Returns the furthest
 * any column moved, in whole pixels rounded up. */
static inline int
spring_line_apply(const spring_line_t* line, const int16_t* rest_q4, int16_t* out_q4, int* changed_lo,
                  int* changed_hi) {
    int furthest_q4 = 0;
    *changed_lo = line->count;
    *changed_hi = 0;
    for (int x = 0; x < line->count; x++) {
        const int32_t rounded = (line->offset[x] + (line->offset[x] < 0 ? -2048 : 2048)) / 4096;
        const int16_t height = (int16_t)(rest_q4[x] + rounded);
        if (height == out_q4[x]) {
            continue;
        }
        const int moved = height < out_q4[x] ? out_q4[x] - height : height - out_q4[x];
        furthest_q4 = moved > furthest_q4 ? moved : furthest_q4;
        out_q4[x] = height;
        *changed_lo = x < *changed_lo ? x : *changed_lo;
        *changed_hi = x + 1;
    }
    if (*changed_hi <= *changed_lo) {
        *changed_lo = 0;
        *changed_hi = 0;
    }
    return (furthest_q4 + 15) / 16;
}

/* Android WindowOrientationListener's gates use our down-vector sign convention. */
#include "display/display.h"
#include "display/display_shell.h"

#include <assert.h>
#include <math.h>

#include "display/viewport.h"
#include "math/scalar/mathi.h"

const char*
display_quarter_name(int quarter) {
    static const char* const names[DISPLAY_QUARTER_COUNT] = {
        [DISPLAY_PORTRAIT] = DISPLAY_PORTRAIT_NAME,
        [DISPLAY_LANDSCAPE] = DISPLAY_LANDSCAPE_NAME,
        [DISPLAY_PORTRAIT_UPSIDE_DOWN] = DISPLAY_PORTRAIT_UPSIDE_DOWN_NAME,
        [DISPLAY_LANDSCAPE_UPSIDE_DOWN] = DISPLAY_LANDSCAPE_UPSIDE_DOWN_NAME,
    };
    return quarter >= 0 && quarter < DISPLAY_QUARTER_COUNT ? names[quarter] : "";
}

int
display_panel_corner_inset(int radius, int canvas_height, int row) {
    if (radius <= 0 || row < 0 || row >= canvas_height) {
        return 0;
    }
    const int from_nearest_edge = row < canvas_height - 1 - row ? row : canvas_height - 1 - row;
    if (from_nearest_edge >= radius) {
        return 0;
    }
    const int up = radius - from_nearest_edge;
    return radius - mathi_half_chord_round(radius, up);
}

void
display_init(display_t* d) {
    *d = (display_t){.predicted = DISPLAY_QUARTER_NONE,
                     .flat_since_us = DISPLAY_TIME_NONE,
                     .flat_ended_us = DISPLAY_TIME_NONE,
                     .swing_ended_us = DISPLAY_TIME_NONE,
                     .accel_ended_us = DISPLAY_TIME_NONE,
                     .touch_ended_us = DISPLAY_TIME_NONE};
}

static void
restart(display_t* d) {
    const int quarter = d->quarter;
    const int64_t touch_us = d->touch_ended_us;
    display_init(d);
    d->quarter = quarter;
    d->touch_ended_us = touch_us;
}

void
display_touch(display_t* d, int64_t now_us) {
    d->touch_ended_us = now_us;
}

static bool
elapsed(int64_t now_us, int64_t event_us, int duration_ms) {
    return event_us == DISPLAY_TIME_NONE
           || (now_us >= event_us && now_us - event_us >= (int64_t)duration_ms * MATH_US_PER_MS);
}

static void
track_swing(display_t* d, float sine, float cosine, int64_t now_us) {
    for (int i = 0; i < d->tilt_count; i++) {
        if (now_us - d->tilts[i].time_us > (int64_t)DISPLAY_SWING_MS * MATH_US_PER_MS) {
            continue;
        }
        const float rise_sin = sine * d->tilts[i].cosine - cosine * d->tilts[i].sine;
        const float rise_cos = cosine * d->tilts[i].cosine + sine * d->tilts[i].sine;
        if (sine > d->tilts[i].sine && (rise_cos < 0.0F || rise_sin >= DISPLAY_SWING_SIN)) {
            d->swing_ended_us = now_us;
            break;
        }
    }
    d->tilts[d->tilt_next].sine = sine;
    d->tilts[d->tilt_next].cosine = cosine;
    d->tilts[d->tilt_next].time_us = now_us;
    d->tilt_next = (d->tilt_next + 1) % DISPLAY_TILT_HISTORY_COUNT;
    if (d->tilt_count < DISPLAY_TILT_HISTORY_COUNT) {
        d->tilt_count++;
    }
}

static void
track_tilt(display_t* d, float sine, float cosine, int64_t now_us) {
    const bool was_flat = d->flat;
    if (sine >= DISPLAY_FLAT_SIN) {
        if (d->flat_since_us == DISPLAY_TIME_NONE) {
            d->flat_since_us = now_us;
        }
        d->flat = elapsed(now_us, d->flat_since_us, DISPLAY_FLAT_MS);
    } else {
        d->flat_since_us = DISPLAY_TIME_NONE;
        d->flat = false;
    }
    if (d->flat || was_flat) {
        d->flat_ended_us = now_us;
    }
    track_swing(d, sine, cosine, now_us);
    if (sine <= DISPLAY_OVERHEAD_ENTER_SIN) {
        d->overhead = true;
    } else if (sine >= DISPLAY_OVERHEAD_EXIT_SIN) {
        d->overhead = false;
    }
}

static bool
tilt_accepted(int quarter, float sine) {
    const int relative = (quarter - DISPLAY_DEFAULT_QUARTER + DISPLAY_QUARTER_COUNT) % DISPLAY_QUARTER_COUNT;
    const float max_sin = relative == 0                           ? DISPLAY_TILT_DEFAULT_MAX_SIN
                          : relative == DISPLAY_QUARTER_COUNT / 2 ? DISPLAY_TILT_OPPOSITE_MAX_SIN
                                                                  : DISPLAY_TILT_ADJACENT_MAX_SIN;
    return fabsf(sine) <= DISPLAY_MAX_TILT_SIN && sine >= DISPLAY_TILT_MIN_SIN && sine <= max_sin;
}

int
display_candidate(vec3f_t v, int current, bool tilt_limits) {
    const float magnitude = sqrtf(vec3f_dot(v, v));
    if (magnitude < DISPLAY_NEAR_ZERO_G) {
        return DISPLAY_QUARTER_NONE;
    }
    const int quarter = fabsf(v.x) > fabsf(v.y) ? (v.x > 0.0F ? DISPLAY_LANDSCAPE_UPSIDE_DOWN : DISPLAY_LANDSCAPE)
                                                : (v.y > 0.0F ? DISPLAY_PORTRAIT : DISPLAY_PORTRAIT_UPSIDE_DOWN);
    if (tilt_limits && !tilt_accepted(quarter, -v.z / magnitude)) {
        return DISPLAY_QUARTER_NONE;
    }
    const viewport_quarter_axes_t axes = viewport_quarter_axes(current);
    const float aligned = v.x * axes.x_down + v.y * axes.y_down;
    const float plane = sqrtf(v.x * v.x + v.y * v.y);
    if (quarter != current && aligned >= plane * DISPLAY_ADJACENT_MIN_COS) {
        return DISPLAY_QUARTER_NONE;
    }
    return quarter;
}

static void
filter_sample(display_t* d, vec3f_t accel_g, int64_t now_us) {
    const bool zero = vec3f_dot(accel_g, accel_g) == 0.0F;
    const bool needs_restart = !d->primed || now_us < d->sample_us
                               || now_us - d->sample_us > (int64_t)DISPLAY_FILTER_RESET_MS * MATH_US_PER_MS || zero;
    if (needs_restart) {
        restart(d);
        if (d->touch_ended_us > now_us) {
            d->touch_ended_us = now_us;
        }
        d->filtered = accel_g;
        d->primed = !zero;
    } else {
        const float dt_us = (float)(now_us - d->sample_us);
        const float alpha = dt_us / ((float)DISPLAY_FILTER_TAU_MS * MATH_US_PER_MS + dt_us);
        d->filtered = vec3f_lerp(d->filtered, accel_g, alpha);
    }
    d->sample_us = now_us;
}

bool
display_update(display_t* d, vec3f_t accel_g, int64_t now_us) {
    filter_sample(d, accel_g, now_us);
    const float magnitude = sqrtf(vec3f_dot(d->filtered, d->filtered));
    int predicted = DISPLAY_QUARTER_NONE;
    if (magnitude >= DISPLAY_NEAR_ZERO_G) {
        if (fabsf(magnitude - 1.0F) > DISPLAY_ACCEL_TOLERANCE_G) {
            d->accel_ended_us = now_us;
        }
        /* Down points into a face-up screen, opposite Android's accelerometer z. */
        const float sine = -d->filtered.z / magnitude;
        const float cosine = sqrtf(fmaxf(0.0F, 1.0F - sine * sine));
        track_tilt(d, sine, cosine, now_us);
        if (!d->overhead) {
            predicted = display_candidate(d->filtered, d->quarter, true);
        }
    } else {
        if (d->flat) {
            d->flat_ended_us = now_us;
        }
        d->flat = false;
        d->flat_since_us = DISPLAY_TIME_NONE;
    }
    if (predicted != d->predicted) {
        d->predicted = predicted;
        d->predicted_us = now_us;
    }
    if (predicted == DISPLAY_QUARTER_NONE || predicted == d->quarter || d->flat
        || !elapsed(now_us, d->predicted_us, DISPLAY_SETTLE_MS)
        || !elapsed(now_us, d->flat_ended_us, DISPLAY_AFTER_FLAT_MS)
        || !elapsed(now_us, d->accel_ended_us, DISPLAY_AFTER_ACCEL_MS)
        || !elapsed(now_us, d->swing_ended_us, DISPLAY_AFTER_SWING_MS)
        || !elapsed(now_us, d->touch_ended_us, DISPLAY_AFTER_TOUCH_MS)) {
        return false;
    }
    d->quarter = predicted;
    return true;
}

int
display_quarter(const display_t* d) {
    return d->quarter;
}

void
display_orientation_init(display_orientation_t* o) {
    display_init(&o->display);
    o->display.quarter = DISPLAY_DEFAULT_QUARTER;
    o->next_sample_us = 0;
    o->held = false;
    o->released = false;
}

bool
display_orientation_sample(display_orientation_t* o, int64_t now_us, display_acceleration_reader_t read) {
    if (o->held || now_us < o->next_sample_us) {
        return false;
    }
    o->next_sample_us = now_us + ((int64_t)DISPLAY_SAMPLE_MS * MATH_US_PER_MS);

    vec3f_t acceleration;
    if (!read(&acceleration)) {
        return false;
    }
    const bool changed = display_update(&o->display, acceleration, now_us) || o->released;
    o->released = false;
    return changed;
}

void
display_orientation_hold(display_orientation_t* o, int quarter) {
    assert(quarter >= DISPLAY_PORTRAIT && quarter <= DISPLAY_LANDSCAPE_UPSIDE_DOWN);
    o->display.quarter = quarter;
    o->held = true;
}

void
display_orientation_release(display_orientation_t* o) {
    o->held = false;
    restart(&o->display);
    o->released = true;
    o->next_sample_us = 0;
}

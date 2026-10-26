/* display: quarter decisions, event gates and shell sampling in all rotations. */
#include "display/display_judge.h"
#include "display/display_shell.h"

#include <math.h>
#include <stdlib.h>

#include "display/viewport.h"
#include "math/scalar/math_const.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#define TEST_ANGLE_MARGIN_DEG 1.0F
#define TEST_HYST_BELOW_DEG   (DISPLAY_ADJACENT_MIN_DEG - DISPLAY_HYSTERESIS_GAP_DEG / 6.0F)
#define TEST_HYST_ABOVE_DEG   (DISPLAY_ADJACENT_MIN_DEG + DISPLAY_HYSTERESIS_GAP_DEG / 18.0F)
#define TEST_STEP_US          ((int64_t)DISPLAY_SAMPLE_MS * MATH_US_PER_MS)
#define TEST_STEADY_MS        (DISPLAY_COMMIT_MS * 2)

static vec3f_t
sample(int quarter, float turn_deg, float tilt_deg, float magnitude) {
    const float radians_per_degree = MATH_RADIANS_PER_DEGREE;
    const float turn = turn_deg * radians_per_degree;
    const float tilt = tilt_deg * radians_per_degree;
    const viewport_quarter_axes_t axes = viewport_quarter_axes(quarter);
    const float x = (float)axes.x_down;
    const float y = (float)axes.y_down;
    return (vec3f_t){(x * cosf(turn) - y * sinf(turn)) * cosf(tilt) * magnitude,
                     (y * cosf(turn) + x * sinf(turn)) * cosf(tilt) * magnitude, -sinf(tilt) * magnitude};
}

static display_t
fixture(int quarter) {
    display_t d;
    display_init(&d);
    d.quarter = quarter;
    return d;
}

static int
feed(display_t* d, vec3f_t a, int64_t* now_us, int duration_ms) {
    const int64_t end = *now_us + (int64_t)duration_ms * MATH_US_PER_MS;
    int changes = 0;
    while (*now_us <= end) {
        changes += display_update(d, a, *now_us);
        *now_us += TEST_STEP_US;
    }
    return changes;
}

static void
test_clean_turn_settles_in_every_rotation(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        const int target = (q + 1) % DISPLAY_QUARTER_COUNT;
        const vec3f_t a = sample(target, 0, 0, 1);
        TEST_ASSERT_FALSE(display_update(&d, a, 0));
        TEST_ASSERT_FALSE(display_update(&d, a, (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS - 1));
        TEST_ASSERT_TRUE(display_update(&d, a, (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS));
        TEST_ASSERT_EQUAL_INT(target, d.quarter);
        TEST_ASSERT_FALSE(display_update(&d, a, (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS));
    }
}

static void
test_filtered_turn_and_hysteresis_in_every_rotation(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        int64_t now = 0;
        feed(&d, sample(q, 0, 0, 1), &now, TEST_STEADY_MS);
        TEST_ASSERT_EQUAL_INT(0, feed(&d, sample(q, TEST_HYST_BELOW_DEG, 0, 1), &now, TEST_STEADY_MS));
        TEST_ASSERT_EQUAL_INT(q, d.quarter);
        TEST_ASSERT_EQUAL_INT(1, feed(&d, sample(q, TEST_HYST_ABOVE_DEG, 0, 1), &now, TEST_STEADY_MS));
        TEST_ASSERT_EQUAL_INT((q + 1) % DISPLAY_QUARTER_COUNT, d.quarter);
    }
}

static void
test_tilt_limits_are_relative_to_default(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        const int relative = (q - DISPLAY_DEFAULT_QUARTER + DISPLAY_QUARTER_COUNT) % DISPLAY_QUARTER_COUNT;
        const float limit = relative == 0                           ? DISPLAY_TILT_DEFAULT_MAX_DEG
                            : relative == DISPLAY_QUARTER_COUNT / 2 ? DISPLAY_TILT_OPPOSITE_MAX_DEG
                                                                    : DISPLAY_TILT_ADJACENT_MAX_DEG;
        display_t d = fixture((q + 1) % DISPLAY_QUARTER_COUNT);
        int64_t now = 0;
        TEST_ASSERT_EQUAL_INT(0, feed(&d, sample(q, 0, limit + TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS));
        TEST_ASSERT_EQUAL_INT(1, feed(&d, sample(q, 0, limit - TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS));
        TEST_ASSERT_EQUAL_INT(q, d.quarter);
        d = fixture((q + 1) % DISPLAY_QUARTER_COUNT);
        now = 0;
        TEST_ASSERT_EQUAL_INT(
            0, feed(&d, sample(q, 0, DISPLAY_TILT_MIN_DEG - TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS));
        TEST_ASSERT_EQUAL_INT(
            1, feed(&d, sample(q, 0, DISPLAY_TILT_MIN_DEG + TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS));
    }
}

static void
test_flat_and_face_up_offset_never_switch(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        int64_t now = 0;
        const int target = (q + 1) % DISPLAY_QUARTER_COUNT;
        TEST_ASSERT_EQUAL_INT(
            0, feed(&d, sample(target, 0, DISPLAY_FLAT_DEG + TEST_ANGLE_MARGIN_DEG, 1), &now, DISPLAY_FLAT_MS));
        TEST_ASSERT_TRUE(d.flat);
        const vec3f_t offset = sample(target, 0, 0, DISPLAY_NEAR_ZERO_G);
        TEST_ASSERT_EQUAL_INT(0, feed(&d, (vec3f_t){offset.x, offset.y, -1.0F}, &now, TEST_STEADY_MS));
        TEST_ASSERT_EQUAL_INT(q, d.quarter);
    }
}

static void
test_flat_cooldown_starts_when_flat_ends(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        int64_t now = 0;
        feed(&d, sample(q, 0, DISPLAY_FLAT_DEG + TEST_ANGLE_MARGIN_DEG, 1), &now, DISPLAY_FLAT_MS);
        const vec3f_t a = sample((q + 1) % DISPLAY_QUARTER_COUNT, 0, 0, 1);
        d.filtered = a;
        TEST_ASSERT_FALSE(display_update(&d, a, now));
        TEST_ASSERT_EQUAL_INT64(now, d.flat_ended_us);
        const int64_t ended = now;
        now += TEST_STEP_US;
        TEST_ASSERT_EQUAL_INT(0, feed(&d, a, &now, DISPLAY_AFTER_FLAT_MS - 2 * DISPLAY_SAMPLE_MS));
        TEST_ASSERT_TRUE(display_update(&d, a, ended + (int64_t)DISPLAY_AFTER_FLAT_MS * MATH_US_PER_MS));
    }
}

static void
test_face_down_has_no_flat_dwell_or_cooldown(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        int64_t now = 0;
        const int target = (q + 1) % DISPLAY_QUARTER_COUNT;
        const vec3f_t down = sample(target, 0, -DISPLAY_FLAT_DEG - TEST_ANGLE_MARGIN_DEG, 1);
        TEST_ASSERT_EQUAL_INT(0, feed(&d, down, &now, DISPLAY_FLAT_MS));
        TEST_ASSERT_FALSE(d.flat);
        TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, d.predicted);
        TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.flat_since_us);
        TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.flat_ended_us);
        const vec3f_t upright = sample(target, 0, 0, 1);
        d.filtered = upright;
        display_update(&d, upright, now);
        TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.flat_ended_us);
    }
}

static void
test_near_zero_interrupts_continuous_flat_dwell(void) {
    display_t d = fixture(DISPLAY_PORTRAIT);
    int64_t now = 0;
    const vec3f_t flat = sample(DISPLAY_PORTRAIT, 0, DISPLAY_FLAT_DEG + TEST_ANGLE_MARGIN_DEG, 1);
    feed(&d, flat, &now, DISPLAY_FLAT_MS / 2);
    const vec3f_t tiny = vec3f_scale(flat, DISPLAY_NEAR_ZERO_G / 2.0F);
    d.filtered = tiny;
    feed(&d, tiny, &now, DISPLAY_FLAT_MS / 2);
    d.filtered = flat;
    display_update(&d, flat, now);
    TEST_ASSERT_FALSE(d.flat);
}

static void
test_overhead_hysteresis_in_every_rotation(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        int64_t now = 0;
        const int target = (q + 1) % DISPLAY_QUARTER_COUNT;
        feed(&d, sample(target, 0, DISPLAY_OVERHEAD_ENTER_DEG - TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS);
        TEST_ASSERT_TRUE(d.overhead);
        feed(&d, sample(target, 0, DISPLAY_OVERHEAD_EXIT_DEG - TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS);
        TEST_ASSERT_TRUE(d.overhead);
        TEST_ASSERT_EQUAL_INT(q, d.quarter);
        TEST_ASSERT_EQUAL_INT(
            1, feed(&d, sample(target, 0, DISPLAY_OVERHEAD_EXIT_DEG + TEST_ANGLE_MARGIN_DEG, 1), &now, TEST_STEADY_MS));
        TEST_ASSERT_FALSE(d.overhead);
    }
}

static void
test_acceleration_cooldown_in_every_rotation(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            display_t d = fixture(q);
            int64_t now = 0;
            const vec3f_t a = sample((q + 1) % DISPLAY_QUARTER_COUNT, 0, 0, 1);
            const float magnitude = 1.0F + (float)sign * (DISPLAY_ACCEL_TOLERANCE_G + DISPLAY_NEAR_ZERO_G);
            TEST_ASSERT_EQUAL_INT(0, feed(&d, vec3f_scale(a, magnitude), &now, DISPLAY_AFTER_ACCEL_MS));
            const int64_t last_event = d.accel_ended_us;
            d.filtered = a;
            TEST_ASSERT_EQUAL_INT(0, feed(&d, a, &now, DISPLAY_AFTER_ACCEL_MS - 2 * DISPLAY_SAMPLE_MS));
            TEST_ASSERT_TRUE(display_update(&d, a, last_event + (int64_t)DISPLAY_AFTER_ACCEL_MS * MATH_US_PER_MS));
        }
    }
}

static void
test_swing_cooldown_in_every_rotation(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        const vec3f_t initial = sample(q, 0, 0, 1);
        display_update(&d, initial, 0);
        const vec3f_t a = sample((q + 1) % DISPLAY_QUARTER_COUNT, 0, DISPLAY_SWING_DEG + TEST_ANGLE_MARGIN_DEG, 1);
        d.filtered = a;
        TEST_ASSERT_FALSE(display_update(&d, a, TEST_STEP_US));
        TEST_ASSERT_EQUAL_INT64(TEST_STEP_US, d.swing_ended_us);
        int64_t now = TEST_STEP_US * 2;
        TEST_ASSERT_EQUAL_INT(0, feed(&d, a, &now, DISPLAY_SWING_MS));
        const int64_t last_event = d.swing_ended_us;
        now = last_event + ((int64_t)DISPLAY_AFTER_SWING_MS * MATH_US_PER_MS) - TEST_STEP_US;
        TEST_ASSERT_FALSE(display_update(&d, a, now));
        TEST_ASSERT_TRUE(display_update(&d, a, now + TEST_STEP_US));
    }
}

static void
test_touch_blocks_then_waits_after_last_down_frame(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_t d = fixture(q);
        const vec3f_t a = sample((q + 1) % DISPLAY_QUARTER_COUNT, 0, 0, 1);
        int64_t now = 0;
        for (; now <= (int64_t)DISPLAY_AFTER_TOUCH_MS * MATH_US_PER_MS; now += TEST_STEP_US) {
            display_touch(&d, now);
            TEST_ASSERT_FALSE(display_update(&d, a, now));
        }
        const int64_t ended = d.touch_ended_us;
        TEST_ASSERT_EQUAL_INT(0, feed(&d, a, &now, DISPLAY_AFTER_TOUCH_MS - 2 * DISPLAY_SAMPLE_MS));
        TEST_ASSERT_TRUE(display_update(&d, a, ended + (int64_t)DISPLAY_AFTER_TOUCH_MS * MATH_US_PER_MS));
    }
}

static void
test_gap_backwards_and_zero_restart_prediction(void) {
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        for (int reset = 0; reset < 3; reset++) {
            display_t d = fixture(q);
            const vec3f_t a = sample((q + 1) % DISPLAY_QUARTER_COUNT, 0, 0, 1);
            const int64_t base = (int64_t)DISPLAY_FILTER_RESET_MS * MATH_US_PER_MS;
            display_update(&d, a, base);
            const int64_t now = reset == 0 ? base * 2 + 1 : base - TEST_STEP_US;
            if (reset == 2) {
                display_update(&d, (vec3f_t){0}, base + TEST_STEP_US);
            }
            TEST_ASSERT_FALSE(display_update(&d, a, now));
            TEST_ASSERT_EQUAL_INT(q, d.quarter);
            TEST_ASSERT_TRUE(display_update(&d, a, now + (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS));
        }
    }
}

typedef struct {
    int reads;
    bool read_ok;
    vec3f_t acceleration;
} scripted_t;

static scripted_t* scripted;

static bool
scripted_acceleration(vec3f_t* out) {
    scripted->reads++;
    *out = scripted->acceleration;
    return scripted->read_ok;
}

static void
cleanup(void) {
    free(scripted);
    scripted = NULL;
}

static void
test_sampler_cadence_failures_and_hold_release(void) {
    scripted = calloc(1, sizeof *scripted);
    TEST_ASSERT_NOT_NULL(scripted);
    suite_set_test_cleanup(cleanup);
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        display_orientation_t o;
        display_orientation_init(&o);
        TEST_ASSERT_EQUAL_INT(DISPLAY_DEFAULT_QUARTER, o.display.quarter);
        scripted->reads = 0;
        scripted->read_ok = false;
        TEST_ASSERT_FALSE(display_orientation_sample(&o, 0, scripted_acceleration));
        display_orientation_sample(&o, TEST_STEP_US - 1, scripted_acceleration);
        TEST_ASSERT_EQUAL_INT(1, scripted->reads);
        display_orientation_sample(&o, TEST_STEP_US, scripted_acceleration);
        TEST_ASSERT_EQUAL_INT(2, scripted->reads);
        display_orientation_hold(&o, q);
        TEST_ASSERT_FALSE(display_orientation_sample(&o, TEST_STEP_US * 2, scripted_acceleration));
        TEST_ASSERT_EQUAL_INT(2, scripted->reads);
        scripted->read_ok = true;
        scripted->acceleration = sample((q + 1) % DISPLAY_QUARTER_COUNT, 0, 0, 1);
        display_orientation_release(&o);
        TEST_ASSERT_TRUE(display_orientation_sample(&o, TEST_STEP_US + 1, scripted_acceleration));
        TEST_ASSERT_EQUAL_INT(q, o.display.quarter);
        TEST_ASSERT_TRUE(display_orientation_sample(&o, TEST_STEP_US + 1 + (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS,
                                                    scripted_acceleration));
        TEST_ASSERT_EQUAL_INT((q + 1) % DISPLAY_QUARTER_COUNT, o.display.quarter);
    }
}

static void
test_angle_companions_match_degrees(void) {
    static const struct {
        float degrees, companion;
        bool cosine;
    } angles[] = {
        {DISPLAY_FLAT_DEG, DISPLAY_FLAT_SIN, false},
        {DISPLAY_MAX_TILT_DEG, DISPLAY_MAX_TILT_SIN, false},
        {DISPLAY_SWING_DEG, DISPLAY_SWING_SIN, false},
        {DISPLAY_OVERHEAD_ENTER_DEG, DISPLAY_OVERHEAD_ENTER_SIN, false},
        {DISPLAY_OVERHEAD_EXIT_DEG, DISPLAY_OVERHEAD_EXIT_SIN, false},
        {DISPLAY_TILT_MIN_DEG, DISPLAY_TILT_MIN_SIN, false},
        {DISPLAY_TILT_DEFAULT_MAX_DEG, DISPLAY_TILT_DEFAULT_MAX_SIN, false},
        {DISPLAY_TILT_ADJACENT_MAX_DEG, DISPLAY_TILT_ADJACENT_MAX_SIN, false},
        {DISPLAY_TILT_OPPOSITE_MAX_DEG, DISPLAY_TILT_OPPOSITE_MAX_SIN, false},
        {DISPLAY_ADJACENT_MIN_DEG, DISPLAY_ADJACENT_MIN_COS, true},
    };

    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        const float radians = angles[i].degrees * MATH_RADIANS_PER_DEGREE;
        TEST_ASSERT_FLOAT_WITHIN(0.000001F, angles[i].cosine ? cosf(radians) : sinf(radians), angles[i].companion);
    }
}

static void
test_quarter_names_and_filter_step_response(void) {
    static const char* const names[] = {DISPLAY_PORTRAIT_NAME, DISPLAY_LANDSCAPE_NAME,
                                        DISPLAY_PORTRAIT_UPSIDE_DOWN_NAME, DISPLAY_LANDSCAPE_UPSIDE_DOWN_NAME};
    for (int q = 0; q < DISPLAY_QUARTER_COUNT; q++) {
        TEST_ASSERT_EQUAL_STRING(names[q], display_quarter_name(q));
    }
    TEST_ASSERT_EQUAL_STRING("", display_quarter_name(-1));
    TEST_ASSERT_EQUAL_STRING("", display_quarter_name(DISPLAY_QUARTER_COUNT));
    display_t d = fixture(DISPLAY_PORTRAIT);
    const vec3f_t first = sample(DISPLAY_PORTRAIT, 0, 0, 1);
    const vec3f_t next = sample(DISPLAY_LANDSCAPE, 0, 0, 1);
    display_update(&d, first, 0);
    display_update(&d, next, (int64_t)DISPLAY_FILTER_TAU_MS * MATH_US_PER_MS);
    TEST_ASSERT_FLOAT_WITHIN(0.000001F, (first.x + next.x) / 2, d.filtered.x);
    TEST_ASSERT_FLOAT_WITHIN(0.000001F, (first.y + next.y) / 2, d.filtered.y);
}

static void
test_swing_direction_history_age_and_wrap(void) {
    display_t d = fixture(DISPLAY_PORTRAIT);
    const vec3f_t level = sample(DISPLAY_PORTRAIT, 0, 0, 1);
    const vec3f_t rise = sample(DISPLAY_PORTRAIT, 0, DISPLAY_SWING_DEG + TEST_ANGLE_MARGIN_DEG, 1);
    display_update(&d, rise, 0);
    d.filtered = level;
    display_update(&d, level, TEST_STEP_US);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.swing_ended_us);
    d = fixture(DISPLAY_PORTRAIT);
    display_update(&d, level, 0);
    d.filtered = rise;
    display_update(&d, rise, (int64_t)DISPLAY_SWING_MS * MATH_US_PER_MS + 1);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.swing_ended_us);
    d = fixture(DISPLAY_PORTRAIT);
    int64_t now = 0;
    feed(&d, level, &now, DISPLAY_SWING_MS * 3);
    TEST_ASSERT_EQUAL_INT(DISPLAY_TILT_HISTORY_COUNT, d.tilt_count);
    TEST_ASSERT_EQUAL_INT((DISPLAY_SWING_MS * 3 / DISPLAY_SAMPLE_MS + 1) % DISPLAY_TILT_HISTORY_COUNT, d.tilt_next);
    d.filtered = rise;
    display_update(&d, rise, now);
    TEST_ASSERT_EQUAL_INT64(now, d.swing_ended_us);
    d = fixture(DISPLAY_PORTRAIT);
    display_update(&d, sample(DISPLAY_PORTRAIT, 0, -DISPLAY_QUARTER_DEG, 1), 0);
    const vec3f_t high = sample(DISPLAY_PORTRAIT, 0, DISPLAY_QUARTER_DEG, 1);
    d.filtered = high;
    display_update(&d, high, TEST_STEP_US);
    TEST_ASSERT_EQUAL_INT64(TEST_STEP_US, d.swing_ended_us);
    d = fixture(DISPLAY_PORTRAIT);
    display_update(&d, high, 0);
    const vec3f_t low = sample(DISPLAY_PORTRAIT, 0, -DISPLAY_QUARTER_DEG, 1);
    d.filtered = low;
    display_update(&d, low, TEST_STEP_US);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.swing_ended_us);
}

static void
test_short_flat_inside_accel_band_and_overhead_middle(void) {
    display_t d = fixture(DISPLAY_PORTRAIT);
    int64_t now = 0;
    feed(&d, sample(DISPLAY_PORTRAIT, 0, DISPLAY_FLAT_DEG - TEST_ANGLE_MARGIN_DEG, 1), &now, DISPLAY_FLAT_MS * 2);
    TEST_ASSERT_FALSE(d.flat);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.flat_since_us);
    d = fixture(DISPLAY_PORTRAIT);
    now = 0;
    feed(&d, sample(DISPLAY_PORTRAIT, 0, DISPLAY_FLAT_DEG + TEST_ANGLE_MARGIN_DEG, 1), &now,
         DISPLAY_FLAT_MS - DISPLAY_SAMPLE_MS);
    TEST_ASSERT_FALSE(d.flat);
    for (int sign = -1; sign <= 1; sign += 2) {
        d = fixture(DISPLAY_PORTRAIT);
        now = 0;
        feed(&d, sample(DISPLAY_PORTRAIT, 0, 0, 1 + sign * (DISPLAY_ACCEL_TOLERANCE_G - DISPLAY_NEAR_ZERO_G)), &now,
             DISPLAY_AFTER_ACCEL_MS);
        TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, d.accel_ended_us);
    }
    d = fixture(DISPLAY_PORTRAIT);
    now = 0;
    const float middle = (DISPLAY_OVERHEAD_ENTER_DEG + DISPLAY_OVERHEAD_EXIT_DEG) / 2;
    feed(&d, sample(DISPLAY_PORTRAIT, 0, middle, 1), &now, DISPLAY_SAMPLE_MS);
    TEST_ASSERT_FALSE(d.overhead);
    d.overhead = true;
    feed(&d, sample(DISPLAY_PORTRAIT, 0, middle, 1), &now, DISPLAY_SAMPLE_MS);
    TEST_ASSERT_TRUE(d.overhead);
}

static void
test_max_tilt_overrides_candidate_tilt_limits(void) {
    display_t d = fixture(DISPLAY_PORTRAIT);
    const vec3f_t high = sample(DISPLAY_LANDSCAPE, 0, DISPLAY_MAX_TILT_DEG + TEST_ANGLE_MARGIN_DEG, 1);
    TEST_ASSERT_EQUAL_INT(DISPLAY_LANDSCAPE, display_candidate(high, d.quarter, false));
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, display_candidate(high, d.quarter, true));
    TEST_ASSERT_FALSE(display_update(&d, high, 0));
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, d.predicted);
}

static void
test_prediction_dropout_restarts_settle(void) {
    display_t d = fixture(DISPLAY_PORTRAIT);
    const vec3f_t turned = sample(DISPLAY_LANDSCAPE, 0, 0, 1);
    display_update(&d, turned, 0);
    const vec3f_t invalid = sample(DISPLAY_LANDSCAPE, 0, 0, DISPLAY_NEAR_ZERO_G / 2);
    d.filtered = invalid;
    display_update(&d, invalid, TEST_STEP_US);
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, d.predicted);
    d.filtered = turned;
    TEST_ASSERT_FALSE(display_update(&d, turned, TEST_STEP_US * 2));
    TEST_ASSERT_EQUAL_INT64(TEST_STEP_US * 2, d.predicted_us);
    TEST_ASSERT_FALSE(display_update(&d, turned, TEST_STEP_US * 2 + (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS - 1));
    TEST_ASSERT_TRUE(display_update(&d, turned, TEST_STEP_US * 2 + (int64_t)DISPLAY_SETTLE_MS * MATH_US_PER_MS));
}

static void
test_release_clears_motion_history_and_preserves_touch(void) {
    display_orientation_t o;
    display_orientation_init(&o);
    o.display.flat = true;
    o.display.overhead = true;
    o.display.primed = true;
    o.display.filtered = sample(DISPLAY_PORTRAIT, 0, DISPLAY_FLAT_DEG, 1);
    o.display.flat_since_us = o.display.flat_ended_us = o.display.swing_ended_us = o.display.accel_ended_us =
        TEST_STEP_US;
    o.display.tilt_count = DISPLAY_TILT_HISTORY_COUNT;
    o.display.tilt_next = DISPLAY_TILT_HISTORY_COUNT - 1;
    o.display.predicted = DISPLAY_PORTRAIT;
    display_touch(&o.display, TEST_STEP_US);
    display_orientation_hold(&o, DISPLAY_LANDSCAPE);
    display_orientation_release(&o);
    TEST_ASSERT_FALSE(o.display.flat);
    TEST_ASSERT_FALSE(o.display.overhead);
    TEST_ASSERT_FALSE(o.display.primed);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, o.display.flat_since_us);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, o.display.flat_ended_us);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, o.display.accel_ended_us);
    TEST_ASSERT_EQUAL_INT64(DISPLAY_TIME_NONE, o.display.swing_ended_us);
    TEST_ASSERT_EQUAL_INT64(TEST_STEP_US, o.display.touch_ended_us);
    TEST_ASSERT_EQUAL_INT(0, o.display.tilt_count);
    TEST_ASSERT_EQUAL_INT(0, o.display.tilt_next);
    TEST_ASSERT_EQUAL_FLOAT(0, vec3f_dot(o.display.filtered, o.display.filtered));
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_NONE, o.display.predicted);
}

static void
test_single_touch_and_backwards_clamp(void) {
    for (int backwards = 0; backwards <= 1; backwards++) {
        display_t d = fixture(DISPLAY_PORTRAIT);
        const vec3f_t turned = sample(DISPLAY_LANDSCAPE, 0, 0, 1);
        display_update(&d, turned, TEST_STEP_US);
        display_touch(&d, TEST_STEP_US);
        const int64_t now = backwards ? 0 : TEST_STEP_US;
        TEST_ASSERT_FALSE(display_update(&d, turned, now));
        TEST_ASSERT_EQUAL_INT64(now, d.touch_ended_us);
        int64_t clock = now + TEST_STEP_US;
        feed(&d, turned, &clock, DISPLAY_AFTER_TOUCH_MS - 2 * DISPLAY_SAMPLE_MS);
        TEST_ASSERT_EQUAL_INT(DISPLAY_PORTRAIT, d.quarter);
        TEST_ASSERT_TRUE(display_update(&d, turned, now + (int64_t)DISPLAY_AFTER_TOUCH_MS * MATH_US_PER_MS));
    }
}

void
run_display_suite(void) {
    RUN_TEST(test_angle_companions_match_degrees);
    RUN_TEST(test_quarter_names_and_filter_step_response);
    RUN_TEST(test_swing_direction_history_age_and_wrap);
    RUN_TEST(test_short_flat_inside_accel_band_and_overhead_middle);
    RUN_TEST(test_max_tilt_overrides_candidate_tilt_limits);
    RUN_TEST(test_prediction_dropout_restarts_settle);
    RUN_TEST(test_release_clears_motion_history_and_preserves_touch);
    RUN_TEST(test_single_touch_and_backwards_clamp);

    RUN_TEST(test_clean_turn_settles_in_every_rotation);
    RUN_TEST(test_filtered_turn_and_hysteresis_in_every_rotation);
    RUN_TEST(test_tilt_limits_are_relative_to_default);
    RUN_TEST(test_flat_and_face_up_offset_never_switch);
    RUN_TEST(test_flat_cooldown_starts_when_flat_ends);
    RUN_TEST(test_face_down_has_no_flat_dwell_or_cooldown);
    RUN_TEST(test_near_zero_interrupts_continuous_flat_dwell);
    RUN_TEST(test_overhead_hysteresis_in_every_rotation);
    RUN_TEST(test_acceleration_cooldown_in_every_rotation);
    RUN_TEST(test_swing_cooldown_in_every_rotation);
    RUN_TEST(test_touch_blocks_then_waits_after_last_down_frame);
    RUN_TEST(test_gap_backwards_and_zero_restart_prediction);
    RUN_TEST(test_sampler_cadence_failures_and_hold_release);
}

SUITE_REGISTER(run_display_suite);

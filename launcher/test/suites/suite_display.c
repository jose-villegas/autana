/* display: quarter decisions, event gates and shell sampling in all rotations. */
#include "display/display.h"
#include "display/display_shell.h"

#include <math.h>
#include <stdlib.h>

#include "display/viewport.h"
#include "math/scalar/math_const.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"

#define TEST_ANGLE_MARGIN_DEG 1.0F
#define TEST_HYST_BELOW_DEG   (DISPLAY_ADJACENT_MIN_DEG - DISPLAY_HYSTERESIS_GAP_DEG / 6.0F)
#define TEST_HYST_ABOVE_DEG   (DISPLAY_ADJACENT_MIN_DEG + DISPLAY_HYSTERESIS_GAP_DEG / 18.0F)
#define TEST_STEP_US          ((int64_t)DISPLAY_SAMPLE_MS * MATH_US_PER_MS)
#define TEST_STEADY_MS        (DISPLAY_COMMIT_MS * 2)

static vec3f_t
sample(int quarter, float turn_deg, float tilt_deg, float magnitude) {
    const float radians_per_degree = MATH_PI / (2.0F * DISPLAY_QUARTER_DEG);
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

void
run_display_suite(void) {
    RUN_TEST(test_clean_turn_settles_in_every_rotation);
    RUN_TEST(test_filtered_turn_and_hysteresis_in_every_rotation);
    RUN_TEST(test_tilt_limits_are_relative_to_default);
    RUN_TEST(test_flat_and_face_up_offset_never_switch);
    RUN_TEST(test_flat_cooldown_starts_when_flat_ends);
    RUN_TEST(test_near_zero_interrupts_continuous_flat_dwell);
    RUN_TEST(test_overhead_hysteresis_in_every_rotation);
    RUN_TEST(test_acceleration_cooldown_in_every_rotation);
    RUN_TEST(test_swing_cooldown_in_every_rotation);
    RUN_TEST(test_touch_blocks_then_waits_after_last_down_frame);
    RUN_TEST(test_gap_backwards_and_zero_restart_prediction);
    RUN_TEST(test_sampler_cadence_failures_and_hold_release);
}

SUITE_REGISTER(run_display_suite);

/*
 * Portable suite: spring_line.h - a row of points on springs, and above all
 * that it comes to rest, which integer physics does not do unasked.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "util/spring_line.h"

#define COLUMNS   200
#define CENTRE    100
#define MAX_TICKS 5000

static int32_t* offset;
static int32_t* velocity;
static spring_line_t line;

static void
fixture_begin(void) {
    offset = malloc(sizeof(int32_t) * COLUMNS);
    velocity = malloc(sizeof(int32_t) * COLUMNS);
    TEST_ASSERT_NOT_NULL(offset);
    TEST_ASSERT_NOT_NULL(velocity);
    spring_line_init(&line, offset, velocity, COLUMNS);
}

static void
fixture_end(void) {
    free(offset);
    free(velocity);
}

static int
ticks_until_rest(void) {
    int ticks = 0;
    while (!spring_line_at_rest(&line) && ticks < MAX_TICKS) {
        spring_line_tick(&line);
        ticks++;
    }
    return ticks;
}

/* Twice the energy, up to a constant. */
static int64_t
energy(void) {
    int64_t total = 0;
    for (int x = 0; x < COLUMNS; x++) {
        const int64_t v = velocity[x] >> 4;
        const int64_t u = offset[x] >> 4;
        const int64_t stretch = x + 1 < COLUMNS ? (offset[x + 1] - offset[x]) >> 4 : 0;
        total += v * v + (u * u * line.stiffness + stretch * stretch * line.tension) / 256;
    }
    return total;
}

static void
test_a_new_line_is_at_rest_and_a_tick_does_nothing(void) {
    fixture_begin();
    TEST_ASSERT_TRUE(spring_line_at_rest(&line));
    spring_line_tick(&line);
    TEST_ASSERT_EQUAL_INT(0, spring_line_advance(&line, 1000));
    TEST_ASSERT_TRUE(spring_line_at_rest(&line));
    fixture_end();
}

static void
test_a_poke_wakes_only_the_columns_it_touches(void) {
    fixture_begin();
    spring_line_poke(&line, CENTRE, 10, -SPRING_LINE_ONE);
    TEST_ASSERT_FALSE(spring_line_at_rest(&line));
    TEST_ASSERT_EQUAL_INT(CENTRE - 10, line.active_lo);
    TEST_ASSERT_EQUAL_INT(CENTRE + 11, line.active_hi);
    TEST_ASSERT_EQUAL_INT32(-SPRING_LINE_ONE, velocity[CENTRE]);
    TEST_ASSERT_EQUAL_INT32(0, velocity[CENTRE - 10]);
    TEST_ASSERT_TRUE(velocity[CENTRE - 5] < 0 && velocity[CENTRE - 5] > -SPRING_LINE_ONE);
    TEST_ASSERT_EQUAL_INT32(velocity[CENTRE - 5], velocity[CENTRE + 5]);
    fixture_end();
}

static void
test_a_poke_off_either_end_is_clipped(void) {
    fixture_begin();
    spring_line_poke(&line, -3, 8, SPRING_LINE_ONE);
    spring_line_nudge(&line, COLUMNS + 2, 8, SPRING_LINE_ONE);
    TEST_ASSERT_EQUAL_INT(0, line.active_lo);
    TEST_ASSERT_EQUAL_INT(COLUMNS, line.active_hi);
    TEST_ASSERT_TRUE(velocity[0] > 0 && offset[COLUMNS - 1] > 0);
    fixture_end();
}

static void
test_the_wave_spreads_one_column_a_tick_and_evenly_both_ways(void) {
    fixture_begin();
    spring_line_nudge(&line, CENTRE, 6, -8 * SPRING_LINE_ONE);
    for (int tick = 1; tick <= 40; tick++) {
        spring_line_tick(&line);
        TEST_ASSERT_TRUE(line.active_lo >= CENTRE - 6 - tick && line.active_hi <= CENTRE + 7 + tick);
        for (int k = 1; k < 60; k++) {
            TEST_ASSERT_EQUAL_INT32(offset[CENTRE - k], offset[CENTRE + k]);
        }
    }
    TEST_ASSERT_TRUE(offset[CENTRE - 30] != 0);
    TEST_ASSERT_EQUAL_INT32(0, offset[CENTRE - 60]);
    fixture_end();
}

/* Tick to tick the sum wobbles - the two halves of a tick are a step apart
 * in time - so the claims are the ones that hold: never above where it
 * started, and lower every hundred ticks. */
static void
test_energy_never_exceeds_its_start_and_keeps_falling(void) {
    fixture_begin();
    spring_line_nudge(&line, CENTRE, 12, -20 * SPRING_LINE_ONE);
    spring_line_poke(&line, 40, 5, 2 * SPRING_LINE_ONE);
    const int64_t start = energy();
    int64_t a_hundred_ago = start;
    bool ok = true;
    for (int tick = 1; tick <= 600 && ok; tick++) {
        spring_line_tick(&line);
        ok = energy() <= start;
        if (ok && tick % 100 == 0) {
            ok = energy() < a_hundred_ago;
            a_hundred_ago = energy();
        }
    }
    fixture_end();
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(a_hundred_ago < start / 1000);
}

/* The first rest rule zeroed points one at a time and, at these settings,
 * never stopped: see the top of spring_line.h. */
static void
test_it_comes_to_rest_at_every_setting_and_exactly(void) {
    const int settings[][3] = {{200, 2, 4}, {230, 3, 5}, {250, 2, 4}, {120, 2, 4}, {60, 8, 12}, {250, 1, 1}};
    for (size_t s = 0; s < sizeof settings / sizeof settings[0]; s++) {
        fixture_begin();
        line.tension = settings[s][0];
        line.stiffness = settings[s][1];
        line.damping = settings[s][2];
        spring_line_poke(&line, 70, 24, -(SPRING_LINE_ONE * 3 / 2));
        spring_line_nudge(&line, 150, 9, 15 * SPRING_LINE_ONE);

        TEST_ASSERT_LESS_THAN_INT_MESSAGE(MAX_TICKS, ticks_until_rest(), "still moving");
        for (int x = 0; x < COLUMNS; x++) {
            TEST_ASSERT_EQUAL_INT32(0, offset[x]);
            TEST_ASSERT_EQUAL_INT32(0, velocity[x]);
        }
        fixture_end();
    }
}

static void
test_a_runaway_offset_is_clamped(void) {
    fixture_begin();
    spring_line_poke(&line, CENTRE, 4, 100 * SPRING_LINE_ONE);
    for (int tick = 0; tick < 50; tick++) {
        spring_line_tick(&line);
        for (int x = 0; x < COLUMNS; x++) {
            TEST_ASSERT_TRUE(offset[x] <= SPRING_LINE_MAX_OFFSET && offset[x] >= -SPRING_LINE_MAX_OFFSET);
        }
    }
    fixture_end();
}

static void
test_advance_runs_whole_ticks_carries_the_rest_and_caps_a_stall(void) {
    fixture_begin();
    spring_line_nudge(&line, CENTRE, 12, -30 * SPRING_LINE_ONE);
    TEST_ASSERT_EQUAL_INT(0, spring_line_advance(&line, SPRING_LINE_TICK_MS - 1));
    TEST_ASSERT_EQUAL_INT(1, spring_line_advance(&line, 1));
    TEST_ASSERT_EQUAL_INT(4, spring_line_advance(&line, 4 * SPRING_LINE_TICK_MS));
    TEST_ASSERT_EQUAL_INT(SPRING_LINE_MAX_TICKS, spring_line_advance(&line, 5000));
    fixture_end();
}

static void
test_apply_reports_what_changed_and_how_far_then_goes_silent(void) {
    fixture_begin();
    int16_t rest[COLUMNS];
    int16_t drawn[COLUMNS];
    for (int x = 0; x < COLUMNS; x++) {
        rest[x] = (int16_t)(1600 + x);
        drawn[x] = rest[x];
    }
    int lo, hi;
    TEST_ASSERT_EQUAL_INT(0, spring_line_apply(&line, rest, drawn, &lo, &hi));
    TEST_ASSERT_EQUAL_INT(0, hi - lo);

    spring_line_nudge(&line, CENTRE, 10, -(5 * SPRING_LINE_ONE + SPRING_LINE_ONE / 2));
    TEST_ASSERT_EQUAL_INT(6, spring_line_apply(&line, rest, drawn, &lo, &hi));
    TEST_ASSERT_TRUE(lo > CENTRE - 10 && lo <= CENTRE - 8);
    TEST_ASSERT_TRUE(hi < CENTRE + 11 && hi >= CENTRE + 9);
    TEST_ASSERT_EQUAL_INT16(rest[CENTRE] - 88, drawn[CENTRE]);
    TEST_ASSERT_EQUAL_INT16(rest[CENTRE - 20], drawn[CENTRE - 20]);

    TEST_ASSERT_EQUAL_INT(0, spring_line_apply(&line, rest, drawn, &lo, &hi));
    TEST_ASSERT_EQUAL_INT(0, hi - lo);

    ticks_until_rest();
    spring_line_apply(&line, rest, drawn, &lo, &hi);
    TEST_ASSERT_EQUAL_INT16_ARRAY(rest, drawn, COLUMNS);
    fixture_end();
}

void
suite_spring_line(void) {
    RUN_TEST(test_a_new_line_is_at_rest_and_a_tick_does_nothing);
    RUN_TEST(test_a_poke_wakes_only_the_columns_it_touches);
    RUN_TEST(test_a_poke_off_either_end_is_clipped);
    RUN_TEST(test_the_wave_spreads_one_column_a_tick_and_evenly_both_ways);
    RUN_TEST(test_energy_never_exceeds_its_start_and_keeps_falling);
    RUN_TEST(test_it_comes_to_rest_at_every_setting_and_exactly);
    RUN_TEST(test_a_runaway_offset_is_clamped);
    RUN_TEST(test_advance_runs_whole_ticks_carries_the_rest_and_caps_a_stall);
    RUN_TEST(test_apply_reports_what_changed_and_how_far_then_goes_silent);
}

SUITE_REGISTER(suite_spring_line);

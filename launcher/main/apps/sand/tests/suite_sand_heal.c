/*
 * Portable suite: sand_heal, the policy deciding which rows sand asks gfx to
 * heal and when.
 */

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_heal.h"

#define SCREEN_ROWS 448

static sand_heal_t heal;
static sand_heal_span_t spans[SAND_HEAL_MAX_SPANS];

static void
fixture(void) {
    sand_heal_init(&heal, SCREEN_ROWS);
}

static int
step(void) {
    return sand_heal_step(&heal, spans, SAND_HEAL_MAX_SPANS);
}

static void
test_a_band_that_moved_heals_once_it_has_settled_and_not_before(void) {
    fixture();
    sand_heal_note_rows(&heal, 100, 102);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, step(), "the frame that moved is not healed yet");

    for (int quiet = 1; quiet < SAND_HEAL_SETTLE_FRAMES; quiet++) {
        TEST_ASSERT_EQUAL_INT(0, step());
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, step(), "SAND_HEAL_SETTLE_FRAMES quiet presents after, it heals");
    TEST_ASSERT_TRUE(spans[0].y0 <= 100 && spans[0].y1 >= 102);

    for (int i = 0; i < 40; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, step(), "a settled band heals once, not on every quiet present");
    }
}

static void
test_rows_nothing_was_sent_to_are_never_healed(void) {
    fixture();
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(0, step());
    }
}

static void
test_movement_before_settling_restarts_the_wait(void) {
    fixture();
    sand_heal_note_rows(&heal, 40, 48);
    step();
    step();
    sand_heal_note_rows(&heal, 40, 48);
    for (int i = 0; i < SAND_HEAL_SETTLE_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, step(), "new movement restarts the settle count");
    }
    TEST_ASSERT_EQUAL_INT(1, step());
}

static void
test_a_band_moving_without_pause_heals_on_the_busy_cadence(void) {
    fixture();
    int heals = 0;
    for (int frame = 0; frame < 2 * SAND_HEAL_BUSY_FRAMES; frame++) {
        sand_heal_note_rows(&heal, 200, 210);
        heals += step() > 0;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, heals, "a band moving the whole time is healed every SAND_HEAL_BUSY_FRAMES");
}

/* Settled liquid still shimmers: a few cells change in a band now and then,
 * and healing each such band a strip at a time would cost more than the
 * shimmer's own sends. */
static void
test_a_band_heals_again_no_sooner_than_the_minimum_gap(void) {
    fixture();
    sand_heal_note_rows(&heal, 120, 122);
    int frame = 0;
    int last_heal = -1;
    int heals = 0;
    for (; frame < 4 * SAND_HEAL_MIN_GAP_FRAMES; frame++) {
        if (frame % (SAND_HEAL_SETTLE_FRAMES + 1) == 0) {
            sand_heal_note_rows(&heal, 120, 122);
        }
        if (step() > 0) {
            if (last_heal >= 0) {
                TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_HEAL_MIN_GAP_FRAMES, frame - last_heal,
                                                         "a band heals at most once per SAND_HEAL_MIN_GAP_FRAMES");
            }
            last_heal = frame;
            heals++;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(3, heals, "a shimmering band is still healed, just not every time");
}

static void
test_neighbouring_bands_heal_as_one_span_and_far_ones_apart(void) {
    fixture();
    sand_heal_note_rows(&heal, 64, 90);
    sand_heal_note_rows(&heal, 300, 301);
    int n = 0;
    for (int i = 0; i <= SAND_HEAL_SETTLE_FRAMES; i++) {
        n = step();
    }
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(64, spans[0].y0);
    TEST_ASSERT_EQUAL_INT(96, spans[0].y1);
    TEST_ASSERT_EQUAL_INT(296, spans[1].y0);
    TEST_ASSERT_EQUAL_INT(304, spans[1].y1);
}

static void
test_rows_off_screen_are_ignored(void) {
    fixture();
    sand_heal_note_rows(&heal, -30, 4);
    sand_heal_note_rows(&heal, SCREEN_ROWS - 2, SCREEN_ROWS + 50);
    int n = 0;
    for (int i = 0; i <= SAND_HEAL_SETTLE_FRAMES; i++) {
        n = step();
    }
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, spans[0].y0);
    TEST_ASSERT_EQUAL_INT(SCREEN_ROWS, spans[1].y1);
}

void
run_sand_heal_suite(void) {
    RUN_TEST(test_a_band_that_moved_heals_once_it_has_settled_and_not_before);
    RUN_TEST(test_rows_nothing_was_sent_to_are_never_healed);
    RUN_TEST(test_movement_before_settling_restarts_the_wait);
    RUN_TEST(test_a_band_moving_without_pause_heals_on_the_busy_cadence);
    RUN_TEST(test_a_band_heals_again_no_sooner_than_the_minimum_gap);
    RUN_TEST(test_neighbouring_bands_heal_as_one_span_and_far_ones_apart);
    RUN_TEST(test_rows_off_screen_are_ignored);
}

SUITE_REGISTER(run_sand_heal_suite);

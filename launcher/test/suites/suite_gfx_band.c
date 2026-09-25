/*
 * Portable suite: the band-ring state machine (gfx_band.h) behind
 * gfx_band_next()/gfx_band_submit() (gfx.c) - driven directly since the
 * header carries no ESP-IDF dependency. The real DMA send and buffer
 * allocation gfx.c wires around this need real device memory and are not
 * covered here.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_band.h"

#define BAND_HEIGHT 32
#define BAND_COUNT  (448 / BAND_HEIGHT)

static void
test_a_fresh_ring_starts_at_band_zero_with_nothing_in_flight(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, BAND_COUNT);

    TEST_ASSERT_FALSE(gfx_band_ring_done(&ring));
    TEST_ASSERT_EQUAL_INT(0, gfx_band_ring_slot(&ring));
    TEST_ASSERT_EQUAL_INT(0, gfx_band_ring_row0(&ring, BAND_HEIGHT));
    TEST_ASSERT_FALSE_MESSAGE(gfx_band_ring_must_wait(&ring), "band 0 has no previous send to wait for");
}

/* The two buffers alternate strictly by band index - what lets the app
 * render band k+1 into the OTHER slot while band k's send is in flight. */
static void
test_slots_alternate_between_the_two_buffers(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, BAND_COUNT);

    int slots[4];
    for (int i = 0; i < 4; i++) {
        slots[i] = gfx_band_ring_slot(&ring);
        gfx_band_ring_advance(&ring);
    }

    TEST_ASSERT_EQUAL_INT_ARRAY(((int[]){0, 1, 0, 1}), slots, 4);
}

static void
test_row0_advances_by_one_band_height_each_time(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, BAND_COUNT);

    for (int i = 0; i < BAND_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(i * BAND_HEIGHT, gfx_band_ring_row0(&ring, BAND_HEIGHT));
        gfx_band_ring_advance(&ring);
    }
}

/* The contract gfx_band_submit() relies on: only band 0 skips the wait -
 * every later band must wait for whichever one is still in flight before
 * its own send can be queued. */
static void
test_only_the_first_band_skips_the_wait(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, BAND_COUNT);

    TEST_ASSERT_FALSE(gfx_band_ring_must_wait(&ring));
    gfx_band_ring_advance(&ring);

    for (int i = 1; i < BAND_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(gfx_band_ring_must_wait(&ring), "every band after the first must wait");
        gfx_band_ring_advance(&ring);
    }
}

static void
test_the_ring_is_done_only_after_every_band_was_handed_out(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, BAND_COUNT);

    for (int i = 0; i < BAND_COUNT; i++) {
        TEST_ASSERT_FALSE(gfx_band_ring_done(&ring));
        gfx_band_ring_advance(&ring);
    }
    TEST_ASSERT_TRUE(gfx_band_ring_done(&ring));
}

/* gfx_band_next() (gfx.c) waits for the final band's send, then settles the
 * ring exactly once - gfx_band_ring_settled() has to go from false to true
 * across that, and stay true without a further wait if asked again. */
static void
test_settling_clears_the_in_flight_band_exactly_once(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, 1);

    gfx_band_ring_advance(&ring);
    TEST_ASSERT_TRUE(gfx_band_ring_done(&ring));
    TEST_ASSERT_FALSE_MESSAGE(gfx_band_ring_settled(&ring), "the one band's send is still unaccounted for");

    gfx_band_ring_settle(&ring);
    TEST_ASSERT_TRUE(gfx_band_ring_settled(&ring));
}

/* A single-band frame (a degenerate GFX_BAND_HEIGHT equal to the whole
 * screen) never asks for a wait at all - there is only ever one band in
 * flight, never a second one racing it. */
static void
test_a_single_band_frame_never_waits(void) {
    gfx_band_ring_t ring;
    gfx_band_ring_begin(&ring, 1);

    TEST_ASSERT_FALSE(gfx_band_ring_must_wait(&ring));
    gfx_band_ring_advance(&ring);
    TEST_ASSERT_TRUE(gfx_band_ring_done(&ring));
}

void
run_gfx_band_suite(void) {
    RUN_TEST(test_a_fresh_ring_starts_at_band_zero_with_nothing_in_flight);
    RUN_TEST(test_slots_alternate_between_the_two_buffers);
    RUN_TEST(test_row0_advances_by_one_band_height_each_time);
    RUN_TEST(test_only_the_first_band_skips_the_wait);
    RUN_TEST(test_the_ring_is_done_only_after_every_band_was_handed_out);
    RUN_TEST(test_settling_clears_the_in_flight_band_exactly_once);
    RUN_TEST(test_a_single_band_frame_never_waits);
}

SUITE_REGISTER(run_gfx_band_suite);

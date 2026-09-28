/* Portable tests for the shell-facing band-loop adapter. */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_band_run.h"

#define BAND_COUNT  4
#define BAND_HEIGHT 32

typedef struct {
    bool dirty[BAND_COUNT];
    int current;
    int begin_calls;
    int draw_calls;
    int replay_calls;
    int submit_calls;
    int skip_calls;
    int row0[BAND_COUNT];
    int row1[BAND_COUNT];
    gfx_color_t pixels[BAND_COUNT];
} band_mock_t;

static band_mock_t* mock;

static void
begin(void* context) {
    mock = context;
    mock->current = 0;
    mock->begin_calls++;
}

static bool
next(void* context) {
    return ((band_mock_t*)context)->current < BAND_COUNT;
}

static bool
dirty(void* context) {
    return ((band_mock_t*)context)->dirty[((band_mock_t*)context)->current];
}

static int
row0(void* context) {
    return ((band_mock_t*)context)->current * BAND_HEIGHT;
}

static int
height(void* context) {
    (void)context;
    return BAND_HEIGHT;
}

static gfx_color_t*
buffer(void* context) {
    band_mock_t* state = context;
    return &state->pixels[state->current];
}

static void
replay(void* context, int first, int last) {
    band_mock_t* state = context;
    TEST_ASSERT_EQUAL_HEX16(0x1111, state->pixels[state->current]);
    state->replay_calls++;
    state->pixels[state->current] = 0x2222;
    TEST_ASSERT_EQUAL_INT(state->current * BAND_HEIGHT, first);
    TEST_ASSERT_EQUAL_INT((state->current + 1) * BAND_HEIGHT, last);
}

static void
submit(void* context) {
    band_mock_t* state = context;
    TEST_ASSERT_EQUAL_HEX16(0x2222, state->pixels[state->current]);
    state->submit_calls++;
    state->current++;
}

static void
skip(void* context) {
    band_mock_t* state = context;
    state->skip_calls++;
    state->current++;
}

static const gfx_band_run_t mock_run = {
    .frame_begin = begin,
    .next = next,
    .dirty = dirty,
    .row0 = row0,
    .height = height,
    .buffer = buffer,
    .replay = replay,
    .submit = submit,
    .skip = skip,
};

static void
draw(int first, int last, gfx_color_t* target) {
    mock->draw_calls++;
    mock->row0[mock->draw_calls - 1] = first;
    mock->row1[mock->draw_calls - 1] = last;
    target[0] = 0x1111;
}

static void
test_dirty_bands_are_drawn_then_replayed_and_submitted(void) {
    band_mock_t state = {.dirty = {true, false, true, false}};

    TEST_ASSERT_TRUE(gfx_band_run(&mock_run, &state, draw));
    TEST_ASSERT_EQUAL_INT(1, state.begin_calls);
    TEST_ASSERT_EQUAL_INT(2, state.draw_calls);
    TEST_ASSERT_EQUAL_INT(2, state.replay_calls);
    TEST_ASSERT_EQUAL_INT(2, state.submit_calls);
    TEST_ASSERT_EQUAL_INT(2, state.skip_calls);
    TEST_ASSERT_EQUAL_INT(0, state.row0[0]);
    TEST_ASSERT_EQUAL_INT(BAND_HEIGHT, state.row1[0]);
    TEST_ASSERT_EQUAL_INT(2 * BAND_HEIGHT, state.row0[1]);
    TEST_ASSERT_EQUAL_INT(3 * BAND_HEIGHT, state.row1[1]);
}

static void
test_full_dirty_frame_covers_every_row_once_in_order(void) {
    band_mock_t state = {.dirty = {true, true, true, true}};

    TEST_ASSERT_TRUE(gfx_band_run(&mock_run, &state, draw));
    TEST_ASSERT_EQUAL_INT(BAND_COUNT, state.draw_calls);
    for (int i = 0; i < BAND_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(i * BAND_HEIGHT, state.row0[i]);
        TEST_ASSERT_EQUAL_INT((i + 1) * BAND_HEIGHT, state.row1[i]);
    }
}

static void
test_an_app_without_a_band_callback_never_enters_the_loop(void) {
    band_mock_t state = {.dirty = {true, true, true, true}};

    TEST_ASSERT_FALSE(gfx_band_run(&mock_run, &state, NULL));
    TEST_ASSERT_EQUAL_INT(0, state.begin_calls);
    TEST_ASSERT_EQUAL_INT(0, state.draw_calls);
    TEST_ASSERT_EQUAL_INT(0, state.submit_calls);
}

void
run_gfx_band_run_suite(void) {
    RUN_TEST(test_dirty_bands_are_drawn_then_replayed_and_submitted);
    RUN_TEST(test_full_dirty_frame_covers_every_row_once_in_order);
    RUN_TEST(test_an_app_without_a_band_callback_never_enters_the_loop);
}

SUITE_REGISTER(run_gfx_band_run_suite);

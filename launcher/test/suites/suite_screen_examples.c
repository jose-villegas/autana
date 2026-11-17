#include "suites.h"

#ifndef DEVICE_BUILD
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "math/linear/affine2x.h"
#include "testkit/cleanup.h"
#include "ui_probe.h"
#include "unity.h"

#include "../screen_examples/settings.c"

#define TOUCH_HOLD_FRAMES 4

static void
cleanup(void) {
    ui_probe_widgets_stop();
    gfx_reset_for_test();
}

static void
fixture(void) {
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_TRUE(gfx_init());
    ui_init();
    ui_set_transform(affine2x_identity());
    ui_probe_widgets_start();
}

static void
tap(settings_state_t* state, const char* key) {
    settings_frame(NULL, state);
    recti_t rect;
    TEST_ASSERT_TRUE(ui_probe_widget(key, &rect));
    input_t input = {.point = {rect.x + rect.w / 2, rect.y + rect.h / 2}, .down = true, .pressed = true};
    input.press = input.point;
    for (int frame = 0; frame < TOUCH_HOLD_FRAMES; frame++) {
        settings_frame(&input, state);
        input.pressed = false;
    }
    input.down = false;
    input.released = true;
    settings_frame(&input, state);
    settings_frame(NULL, state);
}

static void
test_settings_reset_requests_reset(void) {
    fixture();
    settings_state_t state = {0};
    tap(&state, "RESET");
    TEST_ASSERT_TRUE(state.reset_requested);
    TEST_ASSERT_FALSE(state.muted);
}

static void
test_settings_mute_toggles_the_callers_value(void) {
    fixture();
    settings_state_t state = {0};
    tap(&state, "MUTE");
    TEST_ASSERT_TRUE(state.muted);
    tap(&state, "MUTE");
    TEST_ASSERT_FALSE(state.muted);
    TEST_ASSERT_FALSE(state.reset_requested);
}
#endif

void
run_screen_examples_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_settings_reset_requests_reset);
    RUN_TEST(test_settings_mute_toggles_the_callers_value);
#endif
}

SUITE_REGISTER(run_screen_examples_suite);

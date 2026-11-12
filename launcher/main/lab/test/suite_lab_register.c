/*
 * Portable suite: what LAB_REGISTER() and LAB_APP_CALLBACKS() give an app.
 * Each app_t callback reaches its own lab step, so a scene sees enter, frame,
 * update and exit in the slot it was called through, and the console word
 * comes back in capitals as the reply tag.
 */

#include "suites.h"
#include "unity.h"

#include "app/app.h"
#include "input/input.h"
#include "lab/lab.h"

#define FRAME_DT_MS  16
#define UPDATE_DT_MS 33

typedef struct {
    int enters;
    int frames;
    int updates;
    int exits;
    uint32_t frame_dt_ms;
    uint32_t update_dt_ms;
} calls_t;

static calls_t calls;

static void
probe_enter(const lab_scene_t* self) {
    (void)self;
    calls.enters++;
}

static void
probe_frame(uint32_t dt_ms) {
    calls.frames++;
    calls.frame_dt_ms = dt_ms;
}

static void
probe_update(uint32_t dt_ms) {
    calls.updates++;
    calls.update_dt_ms = dt_ms;
}

static void
probe_exit(void) {
    calls.exits++;
}

static const lab_scene_t probe_scene = {
    .key = "probe",
    .name = "Probe",
    .enter = probe_enter,
    .frame = probe_frame,
    .update = probe_update,
    .exit = probe_exit,
};

static const lab_scene_t* const probe_scenes[] = {&probe_scene};

LAB_VIEW(probe_view, "lab_register_probe");
static lab_t probe_lab = {
    .view = &probe_view,
    .console_word = "probe",
    .scenes = probe_scenes,
    .scene_count = 1,
};
LAB_REGISTER(probe_lab);
static const app_t probe_app = {.name = "Probe Lab", LAB_APP_CALLBACKS(probe_lab)};

/* Each callback lands in its own step, once, with the time it was given. */
static void
test_each_callback_reaches_its_own_step(void) {
    const input_t input = {0};
    calls = (calls_t){0};

    probe_app.enter();
    TEST_ASSERT_EQUAL_INT(1, calls.enters);
    TEST_ASSERT_EQUAL_INT(0, calls.frames + calls.updates + calls.exits);

    probe_app.update(UPDATE_DT_MS, &input);
    TEST_ASSERT_EQUAL_INT(1, calls.updates);
    TEST_ASSERT_EQUAL_INT(0, calls.frames);
    TEST_ASSERT_EQUAL_UINT32(UPDATE_DT_MS, calls.update_dt_ms);

    probe_app.frame(FRAME_DT_MS, &input);
    TEST_ASSERT_EQUAL_INT(1, calls.frames);
    TEST_ASSERT_EQUAL_INT(1, calls.updates);
    TEST_ASSERT_EQUAL_UINT32(FRAME_DT_MS, calls.frame_dt_ms);

    probe_app.exit();
    TEST_ASSERT_EQUAL_INT(1, calls.exits);
    TEST_ASSERT_EQUAL_INT(1, calls.enters);
}

/* Registration, before any entry, sets the capitalised reply tag. */
static void
test_registration_sets_the_reply_tag(void) {
    TEST_ASSERT_EQUAL_STRING("PROBE", lab_reply_tag(&probe_lab));
}

void
run_lab_register_suite(void) {
    RUN_TEST(test_registration_sets_the_reply_tag);
    RUN_TEST(test_each_callback_reaches_its_own_step);
}

SUITE_REGISTER(run_lab_register_suite);

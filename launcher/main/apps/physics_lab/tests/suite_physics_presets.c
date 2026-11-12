#include "apps/physics_lab/physics_presets.h"
#include "suites.h"
#include "unity.h"
#ifdef DEVICE_BUILD
#include "apps/physics_lab/physics_lab.h"
#include "display/display.h"
#include "frame_profile.h"
#include "lab/test/lab_test.h"
#include "testkit/cleanup.h"

/* The tag each preset's frame_profile line carries. */
#define TAG "physics_presets"

static lab_test_t saved;

static inline void
release_fixture(void) {
    lab_test_exit(&saved);
}

static inline void
profile_preset(const char* key, uint32_t duration_ms) {
    lab_test_enter(&saved, &physics_lab, key);
    suite_set_test_cleanup(release_fixture);
    const frame_profile_scenario_t scenario = {key, (int64_t)duration_ms * MATH_US_PER_MS};
    int64_t remainder = 0;
    const frame_profile_t profile = frame_profile_play(&scenario, lab_test_profile_frame, &remainder, NULL);
    frame_profile_log(TAG, key, &profile, display_quarter_name(display_quarter_now()));
    TEST_ASSERT_GREATER_THAN_INT(0, profile.frames);
    release_fixture();
    suite_clear_test_cleanup();
}

PHYSICS_PRESETS(PHYSICS_PRESET_CASE)
#endif

static void
fixture(void) {}

static void
test_the_scene_table_holds_the_stage_and_each_preset(void) {
    fixture();
#ifdef DEVICE_BUILD
    TEST_ASSERT_EQUAL_INT(1 + PHYSICS_PRESET_COUNT, physics_lab.scene_count);
#endif
}

void
run_physics_presets_suite(void) {
#ifdef DEVICE_BUILD
    PHYSICS_PRESETS(PHYSICS_PRESET_RUN_CASE)
#endif
    RUN_TEST(test_the_scene_table_holds_the_stage_and_each_preset);
}

SUITE_REGISTER(run_physics_presets_suite);

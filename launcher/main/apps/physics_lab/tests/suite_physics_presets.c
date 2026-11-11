#include "apps/physics_lab/physics_presets.h"
#include "suites.h"
#include "unity.h"
#ifdef DEVICE_BUILD
#include "apps/physics_lab/physics_lab.h"
#include "core/timing.h"
#include "frame_profile.h"
#include "lab/test/lab_test.h"
#include "testkit/cleanup.h"

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
    frame_profile_log("physics_presets", key, &profile, "portrait");
    TEST_ASSERT_GREATER_THAN_INT(0, profile.frames);
    release_fixture();
    suite_clear_test_cleanup();
}

PHYSICS_PRESETS(PHYSICS_PRESET_CASE)
#endif

static void
fixture(void) {}

static void
test_preset_count_matches_scene_table(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(0 PHYSICS_PRESETS(PHYSICS_PRESET_COUNT_ROW), PHYSICS_PRESET_COUNT);
#ifdef DEVICE_BUILD
    TEST_ASSERT_EQUAL_INT(1 + PHYSICS_PRESET_COUNT, physics_lab.scene_count);
#endif
}

void
run_physics_presets_suite(void) {
#ifdef DEVICE_BUILD
    PHYSICS_PRESETS(PHYSICS_PRESET_RUN_CASE)
#endif
    RUN_TEST(test_preset_count_matches_scene_table);
}

SUITE_REGISTER(run_physics_presets_suite);

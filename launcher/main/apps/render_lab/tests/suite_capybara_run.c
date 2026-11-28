#include "suites.h"
#include "unity.h"

#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/capybara_sponza/capybara_run.h"
#include "apps/render_lab/tests/scene_suite.h"
#include "scene/scene.h"
#include "testkit/cleanup.h"

static scene_suite_t loaded;

static void
release_fixture(void) {
    scene_suite_release(&loaded);
}

static void
test_capybara_gallop_ground_speed_follows_instance_scale(void) {
    loaded = scene_suite_load(CAPYBARA_SCENE);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_FALSE(handle_is_none(loaded.scene));
    const entity_t runner = entity_find(loaded.scene, name_hash(CAPYBARA_SUBJECT));
    const float gallop_mps = capybara_run_gallop_mps(runner);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, gallop_mps);
    const float half_scale = 0.5F;
    const motion_profile_t hero = capybara_run_profile(gallop_mps, 1.0F);
    const motion_profile_t smaller = capybara_run_profile(gallop_mps, half_scale);
    const float cruise_s = hero.rest_s + hero.ramp_s;
    TEST_ASSERT_EQUAL_FLOAT(gallop_mps * CAPYBARA_RUN_CM_PER_M, motion_profile_at(&hero, cruise_s).speed);
    TEST_ASSERT_EQUAL_FLOAT(gallop_mps * half_scale * CAPYBARA_RUN_CM_PER_M,
                            motion_profile_at(&smaller, cruise_s).speed);
}

void
run_capybara_run_suite(void) {
    RUN_TEST(test_capybara_gallop_ground_speed_follows_instance_scale);
}

SUITE_REGISTER(run_capybara_run_suite);
SUITE_READS(run_capybara_run_suite, CAPYBARA_SCENE);

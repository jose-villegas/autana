#include "suites.h"
#include "unity.h"

#include "app/app_arena.h"
#include "apps/render_lab/capybara_content.h"
#include "apps/render_lab/capybara_sponza/capybara_run.h"
#include "scene/scene.h"
#include "testkit/cleanup.h"

static entity_t loaded;
/* The app arena where the scene keeps its blocks, as it stood before the load. */
static size_t mark;

static void
release_fixture(void) {
    scene_release(loaded);
    app_arena_rewind(mark);
}

static void
test_capybara_gallop_ground_speed_follows_instance_scale(void) {
    mark = app_arena_mark();
    loaded = scene_load(CAPYBARA_SCENE);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, scene_state(loaded));
    const entity_t runner = entity_find(loaded, name_hash(CAPYBARA_SUBJECT));
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

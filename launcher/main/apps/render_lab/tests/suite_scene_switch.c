/* Device suite: scene requests applied after the shell composes an expanded frame. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include "test_cleanup.h"
#include "unity.h"

#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "scene/scene_shell.h"

static render_lab_test_t saved;

static void
release_fixture(void) {
    render_lab_test_exit(&saved);
}

static void
fixture(void) {
    render_lab_test_enter(&saved, "sponza");
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_TRUE(scene_has_active_camera());
    TEST_ASSERT_NOT_NULL(app_render_lab.console);
}

static void
test_console_scene_requests_survive_expanded_frames(void) {
    fixture();

    static const struct {
        const char* request;
        const char* key;
    } switches[] = {
        {"scene sponza-lite", "sponza-lite"}, {"scene sponza", "sponza"}, {"scene sponza-flat", "sponza-flat"},
        {"scene sponza-lite", "sponza-lite"}, {"scene sponza", "sponza"},
    };

    render_lab_test_frame(false, NULL);
    TEST_ASSERT_FALSE(app_render_lab.console->handle("clips"));
    TEST_ASSERT_FALSE(app_render_lab.console->handle("clip gallop"));
    TEST_ASSERT_FALSE(app_render_lab.console->handle("unknown"));
    for (size_t i = 0; i < sizeof switches / sizeof switches[0]; i++) {
        TEST_ASSERT_TRUE(app_render_lab.console->handle(switches[i].request));
        render_lab_test_frame(true, NULL);
        TEST_ASSERT_EQUAL_STRING(switches[i].key, render_lab_start_scene_key);
        TEST_ASSERT_TRUE(scene_has_active_camera());
        render_lab_test_frame(false, NULL);
    }
}

static void
test_scene_families_list_their_variants_with_fitted_full_first(void) {
    const char* const* names = NULL;
    int selected = 0;
    const int sponza = render_lab_family_variants("Sponza", &names, &selected);
    TEST_ASSERT_GREATER_THAN_INT(1, sponza);
    TEST_ASSERT_EQUAL_STRING("Fitted Full", names[0]);
    TEST_ASSERT_EQUAL_INT(1, render_lab_family_variants("Capybara", &names, &selected));
    TEST_ASSERT_EQUAL_INT(0, render_lab_family_variants("Nothing", &names, &selected));
    TEST_ASSERT_FALSE(render_lab_request_variant("Sponza", sponza));
    TEST_ASSERT_FALSE(render_lab_request_variant("Nothing", 0));
}

#endif

void
run_render_lab_scene_switch_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_console_scene_requests_survive_expanded_frames);
    RUN_TEST(test_scene_families_list_their_variants_with_fitted_full_first);
#endif
}

SUITE_REGISTER(run_render_lab_scene_switch_suite);

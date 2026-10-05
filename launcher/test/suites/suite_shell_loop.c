/*
 * Device suite: the frame loop's engine systems as the board builds them.
 * The scene manager is registered with its own functions in each phase, and
 * a system asking for an overlapped present gets the overlap path even for
 * an app without update().
 */
#include "suites.h"
#include "unity.h"

#include <string.h>

#include "app_arena.h"
#include "scene/scene_shell.h"
#include "shell/shell_system.h"

#ifdef DEVICE_BUILD
void shell_test_fixture(void);
bool shell_test_a_system_asking_for_overlap_takes_the_overlap_path(void);

static void
fixture(void) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, app_arena_mark(), "the app running the suites holds arena memory");
    shell_test_fixture();
}

static const shell_system_t*
registered(const char* name) {
    shell_system_t* const list = shell_system_swap_for_test(NULL);
    shell_system_swap_for_test(list);
    for (const shell_system_t* system = list; system != NULL; system = system->next) {
        if (strcmp(system->name, name) == 0) {
            return system;
        }
    }
    return NULL;
}

static void
test_the_scene_manager_is_registered_with_its_own_function_in_each_phase(void) {
    fixture();
    const shell_system_t* scene = registered("scene");
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, "no system named scene is registered");
    TEST_ASSERT_EQUAL_INT(SHELL_ORDER_SCENE, scene->order);

    const struct {
        const char* phase;
        bool wired;
    } phases[] = {
        {"update is scene_shell_render", scene->update == scene_shell_render},
        {"compose is scene_shell_compose", scene->compose == scene_shell_compose},
        {"app_exit is scene_unload_all", scene->app_exit == scene_unload_all},
        {"overlaps_present is scene_has_active_camera", scene->overlaps_present == scene_has_active_camera},
    };

    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(phases[i].wired, phases[i].phase);
    }
}

static void
test_a_system_asking_for_an_overlapped_present_gets_the_overlap_path(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_a_system_asking_for_overlap_takes_the_overlap_path());
}
#endif

void
run_shell_loop_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_the_scene_manager_is_registered_with_its_own_function_in_each_phase);
    RUN_TEST(test_a_system_asking_for_an_overlapped_present_gets_the_overlap_path);
#endif
}

SUITE_REGISTER(run_shell_loop_suite);

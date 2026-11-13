/*
 * Portable suite: a scene path with its own seek (lab_path_t.seek). The lab
 * reads its length from the path, wraps a seek to it, and hands "pose <ms>"
 * and "pose play" to the scene's hooks, not to an animator.
 */

#include "suites.h"
#include "unity.h"

#include "app/app.h"
#include "lab/lab.h"

#define PATH_MS 18000u
#define LATER   5000u

typedef struct {
    int seeks;
    uint32_t t_ms;
    int plays;
} hook_calls_t;

static hook_calls_t hooks;

static bool
probe_seek(uint32_t t_ms) {
    hooks.seeks++;
    hooks.t_ms = t_ms;
    return true;
}

static void
probe_play(void) {
    hooks.plays++;
}

static lab_path_t
probe_path(void) {
    return (lab_path_t){
        .scene = ENTITY_NONE, .entity = ENTITY_NONE, .seek = probe_seek, .play = probe_play, .duration_ms = PATH_MS};
}

static const lab_scene_t walking_scene = {.key = "walking", .name = "Walking", .path = probe_path};
static const lab_scene_t* const walking_scenes[] = {&walking_scene};

LAB_VIEW(walking_view, "lab_path_probe");
static lab_t walking_lab = {
    .view = &walking_view,
    .console_word = "walking",
    .scenes = walking_scenes,
    .scene_count = 1,
};
LAB_REGISTER(walking_lab);
/* LAB_REGISTER's callbacks go somewhere; these tests reach the lab only through its console and path. */
static const app_t walking_app = {.name = "Walking Lab", LAB_APP_CALLBACKS(walking_lab)};

static void
test_a_seek_path_has_its_own_length(void) {
    TEST_ASSERT_EQUAL_UINT32(PATH_MS, lab_path_duration_ms(&walking_lab));
}

static void
test_a_seek_wraps_to_the_path_and_reaches_the_scene(void) {
    hooks = (hook_calls_t){0};
    TEST_ASSERT_TRUE(lab_seek_path(&walking_lab, PATH_MS + LATER));
    TEST_ASSERT_EQUAL_INT(1, hooks.seeks);
    TEST_ASSERT_EQUAL_UINT32(LATER, hooks.t_ms);
}

static void
test_pose_and_play_reach_the_scene(void) {
    hooks = (hook_calls_t){0};
    TEST_ASSERT_TRUE(lab_console(&walking_lab, "pose 5000"));
    TEST_ASSERT_EQUAL_INT(1, hooks.seeks);
    TEST_ASSERT_EQUAL_UINT32(LATER, hooks.t_ms);
    TEST_ASSERT_TRUE(lab_console(&walking_lab, "pose play"));
    TEST_ASSERT_EQUAL_INT(1, hooks.plays);
}

void
run_lab_path_suite(void) {
    (void)walking_app;
    RUN_TEST(test_a_seek_path_has_its_own_length);
    RUN_TEST(test_a_seek_wraps_to_the_path_and_reaches_the_scene);
    RUN_TEST(test_pose_and_play_reach_the_scene);
}

SUITE_REGISTER(run_lab_path_suite);

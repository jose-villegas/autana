/*
 * Device-only suite: render_lab's use of the app arena, through its real
 * enter, frames, scene switch and exit - each scene starts from the same
 * spot lap after lap, exit gives back all it took, and the path tracer
 * clears what the arena hands it and survives an arena with nothing left.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "app/app.h"
#include "app/app_arena.h"
#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/rt_path.h"
#include "gfx/gfx.h"
#include "ui/ui.h"

extern app_t app_render_lab;
extern const char* render_lab_start_scene_key;
extern const render_lab_scene_t scene_pathtrace;
extern void render_lab_enter(void);
extern void render_lab_exit(void);
extern void render_lab_test_next_scene(void);

#define SCENE_COUNT_MAX  16
#define LAPS             6 /* enough that a switch keeping its memory runs the arena dry */
#define FRAMES_PER_SCENE 3
#define FRAMES_MAX       2000
#define ACCUM_BYTES      (sizeof(rt_path_accum_px_t) * (size_t)GFX_WIDTH * GFX_HEIGHT)

static const char* saved_start_key;
static size_t suite_mark;

static void
enter_on_pathtrace(void) {
    ui_init(); /* the HUD a frame draws needs microui's text metrics */
    saved_start_key = render_lab_start_scene_key;
    render_lab_start_scene_key = scene_pathtrace.key;
    suite_mark = app_arena_mark();
}

static void
leave(void) {
    render_lab_exit();
    render_lab_start_scene_key = saved_start_key;
}

static bool
on_pathtrace(void) {
    return strcmp(render_lab_start_scene_key, scene_pathtrace.key) == 0;
}

static void
run_frames(int count) {
    const input_t no_input = {0};
    for (int i = 0; i < count; i++) {
        app_render_lab.frame(16, &no_input);
    }
}

/* Asserts only after leave(): a failure mid-lap would otherwise leave the
 * app entered for every suite after this one. */
static void
test_every_lap_of_scene_switches_starts_each_scene_at_the_same_arena_use(void) {
    enter_on_pathtrace();
    render_lab_enter();

    size_t first_lap[SCENE_COUNT_MAX];
    int scene_count = 0;
    int laps_that_differ = 0;
    size_t pathtrace_use = 0;
    for (int lap = 0; lap < LAPS; lap++) {
        pathtrace_use = app_arena_mark() - suite_mark;
        bool differs = false;
        int scene = 0;
        do {
            if (lap == 0) {
                first_lap[scene] = app_arena_mark();
            } else {
                differs |= first_lap[scene] != app_arena_mark();
            }
            run_frames(FRAMES_PER_SCENE);
            render_lab_test_next_scene();
            scene++;
        } while (!on_pathtrace() && scene < SCENE_COUNT_MAX);
        if (lap == 0) {
            scene_count = scene;
        }
        laps_that_differ += differs || scene != scene_count;
    }
    leave();
    const size_t after_exit = app_arena_mark();

    TEST_ASSERT_LESS_THAN_INT(SCENE_COUNT_MAX, scene_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, laps_that_differ, "a lap started a scene at a different arena use");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT_MESSAGE(ACCUM_BYTES, pathtrace_use,
                                              "the path tracer lost its accumulator on a late lap");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(suite_mark, after_exit, "exit kept arena memory");
}

static bool
all_zero(const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

static void
test_the_path_tracer_clears_what_the_arena_hands_it(void) {
    enter_on_pathtrace();
    unsigned char* garbage = app_arena_take(ACCUM_BYTES, _Alignof(rt_path_accum_px_t));
    TEST_ASSERT_NOT_NULL(garbage);
    memset(garbage, 0xA5, ACCUM_BYTES);
    app_arena_rewind(suite_mark);

    render_lab_enter();
    const bool cleared = all_zero(garbage, ACCUM_BYTES);
    leave();

    TEST_ASSERT_TRUE_MESSAGE(cleared, "the accumulator started on the arena's old contents");
}

static void
test_the_path_tracer_runs_direct_only_on_an_exhausted_arena(void) {
    enter_on_pathtrace();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES - suite_mark, 1));
    render_lab_enter();

    const char* status = scene_pathtrace.status();
    for (int frame = 0; frame < FRAMES_MAX && strncmp(status, "direct only", 11) != 0; frame++) {
        scene_pathtrace.frame(1, false);
        status = scene_pathtrace.status();
    }
    leave();
    app_arena_rewind(suite_mark);

    TEST_ASSERT_EQUAL_STRING_LEN("direct only", status, 11);
}

void
run_render_lab_arena_suite(void) {
    RUN_TEST(test_every_lap_of_scene_switches_starts_each_scene_at_the_same_arena_use);
    RUN_TEST(test_the_path_tracer_clears_what_the_arena_hands_it);
    RUN_TEST(test_the_path_tracer_runs_direct_only_on_an_exhausted_arena);
}

#else

void
run_render_lab_arena_suite(void) {}

#endif

SUITE_REGISTER(run_render_lab_arena_suite);

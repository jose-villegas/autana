/* lab_test: device suites' fixtures for a lab app (lab/lab.h): enter it on a
 * scene, then draw composed, presented frames as the shell would. */
#pragma once
#ifdef DEVICE_BUILD
#include <stdbool.h>
#include <stdint.h>
#include "app/app_console.h"
#include "input/input.h"
#include "lab/lab.h"
#include "scene/scene.h"

#define LAB_TEST_FRAME_MS 16

/* What lab_test_enter() changed, put back by lab_test_exit(). */
typedef struct {
    lab_t* lab;
    const char* scene;
    const char* camera;
    bool hud;
} lab_test_t;

/* The READY root of the loaded scene `key`, or ENTITY_NONE. */
entity_t lab_test_loaded_scene(const char* key);
/* Enters `lab`'s app on `scene` with no HUD, holding the default quarter;
 * the frames below draw it until lab_test_exit(). */
void lab_test_enter(lab_test_t* saved, lab_t* lab, const char* scene);
void lab_test_exit(const lab_test_t* saved);
/* One frame at `dt_ms`: the app's update, the scene render, the compose and
 * the app's frame, presented. Returns the render's microseconds. */
int64_t lab_test_frame_dt(uint32_t dt_ms, bool switching, const input_t* input);

/* A frame_profile_frame_fn: advances by wall microseconds; ctx points to
 * the carried sub-millisecond remainder, initially zero. */
int64_t lab_test_profile_frame(void* ctx, int64_t dt_us);

/* Inline, so it adds no stack frame to the deepest test chains. */
static inline int64_t
lab_test_frame(bool switching, const input_t* input) {
    return lab_test_frame_dt(LAB_TEST_FRAME_MS, switching, input);
}
#endif

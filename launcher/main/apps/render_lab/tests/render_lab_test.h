/* render_lab_test: app fixtures and the composed, presented frame for device suites. */
#pragma once
#ifdef DEVICE_BUILD
#include <stdbool.h>
#include <stdint.h>
#include "app/app.h"
#include "console/console_verbs.h"
#include "scene/scene.h"

#define RENDER_LAB_TEST_FRAME_MS 16
#define RENDER_LAB_TEST_SCALE    50

typedef struct {
    const char* scene;
    const char* camera;
    bool hud;
} render_lab_test_t;

extern app_t app_render_lab;
scene_t* render_lab_test_loaded_scene(const char* key);
bool scene_capybara_test_reply(const char* args, console_reply_fn reply);
void render_lab_test_enter(render_lab_test_t* saved, const char* scene);
void render_lab_test_exit(const render_lab_test_t* saved);
int64_t render_lab_test_frame(bool switching);
#endif

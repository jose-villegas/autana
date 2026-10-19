/* render_lab_clips: scene playback choices shared by menu and console. */
#pragma once
#include "app/app_console.h"
#include "scene/scene.h"

#define RENDER_LAB_BLEND_DEFAULT_MS             250
#define RENDER_LAB_BLEND_MAX_MS                 1000
#define RENDER_LAB_BLEND_STEP_MS                50
#define RENDER_LAB_CLIPS_STRINGIFY_VALUE(value) #value
#define RENDER_LAB_CLIPS_STRINGIFY(value)       RENDER_LAB_CLIPS_STRINGIFY_VALUE(value)
#define RENDER_LAB_BLEND_TEXT_SIZE              sizeof(RENDER_LAB_CLIPS_STRINGIFY(RENDER_LAB_BLEND_MAX_MS))

typedef struct {
    scene_t* scene;
    scene_entity_t subject;
    const char** items;
    int count;
    int selected;
    int blend_ms;
} render_lab_clips_t;

bool render_lab_clips_init(render_lab_clips_t* choice, scene_t* scene, scene_entity_t subject,
                           const char* default_clip);
void render_lab_clips_destroy(render_lab_clips_t* choice);
bool render_lab_clips_reply(render_lab_clips_t* choice, const char* args, console_reply_fn reply);
/* The clip picker and blend slider, as menu rows `width` wide. */
void render_lab_clips_draw(int width, render_lab_clips_t* choice);

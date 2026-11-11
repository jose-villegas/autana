/* lab_clips: scene playback choices shared by menu and console. */
#pragma once
#include "app/app_console.h"
#include "scene/scene.h"

#define LAB_CLIPS_BLEND_DEFAULT_MS       250
#define LAB_CLIPS_BLEND_MAX_MS           2000
#define LAB_CLIPS_BLEND_STEP_MS          50
#define LAB_CLIPS_STRINGIFY_VALUE(value) #value
#define LAB_CLIPS_STRINGIFY(value)       LAB_CLIPS_STRINGIFY_VALUE(value)
#define LAB_CLIPS_BLEND_TEXT_SIZE        sizeof(LAB_CLIPS_STRINGIFY(LAB_CLIPS_BLEND_MAX_MS))

typedef struct {
    entity_t scene;
    entity_t subject;
    const char** items;
    int count;
    int selected;
    int blend_ms;
} lab_clips_t;

bool lab_clips_init(lab_clips_t* choice, entity_t scene, entity_t subject, const char* default_clip);
void lab_clips_destroy(lab_clips_t* choice);
bool lab_clips_reply(lab_clips_t* choice, const char* args, console_reply_fn reply);
/* The clip picker and blend slider, as menu rows `width` wide. */
void lab_clips_draw(int width, lab_clips_t* choice);

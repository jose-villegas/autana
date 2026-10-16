/* render_lab_clips: scene playback choices shared by menu and console. */
#pragma once
#include "console/console_verbs.h"
#include "scene/scene.h"
#include "ui/layout/ui_scroll.h"
#include "ui/ui_mu.h"

#define RENDER_LAB_BLEND_DEFAULT_MS             250
#define RENDER_LAB_BLEND_MAX_MS                 1000
#define RENDER_LAB_BLEND_STEP_MS                50
#define RENDER_LAB_CLIPS_STRINGIFY_VALUE(value) #value
#define RENDER_LAB_CLIPS_STRINGIFY(value)       RENDER_LAB_CLIPS_STRINGIFY_VALUE(value)
#define RENDER_LAB_BLEND_TEXT_SIZE              sizeof(RENDER_LAB_CLIPS_STRINGIFY(RENDER_LAB_BLEND_MAX_MS))

typedef struct {
    scene_t* scene;
    scene_entity_t subject;
    ui_dropdown_item_t* items;
    int count;
    int selected;
    int blend_ms;
} render_lab_clips_t;

bool render_lab_clips_init(render_lab_clips_t* choice, scene_t* scene, scene_entity_t subject,
                           const char* default_clip);
void render_lab_clips_destroy(render_lab_clips_t* choice);
bool render_lab_clips_reply(render_lab_clips_t* choice, const char* args, console_reply_fn reply);
void render_lab_clips_draw(mu_Context* ctx, ui_flow_t* flow, int width, render_lab_clips_t* choice);

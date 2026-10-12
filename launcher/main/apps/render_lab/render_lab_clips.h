/* render_lab_clips: scene playback choices shared by menu and console. */
#pragma once
#include "scene/scene.h"
#include "ui/ui_scroll.h"

#define RENDER_LAB_BLEND_DEFAULT_MS 250
#define RENDER_LAB_BLEND_MAX_MS     1000
#define RENDER_LAB_BLEND_STEP_MS    50

typedef struct {
    int selected;
    int blend_ms;
} render_lab_clips_t;

int render_lab_clips_find(const scene_t* scene, scene_entity_t subject, const char* name);
bool render_lab_clips_play(scene_t* scene, scene_entity_t subject, render_lab_clips_t* choice, int clip, int blend_ms);
bool render_lab_clips_console(scene_t* scene, scene_entity_t subject, render_lab_clips_t* choice, const char* args);
void render_lab_clips_draw(mu_Context* ctx, ui_flow_t* flow, int width, scene_t* scene, scene_entity_t subject,
                           render_lab_clips_t* choice);

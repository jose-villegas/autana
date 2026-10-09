/* render_lab_scene: a mesh scene hosted behind the shared HUD and menu. */
#pragma once

#include <stdint.h>

typedef struct {
    const char* name;
    const char* key;
    void (*enter)(void);
    void (*frame)(uint32_t dt_ms);
    void (*exit)(void);
    const char* (*status)(void);
} render_lab_scene_t;

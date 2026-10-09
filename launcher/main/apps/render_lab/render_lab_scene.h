/* render_lab_scene: a mesh scene hosted behind the shared HUD and menu. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char* name;
    const char* key;
    void (*enter)(void);
    void (*frame)(uint32_t dt_ms);
    void (*exit)(void);
    void (*invalidate)(void);
    const char* (*status)(void);
    bool shows_view_modes;
} render_lab_scene_t;

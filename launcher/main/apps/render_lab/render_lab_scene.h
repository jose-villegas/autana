/* render_lab_scene: a mesh scene hosted behind the shared HUD and menu. */
#pragma once

#include <stdint.h>

#include "apps/render_lab/ui/render_lab_menu_screen.h"
#include "console/console_verbs.h"
#include "input/input.h"

typedef struct {
    const char* name;
    const char* key;
    void (*enter)(void);
    void (*frame)(uint32_t dt_ms);
    void (*exit)(void);
    const char* (*status)(void);

    /* Optional. Runs while the previous frame is still being sent to the
     * panel, so it may only touch the scene's own memory; never gfx or the
     * framebuffer (app.h's update() contract). Not called while the menu is
     * open. */
    void (*update)(uint32_t dt_ms);

    /* Optional. The frame's touch, just before update() and under the same
     * rules: for a scene the finger steers. Not called while the menu is open. */
    void (*steer)(uint32_t dt_ms, const input_t* input);
    /* Optional. Drawn in the BOOT menu below NEXT SCENE while the scene is
     * paused; may call public scene_* calls only. */
    render_lab_controls_fn controls;
    /* Optional. Handles `render <args>` that Render Lab's scene commands did not;
     * false means the command is not this scene's. */
    bool (*console)(const char* args, console_reply_fn reply);
} render_lab_scene_t;

/* render_lab_scene: a mesh scene hosted behind the shared HUD and menu. */
#pragma once

#include <stdint.h>

#include "app/app_console.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"
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

extern const render_lab_scene_t scene_sponza;
extern const render_lab_scene_t scene_capybara;
extern const render_lab_scene_t scene_sponza_lite;
extern const render_lab_scene_t scene_sponza_flat;
extern const render_lab_scene_t scene_sponza_fitted;
extern const render_lab_scene_t scene_sponza_fitted_full;
extern const render_lab_scene_t scene_sponza_flat_fitted;

/* A scene's `.console`: the hook in a development build, NULL otherwise, as
 * APP_CONSOLE_PTR() does, so a release links none of the console. Declare
 * the hook static inline so a release draws no unused-function warning. */
#if CONFIG_LAUNCHER_DEVELOPMENT
#define RENDER_LAB_SCENE_CONSOLE(hook) (hook)
#else
#define RENDER_LAB_SCENE_CONSOLE(hook) NULL
#endif

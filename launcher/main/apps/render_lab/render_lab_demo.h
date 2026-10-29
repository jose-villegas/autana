/* render_lab_demo: a mesh demo hosted behind the shared HUD and menu. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/app_console.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"
#include "input/input.h"

typedef struct render_lab_demo {
    const char* name; /* the HUD title */
    const char* key;
    /* The Scene dropdown's entry; demos of one family are listed together.
     * variant names this demo in the menu's VARIANT dropdown, drawn when
     * the family has more than one demo (then every one needs it).
     * name, the HUD title, is family and variant spelled out. */
    const char* family;
    const char* variant;
    /* Receives its own demo, so one enter can serve several demos through `data`. */
    void (*enter)(const struct render_lab_demo* self);
    void (*frame)(uint32_t dt_ms);
    void (*exit)(void);
    const char* (*status)(void);
    /* Optional. What `enter` reads from its demo. */
    const void* data;

    /* Optional. Runs while the previous frame is still being sent to the
     * panel, so it may only touch the demo's own memory; never gfx or the
     * framebuffer (app.h's update() contract). Not called while the menu is
     * open. */
    void (*update)(uint32_t dt_ms);

    /* Optional. The frame's touch, just before update() and under the same
     * rules: for a demo the finger steers. Not called while the menu is open. */
    void (*steer)(uint32_t dt_ms, const input_t* input);
    /* Optional. Drawn in the BOOT menu below the Scene row while the demo is
     * paused; may call public scene_* calls only. */
    render_lab_controls_fn controls;
    /* Optional. Handles `render <args>` that Render Lab's demo commands did not;
     * false means the command is not this demo's. */
    bool (*console)(const char* args, console_reply_fn reply);
} render_lab_demo_t;

/* A demo that could not load: logs `what` under `tag` and keeps it in
 * `failure` (`size` bytes) for the status line, in place of its stats. */
void render_lab_demo_fail(const char* tag, char* failure, size_t size, const char* what);

extern const render_lab_demo_t demo_capybara;
extern const render_lab_demo_t demo_water;

/* A demo's `.console`: the hook in a development or host build, NULL otherwise,
 * as APP_CONSOLE_PTR() does, so a release links none of the console; a host
 * render drives it through render_lab_console(). Declare the hook static
 * inline so a release draws no unused-function warning. */
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
#define RENDER_LAB_DEMO_CONSOLE(hook) (hook)
#else
#define RENDER_LAB_DEMO_CONSOLE(hook) NULL
#endif

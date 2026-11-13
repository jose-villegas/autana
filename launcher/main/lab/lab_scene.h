/* lab_scene: one scene of a lab (lab.h), behind its shared HUD and menu. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/app_console.h"
#include "build/build_variant.h"
#include "input/input.h"
#include "lab/ui/lab_menu_screen.h"
#include "scene/scene.h"

/* The longest status line a scene's `status` hook returns, its NUL included. */
#define LAB_STATUS_LEN 48

/* What the scene plays along a clip: the entity animated, in its loaded scene. */
typedef struct {
    entity_t scene; /* ENTITY_NONE when the scene has not loaded */
    entity_t entity;
    const char* clip; /* the clip a frame profile plays; NULL keeps the entity's current one */
    /* Optional, for a path no one animator plays, such as copies walking: puts
     * the whole scene at t_ms (below duration_ms) and holds it there until
     * play() lets it move again; a path with seek has play too. With it,
     * entity and clip are not read. */
    bool (*seek)(uint32_t t_ms);
    void (*play)(void);
    uint32_t duration_ms; /* the seek path's length */
} lab_path_t;

typedef struct lab_scene {
    const char* name; /* the HUD title */
    const char* key;
    /* The Scene dropdown's entry; scenes of one family are listed together.
     * variant names this scene in the menu's VARIANT dropdown, drawn when
     * the family has more than one scene (then every one needs it).
     * name, the HUD title, is family and variant spelled out. */
    const char* family;
    const char* variant;
    /* Receives its own scene, so one enter can serve several scenes through `data`. */
    void (*enter)(const struct lab_scene* self);
    void (*frame)(uint32_t dt_ms);
    void (*exit)(void);
    /* Optional. The HUD's line under the fps reading; development and host builds only. */
    const char* (*status)(void);
    /* Optional. What `enter` reads from its scene. */
    const void* data;

    /* Optional. Runs while the previous frame is still being sent to the
     * panel, so it may only touch the scene's own memory; never gfx or the
     * framebuffer (app.h's update() contract). Not called while the menu is
     * open. */
    void (*update)(uint32_t dt_ms);

    /* Optional. The frame's touch, just before update() and under the same
     * rules: for a scene the finger steers. Not called while the menu is open. */
    void (*steer)(uint32_t dt_ms, const input_t* input);
    /* Optional. Drawn in the BOOT menu below the Scene row while the scene is
     * paused; may call public scene_* calls only. */
    lab_controls_fn controls;
    /* Whether frame generation suits the scene: true where nothing moves on
     * its own, so the camera's pose alone warps a frame. A lab may turn it
     * on for such a scene only. */
    bool generates_frames;
    /* Optional. Handles `<word> <args>` that the lab's own commands did not;
     * false means the command is not this scene's. */
    bool (*console)(const char* args, console_reply_fn reply);
    /* Optional. The clip a frame profile plays: its camera's flight, or the subject's motion. */
    lab_path_t (*path)(void);
} lab_scene_t;

/* A scene that could not load: logs `what` under `tag` and keeps it in
 * `failure` (`size` bytes) for the status line, in place of its stats. */
void lab_scene_fail(const char* tag, char* failure, size_t size, const char* what);

/* Loads the scene `id` and returns its READY root. Otherwise releases it,
 * fails as lab_scene_fail() with "no <label> pack: flash it" or
 * "<label> did not load", and returns ENTITY_NONE. */
entity_t lab_scene_load(const char* tag, char* failure, size_t size, const char* id, const char* label);

/* `bytes` from the app arena, which the lab rewinds when the scene goes; NULL when it is spent. */
void* lab_scene_take(size_t bytes);

/* A scene's numbered copies: each entity `format` (one %d, such as
 * "slot_%02d") names from 0 up to the first `scene` lacks, in order, in the
 * app arena, with their `count`; NULL when the arena is spent. */
entity_t* lab_scene_copies(entity_t scene, const char* format, int* count);

/* A scene's `.console` or `.status`: the hook in a development or host build,
 * NULL otherwise, as APP_CONSOLE_PTR() does, so a release links neither the
 * console nor the HUD's status line; a host render drives them as a device
 * does. Declare the hook static inline so a release draws no unused-function
 * warning. */
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#define LAB_SCENE_DEV(hook) (hook)
#else
#define LAB_SCENE_DEV(hook) NULL
#endif

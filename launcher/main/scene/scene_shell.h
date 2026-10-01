/*
 * scene_shell: what the shell does with scenes, as opposed to what an app
 * does (scene.h). Registering the generated tables, the two halves of a
 * frame and the unload when an app leaves. Apps never include this.
 *
 * scene_render() advances the clocks and draws into the raster's scratch
 * block, touching no framebuffer, so it may overlap the last frame's send.
 * scene_compose() upscales into the target, drawing first if scene_render()
 * did not run, and so waits for the framebuffer to be free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "scene/scene.h"

/* Makes a def loadable by name; runs before main(), like APP_REGISTER(). */
void scene_register(const scene_def_t* def);
#define SCENE_REGISTER(def)                                                                                            \
    __attribute__((constructor)) static void def##_register(void) { scene_register(&(def)); }

/* The picture a frame is composed into. NULL pixels: nothing to draw into,
 * as in band mode. */
typedef struct {
    uint16_t* pixels;
    int width, height;
} scene_target_t;

/* Whether a camera is active and the scene not paused. */
bool scene_has_active_camera(void);
void scene_render(uint32_t dt_ms, int quarter, const scene_target_t* target);
void scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target);

/* The two above on the panel's framebuffer and orientation. Compose marks the
 * framebuffer dirty and does nothing without an active camera. */
void scene_shell_render(uint32_t dt_ms);
void scene_shell_compose(uint32_t dt_ms);

/* An app has exited: unloads every scene, frees the raster's scratch and
 * lifts a pause. */
void scene_unload_all(void);

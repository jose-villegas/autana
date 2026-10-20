/*
 * scene_shell: what the shell does with scenes, as opposed to what an app
 * does (scene.h): the two halves of a frame and the unload when an app
 * leaves. Apps never include this.
 *
 * scene_render() advances the clocks and draws through the render context
 * (render/context/render_context.h), for the picture it will be upscaled
 * into. It names no
 * framebuffer, so it may overlap the last frame's send. scene_compose()
 * upscales into the target, drawing first if scene_render() did not run, and
 * so waits for the framebuffer to be free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/target/gfx_target.h"
#include "scene/scene.h"

/* The picture a frame is composed into. NULL pixels: nothing to upscale into,
 * as in band mode. */
typedef struct {
    uint16_t* pixels;
    int width, height;
    uint16_t* half_pixels; /* NULL disables expansion */
} scene_target_t;

/* Whether a camera needs a frame pass, including a paused expanded picture. */
bool scene_has_active_camera(void);
void scene_render(uint32_t dt_ms, int quarter, int width, int height);
bool scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target);

/* Rows of the picture into `target`, from the frame scene_render() drew
 * last: a picture kept nowhere, redrawn whenever its rows are asked for,
 * paused or not. False when there is no such frame. */
bool scene_compose_rows(gfx_target_t target);

/* The two above on the panel's pictures and orientation. Compose marks an
 * exact-half picture for expansion, otherwise the full framebuffer dirty;
 * it does nothing without an active camera. */
void scene_shell_render(uint32_t dt_ms);
void scene_shell_compose(uint32_t dt_ms);

/* Band mode's compose: rows [row0, row1) of the panel into `target`, or
 * black with no frame to show. */
void scene_shell_compose_band(int row0, int row1, gfx_color_t* target);

/* An app has exited: unloads every scene, releases the render context and
 * lifts a pause. */
void scene_unload_all(void);

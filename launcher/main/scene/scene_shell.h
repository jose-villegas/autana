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

#include "core/memory.h"
#include "scene/scene.h"

/* The picture a frame is composed into. NULL pixels: nothing to upscale into,
 * as in band mode. */
typedef struct {
    uint16_t* pixels;
    int width, height;
    uint16_t* half_pixels; /* NULL disables expansion */
    uint8_t* half_seams;   /* the half picture's seam mask, or NULL */
} scene_target_t;

/* Whether a camera needs a frame pass, including a paused expanded picture. */
bool scene_has_active_camera(void);
void scene_render(uint32_t dt_ms, int quarter, int width, int height);
bool scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target);

/* The two above on the panel's pictures and orientation. Compose marks an
 * exact-half picture for expansion, otherwise the full framebuffer dirty;
 * it does nothing without an active camera. */
void scene_shell_render(uint32_t dt_ms);
void scene_shell_compose(uint32_t dt_ms);

/* An app has exited: unloads every scene, releases the render context and
 * lifts a pause. */
void scene_unload_all(void);

/* Where the memory a loaded scene keeps in PSRAM comes from, the render
 * context's scratch included; NULL, the default, is the heap. Set while no
 * scene is loaded. With a take, scene_unload() gives none of it back: once
 * the last scene is unloaded nothing points into it, and the lender may
 * take it back. Internal-RAM pieces stay on the heap either way. Returns the
take it replaces. */
memory_take_fn scene_set_take(memory_take_fn take);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
typedef struct checkpoint checkpoint_t;
void scene_shell_checkpoint_capture(checkpoint_t* saved);
bool scene_shell_checkpoint_prepare(checkpoint_t* saved);
#endif

/*
 * render_settings: the quality and cost choices a render context draws with,
 * one value an app sets in one call (render_context_set_settings()). A
 * development override (TUNE render.cull and the others) goes on top of it
 * without writing into it.
 */
#pragma once

#include <stdbool.h>

/* Each default, then its tier and what it trades: docs/render/Render-Settings.md
 * reads these lines. Basic is for any app; advanced needs the renderer's
 * workings to judge. */
#define RENDER_SETTINGS_CULL      true /* basic: skips clusters and world boxes outside the picture */
#define RENDER_SETTINGS_DEPTH_FIT true /* advanced: depth steps go where the kept scene is, not to near_z; needs cull */
#define RENDER_SETTINGS_BATCHED   true /* advanced: one transform and one draw fork/join a batch, not an instance */

typedef struct {
    bool cull, depth_fit, batched;
} render_settings_t;

#define RENDER_SETTINGS_DEFAULT                                                                                        \
    ((render_settings_t){RENDER_SETTINGS_CULL, RENDER_SETTINGS_DEPTH_FIT, RENDER_SETTINGS_BATCHED})

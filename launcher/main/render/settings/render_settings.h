/*
 * render_settings: the quality and cost choices a render context draws with,
 * one value an app sets in one call (render_context_set_settings()). A
 * development override (TUNE render.cull and the others) goes on top of it
 * without writing into it.
 */
#pragma once

#include <stdbool.h>

#define RENDER_SETTINGS_CULL      true /* clusters and world boxes outside the picture are skipped */
#define RENDER_SETTINGS_DEPTH_FIT true /* depth is scaled to the nearest kept bound, not near_z */
#define RENDER_SETTINGS_BATCHED   true /* instances draw in batches, not one fork/join each */

typedef struct {
    bool cull, depth_fit, batched;
} render_settings_t;

#define RENDER_SETTINGS_DEFAULT                                                                                        \
    ((render_settings_t){RENDER_SETTINGS_CULL, RENDER_SETTINGS_DEPTH_FIT, RENDER_SETTINGS_BATCHED})

/*
 * render_lab_view: how a lit-mesh scene shows its frame, apart from
 * render_lab.h.
 */
#pragma once

#include <stdbool.h>

#include "render/context/render_context.h"

/* The selected render context view, RENDER_VIEW_SHADED by default. */
int render_lab_view(void);

/* The tunable render_lab.scale in hundredths: 200 renders at half size. */
int render_lab_scale(void);

/* The tunable render_lab.budget: 0 draws at render_lab.scale; otherwise
 * the milliseconds dynamic resolution holds a lit-mesh scene's draw to. */
int render_lab_budget_ms(void);

/* Whether the running scene honours it (render_lab_scene_t). */
bool render_lab_scene_shows_views(void);

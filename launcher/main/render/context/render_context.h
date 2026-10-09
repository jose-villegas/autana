/*
 * render_context: what a frame is drawn at and how well, apart from what is
 * drawn and from where. A caller hands it instances, a view and a clear
 * colour; the context owns the raster and its scratch block, the render size
 * and the debug view, and upscales into the picture it is given.
 *
 * The size is a fixed scale until a caller opts into dynamic resolution
 * (render/resolution/resolution.h): then each frame draws at a step chosen
 * to hold a budget, and what the draw and upscale cost is fed back.
 *
 * The engine draws its frames through one context, render_context_main(),
 * released when an app exits. The view stays perspective only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "render/r3d_instance.h"
#include "render/raster.h"
#include "render/render_view.h"
#include "render/resolution/resolution.h"

/* The share of the destination a frame draws at until set. */
#define RENDER_CONTEXT_DEFAULT_SCALE_PERCENT 50

/* The last frame: its step (-1 at the fixed scale), the size it drew at,
 * what culling kept and, under dynamic resolution only, what its draw (with
 * any census) and upscale cost: at the fixed scale nothing is timed. */
typedef struct {
    int step;
    int width, height;
    raster_stats_t stats;
    int32_t draw_us, upscale_us;
    int32_t predicted_us;
} render_context_frame_t;

typedef struct {
    const char* name;
    size_t state_bytes;
    raster_attachment_t (*attachment)(void* state);
} render_debug_view_t;

#define RENDER_VIEW_UNKNOWN -2
#define RENDER_VIEW_SHADED  0
#define RENDER_VIEW_COUNT   4 /* table rows; views run 0 to this */

typedef struct {
    raster_t raster; /* keeps its upscale maps from frame to frame */
    void* scratch;
    size_t scratch_bytes;
    int scale_percent;

    enum { RENDER_FIXED, RENDER_STEPPED, RENDER_PREDICTED } policy;

    resolution_config_t ladder; /* the steps and thresholds both policies read */
    resolution_control_t control;
    resolution_predict_t predict;
    int view; /* zero is shaded; table rows start at one */
    void* view_state;
    raster_attachment_t view_attachment;
    const raster_attachment_t* view_attached[1];
    render_context_frame_t frame;
} render_context_t;

/* The engine's own context, the one the scene manager draws through. */
render_context_t* render_context_main(void);

/* Frees the scratch block and returns every setting to its default. */
void render_context_release(render_context_t* context);

/* The share of the destination's size each axis draws at, upscaled on the
 * way out; it holds while dynamic resolution is off. */
void render_context_set_scale(render_context_t* context, int percent);

/* Opts in: each frame draws at a step of `config` to hold its budget, chosen
 * by the stepped controller, or by the predictor when `model` is not NULL.
 * NULL `config` returns to the fixed scale. */
void render_context_set_dynamic_resolution(render_context_t* context, const resolution_config_t* config,
                                           const resolution_model_t* model, int first_step);

/* Development views, or RENDER_VIEW_SHADED to detach the view. */
void render_context_set_view(render_context_t* context, int view);
/* The declared row, or NULL for shaded, an unknown index or a release build. */
const render_debug_view_t* render_context_view(int view);

/* A view number, RENDER_VIEW_SHADED, or RENDER_VIEW_UNKNOWN for an unknown name. */
int render_context_view_named(const char* name);

/* Comma-separated names, shaded first, followed by the table order. */
void render_context_print_views(FILE* out);

/* Draws `count` instances through `view`; false when there is no scratch for it. */
bool render_context_draw(render_context_t* context, const r3d_instance_t* instances, int count,
                         const render_view_t* view, uint16_t clear);

/* With `half`, copies an exact-half draw there at its drawn size and
 * returns true; otherwise upscales into `destination` at the view's
 * destination size. Either path feeds its cost to dynamic resolution. */
bool render_context_compose(render_context_t* context, uint16_t* destination, uint16_t* half);

render_context_frame_t render_context_frame(const render_context_t* context);

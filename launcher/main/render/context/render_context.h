/*
 * render_context: what a frame is drawn at and how well, apart from what is
 * drawn and from where. A caller hands it instances, a camera and a clear
 * colour; the context owns the raster and its scratch block, the render size
 * and the debug view, and upscales into the picture it is given.
 *
 * The size is a fixed scale until a caller opts into dynamic resolution
 * (render/resolution/resolution.h): then each frame draws at a step chosen
 * to hold a budget, and what the draw and upscale cost is fed back.
 *
 * The engine draws its frames through one context, render_context_main(),
 * released when an app exits. The camera stays perspective only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/camera.h"
#include "render/r3d_instance.h"
#include "render/raster.h"
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
} render_context_frame_t;

typedef struct {
    raster_t raster; /* keeps its upscale maps from frame to frame */
    void* scratch;
    size_t scratch_bytes;
    int scale_percent;

    enum { RENDER_FIXED, RENDER_STEPPED, RENDER_PREDICTED } policy;

    resolution_config_t ladder; /* the steps and thresholds both policies read */
    resolution_control_t control;
    resolution_predict_t predict;
    raster_show_t debug_view;
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

/* Between draw and upscale, shows the frame as `mode` says; development only. */
void render_context_set_debug_view(render_context_t* context, raster_show_t mode);

/* Draws `count` instances through `camera` for a destination of `width` by
 * `height`, turned for `quarter`; false when there is no scratch for it. */
bool render_context_draw(render_context_t* context, const r3d_instance_t* instances, int count, const camera_t* camera,
                         uint16_t clear, int quarter, int width, int height);

/* Upscales the last draw into `destination`, of the size it was drawn for,
 * and feeds the frame's cost to dynamic resolution. */
void render_context_compose(render_context_t* context, uint16_t* destination);

render_context_frame_t render_context_frame(const render_context_t* context);

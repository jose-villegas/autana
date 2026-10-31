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
 * released when an app exits. A view is perspective only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/memory.h"
#include "render/framegen/framegen.h"
#include "render/r3d_instance.h"
#include "render/raster.h"
#include "render/render_view.h"
#include "render/resolution/resolution.h"

/* The share of the destination a frame draws at until set. */
#define RENDER_CONTEXT_DEFAULT_SCALE_PERCENT 50

/* Frame generation starts only once a generated frame costs under
 * (100 - FRAMEGEN_GAIN_PERCENT)% of a drawn one, since it shows the scene less
 * exactly; it then holds while a generated frame costs less than a drawn one,
 * so a cost near the line does not turn it on and off. */
#define FRAMEGEN_GAIN_PERCENT                10
/* Drawn frames in a row after which a frame is generated anyway, to measure
 * its cost again: the view or the drawn size may have changed. */
#define FRAMEGEN_RETRY_DRAWN                 30

/* The last frame: its step (-1 at the fixed scale), the size it drew at,
 * what culling kept and, under dynamic resolution or frame generation only,
 * what its draw (with any census) and upscale cost: otherwise nothing is
 * timed. A generated frame keeps the drawn one's size, step and stats; its
 * draw_us is the warp's. */
typedef struct {
    int step;
    int width, height;
    raster_stats_t stats;
    int32_t draw_us, upscale_us;
    int32_t predicted_us;
    bool generated; /* warped from the last drawn frame, not drawn */
} render_context_frame_t;

typedef struct {
    const char* name;
    size_t state_bytes;
    raster_attachment_t (*attachment)(void* state);
    /* Optional. After the frame's draw, and after each draw it asks for:
     * true to draw the same instances again through `out`, the frame's
     * stats kept from the first. With `listed` set it has filled the
     * raster's census list with the clusters to draw; otherwise every
     * cluster is drawn. */
    bool (*second_view)(void* state, const raster_t* raster, render_view_t* out, bool* listed);
    /* Optional. Over the composed picture, `width` by `height`, with draw/. */
    void (*overlay)(void* state, int width, int height);
    /* Optional. Before each draw, room in the state for `instances`; false
     * detaches the view, logged, and the frame draws shaded. */
    bool (*reserve)(void* state, int instances);
    /* Optional. Frees what reserve took, before the state is freed. */
    void (*release)(void* state);
} render_debug_view_t;

#define RENDER_DEBUG_VIEW_UNKNOWN -2
#define RENDER_DEBUG_VIEW_SHADED  0
#define RENDER_DEBUG_VIEW_COUNT   6 /* table rows; views run 0 to this */

typedef struct {
    raster_split_t split;
    raster_t raster; /* keeps its upscale maps from frame to frame */
    void* scratch;
    size_t scratch_bytes;
    memory_take_fn take; /* where the scratch comes from; NULL is the heap */
    int scale_percent;

    enum { RENDER_FIXED, RENDER_STEPPED, RENDER_PREDICTED } policy;

    resolution_config_t ladder; /* the steps and thresholds both policies read */
    resolution_control_t control;
    resolution_predict_t predict;
    int64_t chose_at_us; /* when the predictor last chose; 0 before its first choice */
    int debug_view;      /* zero is shaded; table rows start at one */
    void* debug_view_state;
    raster_attachment_t debug_view_attachment;
    const raster_attachment_t* debug_view_attached[1];
    render_context_frame_t frame;
    /* Frame generation (render/framegen): one generated frame after each
     * drawn one, while it costs less than a drawn one. The raster holds the
     * drawn frame, through kept_view; a generated one is composed from
     * `framegen`. */
    bool generating;
    bool kept;
    int32_t generated_us; /* the last generated frame's warp, fill and compose; 0 before one */
    int32_t drawn_us;     /* the last drawn frame's draw and compose; 0 before one */
    int drawn_in_row;     /* drawn frames since the last generated one */
    bool paying;          /* the last verdict: a generated frame costs less than a drawn one */
    int32_t shown_us;     /* the caller's budget per shown frame; the ladder's is per drawn frame */
    render_view_t kept_view;
    framegen_target_t framegen;
} render_context_t;

/* The engine's own context, the one the scene manager draws through. */
render_context_t* render_context_main(void);

/* Drops the scratch block and returns every setting but `take` to its default. */
void render_context_release(render_context_t* context);

/* Takes the scratch from `take` from now on; NULL is the heap. A taken block
 * is only dropped here, never freed: its lender gets it back once
 * render_context_drop_scratch() or render_context_release() has run. */
void render_context_set_take(render_context_t* context, memory_take_fn take);

/* Forgets the scratch block, freeing it when it came from the heap; the next
 * draw takes one again. */
void render_context_drop_scratch(render_context_t* context);

/* Drops feedback after a camera cut. */
void render_context_forget(render_context_t* context);

/* The share of the destination's size each axis draws at, upscaled on the
 * way out; it holds while dynamic resolution is off. */
void render_context_set_scale(render_context_t* context, int percent);

/* Frame generation: after each drawn frame, the next one is warped from it
 * to its own view instead of drawn, while a generated frame costs less than a
 * drawn one (FRAMEGEN_GAIN_PERCENT less to start); otherwise frames are drawn,
 * and one in FRAMEGEN_RETRY_DRAWN is generated to measure it again. A debug
 * view, a camera cut or a new viewport draws the next frame. */
void render_context_set_frame_generation(render_context_t* context, bool on);

/* Opts in: each frame draws at a step of `config` to hold its budget, chosen
 * by the stepped controller, or by the predictor when `model` is not NULL.
 * NULL `config` returns to the fixed scale. */
void render_context_set_dynamic_resolution(render_context_t* context, const resolution_config_t* config,
                                           const resolution_model_t* model, int first_step);

/* Development views, or RENDER_DEBUG_VIEW_SHADED to detach the debug view. */
void render_context_set_debug_view(render_context_t* context, int view);
/* The declared row, or NULL for shaded, an unknown index or a release build. */
const render_debug_view_t* render_context_debug_view(int view);

/* A debug view number, RENDER_DEBUG_VIEW_SHADED, or RENDER_DEBUG_VIEW_UNKNOWN for an unknown name. */
int render_context_debug_view_named(const char* name);

/* Comma-separated names, shaded first, followed by the table order. */
void render_context_print_debug_views(FILE* out);

/* Draws `count` instances through `view`; false when there is no scratch for it. */
bool render_context_draw(render_context_t* context, const r3d_instance_t* instances, int count,
                         const render_view_t* view, uint16_t clear);

/* How a kept-width upscale fills its rows; UPSCALE_FILTER_NEAREST until set. */
void render_context_set_upscale_filter(render_context_t* context, upscale_filter_t filter);
upscale_filter_t render_context_upscale_filter(const render_context_t* context);

/* With `half`, expands a compatible draw into the half picture and
 * returns true, marking `half_seams` (when not NULL) through the upscale
 * filter; otherwise upscales into `destination` at the view's destination
 * size. Either path feeds its cost to dynamic resolution. */
bool render_context_compose(render_context_t* context, uint16_t* destination, uint16_t* half, uint8_t* half_seams);

render_context_frame_t render_context_frame(const render_context_t* context);

/* Whether the debug view draws lines over the composed picture: a caller
 * then composes at full size, not into a half picture. */
bool render_context_has_overlay(const render_context_t* context);

/* The debug view's overlay over a composed picture `width` by `height`;
 * nothing when the view has none. */
void render_context_overlay(const render_context_t* context, int width, int height);

/* The debug view's own state, as its row's hooks see it; NULL when shaded. */
const void* render_context_debug_view_state(const render_context_t* context);

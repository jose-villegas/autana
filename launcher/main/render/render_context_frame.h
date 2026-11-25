/*
 * render_context_frame: what a render context keeps of its last frame, read
 * by the context's policy (render/context) and by a frame record
 * (render/record).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/raster.h"

/* The last frame: its step (-1 at the fixed scale), the size it drew at,
 * what culling kept and what its draw (with any census) and upscale cost.
 * A generated frame keeps the drawn one's size, step and stats; its
 * draw_us is the warp's. */
typedef struct {
    int step;
    int width, height;
    raster_stats_t stats;
    int32_t draw_us, upscale_us;
    int32_t settle_us;
    int settled_rows;
    int32_t predicted_us;
    int32_t unscaled_us; /* under dynamic resolution, the learned cost outside draw and upscale */
    bool generated;      /* warped from the last drawn frame, not drawn */
    bool unchanged;      /* partial redraw: nothing changed, nothing drawn or composed */
    /* The destination rows [rows_first, rows_end) the last compose rewrote,
     * at the view's destination size. */
    int rows_first, rows_end;
} render_context_frame_t;

/* One core's draw of a raster's culled list, timed, for the perf suites. */
#pragma once

#include <stdint.h>
#include <string.h>

#include "core/timing.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"

/* `visible` clusters of the culled list at b->culled + 1 drawn into a picture
 * whose depth is cleared first, untimed, stopped after the stage `stop` names
 * (r3d_span_stop_after); microseconds. */
static inline int64_t
one_core_draw_us(const raster_t* raster, const raster_buffers_t* b, const r3d_lens_t* lens, int visible, int stop) {
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    memset(gfx_target_depth(&b->picture, 0), 0, pixels * sizeof(uint16_t));
    r3d_span_target_t target = {.small_fill = &r3d_span_small_fill_default};
    gfx_target_window(&target.rows, &b->picture, b->picture.first_row, gfx_target_end_row(&b->picture));
    r3d_span_stop_after = stop;
    const int64_t start = timing_now_us();
    r3d_pipeline_draw(raster->instances[0].mesh, lens, b->culled + 1, visible, b->cs, b->rows, &target, b->work[0]);
    const int64_t us = timing_now_us() - start;
    r3d_span_stop_after = R3D_SPAN_STOP_NONE;
    return us;
}

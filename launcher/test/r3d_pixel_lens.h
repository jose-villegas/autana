/*
 * r3d_pixel_lens: the lens that maps a test mesh's positions, pixel *
 * R3D_SUBPIXEL * z ticks, back to the pixel, shared by the suites that draw
 * screen-space meshes through the pipeline.
 *
 * Header-only (static inline) so a suite anywhere under launcher/main/ or
 * launcher/test/ can pull it in without a new compile-list entry in either
 * run_tests.sh or the device test build.
 */
#pragma once

#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

/* The lens over a `width` by `height` screen. */
static inline r3d_lens_t
r3d_pixel_lens(int width, int height) {
    const float s = 1.0F / (float)R3D_SUBPIXEL;
    return (r3d_lens_t){{{{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}},
                        {1.0F, -1.0F, 0.0F, 0.0F, 0.5F},
                        width,
                        height,
                        R3D_SNAP_BIAS,
                        R3D_SNAP_BIAS,
                        0.5F / (float)R3D_SUBPIXEL,
                        1.0F,
                        1.0F};
}

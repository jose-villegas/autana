/* render_view: one frame's world-to-picture basis and perspective fit.
 * Pose: +z forward, +y up, scale ignored. Picture: +y down. */
#pragma once

#include <stdint.h>

#include "display/viewport.h"
#include "math/linear/projectf.h"
#include "math/linear/transformf.h"

typedef enum { CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_SCANLINE, CAMERA_PRESENT_DOUBLE } camera_present_t;

typedef enum { SMALL_FILL_FLAT, SMALL_FILL_GRAIN, SMALL_FILL_DITHER, SMALL_FILL_STYLE_COUNT } small_fill_style_t;

#define SMALL_FILL_CEILING_MAX    16
#define SMALL_FILL_CEILING_SMOOTH 4
#define SMALL_FILL_GATE_SMOOTH    0
#define SMALL_FILL_CEILING_FAST   12
#define SMALL_FILL_GATE_FAST      16
#define SMALL_FILL_SMOOTH         ((small_fill_t){SMALL_FILL_CEILING_SMOOTH, SMALL_FILL_GATE_SMOOTH, SMALL_FILL_FLAT})
#define SMALL_FILL_FAST           ((small_fill_t){SMALL_FILL_CEILING_FAST, SMALL_FILL_GATE_FAST, SMALL_FILL_FLAT})
#define SMALL_FILL_FAST_DITHER    ((small_fill_t){SMALL_FILL_CEILING_FAST, SMALL_FILL_GATE_FAST, SMALL_FILL_DITHER})
#define SMALL_FILL_FAST_GRAIN     ((small_fill_t){SMALL_FILL_CEILING_FAST, SMALL_FILL_GATE_FAST, SMALL_FILL_GRAIN})

/* Every SMALL_FILL_<name> preset, for a list that has to cover them all: X(name) each. */
#define SMALL_FILL_PRESETS(X)     X(SMOOTH) X(FAST) X(FAST_DITHER) X(FAST_GRAIN)

typedef struct {
    uint8_t ceiling;          /* largest bounding-box side in pixel centres; 0 disables small fill */
    uint8_t gate;             /* RGB565 per-channel spread at the ceiling, in packed channel units; 0 disables gating */
    small_fill_style_t style; /* colour kernel above the always-flat side limit */
} small_fill_t;

typedef struct {
    vec3f_t position, screen_x, screen_y, forward;
    projectf_t lens; /* Square pixels: fx == fy. */
    viewport_t viewport;
    camera_present_t present;
    small_fill_t small_fill;
} render_view_t;

render_view_t render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport);

/* Reprojects a retained pose through this picture's shape and turn. */
void render_view_refit(render_view_t* view, viewport_t viewport);

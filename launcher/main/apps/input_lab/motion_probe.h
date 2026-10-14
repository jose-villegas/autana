/* motion_probe: gravity angles and bubble geometry in panel input axes. */
#pragma once

#include "input/input.h"
#include "math/linear/math_const.h"
#include "math/linear/vec2i.h"

#define MOTION_PROBE_DEGREES_PER_RADIAN (MOTION_PROBE_FULL_TURN_DEG / MATH_TAU)
#define MOTION_PROBE_FULL_TURN_DEG      360
#define MOTION_PROBE_UPRIGHT_DEG        (MOTION_PROBE_FULL_TURN_DEG / 4)
#define MOTION_PROBE_MILLI_DIGITS       3
#define MOTION_PROBE_MILLI_SCALE        1000

typedef struct {
    float tilt_deg;
    float bearing_deg;
    float plane_counts;
    float magnitude_counts;
} motion_probe_t;

/* Bearing is zero at +x, increasing toward +y (clockwise on the panel).
 * With no in-plane gravity its bearing is reported as zero. */
motion_probe_t motion_probe_measure(vec3i_t acceleration);
vec2i_t motion_probe_bubble(int x, int y, int radius, int quarter);
int motion_probe_ring_extent(int radius, int row);

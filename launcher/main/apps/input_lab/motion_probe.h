/* motion_probe: panel-axis gravity angles and quarter-oriented bubble geometry. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "input/input.h"
#include "input/motion/gravity.h"
#include "math/linear/vec2i.h"

#define MOTION_PROBE_FULL_TURN_DEG 360
#define MOTION_G_TEXT_SIZE         16
#define MOTION_MILLI_DIGITS        3
#define MOTION_PROBE_OFFSET_DIGITS 1
#define MOTION_PROBE_OFFSET_SCALE  10
#define MOTION_HUD_TEXT_SIZE       96
#define MOTION_HUD_SCALE           1

typedef struct {
    float tilt_deg;
    int bearing_deg;
    float plane_counts;
    float magnitude_counts;
} motion_probe_t;

/* Bearing is rounded to 0..359, zero at +x, increasing toward +y (clockwise on the panel).
 * With no in-plane gravity its bearing is reported as zero. */
motion_probe_t motion_probe_measure(vec3i_t acceleration);
vec2i_t motion_probe_bubble(int x, int y, int radius, int quarter);
void motion_probe_format_fixed(char* out, size_t len, int value, int digits, bool show_plus);
/* A NULL gravity estimate shows an unavailable reading. */
void motion_probe_format_gravity(char* out, size_t len, const gravity_t* gravity, int quarter);

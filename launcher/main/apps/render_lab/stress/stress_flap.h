/* stress_flap: harness stand-in for Loupe's scene timeline (E3). When the timeline
 * binds wing rotations, remove this module and let the presets play the keyed clip. */
#pragma once

#include <stdint.h>
#include "math/linear/transformf.h"
#include "math/scalar/math_const.h"

#define STRESS_FLAP_HZ            4.0F
#define STRESS_FLAP_AMPLITUDE     (55.0F * MATH_PI / 180.0F)
#define STRESS_FLAP_BIAS          (10.0F * MATH_PI / 180.0F)
#define STRESS_FLAP_PHASE_STEP    (MATH_PI * 0.618034F)
#define STRESS_FLAP_MS_PER_SECOND 1000.0F
#define STRESS_FLAP_STAGE_NAME    "stress.flap"
#define STRESS_FLAP_LEFT          1
#define STRESS_FLAP_RIGHT         (-1)

/* Hinge radians: amplitude * sin(2*pi*hz*t + phase(copy)) + bias. */
float stress_flap_angle(uint32_t t_ms, int copy);
/* Rest with a hinge turn: side +1 left, -1 right raises both tips together. */
transformf_t stress_flap_wing(transformf_t rest, float angle, int side);

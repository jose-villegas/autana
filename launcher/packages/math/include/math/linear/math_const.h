/*
 * math_const: float circle constants, degree/radian conversion factors, and
 * the natural log of 2, which turns a half-life into a rate.
 */
#pragma once

#define MATH_PI                 3.14159265359F
#define MATH_TAU                6.28318530718F
#define MATH_LN2                0.693147181F

#define MATH_HALF_TURN_DEGREES  180.0F
#define MATH_DEGREES_PER_RADIAN (MATH_HALF_TURN_DEGREES / MATH_PI)
#define MATH_RADIANS_PER_DEGREE (MATH_PI / MATH_HALF_TURN_DEGREES)

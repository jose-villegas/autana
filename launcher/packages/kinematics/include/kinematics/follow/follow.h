/*
 * follow: a value moved toward a chosen target in closed form, so each step
 * is solved exactly for its dt and the motion is the same at any frame rate.
 * The state is a pair the caller owns: an offset from the target and its
 * speed per second. No mass and no forces: the curve itself is prescribed.
 */
#pragma once

#include <math.h>

#include "math/scalar/math_const.h"

/* What is left of anything halving every half_life after dt. */
static inline float
follow_halved_over(float dt, float half_life) {
    return exp2f(-dt / half_life);
}

/* Speed *v halving every half_life, and *x moved by the exact distance that
 * covers in dt: v(t) = v 2^(-t/h) and x(t) = x + v h / ln 2 (1 - 2^(-t/h)). */
static inline void
follow_coast(float* x, float* v, float dt, float half_life) {
    const float left = follow_halved_over(dt, half_life);
    *x += *v * half_life / MATH_LN2 * (1.0F - left);
    *v *= left;
}

/* A critically damped spring toward 0 from offset *e at speed *v, exactly:
 * e(t) = (e + c t) exp(-w t) and v(t) = (v - w c t) exp(-w t), c = v + w e. */
static inline void
follow_spring(float* e, float* v, float dt, float time_constant) {
    const float w = 1.0F / time_constant;
    const float left = expf(-w * dt);
    const float c = *v + (w * *e);
    *e = (*e + (c * dt)) * left;
    *v = (*v - (w * c * dt)) * left;
}

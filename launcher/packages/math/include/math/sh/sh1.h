/* sh1: RGB first-order spherical harmonics and cosine-convolved irradiance. */
#pragma once
#include "math/linear/mat3f.h"
#include "math/scalar/math_const.h"

#define SH1_COEFFICIENT_COUNT 4
#define SH1_CHANNEL_COUNT     3

/* GCC 16.1 and Xtensa GCC 14.2 reject macros in GCC unroll pragmas. */
enum { SH1_COEFFICIENTS = SH1_COEFFICIENT_COUNT, SH1_CHANNELS = SH1_CHANNEL_COUNT };

#define SH1_Y00 0.282094791774F
#define SH1_Y1  0.488602511903F
#define SH1_A0  MATH_PI
#define SH1_A1  (2.0F * MATH_PI / 3.0F)

/* Sloan, Stupid Spherical Harmonics (SH) Tricks: real radiance ordered
 * Y00, Y1-1 (+y), Y10 (+z), Y11 (+x), each RGB, with SH1_Y00/SH1_Y1 normalisation. */
typedef struct {
    vec3f_t c[SH1_COEFFICIENTS];
} sh1_rgb_t;

typedef struct {
    vec3f_t constant;
    mat3f_t gradient;
} sh1_irradiance_t;

sh1_rgb_t sh1_rgb_lerp(sh1_rgb_t a, sh1_rgb_t b, float t);
sh1_irradiance_t sh1_rgb_irradiance(const sh1_rgb_t* radiance);
void sh1_rgb_add_direction(sh1_rgb_t* sh, vec3f_t direction, vec3f_t radiance_times_weight);

static inline __attribute__((always_inline)) vec3f_t
sh1_irradiance_eval(const sh1_irradiance_t* e, vec3f_t normal) {
    float channels[SH1_CHANNELS] = {e->constant.x, e->constant.y, e->constant.z};
#pragma GCC unroll SH1_CHANNELS
    for (int row = 0; row < SH1_CHANNELS; row++) {
        channels[row] = channels[row] + e->gradient.m[row][0] * normal.x + e->gradient.m[row][1] * normal.y
                        + e->gradient.m[row][2] * normal.z;
    }
    return (vec3f_t){channels[0], channels[1], channels[2]};
}

/* Returns zero for an isotropic field. */
vec3f_t sh1_rgb_dominant_direction(const sh1_rgb_t* radiance, vec3f_t channel_weights);

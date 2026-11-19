/* sh1: Ramamoorthi and Hanrahan 2001, restricted to the constant and linear bands. */
#include "math/sh/sh1.h"

sh1_rgb_t
sh1_rgb_lerp(sh1_rgb_t a, sh1_rgb_t b, float t) {
    sh1_rgb_t out;
    for (int i = 0; i < SH1_COEFFICIENTS; i++) {
        out.c[i] = vec3f_add(vec3f_scale(a.c[i], 1.0F - t), vec3f_scale(b.c[i], t));
    }
    return out;
}

sh1_irradiance_t
sh1_rgb_irradiance(const sh1_rgb_t* radiance) {
    const float linear = SH1_A1 * SH1_Y1;
    const vec3f_t x = vec3f_scale(radiance->c[3], linear);
    const vec3f_t y = vec3f_scale(radiance->c[1], linear);
    const vec3f_t z = vec3f_scale(radiance->c[2], linear);
    return (sh1_irradiance_t){.constant = vec3f_scale(radiance->c[0], SH1_A0 * SH1_Y00),
                              .gradient = {{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}}}};
}

void
sh1_rgb_add_direction(sh1_rgb_t* sh, vec3f_t direction, vec3f_t radiance_times_weight) {
    const float basis[SH1_COEFFICIENTS] = {SH1_Y00, SH1_Y1 * direction.y, SH1_Y1 * direction.z, SH1_Y1 * direction.x};
    for (int i = 0; i < SH1_COEFFICIENTS; i++) {
        sh->c[i] = vec3f_add(sh->c[i], vec3f_scale(radiance_times_weight, basis[i]));
    }
}

vec3f_t
sh1_rgb_dominant_direction(const sh1_rgb_t* radiance, vec3f_t channel_weights) {
    const vec3f_t direction = {vec3f_dot(radiance->c[3], channel_weights), vec3f_dot(radiance->c[1], channel_weights),
                               vec3f_dot(radiance->c[2], channel_weights)};
    return vec3f_dot(direction, direction) > 0 ? vec3f_normalize(direction) : vec3f_zero();
}

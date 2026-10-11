/* Independent float lighting reference for portable and device suites. */
#pragma once
#include <math.h>
#include "render/skin/r3d_skin_light.h"

static inline uint8_t
skin_light_reference_encode(float linear, float white) {
    const float mapped = fminf(linear / (1.0F + linear * white), 1.0F);
    return (uint8_t)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(mapped, 1.0F / R3D_SKIN_LIGHT_GAMMA));
}

static inline void
skin_light_reference(vec3f_t normal, const r3d_skin_lights_t* lights, const uint8_t albedo[3], float white,
                     uint8_t out[3]) {
    float sum[] = {lights->ambient.x, lights->ambient.y, lights->ambient.z};
    for (int i = 0; i < lights->count; i++) {
        const vec3f_t direction = lights->direction[i];
        const float facing = fmaxf(0, normal.x * direction.x + normal.y * direction.y + normal.z * direction.z);
        sum[0] += lights->color[i].x * facing;
        sum[1] += lights->color[i].y * facing;
        sum[2] += lights->color[i].z * facing;
    }
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const float decoded = powf((float)albedo[k] / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
        out[k] = skin_light_reference_encode(decoded * sum[k], white);
    }
}

/* Independent float lighting reference for portable and device suites. */
#pragma once
#include <math.h>
#include "render/skin/r3d_skin_light.h"

/* suite_skin_light.c angular sweep: host worst 43 bytes, plus one byte of rounding margin. */
#define R3D_SKIN_LIGHT_TABLE_ERROR_BOUND    44
/* test_transfer_error_against_float_bake reaches two bytes; a one-byte bound fails. */
#define R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND 2

static inline vec3f_t
skin_light_reference_normal(const r3d_skin_t* skin, const mat3f_t* palette, int vertex, bool renormalise) {
    const uint8_t* r = skin->vertices + (size_t)vertex * skin->vertex_stride;
    const int influences = skin->influences;
    const int8_t* bind = (const int8_t*)(r + 2 * influences);
    float normal[R3D_SKIN_NORMAL_WIDTH] = {0};
    for (int i = 0; i < influences; i++) {
        const mat3f_t* m = &palette[r[i]];
        const float weight = (float)r[influences + i] / R3D_SKIN_WEIGHT_SUM * R3D_SKIN_NORMAL_RECIP;
        for (int axis = 0; axis < R3D_SKIN_NORMAL_WIDTH; axis++) {
            normal[axis] += weight * (m->m[axis][0] * bind[0] + m->m[axis][1] * bind[1] + m->m[axis][2] * bind[2]);
        }
    }
    const vec3f_t blended = {normal[0], normal[1], normal[2]};
    return renormalise ? vec3f_normalize(blended) : blended;
}

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

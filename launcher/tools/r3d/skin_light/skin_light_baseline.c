/* Shared normal rotation, Lambert lighting and octahedral table kernels. */
#include "skin_light_baseline.h"

#include <assert.h>
#include "render/r3d_skin_format.h"
#include "skin_light_baseline_internal.h"

vec3f_t
skin_light_baseline_normal(const mat4f_t* palette, const uint8_t* joints, const float* weights, int influences,
                           vec3f_t normal) {
    assert(influences > 0 && influences <= (int)R3D_SKIN_INFLUENCES_MAX);
    return light_normal(palette, joints, weights, influences, normal);
}

void
skin_light_baseline_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, uint8_t out[3]) {
    assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);
    baseline_light_evaluate(normal, lights, out);
}

/* Octahedral directions: Cigolle et al. 2014. Normal-indexed lighting: Miller and Hoffman 1984. */
void
skin_light_baseline_directions(int size, vec3f_t* directions) {
    assert(size >= 2);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const vec2f_t p = {((float)x + 0.5F) * 2.0F / (float)size - 1.0F,
                               ((float)y + 0.5F) * 2.0F / (float)size - 1.0F};
            directions[y * size + x] = vec3f_from_octahedral(p);
        }
    }
}

void
skin_light_baseline_grid_build(int size, const vec3f_t* directions, const r3d_skin_lights_t* lights,
                               uint8_t (*rgb)[SKIN_LIGHT_BASELINE_CELL_BYTES]) {
    assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);
    for (int c = 0; c < size * size; c++) {
        baseline_light_evaluate(directions[c], lights, rgb[c]);
        rgb[c][R3D_SKIN_LIGHT_CHANNELS] = 0;
    }
}

void
skin_light_baseline_bilinear(int size, const uint8_t (*rgb)[SKIN_LIGHT_BASELINE_CELL_BYTES], vec3f_t normal,
                             uint8_t out[3]) {
    assert(size >= 2);
    light_bilinear(size, rgb, normal, out);
}

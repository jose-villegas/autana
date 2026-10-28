/* scene_water_plane: the plane option's swing and density field. */
#include "scene_water_plane.h"

#include <math.h>

#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"

/* The level is found by Newton's method from the last frame's; this many
 * steps a frame. */
#define PLANE_NEWTON 3
/* The density across the plane ramps from 0 to 1 over this many samples. */
#define PLANE_RAMP   1.0F

void
water_plane_reset(water_plane_t* p, vec3f_t box) {
    p->up = (vec3f_t){0.0F, -1.0F, 0.0F};
    p->rate = (vec3f_t){0};
    p->level = vec3f_dot(p->up, vec3f_scale(box, 0.5F));
}

/* Semi-implicit, the up kept a unit vector with its rate along the sphere. */
void
water_plane_step(water_plane_t* p, float dt_s, vec3f_t accel_g, float hz, float damping) {
    const vec3f_t target = vec3f_normalize(vec3f_scale(accel_g, -1.0F));
    const float w = MATH_TAU * hz;
    const vec3f_t pull =
        vec3f_sub(vec3f_scale(vec3f_sub(target, p->up), w * w), vec3f_scale(p->rate, 2.0F * damping * w));
    p->rate = vec3f_add(p->rate, vec3f_scale(pull, dt_s));
    p->up = vec3f_normalize(vec3f_add(p->up, vec3f_scale(p->rate, dt_s)));
    p->rate = vec3f_sub(p->rate, vec3f_scale(p->up, vec3f_dot(p->rate, p->up)));
}

/* The plane at `level` as density: 1 below it, 0 above, ramping across it.
 * Returns the water's share of the inside samples, and through `band` the
 * samples on the ramp per mm of level. */
static float
fill_at(const water_plane_t* p, const iso_field_t* f, float level, float* band) {
    const float ramp = PLANE_RAMP * fminf(f->step.x, fminf(f->step.y, f->step.z));
    float* const out = (float*)f->values;
    float sum = 0.0F;
    int sloped = 0;
    for (int x = 1; x < f->nx - 1; x++) {
        for (int y = 1; y < f->ny - 1; y++) {
            for (int z = 1; z < f->nz - 1; z++) {
                const vec3f_t at =
                    vec3f_add(f->origin, (vec3f_t){(float)x * f->step.x, (float)y * f->step.y, (float)z * f->step.z});
                const float v = mathf_clamp(0.5F + ((level - vec3f_dot(p->up, at)) / ramp), 0.0F, 1.0F);
                sloped += v > 0.0F && v < 1.0F ? 1 : 0;
                out[(((x * f->ny) + y) * f->nz) + z] = v;
                sum += v;
            }
        }
    }
    const float inside = (float)((f->nx - 2) * (f->ny - 2) * (f->nz - 2));
    *band = (float)sloped / ramp / inside;
    return sum / inside;
}

void
water_plane_field(water_plane_t* p, const iso_field_t* field, float fill) {
    for (int i = 0; i < PLANE_NEWTON; i++) {
        float band;
        const float share = fill_at(p, field, p->level, &band);
        if (band > 0.0F) {
            p->level += (fill - share) / band;
        }
    }
}

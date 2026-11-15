/* contact_blob: a world-plane disc darkens covered RGB565 pixels by one half. */
#include "render/contact/contact_blob.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/scalar/mathf.h"

static vec3f_t
lens_axis(const r3d_lens_t* lens, int row) {
    return (vec3f_t){lens->m.m[row][0], lens->m.m[row][1], lens->m.m[row][2]};
}

void
contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground, vec3f_t up, float radius,
                  float strength) {
    if (!(radius > 0.0F) || !(strength > 0.0F)) {
        return;
    }
    const vec3f_t centre = mat4f_apply(&lens->m, ground);
    if (centre.z + radius <= lens->project.near_z) {
        return;
    }
    const float inverse_fx = 1.0F / lens->focal_x;
    const float inverse_fy = 1.0F / lens->focal_y;
    const vec3f_t normal = {vec3f_dot(lens_axis(lens, 0), up) * inverse_fx * inverse_fx,
                            vec3f_dot(lens_axis(lens, 1), up) * inverse_fy * inverse_fy,
                            vec3f_dot(lens_axis(lens, 2), up)};
    const float plane = vec3f_dot(normal, centre);
    int x0 = 0, x1 = lens->width;
    int y0 = target->rows.first_row, y1 = gfx_target_end_row(&target->rows);
    x1 = mathi_min(x1, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    if (centre.z - radius > lens->project.near_z) {
        const float near_z = centre.z - radius, far_z = centre.z + radius;
        const float left = centre.x - fabsf(lens->focal_x) * radius;
        const float right = centre.x + fabsf(lens->focal_x) * radius;
        const float top = centre.y - fabsf(lens->focal_y) * radius;
        const float bottom = centre.y + fabsf(lens->focal_y) * radius;
        x0 = (int)fmaxf((float)x0, floorf(lens->project.cx + fminf(left / near_z, left / far_z)));
        x1 = (int)fminf((float)x1, ceilf(lens->project.cx + fmaxf(right / near_z, right / far_z)));
        y0 = (int)fmaxf((float)y0, floorf(lens->project.cy + fminf(top / near_z, top / far_z)));
        y1 = (int)fminf((float)y1, ceilf(lens->project.cy + fmaxf(bottom / near_z, bottom / far_z)));
    }
    const float inverse_radius_squared = 1.0F / (radius * radius);
    strength = fminf(strength, 1.0F);
    for (int y = y0; y < y1; y++) {
        gfx_color_t* color = gfx_target_color(&target->rows, y);
        const uint16_t* depth = gfx_target_depth(&target->rows, y);
        for (int x = x0; x < x1; x++) {
            const vec3f_t ray = {x + 0.5F - lens->project.cx, y + 0.5F - lens->project.cy, 1.0F};
            const float denominator = vec3f_dot(normal, ray);
            if (denominator == 0.0F) {
                continue;
            }
            const float z = plane / denominator;
            if (!(z > lens->project.near_z)) {
                continue;
            }
            const float encoded = lens->depth_near * R3D_DEPTH_NEAREST / z;
            if (encoded < depth[x]) {
                continue;
            }
            const vec3f_t delta = vec3f_sub(vec3f_scale(ray, z), centre);
            const vec3f_t units = {delta.x * inverse_fx, delta.y * inverse_fy, delta.z};
            const float coverage = strength * (1.0F - vec3f_dot(units, units) * inverse_radius_squared);
            if (coverage * BAYER4_LEVELS > bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)]) {
                color[x] = rgb565be_unspread(rgb565be_spread(color[x]) >> 1);
            }
        }
    }
}

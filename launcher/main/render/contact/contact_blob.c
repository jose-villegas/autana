/* contact_blob: a world-plane disc darkens covered RGB565 pixels by one half. */
#include "render/contact/contact_blob.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/scalar/mathf.h"

box3f_t
contact_blob_bounds(box3f_t mover, vec3f_t ground) {
    const vec3f_t half = box3f_half_extent(mover);
    const float radius = fmaxf(half.x, half.z);
    const vec3f_t extent = {radius, radius, radius};
    return (box3f_t){vec3f_sub(ground, extent), vec3f_add(ground, extent)};
}

static vec3f_t
lens_axis(const r3d_lens_t* lens, int row) {
    return (vec3f_t){lens->m.m[row][0], lens->m.m[row][1], lens->m.m[row][2]};
}

typedef struct {
    vec3f_t centre, normal;
    float distance, inverse_fx, inverse_fy, inverse_radius_squared, strength;
} blob_plane_t;

static void
draw_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob, int y, int x0, int x1) {
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    for (int x = x0; x < x1; x++) {
        const vec3f_t ray = {x + 0.5F - lens->project.cx, y + 0.5F - lens->project.cy, 1.0F};
        const float denominator = vec3f_dot(blob->normal, ray);
        if (denominator == 0.0F) {
            continue;
        }
        const float z = blob->distance / denominator;
        if (!(z > lens->project.near_z)) {
            continue;
        }
        const float encoded = lens->depth_near * R3D_DEPTH_NEAREST / z;
        if (encoded < depth[x]) {
            continue;
        }
        const vec3f_t delta = vec3f_sub(vec3f_scale(ray, z), blob->centre);
        const vec3f_t units = {delta.x * blob->inverse_fx, delta.y * blob->inverse_fy, delta.z};
        const float coverage = blob->strength * (1.0F - vec3f_dot(units, units) * blob->inverse_radius_squared);
        if (coverage * BAYER4_LEVELS > bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)]) {
            color[x] = rgb565be_unspread(rgb565be_spread(color[x]) >> 1);
        }
    }
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
    const blob_plane_t blob = {.centre = centre,
                               .normal = normal,
                               .distance = plane,
                               .inverse_fx = inverse_fx,
                               .inverse_fy = inverse_fy,
                               .inverse_radius_squared = 1.0F / (radius * radius),
                               .strength = fminf(strength, 1.0F)};
    for (int y = y0; y < y1; y++) {
        draw_row(target, lens, &blob, y, x0, x1);
    }
}

/* contact_blob: a world-plane disc darkens covered RGB565 pixels by one half. */
#include "render/contact/contact_blob.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/scalar/mathf.h"

static vec3f_t
lens_axis(const r3d_lens_t* lens, int row) {
    return (vec3f_t){lens->m.m[row][0], lens->m.m[row][1], lens->m.m[row][2]};
}

typedef struct {
    vec3f_t centre, normal;
    float distance, inverse_fx, inverse_fy, inverse_radius_squared, strength;
} blob_plane_t;

/* Projected vertex snapping shifts coplanar depth by up to one pixel; stored depth has whole units. */
#define CONTACT_BLOB_DEPTH_BIAS_PIXELS 1.0F
#define CONTACT_BLOB_DEPTH_BIAS_UNITS  1.0F

static void
draw_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob, int y, int x0, int x1) {
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    const float row_y = (float)y + 0.5F - lens->project.cy;
    const float depth_scale = lens->depth_near * R3D_DEPTH_NEAREST / blob->distance;
    const float step = blob->normal.x * depth_scale;
    const float bias = CONTACT_BLOB_DEPTH_BIAS_UNITS
                       + CONTACT_BLOB_DEPTH_BIAS_PIXELS * (fabsf(step) + fabsf(blob->normal.y * depth_scale));
    const float row = ((blob->normal.y * row_y) + blob->normal.z) * depth_scale;
    const float first_x = (float)x0 + 0.5F - lens->project.cx;
    float encoded = row + first_x * step;
    const float near_limit = lens->depth_near * R3D_DEPTH_NEAREST / lens->project.near_z;
    for (int x = x0; x < x1; x++, encoded += step) {
        if (!(encoded > 0.0F) || encoded >= near_limit || encoded + bias < (float)depth[x]) {
            continue;
        }
        const float z = lens->depth_near * R3D_DEPTH_NEAREST / encoded;
        const vec3f_t ray = {(float)x + 0.5F - lens->project.cx, row_y, 1.0F};
        const vec3f_t delta = vec3f_sub(vec3f_scale(ray, z), blob->centre);
        const vec3f_t units = {delta.x * blob->inverse_fx, delta.y * blob->inverse_fy, delta.z};
        const float falloff = fmaxf(0.0F, 1.0F - vec3f_dot(units, units) * blob->inverse_radius_squared);
        const float coverage = blob->strength * falloff * falloff;
        if (coverage * BAYER4_LEVELS > (float)bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)]) {
            color[x] = rgb565be_half(color[x]);
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
    if (plane == 0.0F) {
        return;
    }
    int x0 = 0;
    int x1 = lens->width;
    int y0 = target->rows.first_row;
    int y1 = gfx_target_end_row(&target->rows);
    x1 = mathi_min(x1, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    if (centre.z - radius > lens->project.near_z) {
        const float near_z = centre.z - radius;
        const float far_z = centre.z + radius;
        const float left = centre.x - (fabsf(lens->focal_x) * radius);
        const float right = centre.x + (fabsf(lens->focal_x) * radius);
        const float top = centre.y - (fabsf(lens->focal_y) * radius);
        const float bottom = centre.y + (fabsf(lens->focal_y) * radius);
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

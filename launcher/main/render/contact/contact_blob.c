/* contact_blob: coplanar discs darken covered RGB565 pixels once by one half. */
#include "render/contact/contact_blob.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/scalar/mathf.h"

static vec3f_t
lens_axis(const r3d_lens_t* lens, int row) {
    return (vec3f_t){lens->m.m[row][0], lens->m.m[row][1], lens->m.m[row][2]};
}

typedef struct {
    vec3f_t normal;
    float distance, inverse_fx, inverse_fy;
} blob_plane_t;

/* Projected vertex snapping shifts coplanar depth by up to one pixel; stored depth has whole units. */
#define CONTACT_BLOB_DEPTH_BIAS_PIXELS 1.0F
#define CONTACT_BLOB_DEPTH_BIAS_UNITS  1.0F

typedef struct {
    vec3f_t centre, axis, short_axis;
    float inverse_long2, inverse_short2, strength;
    int x0, x1, y0, y1;
} lens_disc_t;

static void
draw_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob, const lens_disc_t* discs,
         int count, int y, int x0, int x1) {
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
        const vec3f_t point = vec3f_scale(ray, z);
        float coverage = 0;
        for (int i = 0; i < count; i++) {
            const lens_disc_t* d = &discs[i];
            if (y < d->y0 || y >= d->y1 || x < d->x0 || x >= d->x1) {
                continue;
            }
            const vec3f_t delta = vec3f_sub(point, d->centre);
            const vec3f_t units = {delta.x * blob->inverse_fx, delta.y * blob->inverse_fy, delta.z};
            const float along = vec3f_dot(units, d->axis), across = vec3f_dot(units, d->short_axis);
            const float d2 = d->inverse_long2 == d->inverse_short2
                                 ? vec3f_dot(units, units) * d->inverse_long2
                                 : along * along * d->inverse_long2 + across * across * d->inverse_short2;
            const float falloff = fmaxf(0, 1 - d2);
            coverage = fmaxf(coverage, d->strength * falloff * falloff);
        }
        if (coverage * BAYER4_LEVELS > (float)bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)]) {
            color[x] = rgb565be_unspread(rgb565be_spread(color[x]) >> 1);
        }
    }
}

static lens_disc_t
project_disc(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t up, const contact_disc_t* disc) {
    const vec3f_t centre = mat4f_apply(&lens->m, disc->centre);
    const vec3f_t short_axis = vec3f_cross(up, disc->axis);
    lens_disc_t out = {.centre = centre,
                       .inverse_long2 = disc->inverse_long2,
                       .inverse_short2 = disc->inverse_short2,
                       .strength = fminf(disc->strength, 1),
                       .x1 = mathi_min(lens->width, target->rows.attachment[GFX_ATTACHMENT_COLOR].width),
                       .y0 = target->rows.first_row,
                       .y1 = gfx_target_end_row(&target->rows)};
    float* axis[] = {&out.axis.x, &out.axis.y, &out.axis.z};
    float* across[] = {&out.short_axis.x, &out.short_axis.y, &out.short_axis.z};
    for (int r = 0; r < 3; r++) {
        const float focal = r == 0 ? lens->focal_x : r == 1 ? lens->focal_y : 1;
        *axis[r] = vec3f_dot(lens_axis(lens, r), disc->axis) / focal;
        *across[r] = vec3f_dot(lens_axis(lens, r), short_axis) / focal;
    }
    const float radius = 1 / sqrtf(fminf(disc->inverse_long2, disc->inverse_short2));
    if (!(disc->strength > 0) || centre.z + radius <= lens->project.near_z) {
        out.y1 = out.y0;
        return out;
    }
    if (centre.z - radius > lens->project.near_z) {
        const float near_z = centre.z - radius, far_z = centre.z + radius;
        const float left = centre.x - fabsf(lens->focal_x) * radius;
        const float right = centre.x + fabsf(lens->focal_x) * radius;
        const float top = centre.y - fabsf(lens->focal_y) * radius;
        const float bottom = centre.y + fabsf(lens->focal_y) * radius;
        out.x0 = (int)fmaxf((float)out.x0, floorf(lens->project.cx + fminf(left / near_z, left / far_z)));
        out.x1 = (int)fminf((float)out.x1, ceilf(lens->project.cx + fmaxf(right / near_z, right / far_z)));
        out.y0 = (int)fmaxf((float)out.y0, floorf(lens->project.cy + fminf(top / near_z, top / far_z)));
        out.y1 = (int)fminf((float)out.y1, ceilf(lens->project.cy + fmaxf(bottom / near_z, bottom / far_z)));
    }
    return out;
}

void
contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t up, const contact_disc_t* discs,
                  int count) {
    if (count <= 0 || count > CONTACT_BLOB_DISCS_MAX) {
        return;
    }
    const vec3f_t centre = mat4f_apply(&lens->m, discs[0].centre);
    const float inverse_fx = 1 / lens->focal_x, inverse_fy = 1 / lens->focal_y;
    const vec3f_t normal = {vec3f_dot(lens_axis(lens, 0), up) * inverse_fx * inverse_fx,
                            vec3f_dot(lens_axis(lens, 1), up) * inverse_fy * inverse_fy,
                            vec3f_dot(lens_axis(lens, 2), up)};
    const float plane = vec3f_dot(normal, centre);
    if (plane == 0) {
        return;
    }
    const blob_plane_t blob = {.normal = normal, .distance = plane, .inverse_fx = inverse_fx, .inverse_fy = inverse_fy};
    lens_disc_t projected[CONTACT_BLOB_DISCS_MAX];
    for (int i = 0; i < count; i++) {
        projected[i] = project_disc(target, lens, up, &discs[i]);
    }
    for (int y = target->rows.first_row; y < gfx_target_end_row(&target->rows); y++) {
        int cursor = 0;
        while (cursor < lens->width) {
            int x0 = lens->width, x1 = 0;
            for (int i = 0; i < count; i++) {
                const lens_disc_t* d = &projected[i];
                if (y >= d->y0 && y < d->y1 && d->x1 > cursor && d->x0 < x0) {
                    x0 = mathi_max(cursor, d->x0);
                    x1 = d->x1;
                }
            }
            if (x0 >= x1) {
                break;
            }
            int end;
            do {
                end = x1;
                for (int i = 0; i < count; i++) {
                    const lens_disc_t* d = &projected[i];
                    if (y >= d->y0 && y < d->y1 && d->x0 <= x1) {
                        x1 = mathi_max(x1, d->x1);
                    }
                }
            } while (end != x1);
            draw_row(target, lens, &blob, projected, count, y, x0, x1);
            cursor = x1;
        }
    }
}

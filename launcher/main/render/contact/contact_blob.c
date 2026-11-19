/* contact_blob: a world-plane disc darkens covered RGB565 pixels by one half. */
#include "render/contact/contact_blob.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/scalar/mathf.h"

/* Square roots of the Bayer thresholds divided by BAYER4_LEVELS, rounded to float. */
static const float radius_roots[BAYER4_LEVELS] = {
    0.0F,         0.25F, 0.353553385F, 0.433012694F, 0.5F,         0.559017003F, 0.612372458F, 0.661437809F,
    0.707106769F, 0.75F, 0.790569425F, 0.82915622F,  0.866025388F, 0.901387811F, 0.935414374F, 0.968245864F};

static vec3f_t
lens_axis(const r3d_lens_t* lens, int row) {
    return (vec3f_t){lens->m.m[row][0], lens->m.m[row][1], lens->m.m[row][2]};
}

typedef struct {
    vec3f_t centre, normal, inverse_scale;
    float distance;
    float inverse_strength_root;
} blob_plane_t;

enum { CONTACT_BLOB_CORNERS = 4, CONTACT_BLOB_CLIPPED_CORNERS = CONTACT_BLOB_CORNERS + 1 };

typedef struct {
    vec2f_t corners[CONTACT_BLOB_CLIPPED_CORNERS];
    int16_t count;
    int16_t first_row, end_row;
} blob_bounds_t;

/* Projected vertex snapping shifts coplanar depth by up to one pixel; stored depth has whole units. */
#define CONTACT_BLOB_DEPTH_BIAS_PIXELS 1.0F
#define CONTACT_BLOB_DEPTH_BIAS_UNITS  1.0F

static void
project_edge(blob_bounds_t* bounds, const r3d_lens_t* lens, vec3f_t a, vec3f_t b) {
    const float near = lens->project.near_z;
    if (a.z >= near) {
        bounds->corners[bounds->count++] = (vec2f_t){lens->project.cx + (a.x / a.z), lens->project.cy + (a.y / a.z)};
    }
    if ((a.z < near) != (b.z < near)) {
        const vec3f_t p = vec3f_add(a, vec3f_scale(vec3f_sub(b, a), (near - a.z) / (b.z - a.z)));
        bounds->corners[bounds->count++] = (vec2f_t){lens->project.cx + (p.x / p.z), lens->project.cy + (p.y / p.z)};
    }
}

static void
project_bounds(blob_bounds_t* bounds, const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground,
               vec3f_t up, float radius) {
    const vec3f_t axis = fabsf(up.x) < fabsf(up.y) ? vec3f_unit_x() : vec3f_unit_y();
    const vec3f_t u = vec3f_scale(vec3f_normalize(vec3f_cross(up, axis)), radius);
    const vec3f_t v = vec3f_scale(vec3f_normalize(vec3f_cross(up, u)), radius);
    *bounds = (blob_bounds_t){0};
    vec3f_t previous = vec3f_zero();
    for (int i = 0; i <= CONTACT_BLOB_CORNERS; i++) {
        const int corner = i % CONTACT_BLOB_CORNERS;
        const int index = corner ^ (corner >> 1);
        const vec3f_t offset = vec3f_add(vec3f_scale(u, index & 1 ? 1 : -1), vec3f_scale(v, index & 2 ? 1 : -1));
        const vec3f_t current = mat4f_apply(&lens->m, vec3f_add(ground, offset));
        if (i > 0) {
            project_edge(bounds, lens, previous, current);
        }
        previous = current;
    }
    if (bounds->count == 0) {
        return;
    }
    float top = bounds->corners[0].y;
    float bottom = top;
    for (int i = 1; i < bounds->count; i++) {
        top = mathf_min(top, bounds->corners[i].y);
        bottom = mathf_max(bottom, bounds->corners[i].y);
    }
    const float first = (float)target->rows.first_row;
    const float end = (float)gfx_target_end_row(&target->rows);
    bounds->first_row = (int16_t)mathf_clamp(floorf(top), first, end);
    bounds->end_row = (int16_t)mathf_clamp(ceilf(bottom), first, end);
    return;
}

static void
draw_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob, int y, int x0, int x1) {
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    const float row_y = (float)y + 0.5F - lens->project.cy;
    const float depth_units = lens->depth_near * R3D_DEPTH_NEAREST;
    const float depth_scale = depth_units / blob->distance;
    const float step = blob->normal.x * depth_scale;
    const float bias = CONTACT_BLOB_DEPTH_BIAS_UNITS
                       + (CONTACT_BLOB_DEPTH_BIAS_PIXELS * (fabsf(step) + fabsf(blob->normal.y * depth_scale)));
    const float row = (blob->normal.y * row_y + blob->normal.z) * depth_scale;
    const float first_x = (float)x0 + 0.5F - lens->project.cx;
    float encoded = row + (first_x * step);
    const float near_limit = depth_units / lens->project.near_z;
    const vec3f_t delta_step = {(depth_units - blob->centre.x * step) * blob->inverse_scale.x,
                                -blob->centre.y * step * blob->inverse_scale.y,
                                -blob->centre.z * step * blob->inverse_scale.z};
    vec3f_t delta = {(first_x * depth_units - blob->centre.x * encoded) * blob->inverse_scale.x,
                     (row_y * depth_units - blob->centre.y * encoded) * blob->inverse_scale.y,
                     (depth_units - blob->centre.z * encoded) * blob->inverse_scale.z};
    for (int x = x0; x < x1; x++, encoded += step, delta = vec3f_add(delta, delta_step)) {
        if (!(encoded > 0.0F) || encoded >= near_limit || encoded + bias < (float)depth[x]) {
            continue;
        }
        const unsigned threshold = bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
        if (vec3f_dot(delta, delta)
            < (1.0F - radius_roots[threshold] * blob->inverse_strength_root) * encoded * encoded) {
            color[x] = rgb565be_half(color[x]);
        }
    }
}

static void
draw_bounded_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob,
                 const blob_bounds_t* bounds, int y) {
    const float row = (float)y + 0.5F;
    const int width = mathi_min(lens->width, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    float left = (float)width;
    float right = 0.0F;
    for (int i = 0; i < bounds->count; i++) {
        const vec2f_t a = bounds->corners[i];
        const vec2f_t b = bounds->corners[(i + 1) % bounds->count];
        if ((a.y <= row && b.y > row) || (b.y <= row && a.y > row)) {
            const float x = a.x + ((b.x - a.x) * (row - a.y) / (b.y - a.y));
            left = fminf(left, x);
            right = fmaxf(right, x);
        }
    }
    const int x0 = mathi_max(0, (int)floorf(left));
    const int x1 = mathi_min(width, (int)ceilf(right));
    if (x0 < x1) {
        draw_row(target, lens, blob, y, x0, x1);
    }
}

void
contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground, vec3f_t up, float radius,
                  float strength) {
    if (!(radius > 0.0F) || !(strength > 0.0F)) {
        return;
    }
    blob_bounds_t bounds;
    project_bounds(&bounds, target, lens, ground, up, radius);
    const vec3f_t centre = mat4f_apply(&lens->m, ground);
    const float inverse_fx = 1.0F / lens->focal_x;
    const float inverse_fy = 1.0F / lens->focal_y;
    const vec3f_t normal = {vec3f_dot(lens_axis(lens, 0), up) * inverse_fx * inverse_fx,
                            vec3f_dot(lens_axis(lens, 1), up) * inverse_fy * inverse_fy,
                            vec3f_dot(lens_axis(lens, 2), up)};
    const float plane = vec3f_dot(normal, centre);
    if (plane == 0.0F) {
        return;
    }
    blob_plane_t blob = {.centre = centre,
                         .normal = normal,
                         .distance = plane,
                         .inverse_scale = {inverse_fx / radius, inverse_fy / radius, 1.0F / radius}};
    blob.inverse_strength_root = 1.0F / sqrtf(fminf(strength, 1.0F));
    for (int y = bounds.first_row; y < bounds.end_row; y++) {
        draw_bounded_row(target, lens, &blob, &bounds, y);
    }
}

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
    vec3f_t centre, normal, inverse_scale;
    float distance;
    float radius_squared[BAYER4_LEVELS];
} blob_plane_t;

static const float bayer_radius[] = {0.0F,         0.25F,        0.353553391F, 0.433012702F, 0.5F,         0.559016994F,
                                     0.612372436F, 0.661437828F, 0.707106781F, 0.75F,        0.790569415F, 0.829156198F,
                                     0.866025404F, 0.901387819F, 0.935414347F, 0.968245837F};
_Static_assert(sizeof bayer_radius / sizeof bayer_radius[0] == BAYER4_LEVELS, "one radius per Bayer level");

enum { CONTACT_BLOB_CORNERS = 4, CONTACT_BLOB_CLIPPED_CORNERS = CONTACT_BLOB_CORNERS + 2 };

typedef struct {
    vec2f_t corners[CONTACT_BLOB_CLIPPED_CORNERS];
    int count;
    recti_t box;
} blob_bounds_t;

/* Projected vertex snapping shifts coplanar depth by up to one pixel; stored depth has whole units. */
#define CONTACT_BLOB_DEPTH_BIAS_PIXELS 1.0F
#define CONTACT_BLOB_DEPTH_BIAS_UNITS  1.0F

static blob_bounds_t
project_bounds(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground, vec3f_t up, float radius) {
    const vec3f_t axis = fabsf(up.x) < fabsf(up.y) ? vec3f_unit_x() : vec3f_unit_y();
    const vec3f_t u = vec3f_scale(vec3f_normalize(vec3f_cross(up, axis)), radius);
    const vec3f_t v = vec3f_scale(vec3f_normalize(vec3f_cross(up, u)), radius);
    vec3f_t corners[CONTACT_BLOB_CORNERS];
    for (int i = 0; i < CONTACT_BLOB_CORNERS; i++) {
        const int index = i ^ (i >> 1);
        const vec3f_t offset = vec3f_add(vec3f_scale(u, index & 1 ? 1 : -1), vec3f_scale(v, index & 2 ? 1 : -1));
        corners[i] = mat4f_apply(&lens->m, vec3f_add(ground, offset));
    }
    blob_bounds_t bounds = {0};
    box3f_t screen = box3f_empty();
    for (int i = 0; i < CONTACT_BLOB_CORNERS; i++) {
        const vec3f_t a = corners[i], b = corners[(i + 1) % CONTACT_BLOB_CORNERS];
        const float near = lens->project.near_z;
        vec3f_t clipped[2];
        int count = 0;
        if (a.z >= near) {
            clipped[count++] = a;
        }
        if ((a.z < near) != (b.z < near)) {
            clipped[count++] = vec3f_add(a, vec3f_scale(vec3f_sub(b, a), (near - a.z) / (b.z - a.z)));
        }
        for (int j = 0; j < count; j++) {
            const vec3f_t p = clipped[j];
            const vec3f_t projected = {lens->project.cx + p.x / p.z, lens->project.cy + p.y / p.z, 0};
            bounds.corners[bounds.count++] = (vec2f_t){projected.x, projected.y};
            screen = box3f_union(screen, box3f_from_point(projected));
        }
    }
    if (box3f_is_empty(screen)) {
        return bounds;
    }
    const int width = mathi_min(lens->width, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    const int first = target->rows.first_row, end = gfx_target_end_row(&target->rows);
    bounds.box = recti_from_edges(
        (int)mathf_clamp(floorf(screen.lo.x), 0, width), (int)mathf_clamp(floorf(screen.lo.y), first, end),
        (int)mathf_clamp(ceilf(screen.hi.x), 0, width), (int)mathf_clamp(ceilf(screen.hi.y), first, end));
    return bounds;
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
                       + CONTACT_BLOB_DEPTH_BIAS_PIXELS * (fabsf(step) + fabsf(blob->normal.y * depth_scale));
    const float row = (blob->normal.y * row_y + blob->normal.z) * depth_scale;
    const float first_x = (float)x0 + 0.5F - lens->project.cx;
    float encoded = row + first_x * step;
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
        if (vec3f_dot(delta, delta) < blob->radius_squared[threshold] * encoded * encoded) {
            color[x] = rgb565be_half(color[x]);
        }
    }
}

static void
draw_bounded_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob,
                 const blob_bounds_t* bounds, int y) {
    const float row = (float)y + 0.5F;
    float left = (float)recti_right(bounds->box), right = (float)bounds->box.x;
    for (int i = 0; i < bounds->count; i++) {
        const vec2f_t a = bounds->corners[i], b = bounds->corners[(i + 1) % bounds->count];
        if ((a.y <= row && b.y > row) || (b.y <= row && a.y > row)) {
            const float x = a.x + (b.x - a.x) * (row - a.y) / (b.y - a.y);
            left = fminf(left, x);
            right = fmaxf(right, x);
        }
    }
    const int x0 = mathi_max(bounds->box.x, (int)floorf(left));
    const int x1 = mathi_min(recti_right(bounds->box), (int)ceilf(right));
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
    const blob_bounds_t bounds = project_bounds(target, lens, ground, up, radius);
    blob_plane_t blob = {.centre = centre,
                         .normal = normal,
                         .distance = plane,
                         .inverse_scale = {inverse_fx / radius, inverse_fy / radius, 1.0F / radius}};
    const float inverse_strength_root = 1.0F / sqrtf(fminf(strength, 1.0F));
    /* Inverting the squared falloff keeps the Bayer decision homogeneous, with no pixel reciprocal. */
    for (unsigned i = 0; i < BAYER4_LEVELS; i++) {
        blob.radius_squared[i] = 1.0F - bayer_radius[i] * inverse_strength_root;
    }
    for (int y = bounds.box.y; y < recti_bottom(bounds.box); y++) {
        draw_bounded_row(target, lens, &blob, &bounds, y);
    }
}

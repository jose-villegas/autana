/* contact_blob: one warped ground rectangle darkens covered RGB565 pixels once. */
#include "render/contact/contact_blob.h"

#include <math.h>
#include "image/pixel/rgb565.h"
#include "math/scalar/mathf.h"

static vec3f_t
lens_axis(const r3d_lens_t* lens, int row) {
    return (vec3f_t){lens->m.m[row][0], lens->m.m[row][1], lens->m.m[row][2]};
}

typedef struct {
    vec3f_t normal, centre, u, v;
    float distance, strength;
    const contact_mask_t* mask;
    unsigned thresholds[BAYER4_LEVELS];
} blob_plane_t;

/* Vertex snapping shifts coplanar depth by up to one pixel; stored depth has whole units. */
#define CONTACT_BLOB_DEPTH_BIAS_PIXELS 1.0F
#define CONTACT_BLOB_DEPTH_BIAS_UNITS  1.0F

static unsigned
sample_coverage(const blob_plane_t* blob, float s, float t) {
    if (blob->mask != NULL) {
        const contact_mask_t* mask = blob->mask;
        const int col = (int)((s + 1) * 0.5F * mask->side);
        const int row = (int)((t + 1) * 0.5F * mask->side);
        return mask->coverage[row * mask->side + col] * BAYER4_LEVELS;
    }
    const float falloff = fmaxf(0, 1 - s * s - t * t);
    return (unsigned)ceilf(BAYER4_LEVELS * blob->strength * falloff * falloff) * UINT8_MAX;
}

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
        const vec3f_t point = vec3f_scale(ray, z);
        const vec3f_t delta = vec3f_sub(point, blob->centre);
        const float s = vec3f_dot(delta, blob->u), t = vec3f_dot(delta, blob->v);
        if (!(fabsf(s) < 1 && fabsf(t) < 1)) {
            continue;
        }
        const unsigned threshold = bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
        const unsigned coverage = sample_coverage(blob, s, t);
        if (coverage > blob->thresholds[threshold]) {
            color[x] = rgb565be_unspread(rgb565be_spread(color[x]) >> 1);
        }
    }
}

static bool
project_blob(const r3d_lens_t* lens, const contact_blob_t* footprint, blob_plane_t* blob) {
    const vec3f_t up = vec3f_normalize(vec3f_cross(footprint->u, footprint->v));
    const vec3f_t centre = mat4f_apply(&lens->m, footprint->centre);
    const float uu = vec3f_dot(footprint->u, footprint->u), vv = vec3f_dot(footprint->v, footprint->v);
    const float uv = vec3f_dot(footprint->u, footprint->v), determinant = uu * vv - uv * uv;
    if (!(determinant > 0 && footprint->strength > 0)) {
        return false;
    }
    const vec3f_t u =
        vec3f_scale(vec3f_sub(vec3f_scale(footprint->u, vv), vec3f_scale(footprint->v, uv)), 1 / determinant);
    const vec3f_t v =
        vec3f_scale(vec3f_sub(vec3f_scale(footprint->v, uu), vec3f_scale(footprint->u, uv)), 1 / determinant);
    *blob = (blob_plane_t){.centre = centre, .strength = fminf(footprint->strength, 1), .mask = footprint->mask};
    for (unsigned i = 0; i < BAYER4_LEVELS; i++) {
        blob->thresholds[i] = footprint->mask != NULL
                                  ? (unsigned)fminf(UINT8_MAX * BAYER4_LEVELS, floorf(i * UINT8_MAX / blob->strength))
                                  : i * UINT8_MAX;
    }
    float* normal[] = {&blob->normal.x, &blob->normal.y, &blob->normal.z};
    float* du[] = {&blob->u.x, &blob->u.y, &blob->u.z};
    float* dv[] = {&blob->v.x, &blob->v.y, &blob->v.z};
    for (int r = 0; r < 3; r++) {
        const float focal = r == 0 ? lens->focal_x : r == 1 ? lens->focal_y : 1;
        *normal[r] = vec3f_dot(lens_axis(lens, r), up) / (focal * focal);
        *du[r] = vec3f_dot(lens_axis(lens, r), u) / (focal * focal);
        *dv[r] = vec3f_dot(lens_axis(lens, r), v) / (focal * focal);
    }
    blob->distance = vec3f_dot(blob->normal, centre);
    if (blob->distance == 0) {
        return false;
    }
    return true;
}

static void
include_projection(box3f_t* bounds, const r3d_lens_t* lens, vec3f_t p) {
    const vec3f_t screen = {lens->project.cx + p.x / p.z, lens->project.cy + p.y / p.z, 0};
    *bounds = box3f_union(*bounds, box3f_from_point(screen));
}

static recti_t
project_bounds(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* footprint) {
    enum { CORNERS = 4 };

    static vec3f_t corners[CORNERS];
    for (int i = 0; i < CORNERS; i++) {
        const int index = i ^ (i >> 1);
        const vec3f_t corner = vec3f_add(footprint->centre, vec3f_add(vec3f_scale(footprint->u, index & 1 ? 1 : -1),
                                                                      vec3f_scale(footprint->v, index & 2 ? 1 : -1)));
        corners[i] = mat4f_apply(&lens->m, corner);
    }
    box3f_t screen = box3f_empty();
    for (int i = 0; i < CORNERS; i++) {
        const vec3f_t a = corners[i], b = corners[(i + 1) % CORNERS];
        const float near = lens->project.near_z;
        if (a.z >= near) {
            include_projection(&screen, lens, a);
        }
        if ((a.z < near) != (b.z < near)) {
            include_projection(&screen, lens, vec3f_add(a, vec3f_scale(vec3f_sub(b, a), (near - a.z) / (b.z - a.z))));
        }
    }
    if (box3f_is_empty(screen)) {
        return recti_empty();
    }
    const int width = mathi_min(lens->width, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    const int x0 = (int)mathf_clamp(floorf(screen.lo.x), 0, width);
    const int x1 = (int)mathf_clamp(ceilf(screen.hi.x), 0, width);
    const int y0 = (int)mathf_clamp(floorf(screen.lo.y), target->rows.first_row, gfx_target_end_row(&target->rows));
    const int y1 = (int)mathf_clamp(ceilf(screen.hi.y), target->rows.first_row, gfx_target_end_row(&target->rows));
    return recti_from_edges(x0, y0, x1, y1);
}

void
contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* footprint) {
    /* The post pass runs serially after the row workers join. */
    static blob_plane_t blob;
    if (!project_blob(lens, footprint, &blob)) {
        return;
    }
    const recti_t box = project_bounds(target, lens, footprint);
    for (int y = box.y; y < recti_bottom(box); y++) {
        draw_row(target, lens, &blob, y, box.x, recti_right(box));
    }
}

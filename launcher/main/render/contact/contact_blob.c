/* contact_blob: ground footprints darken covered RGB565 pixels once. */
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
    float distance, inverse_strength_root;
    float strength;
    vec3f_t u, v;
    const contact_mask_t* mask;
    unsigned thresholds[BAYER4_LEVELS];
    bool circular;
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

static inline __attribute__((always_inline)) void
project_bounds(blob_bounds_t* bounds, const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground,
               vec3f_t u, vec3f_t v) {
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

typedef struct {
    float row_y, depth_units, step, bias, encoded, near_limit, first_x;
} blob_row_t;

static inline __attribute__((always_inline)) blob_row_t
project_row(const r3d_lens_t* lens, const blob_plane_t* blob, int y, int x0) {
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
    return (blob_row_t){row_y, depth_units, step, bias, encoded, near_limit, first_x};
}

static inline __attribute__((always_inline)) void
draw_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blob, int y, int x0, int x1,
         bool circular) {
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    const blob_row_t row = project_row(lens, blob, y, x0);
    float encoded = row.encoded;
    vec3f_t delta_step = {(row.depth_units - blob->centre.x * row.step) * blob->inverse_scale.x,
                          -blob->centre.y * row.step * blob->inverse_scale.y,
                          -blob->centre.z * row.step * blob->inverse_scale.z};
    vec3f_t delta = {(row.first_x * row.depth_units - blob->centre.x * encoded) * blob->inverse_scale.x,
                     (row.row_y * row.depth_units - blob->centre.y * encoded) * blob->inverse_scale.y,
                     (row.depth_units - blob->centre.z * encoded) * blob->inverse_scale.z};
    if (!circular) {
        const vec3f_t first = {(row.first_x * row.depth_units) - (blob->centre.x * encoded),
                               (row.row_y * row.depth_units) - (blob->centre.y * encoded),
                               row.depth_units - (blob->centre.z * encoded)};
        const vec3f_t step_delta = {row.depth_units - (blob->centre.x * row.step), -blob->centre.y * row.step,
                                    -blob->centre.z * row.step};
        delta = (vec3f_t){vec3f_dot(first, blob->u), vec3f_dot(first, blob->v), 0};
        delta_step = (vec3f_t){vec3f_dot(step_delta, blob->u), vec3f_dot(step_delta, blob->v), 0};
    }
    for (int x = x0; x < x1; x++, encoded += row.step, delta = vec3f_add(delta, delta_step)) {
        if (!(encoded > 0.0F) || encoded >= row.near_limit || encoded + row.bias < (float)depth[x]) {
            continue;
        }
        const unsigned threshold = bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
        if (vec3f_dot(delta, delta)
            < (1.0F - radius_roots[threshold] * blob->inverse_strength_root) * encoded * encoded) {
            color[x] = rgb565be_half(color[x]);
        }
    }
}

static unsigned
sample_coverage(const blob_plane_t* blob, float s, float t) {
    if (blob->mask != NULL) {
        const contact_mask_t* mask = blob->mask;
        const int col = (int)((s + 1) * 0.5F * (float)mask->side);
        const int row = (int)((t + 1) * 0.5F * (float)mask->side);
        return mask->coverage[(row * mask->side) + col] * BAYER4_LEVELS;
    }
    const float falloff = fmaxf(0, 1 - (s * s) - (t * t));
    return (unsigned)ceilf(BAYER4_LEVELS * blob->strength * falloff * falloff) * UINT8_MAX;
}

static unsigned
footprint_coverage(const blob_plane_t* blobs, int count, vec3f_t point) {
    unsigned coverage = 0;
    for (int i = 0; i < count; i++) {
        const blob_plane_t* disc = &blobs[i];
        const vec3f_t delta = vec3f_sub(point, disc->centre);
        const float s = vec3f_dot(delta, disc->u);
        const float t = vec3f_dot(delta, disc->v);
        if (fabsf(s) < 1 && fabsf(t) < 1) {
            coverage = mathi_max((int)coverage, (int)sample_coverage(disc, s, t));
        }
    }
    return coverage;
}

static void
draw_coverage_row(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blobs, int count, int y,
                  int x0, int x1) {
    const blob_plane_t* blob = blobs;
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    const blob_row_t row = project_row(lens, blob, y, x0);
    float encoded = row.encoded;
    for (int x = x0; x < x1; x++, encoded += row.step) {
        if (!(encoded > 0.0F) || encoded >= row.near_limit || encoded + row.bias < (float)depth[x]) {
            continue;
        }
        const vec3f_t ray = {(float)x + 0.5F - lens->project.cx, row.row_y, 1.0F};
        const unsigned coverage = footprint_coverage(blobs, count, vec3f_scale(ray, row.depth_units / encoded));
        const unsigned threshold = bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
        if (coverage > blob->thresholds[threshold]) {
            color[x] = rgb565be_half(color[x]);
        }
    }
}

static inline __attribute__((always_inline)) void
bounded_row(const blob_bounds_t* bounds, int width, int y, int* x0, int* x1) {
    const float row = (float)y + 0.5F;
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
    *x0 = mathi_max(0, (int)floorf(left));
    *x1 = mathi_min(width, (int)ceilf(right));
}

static inline __attribute__((always_inline)) void
bounded_rows(const blob_bounds_t* bounds, int count, int width, int y, int* x0, int* x1) {
    if (count == 1) {
        bounded_row(bounds, width, y, x0, x1);
        return;
    }
    recti_t interval = recti_empty();
    for (int i = 0; i < count; i++) {
        if (y >= bounds[i].first_row && y < bounds[i].end_row) {
            bounded_row(&bounds[i], width, y, x0, x1);
            interval = recti_union(interval, (recti_t){*x0, y, *x1 - *x0, 1});
        }
    }
    *x0 = interval.x;
    *x1 = recti_right(interval);
}

/* Constant single-disc arguments remove union and shape dispatch from its rows. */
static inline __attribute__((always_inline)) void
draw_rows(const r3d_span_target_t* target, const r3d_lens_t* lens, const blob_plane_t* blobs,
          const blob_bounds_t* bounds, int count, int first_row, int end_row, bool radial, bool circular) {
    const int width = mathi_min(lens->width, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    for (int y = first_row; y < end_row; y++) {
        int x0;
        int x1;
        bounded_rows(bounds, count, width, y, &x0, &x1);
        if (x0 < x1) {
            if (radial) {
                draw_row(target, lens, blobs, y, x0, x1, circular);
            } else {
                draw_coverage_row(target, lens, blobs, count, y, x0, x1);
            }
        }
    }
}

void
contact_disc_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground, vec3f_t up, float radius,
                  float strength) {
    if (!(radius > 0.0F) || !(strength > 0.0F)) {
        return;
    }
    vec3f_t u;
    vec3f_t v;
    if (up.x == 0 && up.y == 1 && up.z == 0) {
        u = vec3f_scale(vec3f_unit_z(), -radius);
        v = vec3f_scale(vec3f_unit_x(), -radius);
    } else {
        const vec3f_t axis = fabsf(up.x) < fabsf(up.y) ? vec3f_unit_x() : vec3f_unit_y();
        u = vec3f_scale(vec3f_normalize(vec3f_cross(up, axis)), radius);
        v = vec3f_scale(vec3f_normalize(vec3f_cross(up, u)), radius);
    }
    blob_bounds_t bounds;
    project_bounds(&bounds, target, lens, ground, u, v);
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
    blob_plane_t blob;
    blob.centre = centre;
    blob.normal = normal;
    blob.distance = plane;
    blob.inverse_scale = (vec3f_t){inverse_fx / radius, inverse_fy / radius, 1.0F / radius};
    blob.inverse_strength_root = 1.0F / sqrtf(fminf(strength, 1.0F));
    draw_rows(target, lens, &blob, &bounds, 1, bounds.first_row, bounds.end_row, true, true);
}

static bool
project_blob(const r3d_lens_t* lens, const contact_blob_t* footprint, blob_plane_t* blob) {
    const vec3f_t up = vec3f_normalize(vec3f_cross(footprint->u, footprint->v));
    const vec3f_t centre = mat4f_apply(&lens->m, footprint->centre);
    const float uu = vec3f_dot(footprint->u, footprint->u);
    const float vv = vec3f_dot(footprint->v, footprint->v);
    const float uv = vec3f_dot(footprint->u, footprint->v);
    const float determinant = (uu * vv) - (uv * uv);
    if (!(determinant > 0 && footprint->strength > 0)) {
        return false;
    }
    const vec3f_t u =
        vec3f_scale(vec3f_sub(vec3f_scale(footprint->u, vv), vec3f_scale(footprint->v, uv)), 1 / determinant);
    const vec3f_t v =
        vec3f_scale(vec3f_sub(vec3f_scale(footprint->v, uu), vec3f_scale(footprint->u, uv)), 1 / determinant);
    *blob = (blob_plane_t){.centre = centre, .strength = fminf(footprint->strength, 1), .mask = footprint->mask};
    for (unsigned i = 0; i < BAYER4_LEVELS; i++) {
        blob->thresholds[i] = footprint->mask != NULL ? (unsigned)fminf(UINT8_MAX * BAYER4_LEVELS,
                                                                        floorf((float)(i * UINT8_MAX) / blob->strength))
                                                      : i * UINT8_MAX;
    }
    const float inverse_fx = 1.0F / lens->focal_x;
    const float inverse_fy = 1.0F / lens->focal_y;
    blob->normal =
        (vec3f_t){vec3f_dot(lens_axis(lens, 0), up) * inverse_fx * inverse_fx,
                  vec3f_dot(lens_axis(lens, 1), up) * inverse_fy * inverse_fy, vec3f_dot(lens_axis(lens, 2), up)};
    float* du[] = {&blob->u.x, &blob->u.y, &blob->u.z};
    float* dv[] = {&blob->v.x, &blob->v.y, &blob->v.z};
    for (int r = 0; r < 3; r++) {
        const float focal = r == 0 ? lens->focal_x : r == 1 ? lens->focal_y : 1;
        *du[r] = vec3f_dot(lens_axis(lens, r), u) / (focal * focal);
        *dv[r] = vec3f_dot(lens_axis(lens, r), v) / (focal * focal);
    }
    blob->distance = vec3f_dot(blob->normal, centre);
    if (blob->distance == 0) {
        return false;
    }
    blob->circular = uu == vv && uv == 0;
    const float radius = sqrtf(uu);
    blob->inverse_scale = (vec3f_t){inverse_fx / radius, inverse_fy / radius, 1.0F / radius};
    blob->inverse_strength_root = 1.0F / sqrtf(blob->strength);
    return true;
}

void
contact_blobs_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* footprints,
                   int count) {
    /* The post pass runs serially after the row workers join. */
    static blob_plane_t blobs[CONTACT_BLOBS_MAX];
    static blob_bounds_t bounds[CONTACT_BLOBS_MAX];
    int first_row = gfx_target_end_row(&target->rows);
    int end_row = target->rows.first_row;
    int used = 0;
    for (int i = 0; i < count && used < CONTACT_BLOBS_MAX; i++) {
        if (project_blob(lens, &footprints[i], &blobs[used])) {
            project_bounds(&bounds[used], target, lens, footprints[i].centre, footprints[i].u, footprints[i].v);
            first_row = mathi_min(first_row, bounds[used].first_row);
            end_row = mathi_max(end_row, bounds[used].end_row);
            used++;
        }
    }
    draw_rows(target, lens, blobs, bounds, used, first_row, end_row, used == 1 && blobs->mask == NULL, blobs->circular);
}

void
contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* footprint) {
    contact_blobs_draw(target, lens, footprint, 1);
}

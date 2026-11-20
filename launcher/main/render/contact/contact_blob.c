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
    vec2f_t delta, delta_step;
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

static void
project_ellipse_row(blob_plane_t* blob, const blob_row_t* row) {
    const vec3f_t first = {(row->first_x * row->depth_units) - (blob->centre.x * row->encoded),
                           (row->row_y * row->depth_units) - (blob->centre.y * row->encoded),
                           row->depth_units - (blob->centre.z * row->encoded)};
    const vec3f_t step = {row->depth_units - (blob->centre.x * row->step), -blob->centre.y * row->step,
                          -blob->centre.z * row->step};
    blob->delta = (vec2f_t){vec3f_dot(first, blob->u), vec3f_dot(first, blob->v)};
    blob->delta_step = (vec2f_t){vec3f_dot(step, blob->u), vec3f_dot(step, blob->v)};
}

static inline bool
radial_coverage(float distance_squared, float encoded, float inverse_strength_root, unsigned threshold) {
    return distance_squared < (1.0F - radius_roots[threshold] * inverse_strength_root) * encoded * encoded;
}

static inline __attribute__((always_inline)) void
draw_row(const r3d_span_target_t* target, const r3d_lens_t* lens, blob_plane_t* blob, int y, int x0, int x1,
         bool circular) {
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    const blob_row_t row = project_row(lens, blob, y, x0);
    float encoded = row.encoded;
    vec3f_t delta_step;
    vec3f_t delta;
    if (circular) {
        delta_step = (vec3f_t){(row.depth_units - blob->centre.x * row.step) * blob->inverse_scale.x,
                               -blob->centre.y * row.step * blob->inverse_scale.y,
                               -blob->centre.z * row.step * blob->inverse_scale.z};
        delta = (vec3f_t){(row.first_x * row.depth_units - blob->centre.x * encoded) * blob->inverse_scale.x,
                          (row.row_y * row.depth_units - blob->centre.y * encoded) * blob->inverse_scale.y,
                          (row.depth_units - blob->centre.z * encoded) * blob->inverse_scale.z};
    } else {
        project_ellipse_row(blob, &row);
        delta = (vec3f_t){blob->delta.x, blob->delta.y, 0};
        delta_step = (vec3f_t){blob->delta_step.x, blob->delta_step.y, 0};
    }
    const float inverse_strength_root = blob->inverse_strength_root;
    for (int x = x0; x < x1; x++, encoded += row.step, delta = vec3f_add(delta, delta_step)) {
        if (!(encoded > 0.0F) || encoded >= row.near_limit || encoded + row.bias < (float)depth[x]) {
            continue;
        }
        const unsigned threshold = bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
        if (radial_coverage(vec3f_dot(delta, delta), encoded, inverse_strength_root, threshold)) {
            color[x] = rgb565be_half(color[x]);
        }
    }
}

static unsigned
sample_coverage(const contact_mask_t* mask, float s, float t) {
    const int col = mathi_min(CONTACT_MASK_SIDE - 1, (int)((s + 1) * 0.5F * CONTACT_MASK_SIDE));
    const int row = mathi_min(CONTACT_MASK_SIDE - 1, (int)((t + 1) * 0.5F * CONTACT_MASK_SIDE));
    return mask->coverage[(row * CONTACT_MASK_SIDE) + col] * BAYER4_LEVELS;
}

static bool
footprint_coverage(blob_plane_t* blobs, int count, float encoded, unsigned threshold, const unsigned* thresholds,
                   bool visible) {
    bool covered = false;
    const float reciprocal = visible && blobs->mask != NULL ? 1.0F / encoded : 0;
    for (int i = 0; i < count; i++) {
        blob_plane_t* blob = &blobs[i];
        if (visible && !covered) {
            if (blob->mask == NULL) {
                covered = radial_coverage(vec2f_dot(blob->delta, blob->delta), encoded, blob->inverse_strength_root,
                                          threshold);
            } else {
                const float s = blob->delta.x * reciprocal;
                const float t = blob->delta.y * reciprocal;
                covered = fabsf(s) < 1 && fabsf(t) < 1 && sample_coverage(blob->mask, s, t) > thresholds[threshold];
            }
        }
        blob->delta = vec2f_add(blob->delta, blob->delta_step);
    }
    return covered;
}

/* Coplanar footprints share strength, so one depth test and threshold cover their union. */
static void
draw_coverage_row(const r3d_span_target_t* target, const r3d_lens_t* lens, blob_plane_t* blobs, int count,
                  const unsigned* thresholds, int y, int x0, int x1) {
    gfx_color_t* color = gfx_target_color(&target->rows, y);
    const uint16_t* depth = gfx_target_depth(&target->rows, y);
    const blob_row_t row = project_row(lens, blobs, y, x0);
    float encoded = row.encoded;
    for (int i = 0; i < count; i++) {
        project_ellipse_row(&blobs[i], &row);
    }
    for (int x = x0; x < x1; x++, encoded += row.step) {
        const unsigned threshold = bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
        const bool visible = encoded > 0.0F && encoded < row.near_limit && encoded + row.bias >= (float)depth[x];
        const bool covered = footprint_coverage(blobs, count, encoded, threshold, thresholds, visible);
        if (covered) {
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
draw_rows(const r3d_span_target_t* target, const r3d_lens_t* lens, blob_plane_t* blobs, const blob_bounds_t* bounds,
          const unsigned* thresholds, int count, int first_row, int end_row, bool radial, bool circular) {
    const int width = mathi_min(lens->width, target->rows.attachment[GFX_ATTACHMENT_COLOR].width);
    for (int y = first_row; y < end_row; y++) {
        int x0;
        int x1;
        bounded_rows(bounds, count, width, y, &x0, &x1);
        if (x0 < x1) {
            if (radial) {
                draw_row(target, lens, blobs, y, x0, x1, circular);
            } else {
                draw_coverage_row(target, lens, blobs, count, thresholds, y, x0, x1);
            }
        }
    }
}

static bool
project_shape(const r3d_lens_t* lens, const contact_blob_t* footprint, blob_plane_t* blob, bool single, vec3f_t* out_up,
              float* out_radius) {
    const float uu = vec3f_dot(footprint->u, footprint->u);
    const float vv = vec3f_dot(footprint->v, footprint->v);
    const float uv = vec3f_dot(footprint->u, footprint->v);
    const float determinant = (uu * vv) - (uv * uv);
    if (!(determinant > 0)) {
        return false;
    }
    blob->circular = uu == vv && uv == 0;
    *out_up = vec3f_normalize(vec3f_cross(footprint->u, footprint->v));
    *out_radius = sqrtf(uu);
    if (!(single && blob->mask == NULL && blob->circular)) {
        const vec3f_t u =
            vec3f_scale(vec3f_sub(vec3f_scale(footprint->u, vv), vec3f_scale(footprint->v, uv)), 1 / determinant);
        const vec3f_t v =
            vec3f_scale(vec3f_sub(vec3f_scale(footprint->v, uu), vec3f_scale(footprint->u, uv)), 1 / determinant);
        float* du[] = {&blob->u.x, &blob->u.y, &blob->u.z};
        float* dv[] = {&blob->v.x, &blob->v.y, &blob->v.z};
        for (int r = 0; r < 3; r++) {
            const float focal = r == 0 ? lens->focal_x : r == 1 ? lens->focal_y : 1;
            *du[r] = vec3f_dot(lens_axis(lens, r), u) / (focal * focal);
            *dv[r] = vec3f_dot(lens_axis(lens, r), v) / (focal * focal);
        }
    }
    return true;
}

static bool
project_blob(const r3d_lens_t* lens, const contact_blob_t* footprint, blob_plane_t* blob, bool single) {
    if (!(footprint->strength > 0)) {
        return false;
    }
    *blob = (blob_plane_t){.centre = mat4f_apply(&lens->m, footprint->centre),
                           .strength = fminf(footprint->strength, 1),
                           .mask = footprint->mask};
    float radius;
    vec3f_t up;
    const bool ground_circle = single && blob->mask == NULL && footprint->u.y == 0 && footprint->u.z == 0
                               && footprint->v.x == 0 && footprint->v.y == 0
                               && fabsf(footprint->u.x) == fabsf(footprint->v.z);
    if (ground_circle) {
        radius = fabsf(footprint->u.x);
        if (!(radius > 0)) {
            return false;
        }
        up = vec3f_unit_y();
        blob->circular = true;
    } else if (!project_shape(lens, footprint, blob, single, &up, &radius)) {
        return false;
    }
    const float inverse_fx = 1.0F / lens->focal_x;
    const float inverse_fy = 1.0F / lens->focal_y;
    blob->normal =
        (vec3f_t){vec3f_dot(lens_axis(lens, 0), up) * inverse_fx * inverse_fx,
                  vec3f_dot(lens_axis(lens, 1), up) * inverse_fy * inverse_fy, vec3f_dot(lens_axis(lens, 2), up)};
    blob->distance = vec3f_dot(blob->normal, blob->centre);
    if (blob->distance == 0) {
        return false;
    }
    blob->inverse_scale = (vec3f_t){inverse_fx / radius, inverse_fy / radius, 1.0F / radius};
    blob->inverse_strength_root = 1.0F / sqrtf(blob->strength);
    return true;
}

typedef struct {
    contact_blob_t footprints[CONTACT_BLOBS_MAX];
    blob_plane_t blobs[CONTACT_BLOBS_MAX];
    blob_bounds_t bounds[CONTACT_BLOBS_MAX];
    unsigned thresholds[BAYER4_LEVELS];
} blob_work_t;

size_t
contact_blobs_work_bytes(void) {
    return sizeof(blob_work_t);
}

contact_blob_t*
contact_blobs_work_footprints(void* scratch) {
    blob_work_t* work = scratch;
    return work->footprints;
}

void
contact_blobs_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* footprints, int count,
                   void* scratch) {
    blob_work_t* work = scratch;
    blob_plane_t single_blob;
    blob_bounds_t single_bounds;
    blob_plane_t* blobs = count == 1 ? &single_blob : work->blobs;
    blob_bounds_t* bounds = count == 1 ? &single_bounds : work->bounds;
    int first_row = gfx_target_end_row(&target->rows);
    int end_row = target->rows.first_row;
    int used = 0;
    for (int i = 0; i < count && used < CONTACT_BLOBS_MAX; i++) {
        if (project_blob(lens, &footprints[i], &blobs[used], count == 1)) {
            project_bounds(&bounds[used], target, lens, footprints[i].centre, footprints[i].u, footprints[i].v);
            first_row = mathi_min(first_row, bounds[used].first_row);
            end_row = mathi_max(end_row, bounds[used].end_row);
            used++;
        }
    }
    if (used == 0) {
        return;
    }
    if (blobs->mask != NULL) {
        for (unsigned i = 0; i < BAYER4_LEVELS; i++) {
            work->thresholds[i] =
                (unsigned)fminf(UINT8_MAX * BAYER4_LEVELS, floorf((float)(i * UINT8_MAX) / blobs->strength));
        }
    }
    if (used == 1 && blobs->mask == NULL && blobs->circular) {
        draw_rows(target, lens, blobs, bounds, NULL, 1, first_row, end_row, true, true);
    } else {
        draw_rows(target, lens, blobs, bounds, work->thresholds, used, first_row, end_row,
                  used == 1 && blobs->mask == NULL, false);
    }
}

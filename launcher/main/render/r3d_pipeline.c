#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "render/code_layout.h"
#include "render/viewport.h"
#include "util/scalar/mathf.h"
#include "util/scalar/mathi.h"

static void
set_row(float row[4], vec3f_t axis, vec3f_t eye, float scale, float ticks_to_units) {
    row[0] = axis.x * scale * ticks_to_units;
    row[1] = axis.y * scale * ticks_to_units;
    row[2] = axis.z * scale * ticks_to_units;
    row[3] = -vec3f_dot(axis, eye) * scale;
}

void
r3d_lens_init(r3d_lens_t* lens, const render_view_t* view, int position_scale) {
    const float ticks_to_units = 1.0F / (float)position_scale;
    lens->m = mat4f_identity();
    set_row(lens->m.m[0], view->screen_x, view->position, view->pixels_per_unit, ticks_to_units);
    set_row(lens->m.m[1], view->screen_y, view->position, view->pixels_per_unit, ticks_to_units);
    set_row(lens->m.m[2], view->forward, view->position, 1.0F, ticks_to_units);
    lens->center_x = view->center_x;
    lens->center_y = view->center_y;
    lens->near_z = view->near_z;
    lens->near_subpixels = view->near_z / (float)R3D_SUBPIXEL;
    lens->snap_cx = (lens->center_x * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->snap_cy = (lens->center_y * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->width = view->viewport.width;
    lens->height = view->viewport.height;
}

void
r3d_lens_fit(r3d_lens_t* lens, int width, int height) {
    const float scale_x = (float)width / (float)lens->width;
    const float scale_y = (float)height / (float)lens->height;
    for (int j = 0; j < 4; j++) {
        lens->m.m[0][j] *= scale_x;
        lens->m.m[1][j] *= scale_y;
    }
    lens->center_x = (float)width * 0.5F;
    lens->center_y = (float)height * 0.5F;
    lens->snap_cx = (lens->center_x * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->snap_cy = (lens->center_y * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->width = width;
    lens->height = height;
}

/* Returns the placement as a matrix on position ticks: its position, in model
 * units, scaled by position_scale ticks per unit. */
static mat4f_t
r3d_placement_matrix(const r3d_placement_t* placement, int position_scale) {
    const vec3f_t p = vec3f_scale(placement->position, (float)position_scale);
    return (mat4f_t){{
        {placement->m[0][0], placement->m[0][1], placement->m[0][2], p.x},
        {placement->m[1][0], placement->m[1][1], placement->m[1][2], p.y},
        {placement->m[2][0], placement->m[2][1], placement->m[2][2], p.z},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }};
}

void
r3d_lens_place(r3d_lens_t* lens, const r3d_placement_t* placement, int position_scale) {
    lens->m = mat4f_mul_affine(lens->m, r3d_placement_matrix(placement, position_scale));
}

#define PLANE_COUNT    5
#define ALL_PLANES     ((1u << PLANE_COUNT) - 1u)
#define WALK_STACK_MAX 256

typedef struct {
    float w[4]; /* inside where w . (x, y, z, 1) >= 0, in position ticks */
} plane_t;

/* The camera's five planes (near, left, right, top, bottom) written in lens
 * space, then carried back through the lens's matrix to position ticks, so a
 * box is tested without transforming its corners. */
static void
frustum_planes(const r3d_lens_t* lens, plane_t planes[PLANE_COUNT]) {
    const float right = (float)lens->width - lens->center_x;
    const float bottom = (float)lens->height - lens->center_y;
    const float in_lens[PLANE_COUNT][4] = {
        {0.0F, 0.0F, 1.0F, -lens->near_z},  {1.0F, 0.0F, lens->center_x, 0.0F}, {-1.0F, 0.0F, right, 0.0F},
        {0.0F, 1.0F, lens->center_y, 0.0F}, {0.0F, -1.0F, bottom, 0.0F},
    };
    for (int p = 0; p < PLANE_COUNT; p++) {
        for (int j = 0; j < 4; j++) {
            planes[p].w[j] =
                in_lens[p][0] * lens->m.m[0][j] + in_lens[p][1] * lens->m.m[1][j] + in_lens[p][2] * lens->m.m[2][j];
        }
        planes[p].w[3] += in_lens[p][3];
    }
}

typedef enum { BOX_OUTSIDE, BOX_CROSSING } box_side_t;

/* Clears from *mask every plane the box lies wholly inside, so a node's
 * children skip it. */
static box_side_t
classify_box(const int16_t lo[3], const int16_t hi[3], const plane_t planes[PLANE_COUNT], unsigned* mask) {
    const float c[3] = {0.5F * ((float)lo[0] + (float)hi[0]), 0.5F * ((float)lo[1] + (float)hi[1]),
                        0.5F * ((float)lo[2] + (float)hi[2])};
    const float e[3] = {0.5F * ((float)hi[0] - (float)lo[0]), 0.5F * ((float)hi[1] - (float)lo[1]),
                        0.5F * ((float)hi[2] - (float)lo[2])};
    for (int p = 0; p < PLANE_COUNT; p++) {
        if (!(*mask & (1U << p))) {
            continue;
        }
        const float* w = planes[p].w;
        const float d = (w[0] * c[0]) + (w[1] * c[1]) + (w[2] * c[2]) + w[3];
        const float r = (fabsf(w[0]) * e[0]) + (fabsf(w[1]) * e[1]) + (fabsf(w[2]) * e[2]);
        if (d + r < 0.0F) {
            return BOX_OUTSIDE;
        }
        if (d - r >= 0.0F) {
            *mask &= ~(1U << p);
        }
    }
    return BOX_CROSSING;
}

static float
box_depth(const r3d_lens_t* lens, const int16_t lo[3], const int16_t hi[3]) {
    const float* f = lens->m.m[2];
    return (f[0] * 0.5F * ((float)lo[0] + (float)hi[0])) + (f[1] * 0.5F * ((float)lo[1] + (float)hi[1]))
           + (f[2] * 0.5F * ((float)lo[2] + (float)hi[2])) + f[3];
}

typedef struct {
    uint16_t node;
    uint8_t mask;
} walk_entry_t;

#define CLIP_PLANES     5
#define CLIP_VERTEX_MAX (3 + CLIP_PLANES)

typedef struct {
    float x, y, z, r, g, b;
} clip_vertex_t;

typedef struct {
    float x, y, z, w; /* inside where x lx + y ly + z lz + w >= 0 */
} clip_plane_t;

struct r3d_pipeline_work {
    union {
        struct {
            plane_t planes[PLANE_COUNT];
            walk_entry_t stack[WALK_STACK_MAX];
        } cull;

        struct {
            clip_vertex_t poly[CLIP_VERTEX_MAX], other[CLIP_VERTEX_MAX];
            r3d_span_vertex_t projected[CLIP_VERTEX_MAX];
            clip_plane_t planes[CLIP_PLANES];
        } clip;
    };
};

_Static_assert(_Alignof(r3d_pipeline_work_t) <= R3D_PIPELINE_WORK_ALIGNMENT, "pipeline work alignment");

size_t
r3d_pipeline_work_bytes(void) {
    return mathi_size_ceil(sizeof(r3d_pipeline_work_t), R3D_PIPELINE_WORK_ALIGNMENT);
}

/* Children go on the stack farthest first, so the nearest pops first and
 * the walk visits leaves roughly front to back. */
static int
push_children(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_node_t* node, unsigned mask,
              walk_entry_t* stack, int top) {
    uint16_t order[8];
    float depth[8];
    int n = 0;
    for (int i = 0; i < node->count; i++) {
        const uint16_t child = (uint16_t)(node->first + i);
        const float d = box_depth(lens, mesh->nodes[child].lo, mesh->nodes[child].hi);
        int slot = n++;
        while (slot > 0 && depth[slot - 1] < d) {
            depth[slot] = depth[slot - 1];
            order[slot] = order[slot - 1];
            slot--;
        }
        depth[slot] = d;
        order[slot] = child;
    }
    assert(top + n <= WALK_STACK_MAX); /* depth times seven, far below this */
    for (int i = 0; i < n; i++) {
        stack[top++] = (walk_entry_t){order[i], (uint8_t)mask};
    }
    return top;
}

int
r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out, r3d_pipeline_work_t* work) {
    plane_t* planes = work->cull.planes;
    frustum_planes(lens, planes);

    walk_entry_t* stack = work->cull.stack;
    int top = 0;
    stack[top++] = (walk_entry_t){0, ALL_PLANES};
    int count = 0;
    while (top > 0) {
        const walk_entry_t entry = stack[--top];
        const r3d_lit_node_t* node = &mesh->nodes[entry.node];
        unsigned mask = entry.mask;
        if (mask != 0 && classify_box(node->lo, node->hi, planes, &mask) == BOX_OUTSIDE) {
            continue;
        }
        if (!node->leaf) {
            top = push_children(mesh, lens, node, mask, stack, top);
            continue;
        }
        for (int i = 0; i < node->count; i++) {
            const int c = node->first + i;
            unsigned cluster_mask = mask;
            if (cluster_mask == 0
                || classify_box(mesh->clusters[c].lo, mesh->clusters[c].hi, planes, &cluster_mask) != BOX_OUTSIDE) {
                out[count++] = (uint16_t)c;
            }
        }
    }
    return count;
}

/* The rebuilt path clips to GUARD_PIXELS from the origin, inside what
 * r3d_span takes; the fast path keeps vertices within FAST_PIXELS, inside
 * the guard, so a clip never cuts an edge that a fast triangle shares. */
#define RANGE_PIXELS (R3D_SPAN_RANGE >> R3D_SUBPIXEL_SHIFT)
#define GUARD_PIXELS (RANGE_PIXELS - 16)
#define FAST_PIXELS  (GUARD_PIXELS - 16)
#define FAST_LO      (R3D_SNAP_BIAS - (float)(FAST_PIXELS * R3D_SUBPIXEL))
#define FAST_HI      (R3D_SNAP_BIAS + (float)(FAST_PIXELS * R3D_SUBPIXEL))

typedef struct {
    float x, y; /* subpixels plus R3D_SNAP_BIAS */
} biased_t;

/* The one place a lens position becomes a screen position, so a vertex
 * that both paths project snaps alike. `inv` is R3D_SUBPIXEL / z. */
static inline biased_t
biased_screen(const r3d_lens_t* lens, float lx, float ly, float inv) {
    return (biased_t){lens->snap_cx + (lx * inv), lens->snap_cy + (ly * inv)};
}

static inline r3d_pipeline_rows_t
transform_cluster(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_cluster_t* c,
                  r3d_pipeline_vertex_t* cs) {
    const int end = c->vertex_first + c->vertex_count;
    int y0 = INT16_MAX;
    int y1 = INT16_MIN;
    bool unbounded = false;
    for (int v = c->vertex_first; v < end; v++) {
        const int16_t* p = mesh->positions[v];
        const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
        r3d_pipeline_vertex_t* out = &cs[v];
        if (l.z <= lens->near_z) {
            out->iz = 0.0F;
            unbounded = true;
            continue;
        }
        const float inv = (float)R3D_SUBPIXEL * mathf_recip(l.z);
        const biased_t b = biased_screen(lens, l.x, l.y, inv);
        if (!(b.x > FAST_LO && b.x < FAST_HI && b.y > FAST_LO && b.y < FAST_HI)) {
            out->iz = -1.0F;
            unbounded = true;
            continue;
        }
        out->sx = (int16_t)r3d_span_unbias(b.x);
        out->sy = (int16_t)r3d_span_unbias(b.y);
        out->iz = lens->near_subpixels * inv;
        y0 = out->sy < y0 ? out->sy : y0;
        y1 = out->sy > y1 ? out->sy : y1;
    }
    const float to_pixels = 1.0F / (float)R3D_SUBPIXEL;
    return (r3d_pipeline_rows_t){(float)y0 * to_pixels, (float)y1 * to_pixels, unbounded};
}

void
r3d_pipeline_transform(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                       r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows) {
    for (int i = 0; i < count; i++) {
        const r3d_pipeline_rows_t r = transform_cluster(mesh, lens, &mesh->clusters[clusters[i]], cs);
        if (rows != NULL) {
            rows[clusters[i]] = r;
        }
    }
}

int
r3d_pipeline_transform_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += mesh->clusters[clusters[i]].vertex_count;
    }
    int prefix = 0;
    for (int i = 0; i < count; i++) {
        prefix += mesh->clusters[clusters[i]].vertex_count;
        if (2 * prefix >= total) {
            return i + 1;
        }
    }
    return count;
}

#define DRAW_SPLIT_BUCKETS 64

static void
draw_split_histogram(int* weight, const r3d_lit_mesh_t* mesh, const uint16_t* clusters, const r3d_pipeline_rows_t* rows,
                     int count, int height) {
    for (int i = 0; i < count; i++) {
        const r3d_pipeline_rows_t* r = &rows[clusters[i]];
        const int first = mathi_clamp(r->unbounded ? 0 : (int)r->y0, 0, height - 1);
        const int last = mathi_clamp(r->unbounded ? height - 1 : (int)r->y1, 0, height - 1);
        const int cost = mesh->clusters[clusters[i]].triangle_count;
        const int bucket_first = first * DRAW_SPLIT_BUCKETS / height;
        const int bucket_last = last * DRAW_SPLIT_BUCKETS / height;
        weight[bucket_first] += cost;
        weight[bucket_last + 1] -= cost;
    }
}

static int
draw_split_row(const int* weight, int height) {
    int total = 0;
    int current = 0;
    for (int bucket = 0; bucket < DRAW_SPLIT_BUCKETS; bucket++) {
        current += weight[bucket];
        total += current;
    }
    if (total == 0) {
        return height / 2;
    }
    current = 0;
    int prefix = 0;
    for (int bucket = 0; bucket < DRAW_SPLIT_BUCKETS; bucket++) {
        current += weight[bucket];
        prefix += current;
        if (2 * prefix >= total) {
            return mathi_clamp((2 * bucket + 1) * height / (2 * DRAW_SPLIT_BUCKETS), 1, height - 1);
        }
    }
    return height / 2;
}

int
r3d_pipeline_draw_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, const r3d_pipeline_rows_t* rows,
                        int count, int height) {
    int weight[DRAW_SPLIT_BUCKETS + 1] = {0};
    draw_split_histogram(weight, mesh, clusters, rows, count, height);
    return draw_split_row(weight, height);
}

static r3d_span_vertex_t
project(const r3d_lens_t* lens, const clip_vertex_t* v) {
    const float inv = (float)R3D_SUBPIXEL * mathf_recip(v->z);
    const biased_t b = biased_screen(lens, v->x, v->y, inv);
    return (r3d_span_vertex_t){
        r3d_span_unbias(b.x), r3d_span_unbias(b.y), lens->near_subpixels * inv, v->r, v->g, v->b};
}

/* Front faces wind negative on screen: counter-clockwise in a y-up world
 * turns clockwise once screen y points down. */
static inline bool
facing_away(int32_t area2, bool double_sided) {
    return !double_sided && area2 >= 0;
}

/* Twice the signed area; with coordinates inside R3D_SPAN_RANGE each
 * product is under 2^30. */
static inline int32_t
signed_area2(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy) {
    return ((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay));
}

static inline float
plane_distance(const clip_plane_t* p, const clip_vertex_t* v) {
    return (p->x * v->x) + (p->y * v->y) + (p->z * v->z) + p->w;
}

static int
clip_to_plane(const clip_plane_t* p, const clip_vertex_t* in, int n, clip_vertex_t* out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const clip_vertex_t* a = &in[i];
        const clip_vertex_t* b = &in[(i + 1) % n];
        const float da = plane_distance(p, a);
        const float db = plane_distance(p, b);
        if (da >= 0.0F) {
            out[m++] = *a;
        }
        if ((da >= 0.0F) != (db >= 0.0F)) {
            const float t = da / (da - db);
            out[m++] =
                (clip_vertex_t){a->x + ((b->x - a->x) * t), a->y + ((b->y - a->y) * t), a->z + ((b->z - a->z) * t),
                                a->r + ((b->r - a->r) * t), a->g + ((b->g - a->g) * t), a->b + ((b->b - a->b) * t)};
        }
    }
    return m;
}

/* The near plane, then the screen's guard band: every corner left projects
 * inside what r3d_span takes. */
static int
clip_to_guard(const r3d_lens_t* lens, r3d_pipeline_work_t* work, int n) {
    clip_vertex_t* poly = work->clip.poly;
    const float g = (float)GUARD_PIXELS;
    clip_plane_t* planes = work->clip.planes;
    planes[0] = (clip_plane_t){0.0F, 0.0F, 1.0F, -lens->near_z};
    planes[1] = (clip_plane_t){1.0F, 0.0F, lens->center_x + g, 0.0F};
    planes[2] = (clip_plane_t){-1.0F, 0.0F, g - lens->center_x, 0.0F};
    planes[3] = (clip_plane_t){0.0F, 1.0F, lens->center_y + g, 0.0F};
    planes[4] = (clip_plane_t){0.0F, -1.0F, g - lens->center_y, 0.0F};
    clip_vertex_t* other = work->clip.other;
    for (int p = 0; p < CLIP_PLANES && n >= 3; p++) {
        n = clip_to_plane(&planes[p], poly, n, other);
        memcpy(poly, other, sizeof(clip_vertex_t) * (size_t)n);
    }
    return n;
}

/* One triangle, then the target's writers over it when `writes`: a constant
 * at every caller, so the draw without writers carries none of it. */
static inline __attribute__((always_inline)) void
fill_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
              const r3d_span_vertex_t* c, const uint16_t* face_color, bool writes) {
    if (face_color == NULL) {
        r3d_span_triangle(target, a, b, c);
    } else {
        r3d_span_triangle_solid(target, a, b, c, *face_color);
    }
    if (writes) {
        r3d_span_triangle_write(target, a, b, c);
    }
}

static inline __attribute__((always_inline)) void
draw_clipped(const r3d_lens_t* lens, bool double_sided, const uint16_t* face_color, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    clip_vertex_t* poly = work->clip.poly;
    const int n = clip_to_guard(lens, work, 3);
    if (n < 3) {
        return;
    }
    r3d_span_vertex_t* s = work->clip.projected;
    int64_t area2 = 0;
    for (int i = 0; i < n; i++) {
        s[i] = project(lens, &poly[i]);
    }
    for (int i = 1; i + 1 < n; i++) {
        area2 += signed_area2(s[0].x, s[0].y, s[i].x, s[i].y, s[i + 1].x, s[i + 1].y);
    }
    if (!double_sided && area2 >= 0) {
        return;
    }
    for (int i = 1; i + 1 < n; i++) {
        fill_triangle(target, &s[0], &s[i], &s[i + 1], face_color, target->writer_count != 0);
    }
}

/* True when the bounding box holds no pixel centre inside the target, so
 * the rasterizer would fill nothing: most of these are triangles smaller
 * than a pixel falling between centres. */
static inline bool
misses_every_centre(const r3d_pipeline_vertex_t* a, const r3d_pipeline_vertex_t* b, const r3d_pipeline_vertex_t* c,
                    const r3d_span_target_t* target) {
    const int x_first = r3d_span_first_centre(r3d_span_min3(a->sx, b->sx, c->sx));
    const int x_end = r3d_span_first_centre(r3d_span_max3(a->sx, b->sx, c->sx));
    const int y_first = r3d_span_first_centre(r3d_span_min3(a->sy, b->sy, c->sy));
    const int y_end = r3d_span_first_centre(r3d_span_max3(a->sy, b->sy, c->sy));
    return x_first >= x_end || y_first >= y_end || x_end <= 0 || x_first >= target->rows.width
           || y_end <= target->rows.row0 || y_first >= target->rows.row1;
}

static inline bool
rows_miss_target(const r3d_pipeline_rows_t* r, const r3d_span_target_t* target) {
    return !r->unbounded && (r->y1 < (float)target->rows.row0 || r->y0 > (float)target->rows.row1);
}

/* A triangle with a corner behind the near plane or too far off screen to
 * snap is rebuilt from the mesh and clipped. */
static __attribute__((noinline, cold)) void
draw_rebuilt(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* tri, const uint16_t* face_color,
             bool double_sided, const r3d_span_target_t* target, r3d_pipeline_work_t* work) {
    clip_vertex_t* in = work->clip.poly;
    for (int k = 0; k < 3; k++) {
        const int16_t* p = mesh->positions[tri[k]];
        const uint8_t* rgb = face_color == NULL ? mesh->colors[tri[k]] : (const uint8_t[3]){0, 0, 0};
        const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
        in[k] = (clip_vertex_t){l.x, l.y, l.z, rgb[0], rgb[1], rgb[2]};
    }
    draw_clipped(lens, double_sided, face_color, target, work);
}

static inline __attribute__((always_inline)) void
draw_in_front(const r3d_lit_mesh_t* mesh, const r3d_pipeline_vertex_t* const v[3], const uint16_t* tri,
              const uint16_t* face_color, bool double_sided, const r3d_span_target_t* target, bool writes) {
    if (misses_every_centre(v[0], v[1], v[2], target)) {
        return;
    }
    if (facing_away(signed_area2(v[0]->sx, v[0]->sy, v[1]->sx, v[1]->sy, v[2]->sx, v[2]->sy), double_sided)) {
        return;
    }
    r3d_span_vertex_t s[3];
    for (int k = 0; k < 3; k++) {
        const uint8_t* rgb = face_color == NULL ? mesh->colors[tri[k]] : (const uint8_t[3]){0, 0, 0};
        s[k] = (r3d_span_vertex_t){v[k]->sx, v[k]->sy, v[k]->iz, rgb[0], rgb[1], rgb[2]};
    }
    fill_triangle(target, &s[0], &s[1], &s[2], face_color, writes);
}

static inline __attribute__((always_inline)) void
draw_cluster(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_cluster_t* c,
             const r3d_pipeline_vertex_t* cs, const r3d_span_target_t* target, bool writes, r3d_pipeline_work_t* work) {
    const int end = c->triangle_first + c->triangle_count;
    for (int t = c->triangle_first; t < end; t++) {
        const uint16_t* tri = mesh->triangles[t];
        const uint16_t* face_color = mesh->face_colors == NULL ? NULL : &mesh->face_colors[t];
        const r3d_pipeline_vertex_t* const v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
        if (v[0]->iz > 0.0F && v[1]->iz > 0.0F && v[2]->iz > 0.0F) {
            draw_in_front(mesh, v, tri, face_color, c->double_sided, target, writes);
        } else if (v[0]->iz != 0.0F || v[1]->iz != 0.0F || v[2]->iz != 0.0F) {
            draw_rebuilt(mesh, lens, tri, face_color, c->double_sided, target, work);
        }
    }
}

/* The draw for a target with writers, kept out of the hot one's code. */
static __attribute__((noinline, cold)) void
draw_writing(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
             const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    r3d_span_writer_t writers[GFX_ATTACHMENTS_MAX];
    r3d_span_target_t writing = *target;
    writing.writers = writers;
    for (int k = 0; k < target->writer_count; k++) {
        writers[k] = target->writers[k];
    }
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < target->writer_count; k++) {
            if (writers[k].per_cluster) {
                writers[k].value = target->writers[k].value + clusters[i];
            }
        }
        if (rows == NULL || !rows_miss_target(&rows[clusters[i]], target)) {
            draw_cluster(mesh, lens, &mesh->clusters[clusters[i]], cs, &writing, true, work);
        }
    }
}

/* Chooses once per draw whether the target's writers follow each fill. */
RENDER_ENTRY_OFFSET(4) void
r3d_pipeline_draw(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                  const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
                  r3d_pipeline_work_t* work) {
    if (target->writer_count != 0) {
        draw_writing(mesh, lens, clusters, count, cs, rows, target, work);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (rows != NULL && rows_miss_target(&rows[clusters[i]], target)) {
            continue;
        }
        draw_cluster(mesh, lens, &mesh->clusters[clusters[i]], cs, target, false, work);
    }
}

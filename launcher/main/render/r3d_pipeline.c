#include "render/r3d_pipeline.h"

#include "render/r3d_span_internal.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "collections/sort.h"
#include "core/job.h"
#include "display/viewport.h"
#include "math/geometry/planef.h"
#include "math/linear/mat3f.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "profile/frame_laps.h"
#include "render/code_layout.h"
#include "render/r3d_triangle_tests.h"

void
r3d_lens_init(r3d_lens_t* lens, const render_view_t* view, int position_scale) {
    const float ticks_to_units = 1.0F / (float)position_scale;
    const mat4f_t camera = mat4f_view_from_axes(view->screen_x, view->screen_y, view->forward, view->position);
    const mat4f_t pixels = mat4f_scale_rows(camera, (vec3f_t){view->lens.fx, view->lens.fy, 1.0F});
    lens->m = mat4f_scale_columns(pixels, (vec3f_t){ticks_to_units, ticks_to_units, ticks_to_units});
    lens->project = (projectf_t){1.0F, -1.0F, view->lens.cx, view->lens.cy, view->lens.near_z};
    r3d_lens_set_depth_near(lens, view->lens.near_z);
    lens->snap_cx = (lens->project.cx * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->snap_cy = (lens->project.cy * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->width = view->viewport.width;
    lens->height = view->viewport.height;
}

void
r3d_lens_set_depth_near(r3d_lens_t* lens, float depth_near) {
    lens->depth_subpixels = depth_near / (float)R3D_SUBPIXEL;
}

void
r3d_lens_fit(r3d_lens_t* lens, int width, int height) {
    const float scale_x = (float)width / (float)lens->width;
    const float scale_y = (float)height / (float)lens->height;
    lens->m = mat4f_scale_rows(lens->m, (vec3f_t){scale_x, scale_y, 1.0F});
    /* The same picture at another size: the optical centre scales with it,
     * so a shifted lens stays shifted. */
    lens->project.cx *= scale_x;
    lens->project.cy *= scale_y;
    lens->snap_cx = (lens->project.cx * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->snap_cy = (lens->project.cy * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->width = width;
    lens->height = height;
}

void
r3d_lens_place(r3d_lens_t* lens, const mat4f_t* model, int position_scale) {
    mat4f_t placed = *model;
    for (int row = 0; row < MAT3F_AXES; row++) {
        placed.m[row][MAT3F_AXES] *= (float)position_scale;
    }
    lens->m = mat4f_mul_affine(lens->m, placed);
}

#define ALL_PLANES ((1u << PLANE_FRUSTUM_COUNT) - 1u)

typedef struct {
    uint16_t node;
    uint8_t mask;
} walk_entry_t;

#define CLIP_VERTEX_MAX (3 + PLANE_FRUSTUM_COUNT)

typedef struct {
    float x, y, z, r, g, b;
} clip_vertex_t;

struct r3d_pipeline_work {
    union {
        struct {
            planef_t planes[PLANE_FRUSTUM_COUNT];
            walk_entry_t stack[R3D_LIT_WALK_STACK_MAX];
        } cull;

        struct {
            clip_vertex_t poly[CLIP_VERTEX_MAX], other[CLIP_VERTEX_MAX];
            r3d_span_vertex_t projected[CLIP_VERTEX_MAX];
            planef_t planes[PLANE_FRUSTUM_COUNT];
        } clip;
    };
};

_Static_assert(_Alignof(r3d_pipeline_work_t) <= R3D_PIPELINE_WORK_ALIGNMENT, "pipeline work alignment");

size_t
r3d_pipeline_work_bytes(void) {
    return mathi_size_ceil(sizeof(r3d_pipeline_work_t), R3D_PIPELINE_WORK_ALIGNMENT);
}

typedef struct {
    float depth;
    uint32_t node;
} child_depth_t;

static inline bool
farther(child_depth_t a, child_depth_t b) {
    return a.depth > b.depth;
}

SORT_DEFINE_INSERTION(children, child_depth_t, farther)

/* Children go on the stack farthest first, so the nearest pops first and
 * the walk visits leaves roughly front to back. */
static int
push_children(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_node_t* node, unsigned mask,
              walk_entry_t* stack, int top) {
    child_depth_t order[R3D_LIT_NODE_CHILDREN_MAX];
    const float* f = lens->m.m[2];
    const planef_t depth = {{f[0], f[1], f[2]}, f[3]};
    const int n = node->count;
    assert(n <= R3D_LIT_NODE_CHILDREN_MAX);
    for (int i = 0; i < n; i++) {
        const uint16_t child = (uint16_t)(node->first + i);
        /* The lens maps ticks, so the bounds scale is one. */
        const float d = planef_distance(depth, box3f_centre(box3f_from_box3s(mesh->nodes[child].bounds, 1.0F)));
        children_insert(order, (size_t)i, (child_depth_t){d, child});
    }
    assert(top + n <= R3D_LIT_WALK_STACK_MAX);
    for (int i = 0; i < n; i++) {
        stack[top++] = (walk_entry_t){(uint16_t)order[i].node, (uint8_t)mask};
    }
    return top;
}

int
r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out, r3d_pipeline_work_t* work) {
    planef_t* planes = work->cull.planes;
    planef_frustum(&lens->m, &lens->project, rectf_from_edges(0.0F, 0.0F, (float)lens->width, (float)lens->height),
                   planes);

    walk_entry_t* stack = work->cull.stack;
    int top = 0;
    stack[top++] = (walk_entry_t){0, ALL_PLANES};
    int count = 0;
    while (top > 0) {
        const walk_entry_t entry = stack[--top];
        const r3d_lit_node_t* node = &mesh->nodes[entry.node];
        unsigned mask = entry.mask;
        if (mask != 0
            && box3f_outside_planes(box3f_from_box3s(node->bounds, 1.0F), planes, PLANE_FRUSTUM_COUNT, &mask)) {
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
                || !box3f_outside_planes(box3f_from_box3s(mesh->clusters[c].bounds, 1.0F), planes, PLANE_FRUSTUM_COUNT,
                                         &cluster_mask)) {
                out[count++] = (uint16_t)c;
            }
        }
    }
    return count;
}

float
r3d_pipeline_nearest(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                     float nearest) {
    /* Lens space's z is view depth, so the plane z = 0 measures it. */
    const planef_t depth = planef_from_rows(&lens->m, 0.0F, 0.0F, 1.0F, 0.0F);
    planef_t frustum[PLANE_FRUSTUM_COUNT];
    planef_frustum(&lens->m, &lens->project, rectf_from_edges(0.0F, 0.0F, (float)lens->width, (float)lens->height),
                   frustum);
    for (int i = 0; i < count; i++) {
        const box3f_t box = box3f_from_box3s(mesh->clusters[clusters[i]].bounds, 1.0F);
        float least = box3f_plane_min(box, depth);
        /* Only a box that could lower the bound pays for the part of it
         * inside each frustum plane: a big one reaching behind the eye
         * is drawn only where the frustum keeps it. */
        for (int p = 0; p < PLANE_FRUSTUM_COUNT && least < nearest; p++) {
            least = mathf_max(least, box3f_plane_min_inside(box, depth, frustum[p]));
        }
        nearest = mathf_min(nearest, least);
    }
    return nearest;
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
        FRAME_LAP("r3d.transform.apply");
        const int16_t* p = mesh->positions[v];
        const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
        r3d_pipeline_vertex_t* out = &cs[v];
        if (l.z <= lens->project.near_z) {
            out->iz = 0.0F;
            unbounded = true;
            continue;
        }
        FRAME_LAP("r3d.transform.project");
        const float inv = (float)R3D_SUBPIXEL * mathf_recip(l.z);
        const biased_t b = biased_screen(lens, l.x, l.y, inv);
        if (!(b.x > FAST_LO && b.x < FAST_HI && b.y > FAST_LO && b.y < FAST_HI)) {
            out->iz = -1.0F;
            unbounded = true;
            continue;
        }
        out->sx = (int16_t)r3d_span_unbias(b.x);
        out->sy = (int16_t)r3d_span_unbias(b.y);
        out->iz = lens->depth_subpixels * inv;
        y0 = out->sy < y0 ? out->sy : y0;
        y1 = out->sy > y1 ? out->sy : y1;
    }
    FRAME_LAP("r3d.transform.cluster");
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
r3d_pipeline_transform_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, int count, int first_permille) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += mesh->clusters[clusters[i]].vertex_count;
    }
    int prefix = 0;
    for (int i = 0; i < count; i++) {
        prefix += mesh->clusters[clusters[i]].vertex_count;
        if ((int64_t)prefix * JOB_SHARE_PERMILLE >= (int64_t)total * first_permille) {
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
draw_split_row(const int* weight, int height, int top_permille) {
    int total = 0;
    int current = 0;
    for (int bucket = 0; bucket < DRAW_SPLIT_BUCKETS; bucket++) {
        current += weight[bucket];
        total += current;
    }
    if (total == 0) {
        return height / 2;
    }
    const int64_t target = (int64_t)total * top_permille;
    current = 0;
    int prefix = 0;
    for (int bucket = 0; bucket < DRAW_SPLIT_BUCKETS; bucket++) {
        current += weight[bucket];
        if (current > 0 && (int64_t)(prefix + current) * JOB_SHARE_PERMILLE >= target) {
            const int64_t bucket_weight = (int64_t)current * JOB_SHARE_PERMILLE;
            const int64_t position = ((bucket * bucket_weight) + target) - ((int64_t)prefix * JOB_SHARE_PERMILLE);
            return mathi_clamp((int)(position * height / (bucket_weight * DRAW_SPLIT_BUCKETS)), 1, height - 1);
        }
        prefix += current;
    }
    return height / 2;
}

int
r3d_pipeline_draw_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, const r3d_pipeline_rows_t* rows,
                        int count, int height, int top_permille) {
    assert(top_permille >= 0 && top_permille <= JOB_SHARE_PERMILLE);
    int weight[DRAW_SPLIT_BUCKETS + 1] = {0};
    draw_split_histogram(weight, mesh, clusters, rows, count, height);
    return draw_split_row(weight, height, top_permille);
}

static r3d_span_vertex_t
project(const r3d_lens_t* lens, const clip_vertex_t* v) {
    const float inv = (float)R3D_SUBPIXEL * mathf_recip(v->z);
    const biased_t b = biased_screen(lens, v->x, v->y, inv);
    return (r3d_span_vertex_t){
        r3d_span_unbias(b.x), r3d_span_unbias(b.y), lens->depth_subpixels * inv, v->r, v->g, v->b};
}

/* One Sutherland-Hodgman pass (Sutherland and Hodgman 1974): the polygon's part on the plane's inside. */
static int
clip_to_plane(const planef_t* p, const clip_vertex_t* in, int n, clip_vertex_t* out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const clip_vertex_t* a = &in[i];
        const clip_vertex_t* b = &in[(i + 1) % n];
        const float da = planef_distance(*p, (vec3f_t){a->x, a->y, a->z});
        const float db = planef_distance(*p, (vec3f_t){b->x, b->y, b->z});
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

/* Guard planes are evaluated in lens space. */
static const mat4f_t LENS_SPACE = MAT4F_IDENTITY;

/* The near plane, then the screen's guard band: every corner left projects
 * inside what r3d_span takes. */
static int
clip_to_guard(r3d_pipeline_work_t* work, int n) {
    clip_vertex_t* poly = work->clip.poly;
    const planef_t* planes = work->clip.planes;
    clip_vertex_t* other = work->clip.other;
    for (int p = 0; p < PLANE_FRUSTUM_COUNT && n >= 3; p++) {
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
    FRAME_LAP("r3d.draw.set_up");
    if (face_color == NULL) {
        r3d_span_triangle(target, a, b, c);
    } else {
        r3d_span_triangle_solid(target, a, b, c, *face_color);
    }
    FRAME_LAP("r3d.draw.cluster");
    if (writes) {
        r3d_span_triangle_write(target, a, b, c, face_color != NULL);
    }
}

static inline __attribute__((always_inline)) void
draw_clipped(const r3d_lens_t* lens, bool double_sided, const uint16_t* face_color, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    clip_vertex_t* poly = work->clip.poly;
    const int n = clip_to_guard(work, 3);
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

static inline bool
rows_miss_target(const r3d_pipeline_rows_t* r, const r3d_span_target_t* target) {
    return !r->unbounded && (r->y1 < (float)target->rows.first_row || r->y0 > (float)gfx_target_end_row(&target->rows));
}

/* A triangle with a corner behind the near plane or too far off screen to
 * snap is rebuilt from the mesh and clipped. */
static __attribute__((noinline, cold)) void
draw_rebuilt(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* tri, const uint16_t* face_color,
             bool double_sided, const r3d_span_target_t* target, r3d_pipeline_work_t* work) {
    FRAME_LAP("r3d.draw.clip");
    clip_vertex_t* in = work->clip.poly;
    for (int k = 0; k < 3; k++) {
        const int16_t* p = mesh->positions[tri[k]];
        const uint8_t* rgb = face_color == NULL ? mesh->colors[tri[k]] : (const uint8_t[3]){0, 0, 0};
        const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
        in[k] = (clip_vertex_t){l.x, l.y, l.z, rgb[0], rgb[1], rgb[2]};
    }
    draw_clipped(lens, double_sided, face_color, target, work);
    FRAME_LAP("r3d.draw.cluster");
}

static inline __attribute__((always_inline)) void
draw_in_front(const r3d_lit_mesh_t* mesh, const r3d_pipeline_vertex_t* const v[3], const uint16_t* tri,
              const uint16_t* face_color, bool double_sided, const r3d_span_target_t* target, bool writes,
              r3d_pipeline_work_t* work) {
    FRAME_LAP("r3d.draw.tests");
    if (misses_every_centre(v[0], v[1], v[2], target)) {
        FRAME_LAP("r3d.draw.cluster");
        return;
    }
    if (facing_away(signed_area2(v[0]->sx, v[0]->sy, v[1]->sx, v[1]->sy, v[2]->sx, v[2]->sy), double_sided)) {
        FRAME_LAP("r3d.draw.cluster");
        return;
    }
    FRAME_LAP("r3d.draw.vertices");
    r3d_span_vertex_t s[3];
    for (int k = 0; k < 3; k++) {
        const uint8_t* rgb = face_color == NULL ? mesh->colors[tri[k]] : (const uint8_t[3]){0, 0, 0};
        s[k] = (r3d_span_vertex_t){v[k]->sx, v[k]->sy, v[k]->iz, rgb[0], rgb[1], rgb[2]};
    }
    fill_triangle(target, &s[0], &s[1], &s[2], face_color, writes);
}

/* Triangles first to end - 1 of one cluster. */
static inline __attribute__((always_inline)) void
draw_triangles(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_cluster_t* c, int first, int end,
               const r3d_pipeline_vertex_t* cs, const r3d_span_target_t* target, bool writes,
               r3d_pipeline_work_t* work) {
    for (int t = first; t < end; t++) {
        const uint16_t* tri = mesh->triangles[t];
        const uint16_t* face_color = mesh->face_colors == NULL ? NULL : &mesh->face_colors[t];
        const r3d_pipeline_vertex_t* const v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
        if (v[0]->iz > 0.0F && v[1]->iz > 0.0F && v[2]->iz > 0.0F) {
            draw_in_front(mesh, v, tri, face_color, c->double_sided, target, writes, work);
        } else if (v[0]->iz != 0.0F || v[1]->iz != 0.0F || v[2]->iz != 0.0F) {
            draw_rebuilt(mesh, lens, tri, face_color, c->double_sided, target, work);
        }
    }
}

/* Each writer's value for this cluster and triangle, by its step. */
static void
retag(r3d_span_writer_t* writers, const r3d_span_writer_t* given, int count, uint32_t cluster, uint32_t triangle) {
    for (int k = 0; k < count; k++) {
        const uint32_t step = writers[k].step == R3D_SPAN_STEP_CLUSTER    ? cluster
                              : writers[k].step == R3D_SPAN_STEP_TRIANGLE ? triangle
                                                                          : 0U;
        writers[k].value = given[k].value + step;
    }
}

/* The draw for a target with writers, kept out of the hot one's code. */
static __attribute__((noinline, cold)) void
draw_writing(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
             const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    assert(target->writer_count <= GFX_ATTACHMENTS_FURTHER_MAX);
    r3d_span_writer_t writers[GFX_ATTACHMENTS_FURTHER_MAX];
    r3d_span_target_t writing = *target;
    writing.writers = writers;
    bool per_triangle = false;
    for (int k = 0; k < target->writer_count; k++) {
        writers[k] = target->writers[k];
        per_triangle = per_triangle || writers[k].step == R3D_SPAN_STEP_TRIANGLE;
    }
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        if (rows != NULL && rows_miss_target(&rows[clusters[i]], target)) {
            continue;
        }
        /* Whole clusters, or one triangle at a time when a writer steps per
         * triangle, so it is retagged here and the cluster draw stays the
         * hot one's. */
        const int end = c->triangle_first + c->triangle_count;
        const int step = per_triangle ? 1 : c->triangle_count;
        for (int t = c->triangle_first; t < end; t += step) {
            retag(writers, target->writers, target->writer_count, clusters[i], (uint32_t)t);
            draw_triangles(mesh, lens, c, t, t + step, cs, &writing, true, work);
        }
    }
}

/* Keep once-per-draw plane setup in flash to preserve the hot-path IRAM budget. */
static __attribute__((noinline)) void
build_guard_planes(const r3d_lens_t* lens, r3d_pipeline_work_t* work) {
    const float g = (float)GUARD_PIXELS;
    planef_frustum(&LENS_SPACE, &lens->project, rectf_from_edges(-g, -g, g, g), work->clip.planes);
}

static RENDER_IRAM_ATTR __attribute__((noinline)) void
draw_reading(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
             const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    for (int i = 0; i < count; i++) {
        if (rows != NULL && rows_miss_target(&rows[clusters[i]], target)) {
            continue;
        }
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        draw_triangles(mesh, lens, c, c->triangle_first, c->triangle_first + c->triangle_count, cs, target, false,
                       work);
    }
}

/* Chooses once per draw whether the target's writers follow each fill. */
void
r3d_pipeline_draw(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                  const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
                  r3d_pipeline_work_t* work) {
    build_guard_planes(lens, work);
    if (target->writer_count != 0) {
        draw_writing(mesh, lens, clusters, count, cs, rows, target, work);
        return;
    }
    draw_reading(mesh, lens, clusters, count, cs, rows, target, work);
}

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
#include "math/geometry/box3f.h"
#include "math/geometry/planef.h"
#include "math/linear/mat3f.h"
#include "math/projection/inverse_depth.h"
#include "math/scalar/float_bits.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "profile/frame_laps.h"
#include "render/code_layout.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_triangle_tests.h"

void
r3d_lens_init(r3d_lens_t* lens, const render_view_t* view, int position_scale) {
    const float ticks_to_units = 1.0F / (float)position_scale;
    const mat4f_t camera = mat4f_view_from_axes(view->screen_x, view->screen_y, view->forward, view->position);
    const mat4f_t pixels = mat4f_scale_rows(camera, (vec3f_t){view->lens.fx, view->lens.fy, 1.0F});
    lens->m = mat4f_scale_columns(pixels, (vec3f_t){ticks_to_units, ticks_to_units, ticks_to_units});
    lens->focal_x = view->lens.fx;
    lens->focal_y = view->lens.fy;
    lens->project = (projectf_t){1.0F, -1.0F, view->lens.cx, view->lens.cy, view->lens.near_z};
    r3d_lens_set_depth_near(lens, view->lens.near_z);
    lens->snap_cx = (lens->project.cx * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->snap_cy = (lens->project.cy * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS;
    lens->width = view->viewport.width;
    lens->height = view->viewport.height;
}

void
r3d_lens_fit(r3d_lens_t* lens, int width, int height) {
    const float scale_x = (float)width / (float)lens->width;
    const float scale_y = (float)height / (float)lens->height;
    lens->m = mat4f_scale_rows(lens->m, (vec3f_t){scale_x, scale_y, 1.0F});
    lens->focal_x *= scale_x;
    lens->focal_y *= scale_y;
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
            r3d_span_mask_t mask; /* a masked triangle's atlas and u, v; atlas NULL when it is not masked */
            vec3f_t source[R3D_SPAN_TRIANGLE_VERTICES]; /* a masked clipped triangle's corners, before clipping */
            float source_u[R3D_SPAN_TRIANGLE_VERTICES], source_v[R3D_SPAN_TRIANGLE_VERTICES];
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

/* Lens space's z is view depth: the plane z = 0, read in mesh ticks. */
static inline planef_t
lens_depth_plane(const r3d_lens_t* lens) {
    const float* f = lens->m.m[2];
    return (planef_t){{f[0], f[1], f[2]}, f[3]};
}

/* Children go on the stack farthest first, so the nearest pops first and
 * the walk visits leaves roughly front to back. */
static int
push_children(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_node_t* node, unsigned mask,
              walk_entry_t* stack, int top) {
    child_depth_t order[R3D_LIT_NODE_CHILDREN_MAX];
    const planef_t depth = lens_depth_plane(lens);
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

/* Lowers `fit` to the scale of the least view depth of the part of `box`
 * inside the planes it crosses (`crossing`, a mask of `planes`): a box
 * reaching behind the eye is drawn only where the frustum keeps it. A NULL
 * fit, or one already at near_z, needs nothing. A bound at or past
 * fit->depth_near cannot lower the scale, so the planes are tried only until
 * it gets there. */
static void
fit_inside(box3f_t box, const planef_t* planes, unsigned crossing, planef_t depth, r3d_depth_fit_t* fit) {
    if (fit == NULL || !(fit->depth_near > fit->near_z)) {
        return;
    }
    float least = box3f_plane_distance_min(box, depth);
    for (int p = 0; p < PLANE_FRUSTUM_COUNT && least < fit->depth_near; p++) {
        if (crossing & (1U << p)) {
            least = mathf_max(least, box3f_plane_distance_min_inside(box, depth, planes[p]));
        }
    }
    if (least < fit->depth_near) {
        fit->depth_near = mathf_min(fit->depth_near, inverse_depth_scale_fit(fit->near_z, least));
    }
}

/* Writes cluster `c` to `out` at `count` when the planes in `mask` keep it,
 * lowering `fit`, and returns the new count. */
static inline int
cull_cluster(const r3d_lit_mesh_t* mesh, int c, unsigned mask, const planef_t* planes, planef_t depth, uint16_t* out,
             int count, r3d_depth_fit_t* fit) {
    const box3f_t box = box3f_from_box3s(mesh->clusters[c].bounds, 1.0F);
    if (mask == 0 || !box3f_outside_planes(box, planes, PLANE_FRUSTUM_COUNT, &mask)) {
        out[count++] = (uint16_t)c;
        fit_inside(box, planes, mask, depth, fit);
    }
    return count;
}

/* The clusters of a leaf the planes in `mask` keep, as cull_cluster(): all
 * of them when `every`, else those r3d_lod_drawn() draws under `lod_coarse`. */
static int
cull_leaf(const r3d_lit_mesh_t* mesh, const uint32_t* lod_coarse, bool every, const r3d_lit_node_t* leaf, unsigned mask,
          const planef_t* planes, planef_t depth, uint16_t* out, int count, r3d_depth_fit_t* fit) {
    for (int i = 0; i < leaf->count; i++) {
        const int c = leaf->first + i;
        if (every || r3d_lod_drawn(mesh, c, lod_coarse)) {
            count = cull_cluster(mesh, c, mask, planes, depth, out, count, fit);
        }
    }
    return count;
}

/* The first cluster from `lo` whose own group is at least `g`: the coarse
 * clusters run in the order of their own group. */
static int
group_run(const r3d_lit_mesh_t* mesh, int lo, uint16_t g) {
    int hi = mesh->cluster_count;
    while (lo < hi) {
        const int mid = lo + ((hi - lo) / 2);
        if (mesh->cluster_lod[mid].own < g) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Groups come in rising order, so each search starts where the last run
 * ended. */
int
r3d_pipeline_cull_cut(const r3d_lit_mesh_t* mesh, const uint32_t* lod_coarse, const planef_t* planes, planef_t depth,
                      uint16_t* out, int count, r3d_depth_fit_t* fit, int* tested) {
    int from = mesh->lod_coarse_first;
    for (int w = 0; w < R3D_LOD_WORDS(mesh->lod_group_count); w++) {
        for (uint32_t bits = lod_coarse[w]; bits != 0; bits &= bits - 1U) {
            const uint16_t g = (uint16_t)((w * R3D_LOD_WORD_BITS) + __builtin_ctz(bits));
            int c = group_run(mesh, from, g);
            for (; c < mesh->cluster_count && mesh->cluster_lod[c].own == g; c++) {
                const uint16_t parent = mesh->cluster_lod[c].parent;
                if (parent == R3D_LOD_NONE || !r3d_lod_coarse(lod_coarse, parent)) {
                    *tested += 1;
                    count = cull_cluster(mesh, c, ALL_PLANES, planes, depth, out, count, fit);
                }
            }
            from = c;
        }
    }
    return count;
}

int
r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* lod_coarse, uint16_t* out,
                  r3d_pipeline_work_t* work, r3d_depth_fit_t* fit) {
    planef_t* planes = work->cull.planes;
    r3d_lens_frustum(lens, planes);
    const planef_t depth = lens_depth_plane(lens);

    walk_entry_t* stack = work->cull.stack;
    int top = 0;
    /* With the levels laid out apart, the walk covers level 0 and the cut's
     * coarse clusters come from their groups. */
    const int start = mesh->lod_finest_root;
    /* Nothing coarse: level 0's tree, like a mesh without levels, holds only
     * what is drawn, so the walk tests no level and there is no cut. */
    const bool cut = lod_coarse != NULL && r3d_lod_any(lod_coarse, mesh->lod_group_count);
    const bool every = r3d_lod_finest_untested(mesh, cut);
    stack[top++] = (walk_entry_t){(uint16_t)start, ALL_PLANES};
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
        count = cull_leaf(mesh, lod_coarse, every, node, mask, planes, depth, out, count, fit);
    }
    if (start != 0 && cut) {
        int tested = 0;
        count = r3d_pipeline_cull_cut(mesh, lod_coarse, planes, depth, out, count, fit, &tested);
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

_Static_assert(R3D_SPAN_RANGE > FAST_PIXELS * R3D_SUBPIXEL, "FAST bit range test requires positive FAST_LO");

/* Positive floats order like their bits; negatives and NaNs fall outside the span. */
static inline bool
biased_inside_fast(uint32_t bits, uint32_t lo_bits, uint32_t hi_bits) {
    return (uint32_t)(bits - lo_bits - 1U) < hi_bits - lo_bits - 1U;
}

typedef struct {
    float x, y; /* subpixels plus R3D_SNAP_BIAS */
} biased_t;

/* The one place a position becomes a biased screen position, so both paths
 * snap alike. x and y are in subpixels; r is 1/z. */
static inline biased_t
biased_screen(const r3d_lens_t* lens, float x, float y, float r) {
    return (biased_t){lens->snap_cx + (x * r), lens->snap_cy + (y * r)};
}

static inline __attribute__((always_inline)) vec3f_t
transform_position(const mat4f_t* screen, const int16_t p[3]) {
    return mat4f_apply(screen, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
}

static inline __attribute__((always_inline)) void
transform_store(const r3d_lens_t* lens, r3d_pipeline_vertex_t* out, vec3f_t l, biased_t b, bool inside_x, bool inside_y,
                float iz, int* y0, int* y1, bool* unbounded, bool boxed, int* x0, int* x1, float* nearest) {
    if (l.z <= lens->project.near_z) {
        out->iz = 0.0F;
        *unbounded = true;
        return;
    }
    if (!(inside_x && inside_y)) {
        out->iz = -1.0F;
        *unbounded = true;
        return;
    }
    out->sx = (int16_t)r3d_span_unbias(b.x);
    out->sy = (int16_t)r3d_span_unbias(b.y);
    out->iz = iz;
    *y0 = mathi_min(out->sy, *y0);
    *y1 = mathi_max(out->sy, *y1);
    if (boxed) {
        *x0 = mathi_min(out->sx, *x0);
        *x1 = mathi_max(out->sx, *x1);
        *nearest = mathf_max(out->iz, *nearest);
    }
}

typedef struct {
    biased_t b;
    bool inside_x, inside_y;
    float iz;
} transform_projection_t;

static inline __attribute__((always_inline)) transform_projection_t
transform_project(const r3d_lens_t* lens, vec3f_t l, float r, uint32_t lo_bits, uint32_t hi_bits) {
    const biased_t b = biased_screen(lens, l.x, l.y, r);
    return (transform_projection_t){b, biased_inside_fast(mathf_bits(b.x), lo_bits, hi_bits),
                                    biased_inside_fast(mathf_bits(b.y), lo_bits, hi_bits), lens->depth_near * r};
}

static inline __attribute__((always_inline)) r3d_pipeline_rows_t
transform_cluster(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const mat4f_t* screen, const r3d_lit_cluster_t* c,
                  r3d_pipeline_vertex_t* cs, uint32_t lo_bits, uint32_t hi_bits, bool boxed) {
    const int end = c->vertex_first + c->vertex_count;
    int x0 = INT16_MAX;
    int x1 = INT16_MIN;
    int y0 = INT16_MAX;
    int y1 = INT16_MIN;
    float iz = 0.0F;
    bool unbounded = false;
    int v = c->vertex_first;
    for (; v + 1 < end; v += 2) {
        FRAME_LAP("r3d.transform.apply");
        const vec3f_t l0 = transform_position(screen, mesh->positions[v]);
        const vec3f_t l1 = transform_position(screen, mesh->positions[v + 1]);
        FRAME_LAP("r3d.transform.project");
        const float r0 = mathf_recip(l0.z);
        const float r1 = mathf_recip(l1.z);
        const transform_projection_t p0 = transform_project(lens, l0, r0, lo_bits, hi_bits);
        const transform_projection_t p1 = transform_project(lens, l1, r1, lo_bits, hi_bits);
        transform_store(lens, &cs[v], l0, p0.b, p0.inside_x, p0.inside_y, p0.iz, &y0, &y1, &unbounded, boxed, &x0, &x1,
                        &iz);
        transform_store(lens, &cs[v + 1], l1, p1.b, p1.inside_x, p1.inside_y, p1.iz, &y0, &y1, &unbounded, boxed, &x0,
                        &x1, &iz);
    }
    for (; v < end; v++) {
        FRAME_LAP("r3d.transform.apply");
        const vec3f_t l = transform_position(screen, mesh->positions[v]);
        FRAME_LAP("r3d.transform.project");
        const float r = mathf_recip(l.z);
        const transform_projection_t p = transform_project(lens, l, r, lo_bits, hi_bits);
        transform_store(lens, &cs[v], l, p.b, p.inside_x, p.inside_y, p.iz, &y0, &y1, &unbounded, boxed, &x0, &x1, &iz);
    }
    FRAME_LAP("r3d.transform.cluster");
    return (r3d_pipeline_rows_t){
        (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, boxed ? r3d_span_depth_limit(iz) : 0, unbounded};
}

static inline __attribute__((always_inline)) void
transform_clusters(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                   r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows, bool boxed) {
    const mat4f_t screen = mat4f_scale_rows(lens->m, (vec3f_t){R3D_SUBPIXEL, R3D_SUBPIXEL, 1});
    const uint32_t lo_bits = mathf_bits(FAST_LO);
    const uint32_t hi_bits = mathf_bits(FAST_HI);
    for (int i = 0; i < count; i++) {
        const r3d_pipeline_rows_t r =
            transform_cluster(mesh, lens, &screen, &mesh->clusters[clusters[i]], cs, lo_bits, hi_bits, boxed);
        if (rows != NULL) {
            rows[clusters[i]] = r;
        }
    }
}

/* The transform of a view that skips hidden clusters, boxing each one. */
static RENDER_SKIP_ATTR __attribute__((noinline)) void
transform_boxed(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows) {
    transform_clusters(mesh, lens, clusters, count, cs, rows, true);
}

void RENDER_IRAM_ATTR
r3d_pipeline_transform(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                       r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows, bool boxed) {
    if (boxed) {
        transform_boxed(mesh, lens, clusters, count, cs, rows);
    } else {
        transform_clusters(mesh, lens, clusters, count, cs, rows, false);
    }
}

int
r3d_pipeline_transform_split(const r3d_pipeline_part_t* parts, int part_count, int first_permille) {
    int total = 0;
    for (int p = 0; p < part_count; p++) {
        for (int i = 0; i < parts[p].count; i++) {
            total += parts[p].mesh->clusters[parts[p].clusters[i]].vertex_count;
        }
    }
    int prefix = 0;
    int position = 0;
    for (int p = 0; p < part_count; p++) {
        for (int i = 0; i < parts[p].count; i++) {
            prefix += parts[p].mesh->clusters[parts[p].clusters[i]].vertex_count;
            position++;
            if ((int64_t)prefix * JOB_SHARE_PERMILLE >= (int64_t)total * first_permille) {
                return position;
            }
        }
    }
    return position;
}

#define DRAW_SPLIT_BUCKETS 64

static void
draw_split_histogram(int* weight, const r3d_pipeline_part_t* part, int height) {
    const r3d_lit_mesh_t* mesh = part->mesh;
    const uint16_t* clusters = part->clusters;
    for (int i = 0; i < part->count; i++) {
        const r3d_pipeline_rows_t* r = &part->rows[clusters[i]];
        const int first = mathi_clamp(r->unbounded ? 0 : r->y0 / R3D_SUBPIXEL, 0, height - 1);
        const int last = mathi_clamp(r->unbounded ? height - 1 : r->y1 / R3D_SUBPIXEL, 0, height - 1);
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
r3d_pipeline_draw_split(const r3d_pipeline_part_t* parts, int part_count, int height, int top_permille) {
    assert(top_permille >= 0 && top_permille <= JOB_SHARE_PERMILLE);
    int weight[DRAW_SPLIT_BUCKETS + 1] = {0};
    for (int p = 0; p < part_count; p++) {
        draw_split_histogram(weight, &parts[p], height);
    }
    return draw_split_row(weight, height, top_permille);
}

static inline __attribute__((always_inline)) r3d_span_vertex_t
project(const r3d_lens_t* lens, const clip_vertex_t* v) {
    /* Scaling by 16 is exact: (16*x)*r == x*(16*r), and (d/16)*(16*r) == d*r,
     * so lens-space clip vertices and the screen rows snap and encode depth alike. */
    const float r = mathf_recip(v->z);
    const biased_t b = biased_screen(lens, v->x * R3D_SUBPIXEL, v->y * R3D_SUBPIXEL, r);
    return (r3d_span_vertex_t){r3d_span_unbias(b.x), r3d_span_unbias(b.y), lens->depth_near * r, v->r, v->g, v->b};
}

/* Fan triangle i's mask. A clipped corner lies in its source triangle's plane, so its u, v are the
 * source corners' weighted by its barycentric coordinates there; only a
 * masked fan pays for them, and the clip carries none. */
static r3d_span_mask_t*
fan_mask(r3d_pipeline_work_t* work, int i) {
    r3d_span_mask_t* mask = &work->clip.mask;
    const vec3f_t* s = work->clip.source;
    const vec3f_t e1 = vec3f_sub(s[1], s[0]);
    const vec3f_t e2 = vec3f_sub(s[2], s[0]);
    const vec3f_t n = vec3f_cross(e1, e2);
    const float inverse = mathf_recip(vec3f_dot(n, n));
    const int corner[] = {0, i, i + 1};
    for (int k = 0; k < R3D_SPAN_TRIANGLE_VERTICES; k++) {
        const clip_vertex_t* p = &work->clip.poly[corner[k]];
        const vec3f_t d = vec3f_sub((vec3f_t){p->x, p->y, p->z}, s[0]);
        const float w1 = vec3f_dot(vec3f_cross(d, e2), n) * inverse;
        const float w2 = vec3f_dot(vec3f_cross(e1, d), n) * inverse;
        const float w0 = 1.0F - w1 - w2;
        mask->u[k] = (w0 * work->clip.source_u[0]) + (w1 * work->clip.source_u[1]) + (w2 * work->clip.source_u[2]);
        mask->v[k] = (w0 * work->clip.source_v[0]) + (w1 * work->clip.source_v[1]) + (w2 * work->clip.source_v[2]);
    }
    return mask;
}

/* One Sutherland-Hodgman pass (Sutherland and Hodgman 1974): the polygon's part on the plane's inside. */
static inline __attribute__((always_inline)) int
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
 * inside what r3d_span takes. Inline in each rebuild: shared out of line,
 * it costs every opaque rebuild a call. */
static inline __attribute__((always_inline)) int
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

/* How one cluster's triangles draw. A bit a copy never sets, or always
 * sets, folds away in that copy; DRAW_BOUNDED is chosen per cluster. */
enum {
    DRAW_BOUNDED = 1U << 0,  /* every vertex passed the near and range tests: no iz tests */
    DRAW_WRITES = 1U << 1,   /* the target's writers follow each fill */
    DRAW_SKIPPING = 1U << 2, /* clusters behind drawn depth are skipped; fills cap depth */
};

typedef unsigned draw_mode_t;

/* Where a triangle's mask comes from: none (the opaque fills), its cluster's
 * atlas, or the target's screen tile. */
typedef enum { MASK_NONE, MASK_ATLAS, MASK_SCREEN } mask_source_t;

/* One triangle, then the target's writers over it with DRAW_WRITES; a
 * masked one's writers follow inside its fill. */
static inline __attribute__((always_inline)) void
fill_triangle(const r3d_span_target_t* target, const r3d_span_mask_t* mask, const r3d_span_vertex_t* a,
              const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, const uint16_t* face_color, draw_mode_t mode) {
    FRAME_LAP("r3d.draw.set_up");
    if (mask != NULL) {
        r3d_span_masked_triangle(target, mask, a, b, c, face_color);
        FRAME_LAP("r3d.draw.cluster");
        return;
    }
    if ((mode & DRAW_SKIPPING) != 0) {
        r3d_span_capped_triangle(target, a, b, c, face_color);
    } else if (face_color == NULL) {
        r3d_span_triangle(target, a, b, c);
    } else {
        r3d_span_triangle_solid(target, a, b, c, *face_color);
    }
    FRAME_LAP("r3d.draw.cluster");
    if ((mode & DRAW_WRITES) != 0) {
        r3d_span_triangle_write(target, a, b, c, face_color != NULL);
    }
}

static inline __attribute__((always_inline)) void
draw_clipped(const r3d_lens_t* lens, bool double_sided, const uint16_t* face_color, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work, mask_source_t source) {
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
        r3d_span_mask_t* mask = source == MASK_ATLAS ? fan_mask(work, i) : NULL;
        mask = source == MASK_SCREEN ? &work->clip.mask : mask;
        fill_triangle(target, mask, &s[0], &s[i], &s[i + 1], face_color,
                      (target->writer_count != 0 ? DRAW_WRITES : 0U) | (target->skip_hidden ? DRAW_SKIPPING : 0U));
    }
}

static inline bool
rows_miss_target(const r3d_pipeline_rows_t* r, const r3d_span_target_t* target) {
    return !r->unbounded
           && (r->y1 < target->rows.first_row * R3D_SUBPIXEL
               || r->y0 > gfx_target_end_row(&target->rows) * R3D_SUBPIXEL);
}

/* Drawn nearest first, a cluster behind what is already drawn is skipped
 * before any of its triangles is tested. Its box holds every centre its
 * triangles cover, as r3d_span_extent() finds them. Out of line, so its box
 * stays off the stack of the draw it guards while that draw fills, and in
 * flash: once a cluster, it costs too little to earn IRAM. */
__attribute__((noinline)) bool
r3d_pipeline_cluster_hidden(const r3d_pipeline_rows_t* r, const r3d_span_target_t* target) {
    if (r->unbounded) {
        return false;
    }
    /* + 1: a tag writer also writes where the fill's depth equals what is there. */
    return r3d_span_hidden(target, (int32_t)r->depth + 1,
                           r3d_span_window_box(target, r3d_span_centres(r->x0, r->y0, r->x1, r->y1)));
}

/* Rows outside the target's skip a cluster always; with DRAW_SKIPPING, so
 * does depth already drawn over its box. */
static inline __attribute__((always_inline)) bool
cluster_skipped(const r3d_pipeline_rows_t* r, const r3d_span_target_t* target, draw_mode_t mode) {
    return rows_miss_target(r, target) || ((mode & DRAW_SKIPPING) != 0 && r3d_pipeline_cluster_hidden(r, target));
}

/* A triangle with a corner behind the near plane or too far off screen to
 * snap is rebuilt from the mesh and clipped; a masked one also keeps its
 * corners and their u, v for its fan's masks. */
static inline __attribute__((always_inline)) void
rebuild(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* tri, const uint16_t* face_color,
        bool double_sided, const r3d_span_target_t* target, r3d_pipeline_work_t* work, mask_source_t source) {
    FRAME_LAP("r3d.draw.clip");
    clip_vertex_t* in = work->clip.poly;
    for (int k = 0; k < 3; k++) {
        const int16_t* p = mesh->positions[tri[k]];
        const uint8_t* rgb = face_color == NULL ? mesh->colors[tri[k]] : (const uint8_t[3]){0, 0, 0};
        const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
        in[k] = (clip_vertex_t){l.x, l.y, l.z, rgb[0], rgb[1], rgb[2]};
        if (source == MASK_ATLAS) {
            const uint16_t* uv = mesh->mask_uvs[tri[k] - mesh->mask_vertex_first];
            work->clip.source[k] = l;
            work->clip.source_u[k] = uv[0];
            work->clip.source_v[k] = uv[1];
        }
    }
    draw_clipped(lens, double_sided, face_color, target, work, source);
    FRAME_LAP("r3d.draw.cluster");
}

static __attribute__((noinline, cold)) void
draw_rebuilt(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* tri, const uint16_t* face_color,
             bool double_sided, const r3d_span_target_t* target, r3d_pipeline_work_t* work) {
    rebuild(mesh, lens, tri, face_color, double_sided, target, work, MASK_NONE);
}

/* rebuild() of a masked triangle, through work->clip.mask's atlas: apart, so
 * the opaque one tests no mask. */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
draw_masked_rebuilt(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* tri, const uint16_t* face_color,
                    bool double_sided, const r3d_span_target_t* target, r3d_pipeline_work_t* work) {
    rebuild(mesh, lens, tri, face_color, double_sided, target, work, MASK_ATLAS);
}

/* rebuild() through the target's screen tile. */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
draw_screen_rebuilt(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* tri, const uint16_t* face_color,
                    bool double_sided, const r3d_span_target_t* target, r3d_pipeline_work_t* work) {
    work->clip.mask.atlas = NULL;
    rebuild(mesh, lens, tri, face_color, double_sided, target, work, MASK_SCREEN);
}

static inline __attribute__((always_inline)) void
draw_in_front(const r3d_lit_mesh_t* mesh, const r3d_pipeline_vertex_t* const v[3], const uint16_t* tri,
              const uint16_t* face_color, bool double_sided, r3d_span_mask_t* mask, const r3d_span_target_t* target,
              draw_mode_t mode, r3d_span_vertex_t s[3]) {
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
    for (int k = 0; k < 3; k++) {
        const uint8_t* rgb = face_color == NULL ? mesh->colors[tri[k]] : (const uint8_t[3]){0, 0, 0};
        s[k] = (r3d_span_vertex_t){v[k]->sx, v[k]->sy, v[k]->iz, rgb[0], rgb[1], rgb[2]};
        if (mask != NULL && mask->atlas != NULL) {
            const uint16_t* uv = mesh->mask_uvs[tri[k] - mesh->mask_vertex_first];
            mask->u[k] = uv[0];
            mask->v[k] = uv[1];
        }
    }
    fill_triangle(target, mask, &s[0], &s[1], &s[2], face_color, mode);
}

/* Triangle t's face colour, or NULL when the mesh shades by vertex. */
static inline __attribute__((always_inline)) const uint16_t*
face_color_of(const r3d_lit_mesh_t* mesh, int t) {
    return mesh->face_colors == NULL ? NULL : &mesh->face_colors[t];
}

/* Masked triangle t in front: out of line, its span vertices in the clip's
 * own (here idle) buffer, and its arguments few enough to pass in registers,
 * so none of them sits on a stack stack_chain_gate bounds. A masked fill
 * takes its writers and its cap from the target, so it needs no mode. */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
draw_masked_in_front(const r3d_lit_mesh_t* mesh, const r3d_lit_cluster_t* c, int t,
                     const r3d_pipeline_vertex_t* const v[3], const r3d_span_target_t* target,
                     r3d_pipeline_work_t* work) {
    const uint16_t* face_color = face_color_of(mesh, t);
    work->clip.mask.atlas = c->masked ? &mesh->mask : NULL;
    draw_in_front(mesh, v, mesh->triangles[t], face_color, c->double_sided, &work->clip.mask, target, 0U,
                  work->clip.projected);
}

/* Triangle t, all in front: masked out of line, or with writers (a cold
 * draw) its vertices in the clip's idle buffer, off a stack stack_chain_gate
 * bounds. */
static inline __attribute__((always_inline)) void
draw_one_in_front(const r3d_lit_mesh_t* mesh, const r3d_lit_cluster_t* c, int t,
                  const r3d_pipeline_vertex_t* const v[3], mask_source_t source, const r3d_span_target_t* target,
                  draw_mode_t mode, r3d_pipeline_work_t* work) {
    if (source != MASK_NONE) {
        draw_masked_in_front(mesh, c, t, v, target, work);
        return;
    }
    const uint16_t* face_color = face_color_of(mesh, t);
    r3d_span_vertex_t s[3];
    draw_in_front(mesh, v, mesh->triangles[t], face_color, c->double_sided, NULL, target, mode,
                  (mode & DRAW_WRITES) != 0 ? work->clip.projected : s);
}

/* Triangles first to end - 1 of one cluster, through `mask` when it is masked. */
static inline __attribute__((always_inline)) void
draw_triangles_of(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_cluster_t* c, int first, int end,
                  const r3d_pipeline_vertex_t* cs, mask_source_t source, const r3d_span_target_t* target,
                  draw_mode_t mode, r3d_pipeline_work_t* work) {
    for (int t = first; t < end; t++) {
        const uint16_t* tri = mesh->triangles[t];
        const uint16_t* face_color = face_color_of(mesh, t);
        const r3d_pipeline_vertex_t* const v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
        /* Cluster-owned vertices all passed near and range tests with DRAW_BOUNDED. */
        if ((mode & DRAW_BOUNDED) != 0 || (v[0]->iz > 0.0F && v[1]->iz > 0.0F && v[2]->iz > 0.0F)) {
            draw_one_in_front(mesh, c, t, v, source, target, mode, work);
        } else if (v[0]->iz != 0.0F || v[1]->iz != 0.0F || v[2]->iz != 0.0F) {
            if (source == MASK_ATLAS) {
                work->clip.mask.atlas = &mesh->mask;
                draw_masked_rebuilt(mesh, lens, tri, face_color, c->double_sided, target, work);
            } else if (source == MASK_SCREEN) {
                draw_screen_rebuilt(mesh, lens, tri, face_color, c->double_sided, target, work);
            } else {
                draw_rebuilt(mesh, lens, tri, face_color, c->double_sided, target, work);
            }
        }
    }
}

/* Out of line and at the flash tail: the masked fill stays out of the hot
 * draw's IRAM and away from its flash neighbours. */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
draw_masked(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_cluster_t* c, int first, int end,
            const r3d_pipeline_vertex_t* cs, const r3d_span_target_t* target, draw_mode_t mode,
            r3d_pipeline_work_t* work) {
    draw_triangles_of(mesh, lens, c, first, end, cs, MASK_ATLAS, target, mode, work);
}

static inline __attribute__((always_inline)) void
draw_triangles(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const r3d_lit_cluster_t* c, int first, int end,
               const r3d_pipeline_vertex_t* cs, const r3d_span_target_t* target, draw_mode_t mode,
               r3d_pipeline_work_t* work) {
    if (c->masked) {
        draw_masked(mesh, lens, c, first, end, cs, target, mode, work);
    } else {
        draw_triangles_of(mesh, lens, c, first, end, cs, MASK_NONE, target, mode, work);
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
static inline __attribute__((always_inline)) void
draw_writing_of(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
                draw_mode_t mode, r3d_pipeline_work_t* work) {
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
        if (rows != NULL && cluster_skipped(&rows[clusters[i]], target, mode)) {
            continue;
        }
        /* Whole clusters, or one triangle at a time when a writer steps per
         * triangle, so it is retagged here and the cluster draw stays the
         * hot one's. */
        const int end = c->triangle_first + c->triangle_count;
        const int step = per_triangle ? 1 : c->triangle_count;
        const mask_source_t source = c->masked ? MASK_ATLAS : target->screen_mask != 0 ? MASK_SCREEN : MASK_NONE;
        for (int t = c->triangle_first; t < end; t += step) {
            retag(writers, target->writers, target->writer_count, clusters[i], (uint32_t)t);
            /* No DRAW_BOUNDED: one more live value puts this frame over stack_chain_gate. The mask
             * is chosen here, not by draw_masked(): its frame would put a masked chain over too. */
            draw_triangles_of(mesh, lens, c, t, t + step, cs, source, &writing, mode, work);
        }
    }
}

static __attribute__((noinline, cold)) void
draw_writing(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
             const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    draw_writing_of(mesh, lens, clusters, count, cs, rows, target,
                    DRAW_WRITES | (target->skip_hidden ? DRAW_SKIPPING : 0U), work);
}

/* Keep once-per-draw plane setup in flash to preserve the hot-path IRAM budget. */
static __attribute__((noinline)) void
build_guard_planes(const r3d_lens_t* lens, r3d_pipeline_work_t* work) {
    const float g = (float)GUARD_PIXELS;
    planef_frustum(&LENS_SPACE, &lens->project, rectf_from_edges(-g, -g, g, g), work->clip.planes);
}

static inline __attribute__((always_inline)) void
draw_reading_of(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
                draw_mode_t mode, r3d_pipeline_work_t* work) {
    for (int i = 0; i < count; i++) {
        if (rows != NULL && cluster_skipped(&rows[clusters[i]], target, mode)) {
            continue;
        }
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        const draw_mode_t bounded = rows != NULL && !rows[clusters[i]].unbounded ? DRAW_BOUNDED : 0U;
        draw_triangles(mesh, lens, c, c->triangle_first, c->triangle_first + c->triangle_count, cs, target,
                       mode | bounded, work);
    }
}

static RENDER_IRAM_ATTR __attribute__((noinline)) void
draw_reading(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
             const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
             r3d_pipeline_work_t* work) {
    draw_reading_of(mesh, lens, clusters, count, cs, rows, target, 0U, work);
}

/* The read-only draw of a view that skips hidden clusters. */
static RENDER_SKIP_ATTR __attribute__((noinline)) void
draw_reading_skipping(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                      const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
                      r3d_pipeline_work_t* work) {
    draw_reading_of(mesh, lens, clusters, count, cs, rows, target, DRAW_SKIPPING, work);
}

/* A target with a screen tile: every triangle through the masked fill, a
 * masked cluster's through its own atlas. At the flash tail, so the opaque
 * draws keep their code and placement. */
static RENDER_TAIL_ATTR __attribute__((noinline)) void
draw_screen(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
            const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
            r3d_pipeline_work_t* work) {
    const draw_mode_t mode = target->skip_hidden ? DRAW_SKIPPING : 0U;
    for (int i = 0; i < count; i++) {
        if (rows != NULL && cluster_skipped(&rows[clusters[i]], target, mode)) {
            continue;
        }
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        draw_triangles_of(mesh, lens, c, c->triangle_first, c->triangle_first + c->triangle_count, cs,
                          c->masked ? MASK_ATLAS : MASK_SCREEN, target, mode, work);
    }
}

/* Chooses once per draw whether the target's writers follow each fill, and
 * whether a screen tile masks it, and whether clusters behind drawn depth are skipped. */
void
r3d_pipeline_draw(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                  const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target,
                  r3d_pipeline_work_t* work) {
    build_guard_planes(lens, work);
    if (target->writer_count != 0) {
        draw_writing(mesh, lens, clusters, count, cs, rows, target, work);
    } else if (target->screen_mask != 0) {
        draw_screen(mesh, lens, clusters, count, cs, rows, target, work);
    } else if (target->skip_hidden) {
        draw_reading_skipping(mesh, lens, clusters, count, cs, rows, target, work);
    } else {
        draw_reading(mesh, lens, clusters, count, cs, rows, target, work);
    }
}

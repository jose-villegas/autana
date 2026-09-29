#include "render/r3d_lit_pipeline.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "render/r3d_ray.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

static void
set_row(float row[4], r3d_vec3f_t axis, r3d_vec3f_t eye, float scale, float ticks_to_units) {
    row[0] = axis.x * scale * ticks_to_units;
    row[1] = axis.y * scale * ticks_to_units;
    row[2] = axis.z * scale * ticks_to_units;
    row[3] = -r3d_vec3f_dot(axis, eye) * scale;
}

static r3d_vec3f_t
upright_step(r3d_vec3f_t right, r3d_vec3f_t down, int step_right, int step_down) {
    return r3d_vec3f_add(r3d_vec3f_scale(right, (float)step_right), r3d_vec3f_scale(down, (float)step_down));
}

void
r3d_lit_view_look(r3d_lit_view_t* view, r3d_vec3f_t eye, r3d_vec3f_t forward, float half_fov_short_tan, float near_z,
                  int position_scale, r3d_viewport_t viewport) {
    const r3d_vec3f_t f = r3d_vec3f_normalize(forward);
    const r3d_vec3f_t right = r3d_vec3f_normalize(r3d_vec3f_cross(f, (r3d_vec3f_t){0.0F, 1.0F, 0.0F}));
    const r3d_vec3f_t down = r3d_vec3f_cross(f, right);

    const int shorter = viewport.width < viewport.height ? viewport.width : viewport.height;
    const float k = (float)shorter / (2.0F * half_fov_short_tan);
    const float ticks_to_units = 1.0F / (float)position_scale;

    const r3d_quarter_axes_t a = r3d_quarter_axes(viewport.quarter);
    set_row(view->m[0], upright_step(right, down, a.x_right, a.x_down), eye, k, ticks_to_units);
    set_row(view->m[1], upright_step(right, down, a.y_right, a.y_down), eye, k, ticks_to_units);
    set_row(view->m[2], f, eye, 1.0F, ticks_to_units);
    view->center_x = (float)viewport.width * 0.5F;
    view->center_y = (float)viewport.height * 0.5F;
    view->near_z = near_z;
    view->width = viewport.width;
    view->height = viewport.height;
}

static inline r3d_vec3f_t
to_lens(const r3d_lit_view_t* view, float x, float y, float z) {
    const float(*m)[4] = view->m;
    return (r3d_vec3f_t){
        (m[0][0] * x) + (m[0][1] * y) + (m[0][2] * z) + m[0][3],
        (m[1][0] * x) + (m[1][1] * y) + (m[1][2] * z) + m[1][3],
        (m[2][0] * x) + (m[2][1] * y) + (m[2][2] * z) + m[2][3],
    };
}

#define PLANE_COUNT    5
#define ALL_PLANES     ((1u << PLANE_COUNT) - 1u)
#define WALK_STACK_MAX 256

typedef struct {
    float w[4]; /* inside where w . (x, y, z, 1) >= 0, in position ticks */
} plane_t;

/* The view's five planes (near, left, right, top, bottom) written in lens
 * space, then carried back through the view matrix to position ticks, so a
 * box is tested without transforming its corners. */
static void
frustum_planes(const r3d_lit_view_t* view, plane_t planes[PLANE_COUNT]) {
    const float right = (float)view->width - view->center_x;
    const float bottom = (float)view->height - view->center_y;
    const float lens[PLANE_COUNT][4] = {
        {0.0F, 0.0F, 1.0F, -view->near_z},  {1.0F, 0.0F, view->center_x, 0.0F}, {-1.0F, 0.0F, right, 0.0F},
        {0.0F, 1.0F, view->center_y, 0.0F}, {0.0F, -1.0F, bottom, 0.0F},
    };
    for (int p = 0; p < PLANE_COUNT; p++) {
        for (int j = 0; j < 4; j++) {
            planes[p].w[j] = lens[p][0] * view->m[0][j] + lens[p][1] * view->m[1][j] + lens[p][2] * view->m[2][j];
        }
        planes[p].w[3] += lens[p][3];
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
box_depth(const r3d_lit_view_t* view, const int16_t lo[3], const int16_t hi[3]) {
    const float* f = view->m[2];
    return (f[0] * 0.5F * ((float)lo[0] + (float)hi[0])) + (f[1] * 0.5F * ((float)lo[1] + (float)hi[1]))
           + (f[2] * 0.5F * ((float)lo[2] + (float)hi[2])) + f[3];
}

typedef struct {
    uint16_t node;
    uint8_t mask;
} walk_entry_t;

/* Children go on the stack farthest first, so the nearest pops first and
 * the walk visits leaves roughly front to back. */
static int
push_children(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const r3d_lit_node_t* node, unsigned mask,
              walk_entry_t* stack, int top) {
    uint16_t order[8];
    float depth[8];
    int n = 0;
    for (int i = 0; i < node->count; i++) {
        const uint16_t child = (uint16_t)(node->first + i);
        const float d = box_depth(view, mesh->nodes[child].lo, mesh->nodes[child].hi);
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
r3d_lit_cull_clusters(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, uint16_t* out) {
    plane_t planes[PLANE_COUNT];
    frustum_planes(view, planes);

    walk_entry_t stack[WALK_STACK_MAX];
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
            top = push_children(mesh, view, node, mask, stack, top);
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

/* Pixels from the origin a snapped position can reach in int16. */
#define SNAP_LIMIT 2047.0F

static inline r3d_lit_rows_t
transform_cluster(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const r3d_lit_cluster_t* c,
                  r3d_lit_vertex_t* cs) {
    const int end = c->vertex_first + c->vertex_count;
    r3d_lit_rows_t rows = {INFINITY, -INFINITY, false};
    for (int v = c->vertex_first; v < end; v++) {
        const int16_t* p = mesh->positions[v];
        const r3d_vec3f_t l = to_lens(view, (float)p[0], (float)p[1], (float)p[2]);
        r3d_lit_vertex_t* out = &cs[v];
        if (l.z <= view->near_z) {
            out->iz = 0.0F;
            rows.crosses_near = true;
            continue;
        }
        const float inv = 1.0F / l.z;
        const float sx = view->center_x + l.x * inv;
        const float sy = view->center_y + l.y * inv;
        if (!(fabsf(sx) < SNAP_LIMIT && fabsf(sy) < SNAP_LIMIT)) {
            out->iz = -1.0F;
            rows.crosses_near = true;
            continue;
        }
        out->sx = (int16_t)r3d_span_snap_near(sx);
        out->sy = (int16_t)r3d_span_snap_near(sy);
        out->iz = view->near_z * inv;
        rows.y0 = sy < rows.y0 ? sy : rows.y0;
        rows.y1 = sy > rows.y1 ? sy : rows.y1;
    }
    return rows;
}

void
r3d_lit_transform(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int count,
                  r3d_lit_vertex_t* cs, r3d_lit_rows_t* rows) {
    for (int i = 0; i < count; i++) {
        const r3d_lit_rows_t r = transform_cluster(mesh, view, &mesh->clusters[clusters[i]], cs);
        if (rows != NULL) {
            rows[clusters[i]] = r;
        }
    }
}

typedef struct {
    float x, y, z, r, g, b;
} clip_vertex_t;

static r3d_span_vertex_t
project(const r3d_lit_view_t* view, const clip_vertex_t* v) {
    const float inv = 1.0F / v->z;
    return (r3d_span_vertex_t){r3d_span_snap(view->center_x + (v->x * inv)),
                               r3d_span_snap(view->center_y + (v->y * inv)),
                               view->near_z * inv,
                               v->r,
                               v->g,
                               v->b};
}

static inline int64_t
signed_area2(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy) {
    return ((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay));
}

/* Front faces wind negative on screen: counter-clockwise in a y-up world
 * turns clockwise once screen y points down. */
static inline bool
facing_away(int64_t area2, bool double_sided) {
    return !double_sided && area2 >= 0;
}

static void
draw_near_clipped(const r3d_lit_view_t* view, const clip_vertex_t in[3], bool double_sided,
                  const r3d_span_target_t* target) {
    clip_vertex_t poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const clip_vertex_t* a = &in[i];
        const clip_vertex_t* b = &in[(i + 1) % 3];
        const bool a_in = a->z > view->near_z;
        const bool b_in = b->z > view->near_z;
        if (a_in) {
            poly[n++] = *a;
        }
        if (a_in != b_in) {
            const float t = (view->near_z - a->z) / (b->z - a->z);
            poly[n++] =
                (clip_vertex_t){a->x + ((b->x - a->x) * t), a->y + ((b->y - a->y) * t), view->near_z,
                                a->r + ((b->r - a->r) * t), a->g + ((b->g - a->g) * t), a->b + ((b->b - a->b) * t)};
        }
    }
    if (n < 3) {
        return;
    }
    r3d_span_vertex_t s[4];
    for (int i = 0; i < n; i++) {
        s[i] = project(view, &poly[i]);
    }
    if (facing_away(signed_area2(s[0].x, s[0].y, s[1].x, s[1].y, s[2].x, s[2].y), double_sided)) {
        return;
    }
    r3d_span_triangle(target, &s[0], &s[1], &s[2]);
    if (n == 4) {
        r3d_span_triangle(target, &s[0], &s[2], &s[3]);
    }
}

static inline int
min3(int a, int b, int c) {
    return a < b ? (a < c ? a : c) : (b < c ? b : c);
}

static inline int
max3(int a, int b, int c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* True when the bounding box holds no pixel centre inside the target, so
 * the rasterizer would fill nothing: most of these are triangles smaller
 * than a pixel falling between centres. */
static inline bool
misses_every_centre(const r3d_lit_vertex_t* a, const r3d_lit_vertex_t* b, const r3d_lit_vertex_t* c,
                    const r3d_span_target_t* target) {
    const int x_first = r3d_span_first_centre(min3(a->sx, b->sx, c->sx));
    const int x_end = r3d_span_first_centre(max3(a->sx, b->sx, c->sx));
    const int y_first = r3d_span_first_centre(min3(a->sy, b->sy, c->sy));
    const int y_end = r3d_span_first_centre(max3(a->sy, b->sy, c->sy));
    return x_first >= x_end || y_first >= y_end || x_end <= 0 || x_first >= target->width || y_end <= target->row0
           || y_first >= target->row1;
}

static inline bool
rows_miss_target(const r3d_lit_rows_t* r, const r3d_span_target_t* target) {
    return !r->crosses_near && (r->y1 < (float)target->row0 || r->y0 > (float)target->row1);
}

static void
draw_crossing_near(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* tri, bool double_sided,
                   const r3d_span_target_t* target) {
    clip_vertex_t in[3];
    for (int k = 0; k < 3; k++) {
        const int16_t* p = mesh->positions[tri[k]];
        const uint8_t* rgb = mesh->colors[tri[k]];
        const r3d_vec3f_t l = to_lens(view, (float)p[0], (float)p[1], (float)p[2]);
        in[k] = (clip_vertex_t){l.x, l.y, l.z, rgb[0], rgb[1], rgb[2]};
    }
    draw_near_clipped(view, in, double_sided, target);
}

static inline void
draw_in_front(const r3d_lit_mesh_t* mesh, const r3d_lit_vertex_t* const v[3], const uint16_t* tri, bool double_sided,
              const r3d_span_target_t* target) {
    if (misses_every_centre(v[0], v[1], v[2], target)) {
        return;
    }
    const int64_t area2 = signed_area2(v[0]->sx, v[0]->sy, v[1]->sx, v[1]->sy, v[2]->sx, v[2]->sy);
    if (facing_away(area2, double_sided)) {
        return;
    }
    r3d_span_vertex_t s[3];
    for (int k = 0; k < 3; k++) {
        const uint8_t* rgb = mesh->colors[tri[k]];
        s[k] = (r3d_span_vertex_t){v[k]->sx, v[k]->sy, v[k]->iz, rgb[0], rgb[1], rgb[2]};
    }
    r3d_span_triangle(target, &s[0], &s[1], &s[2]);
}

static void
draw_cluster(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const r3d_lit_cluster_t* c,
             const r3d_lit_vertex_t* cs, const r3d_span_target_t* target) {
    const int end = c->triangle_first + c->triangle_count;
    for (int t = c->triangle_first; t < end; t++) {
        const uint16_t* tri = mesh->triangles[t];
        const r3d_lit_vertex_t* const v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
        if (v[0]->iz > 0.0F && v[1]->iz > 0.0F && v[2]->iz > 0.0F) {
            draw_in_front(mesh, v, tri, c->double_sided, target);
        } else if (v[0]->iz != 0.0F || v[1]->iz != 0.0F || v[2]->iz != 0.0F) {
            draw_crossing_near(mesh, view, tri, c->double_sided, target);
        }
    }
}

void
r3d_lit_draw(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int count,
             const r3d_lit_vertex_t* cs, const r3d_lit_rows_t* rows, const r3d_span_target_t* target) {
    for (int i = 0; i < count; i++) {
        if (rows != NULL && rows_miss_target(&rows[clusters[i]], target)) {
            continue;
        }
        draw_cluster(mesh, view, &mesh->clusters[clusters[i]], cs, target);
    }
}

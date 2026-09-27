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
    const r3d_vec3f_t right = r3d_vec3f_normalize(r3d_vec3f_cross(f, (r3d_vec3f_t){0.0f, 1.0f, 0.0f}));
    const r3d_vec3f_t down = r3d_vec3f_cross(f, right);

    const int shorter = viewport.width < viewport.height ? viewport.width : viewport.height;
    const float k = (float)shorter / (2.0f * half_fov_short_tan);
    const float ticks_to_units = 1.0f / (float)position_scale;

    const r3d_quarter_axes_t a = r3d_quarter_axes(viewport.quarter);
    set_row(view->m[0], upright_step(right, down, a.x_right, a.x_down), eye, k, ticks_to_units);
    set_row(view->m[1], upright_step(right, down, a.y_right, a.y_down), eye, k, ticks_to_units);
    set_row(view->m[2], f, eye, 1.0f, ticks_to_units);
    view->center_x = (float)viewport.width * 0.5f;
    view->center_y = (float)viewport.height * 0.5f;
    view->near_z = near_z;
    view->width = viewport.width;
    view->height = viewport.height;
}

static inline r3d_vec3f_t
to_lens(const r3d_lit_view_t* view, float x, float y, float z) {
    const float(*m)[4] = view->m;
    return (r3d_vec3f_t){
        m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
        m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
        m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3],
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
        {0.0f, 0.0f, 1.0f, -view->near_z},  {1.0f, 0.0f, view->center_x, 0.0f}, {-1.0f, 0.0f, right, 0.0f},
        {0.0f, 1.0f, view->center_y, 0.0f}, {0.0f, -1.0f, bottom, 0.0f},
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
    const float c[3] = {0.5f * ((float)lo[0] + (float)hi[0]), 0.5f * ((float)lo[1] + (float)hi[1]),
                        0.5f * ((float)lo[2] + (float)hi[2])};
    const float e[3] = {0.5f * ((float)hi[0] - (float)lo[0]), 0.5f * ((float)hi[1] - (float)lo[1]),
                        0.5f * ((float)hi[2] - (float)lo[2])};
    for (int p = 0; p < PLANE_COUNT; p++) {
        if (!(*mask & (1u << p))) {
            continue;
        }
        const float* w = planes[p].w;
        const float d = w[0] * c[0] + w[1] * c[1] + w[2] * c[2] + w[3];
        const float r = fabsf(w[0]) * e[0] + fabsf(w[1]) * e[1] + fabsf(w[2]) * e[2];
        if (d + r < 0.0f) {
            return BOX_OUTSIDE;
        }
        if (d - r >= 0.0f) {
            *mask &= ~(1u << p);
        }
    }
    return BOX_CROSSING;
}

static float
box_depth(const r3d_lit_view_t* view, const int16_t lo[3], const int16_t hi[3]) {
    const float* f = view->m[2];
    return f[0] * 0.5f * ((float)lo[0] + (float)hi[0]) + f[1] * 0.5f * ((float)lo[1] + (float)hi[1])
           + f[2] * 0.5f * ((float)lo[2] + (float)hi[2]) + f[3];
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

void
r3d_lit_transform(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int count,
                  r3d_lit_vertex_t* cs, r3d_lit_rows_t* rows) {
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        const int end = c->vertex_first + c->vertex_count;
        float y0 = INFINITY, y1 = -INFINITY;
        bool crosses_near = false;
        for (int v = c->vertex_first; v < end; v++) {
            const int16_t* p = mesh->positions[v];
            const r3d_vec3f_t l = to_lens(view, (float)p[0], (float)p[1], (float)p[2]);
            r3d_lit_vertex_t* out = &cs[v];
            out->z = l.z;
            if (l.z > view->near_z) {
                const float inv = 1.0f / l.z;
                out->sx = view->center_x + l.x * inv;
                out->sy = view->center_y + l.y * inv;
                out->iz = view->near_z * inv;
                y0 = out->sy < y0 ? out->sy : y0;
                y1 = out->sy > y1 ? out->sy : y1;
            } else {
                crosses_near = true;
            }
        }
        if (rows != NULL) {
            rows[clusters[i]] = (r3d_lit_rows_t){y0, y1, crosses_near};
        }
    }
}

typedef struct {
    float x, y, z, r, g, b;
} clip_vertex_t;

static r3d_span_vertex_t
project(const r3d_lit_view_t* view, const clip_vertex_t* v) {
    const float inv = 1.0f / v->z;
    return (r3d_span_vertex_t){
        view->center_x + v->x * inv, view->center_y + v->y * inv, view->near_z * inv, v->r, v->g, v->b};
}

static inline float
signed_area2(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c) {
    return (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
}

/* Front faces wind negative on screen: counter-clockwise in a y-up world
 * turns clockwise once screen y points down. */
static inline bool
facing_away(float area2, bool double_sided) {
    return !double_sided && area2 >= 0.0f;
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
            poly[n++] = (clip_vertex_t){a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t, view->near_z,
                                        a->r + (b->r - a->r) * t, a->g + (b->g - a->g) * t, a->b + (b->b - a->b) * t};
        }
    }
    if (n < 3) {
        return;
    }
    r3d_span_vertex_t s[4];
    for (int i = 0; i < n; i++) {
        s[i] = project(view, &poly[i]);
    }
    if (facing_away(signed_area2(&s[0], &s[1], &s[2]), double_sided)) {
        return;
    }
    r3d_span_triangle(target, &s[0], &s[1], &s[2]);
    if (n == 4) {
        r3d_span_triangle(target, &s[0], &s[2], &s[3]);
    }
}

static inline bool
outside_target(const r3d_lit_vertex_t* a, const r3d_lit_vertex_t* b, const r3d_lit_vertex_t* c,
               const r3d_span_target_t* target) {
    const float w = (float)target->width;
    const float top = (float)target->row0, bottom = (float)target->row1;
    return (a->sx < 0.0f && b->sx < 0.0f && c->sx < 0.0f) || (a->sx > w && b->sx > w && c->sx > w)
           || (a->sy < top && b->sy < top && c->sy < top) || (a->sy > bottom && b->sy > bottom && c->sy > bottom);
}

void
r3d_lit_draw(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int count,
             const r3d_lit_vertex_t* cs, const r3d_lit_rows_t* rows, const r3d_span_target_t* target) {
    for (int i = 0; i < count; i++) {
        if (rows != NULL) {
            const r3d_lit_rows_t* r = &rows[clusters[i]];
            if (!r->crosses_near && (r->y1 < (float)target->row0 || r->y0 > (float)target->row1)) {
                continue;
            }
        }
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        const int end = c->triangle_first + c->triangle_count;
        for (int t = c->triangle_first; t < end; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const r3d_lit_vertex_t* v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
            const int in_front = (v[0]->z > view->near_z) + (v[1]->z > view->near_z) + (v[2]->z > view->near_z);
            if (in_front == 0) {
                continue;
            }
            const uint8_t* rgb[3] = {mesh->colors[tri[0]], mesh->colors[tri[1]], mesh->colors[tri[2]]};
            if (in_front < 3) {
                clip_vertex_t in[3];
                for (int k = 0; k < 3; k++) {
                    const int16_t* p = mesh->positions[tri[k]];
                    const r3d_vec3f_t l = to_lens(view, (float)p[0], (float)p[1], (float)p[2]);
                    in[k] = (clip_vertex_t){l.x, l.y, l.z, rgb[k][0], rgb[k][1], rgb[k][2]};
                }
                draw_near_clipped(view, in, c->double_sided, target);
                continue;
            }
            if (outside_target(v[0], v[1], v[2], target)) {
                continue;
            }
            const float area2 =
                (v[1]->sx - v[0]->sx) * (v[2]->sy - v[0]->sy) - (v[2]->sx - v[0]->sx) * (v[1]->sy - v[0]->sy);
            if (facing_away(area2, c->double_sided)) {
                continue;
            }

            r3d_span_vertex_t s[3];
            for (int k = 0; k < 3; k++) {
                s[k] = (r3d_span_vertex_t){v[k]->sx, v[k]->sy, v[k]->iz, rgb[k][0], rgb[k][1], rgb[k][2]};
            }
            r3d_span_triangle(target, &s[0], &s[1], &s[2]);
        }
    }
}

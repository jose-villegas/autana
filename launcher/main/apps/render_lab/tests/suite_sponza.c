/*
 * Portable suite: the baked Sponza mesh (sponza_mesh_generated.h) and the
 * camera loop through it (sponza_flythrough.h). The mesh is checked for the
 * structure lit_pipeline.h relies on, never against the generator; the path
 * is checked against the shipped mesh itself.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_lite_mesh_generated.h"
#include "apps/render_lab/sponza_mesh_generated.h"

_Static_assert(SPONZA_VERTEX_COUNT <= 65535 && SPONZA_LITE_VERTEX_COUNT <= 65535,
               "triangles index vertices with uint16_t");
_Static_assert(SPONZA_LITE_CLUSTER_COUNT <= SPONZA_CLUSTER_COUNT,
               "the working arrays below are sized for the larger bake");

static void
check_clusters_tile_both_arrays_in_order(const lit_mesh_t* mesh) {
    int next_vertex = 0, next_triangle = 0;
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        TEST_ASSERT_EQUAL_INT(next_vertex, c->vertex_first);
        TEST_ASSERT_EQUAL_INT(next_triangle, c->triangle_first);
        next_vertex += c->vertex_count;
        next_triangle += c->triangle_count;
    }
    TEST_ASSERT_EQUAL_INT(mesh->vertex_count, next_vertex);
    TEST_ASSERT_EQUAL_INT(mesh->triangle_count, next_triangle);
}

static void
check_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster(const lit_mesh_t* mesh) {
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            for (int k = 0; k < 3; k++) {
                TEST_ASSERT_TRUE(tri[k] >= c->vertex_first && tri[k] < c->vertex_first + c->vertex_count);
            }
            TEST_ASSERT_TRUE(tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]);
        }
    }
}

static void
check_cluster_bounds_hold_their_vertices(const lit_mesh_t* mesh) {
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        for (int v = c->vertex_first; v < c->vertex_first + c->vertex_count; v++) {
            for (int k = 0; k < 3; k++) {
                TEST_ASSERT_TRUE(mesh->positions[v][k] >= c->lo[k] && mesh->positions[v][k] <= c->hi[k]);
            }
        }
    }
}

static void
visit(const lit_mesh_t* mesh, int node, uint8_t* reached) {
    const lit_node_t* n = &mesh->nodes[node];
    if (n->leaf) {
        for (int c = n->first; c < n->first + n->count; c++) {
            reached[c]++;
            for (int k = 0; k < 3; k++) {
                TEST_ASSERT_TRUE(mesh->clusters[c].lo[k] >= n->lo[k] && mesh->clusters[c].hi[k] <= n->hi[k]);
            }
        }
        return;
    }
    for (int child = n->first; child < n->first + n->count; child++) {
        TEST_ASSERT_TRUE_MESSAGE(child > node, "a child must follow its parent");
        for (int k = 0; k < 3; k++) {
            TEST_ASSERT_TRUE(mesh->nodes[child].lo[k] >= n->lo[k] && mesh->nodes[child].hi[k] <= n->hi[k]);
        }
        visit(mesh, child, reached);
    }
}

static void
check_the_tree_holds_every_cluster_once_inside_its_ancestors_bounds(const lit_mesh_t* mesh) {
    static uint8_t reached[SPONZA_CLUSTER_COUNT];
    memset(reached, 0, sizeof reached);
    visit(mesh, 0, reached);
    for (int c = 0; c < mesh->cluster_count; c++) {
        TEST_ASSERT_EQUAL_UINT8(1, reached[c]);
    }
}

/* The flat reference: every cluster's eight corners against each plane. */
static bool
cluster_in_view(const lit_cluster_t* c, const lit_view_t* view) {
    int beyond[5] = {0};
    for (int i = 0; i < 8; i++) {
        const float x = i & 1 ? c->hi[0] : c->lo[0], y = i & 2 ? c->hi[1] : c->lo[1], z = i & 4 ? c->hi[2] : c->lo[2];
        const float lx = view->m[0][0] * x + view->m[0][1] * y + view->m[0][2] * z + view->m[0][3];
        const float ly = view->m[1][0] * x + view->m[1][1] * y + view->m[1][2] * z + view->m[1][3];
        const float lz = view->m[2][0] * x + view->m[2][1] * y + view->m[2][2] * z + view->m[2][3];
        beyond[0] += lz < view->near_z;
        beyond[1] += lx < -view->center_x * lz;
        beyond[2] += lx > ((float)view->width - view->center_x) * lz;
        beyond[3] += ly < -view->center_y * lz;
        beyond[4] += ly > ((float)view->height - view->center_y) * lz;
    }
    for (int p = 0; p < 5; p++) {
        if (beyond[p] == 8) {
            return false;
        }
    }
    return true;
}

static void
check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(const lit_mesh_t* mesh) {
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    static uint16_t walked[SPONZA_CLUSTER_COUNT];
    static uint8_t kept[SPONZA_CLUSTER_COUNT];
    for (uint32_t t = 0; t < period; t += 2500) {
        lit_vec3_t eye, forward;
        camera_path_sample(&sponza_flythrough, t, &eye, &forward);
        lit_view_t view;
        lit_view_look(&view, eye, forward, 0.62f, 6.0f, mesh->position_scale, 368, 448, (int)(t / 2500) & 3);

        memset(kept, 0, sizeof kept);
        const int count = lit_cull_clusters(mesh, &view, walked);
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, kept[walked[i]], "a cluster was listed twice");
            kept[walked[i]] = 1;
        }
        int flat = 0;
        for (int c = 0; c < mesh->cluster_count; c++) {
            const bool in_view = cluster_in_view(&mesh->clusters[c], &view);
            flat += in_view;
            TEST_ASSERT_EQUAL_MESSAGE(in_view, kept[c], "the walk and the flat test disagree on a cluster");
        }
        TEST_ASSERT_EQUAL_INT(flat, count);
    }
}

typedef struct {
    float x, y, z;
} v3;

static v3
sub(v3 a, v3 b) {
    return (v3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static float
dot(v3 a, v3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static v3
add_scaled(v3 a, v3 d, float s) {
    return (v3){a.x + d.x * s, a.y + d.y * s, a.z + d.z * s};
}

/* Ericson, Real-Time Collision Detection 5.1.5. */
static float
point_triangle_distance(v3 p, v3 a, v3 b, v3 c) {
    const v3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    v3 q;
    if (d1 <= 0 && d2 <= 0) {
        q = a;
    } else {
        const v3 bp = sub(p, b);
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        const v3 cp = sub(p, c);
        const float d5 = dot(ab, cp), d6 = dot(ac, cp);
        const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0 && d4 <= d3) {
            q = b;
        } else if (d6 >= 0 && d5 <= d6) {
            q = c;
        } else if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            q = add_scaled(a, ab, d1 / (d1 - d3));
        } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            q = add_scaled(a, ac, d2 / (d2 - d6));
        } else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            q = add_scaled(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
        } else {
            const float denom = 1.0f / (va + vb + vc);
            q = add_scaled(add_scaled(a, ab, vb * denom), ac, vc * denom);
        }
    }
    const v3 d = sub(p, q);
    return sqrtf(dot(d, d));
}

static v3
vertex(const lit_mesh_t* mesh, int i) {
    const float s = 1.0f / (float)mesh->position_scale;
    return (v3){mesh->positions[i][0] * s, mesh->positions[i][1] * s, mesh->positions[i][2] * s};
}

static float
box_distance(const lit_mesh_t* mesh, const lit_cluster_t* c, v3 p) {
    const float s = 1.0f / (float)mesh->position_scale;
    const float point[3] = {p.x, p.y, p.z};
    float sum = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float lo = c->lo[k] * s, hi = c->hi[k] * s;
        const float d = point[k] < lo ? lo - point[k] : (point[k] > hi ? point[k] - hi : 0.0f);
        sum += d * d;
    }
    return sqrtf(sum);
}

static float
clearance(const lit_mesh_t* mesh, v3 p) {
    float best = INFINITY;
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        if (box_distance(mesh, c, p) >= best) {
            continue;
        }
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const float d =
                point_triangle_distance(p, vertex(mesh, tri[0]), vertex(mesh, tri[1]), vertex(mesh, tri[2]));
            if (d < best) {
                best = d;
            }
        }
    }
    return best;
}

static void
check_the_flythrough_keeps_clear_of_every_triangle(const lit_mesh_t* mesh) {
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    for (uint32_t t = 0; t < period; t += 100) {
        lit_vec3_t eye, forward;
        camera_path_sample(&sponza_flythrough, t, &eye, &forward);
        const float d = clearance(mesh, (v3){eye.x, eye.y, eye.z});
        if (d < SPONZA_FLYTHROUGH_CLEARANCE) {
            char message[96];
            snprintf(message, sizeof message, "t=%u ms eye (%.0f, %.0f, %.0f) is %.1f from a triangle", (unsigned)t,
                     (double)eye.x, (double)eye.y, (double)eye.z, (double)d);
            TEST_FAIL_MESSAGE(message);
        }
    }
}

/* Every check holds for both bakes. */
static void
test_clusters_tile_both_arrays_in_order(void) {
    check_clusters_tile_both_arrays_in_order(&sponza_mesh);
    check_clusters_tile_both_arrays_in_order(&sponza_lite_mesh);
}

static void
test_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster(void) {
    check_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster(&sponza_mesh);
    check_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster(&sponza_lite_mesh);
}

static void
test_cluster_bounds_hold_their_vertices(void) {
    check_cluster_bounds_hold_their_vertices(&sponza_mesh);
    check_cluster_bounds_hold_their_vertices(&sponza_lite_mesh);
}

static void
test_the_tree_holds_every_cluster_once_inside_its_ancestors_bounds(void) {
    check_the_tree_holds_every_cluster_once_inside_its_ancestors_bounds(&sponza_mesh);
    check_the_tree_holds_every_cluster_once_inside_its_ancestors_bounds(&sponza_lite_mesh);
}

static void
test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(void) {
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(&sponza_mesh);
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(&sponza_lite_mesh);
}

static void
test_the_flythrough_keeps_clear_of_every_triangle(void) {
    check_the_flythrough_keeps_clear_of_every_triangle(&sponza_mesh);
    check_the_flythrough_keeps_clear_of_every_triangle(&sponza_lite_mesh);
}

static void
run_sponza_suite(void) {
    RUN_TEST(test_clusters_tile_both_arrays_in_order);
    RUN_TEST(test_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster);
    RUN_TEST(test_cluster_bounds_hold_their_vertices);
    RUN_TEST(test_the_tree_holds_every_cluster_once_inside_its_ancestors_bounds);
    RUN_TEST(test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps);
    RUN_TEST(test_the_flythrough_keeps_clear_of_every_triangle);
}

SUITE_REGISTER(run_sponza_suite);

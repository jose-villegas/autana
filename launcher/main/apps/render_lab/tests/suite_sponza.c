/*
 * Portable suite: the baked Sponza mesh (sponza_mesh_generated.h) and the
 * camera loop through it (sponza_flythrough.h). The mesh is checked for the
 * structure lit_pipeline.h relies on, never against the generator; the path
 * is checked against the shipped mesh itself.
 *
 * Clusters come in two kinds, told apart by the tree alone: a leaf's (full
 * detail) and a node's proxy. Working arrays sized by the mesh live in
 * PSRAM, never static, so the suite fits a device's internal RAM.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "suites.h"
#include "unity.h"

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_mesh_generated.h"

#define WALK_STEP_MS 2500

static const lit_mesh_t*
fixture(void) {
    return &sponza_mesh;
}

static void*
scratch(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static void*
scratch_zeroed(size_t bytes) {
    return memset(scratch(bytes), 0, bytes);
}

/* Which clusters a leaf holds, and each node's leaf clusters as a range:
 * the bake lays a subtree's leaf clusters out together. */
typedef struct {
    uint8_t* detail;       /* cluster_count entries */
    uint16_t *first, *end; /* node_count entries */
} tree_facts_t;

static void
gather(const lit_mesh_t* mesh, int node, tree_facts_t* facts) {
    const lit_node_t* n = &mesh->nodes[node];
    if (n->leaf) {
        for (int c = n->first; c < n->first + n->count; c++) {
            facts->detail[c] = 1;
        }
        facts->first[node] = n->first;
        facts->end[node] = (uint16_t)(n->first + n->count);
        return;
    }
    int first = mesh->cluster_count, end = 0, held = 0;
    for (int child = n->first; child < n->first + n->count; child++) {
        gather(mesh, child, facts);
        first = facts->first[child] < first ? facts->first[child] : first;
        end = facts->end[child] > end ? facts->end[child] : end;
        held += facts->end[child] - facts->first[child];
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(end - first, held, "a subtree's leaf clusters must sit together");
    facts->first[node] = (uint16_t)first;
    facts->end[node] = (uint16_t)end;
}

static tree_facts_t
tree_facts(const lit_mesh_t* mesh) {
    tree_facts_t facts = {scratch_zeroed((size_t)mesh->cluster_count),
                          scratch(sizeof(uint16_t) * (size_t)mesh->node_count),
                          scratch(sizeof(uint16_t) * (size_t)mesh->node_count)};
    gather(mesh, 0, &facts);
    return facts;
}

static void
free_tree_facts(tree_facts_t* facts) {
    heap_caps_free(facts->detail);
    heap_caps_free(facts->first);
    heap_caps_free(facts->end);
}

static void
test_clusters_tile_both_arrays_in_order(void) {
    const lit_mesh_t* mesh = fixture();
    uint32_t next_vertex = 0, next_triangle = 0;
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        TEST_ASSERT_EQUAL_UINT32(next_vertex, c->vertex_first);
        TEST_ASSERT_EQUAL_UINT32(next_triangle, c->triangle_first);
        next_vertex += c->vertex_count;
        next_triangle += c->triangle_count;
    }
    TEST_ASSERT_EQUAL_INT(mesh->vertex_count, (int)next_vertex);
    TEST_ASSERT_EQUAL_INT(mesh->triangle_count, (int)next_triangle);
}

static void
test_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster(void) {
    const lit_mesh_t* mesh = fixture();
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        for (uint32_t t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            for (int k = 0; k < 3; k++) {
                TEST_ASSERT_TRUE(tri[k] < c->vertex_count);
            }
            TEST_ASSERT_TRUE(tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]);
        }
    }
}

static void
test_cluster_bounds_hold_their_vertices(void) {
    const lit_mesh_t* mesh = fixture();
    for (int i = 0; i < mesh->cluster_count; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        for (uint32_t v = c->vertex_first; v < c->vertex_first + c->vertex_count; v++) {
            for (int k = 0; k < 3; k++) {
                TEST_ASSERT_TRUE(mesh->positions[v][k] >= c->lo[k] && mesh->positions[v][k] <= c->hi[k]);
            }
        }
    }
}

static bool
box_inside(const int16_t lo[3], const int16_t hi[3], const lit_node_t* n) {
    for (int k = 0; k < 3; k++) {
        if (lo[k] < n->lo[k] || hi[k] > n->hi[k]) {
            return false;
        }
    }
    return true;
}

static void
visit(const lit_mesh_t* mesh, int node, uint8_t* reached) {
    const lit_node_t* n = &mesh->nodes[node];
    for (int c = n->lod_first; c < n->lod_first + n->lod_count; c++) {
        reached[c]++;
        TEST_ASSERT_TRUE_MESSAGE(box_inside(mesh->clusters[c].lo, mesh->clusters[c].hi, n),
                                 "a proxy reaches outside its node");
    }
    if (n->leaf) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, n->lod_count, "a leaf carries a proxy");
        for (int c = n->first; c < n->first + n->count; c++) {
            reached[c]++;
            TEST_ASSERT_TRUE(box_inside(mesh->clusters[c].lo, mesh->clusters[c].hi, n));
        }
        return;
    }
    for (int child = n->first; child < n->first + n->count; child++) {
        TEST_ASSERT_TRUE_MESSAGE(child > node, "a child must follow its parent");
        TEST_ASSERT_TRUE(box_inside(mesh->nodes[child].lo, mesh->nodes[child].hi, n));
        visit(mesh, child, reached);
    }
}

static void
test_the_tree_holds_every_cluster_once_as_a_leafs_or_a_proxy(void) {
    const lit_mesh_t* mesh = fixture();
    uint8_t* reached = scratch_zeroed((size_t)mesh->cluster_count);
    visit(mesh, 0, reached);
    for (int c = 0; c < mesh->cluster_count; c++) {
        TEST_ASSERT_EQUAL_UINT8(1, reached[c]);
    }
    heap_caps_free(reached);
}

static int
triangles_in(const lit_mesh_t* mesh, int first, int end) {
    int n = 0;
    for (int c = first; c < end; c++) {
        n += mesh->clusters[c].triangle_count;
    }
    return n;
}

static void
test_a_proxy_has_fewer_triangles_than_the_detail_it_replaces(void) {
    const lit_mesh_t* mesh = fixture();
    tree_facts_t facts = tree_facts(mesh);
    int proxies = 0;
    for (int i = 0; i < mesh->node_count; i++) {
        const lit_node_t* n = &mesh->nodes[i];
        if (n->lod_count == 0) {
            continue;
        }
        proxies++;
        TEST_ASSERT_LESS_THAN_INT(triangles_in(mesh, facts.first[i], facts.end[i]),
                                  triangles_in(mesh, n->lod_first, n->lod_first + n->lod_count));
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, proxies, "the mesh carries no proxy at all");
    free_tree_facts(&facts);
}

/* Edges by position: a crack is two edges that should meet but do not. */
typedef struct {
    uint64_t a, b;
} edge_t;

static uint64_t
position_key(const lit_mesh_t* mesh, uint32_t v) {
    const int16_t* p = mesh->positions[v];
    return ((uint64_t)(p[0] + 32768) << 32) | ((uint64_t)(p[1] + 32768) << 16) | (uint64_t)(p[2] + 32768);
}

static int
edge_order(const void* pa, const void* pb) {
    const edge_t *x = pa, *y = pb;
    if (x->a != y->a) {
        return x->a < y->a ? -1 : 1;
    }
    return x->b < y->b ? -1 : (x->b > y->b ? 1 : 0);
}

static int
collect_edges(const lit_mesh_t* mesh, int first, int end, edge_t* out) {
    int n = 0;
    for (int ci = first; ci < end; ci++) {
        const lit_cluster_t* c = &mesh->clusters[ci];
        for (uint32_t t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            for (int k = 0; k < 3; k++) {
                const uint64_t p = position_key(mesh, c->vertex_first + mesh->triangles[t][k]);
                const uint64_t q = position_key(mesh, c->vertex_first + mesh->triangles[t][(k + 1) % 3]);
                out[n++] = (edge_t){p < q ? p : q, p < q ? q : p};
            }
        }
    }
    qsort(out, (size_t)n, sizeof *out, edge_order);
    return n;
}

static int
occurrences(const edge_t* sorted, int n, edge_t e) {
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (edge_order(&sorted[mid], &e) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    int count = 0;
    while (lo + count < n && edge_order(&sorted[lo + count], &e) == 0) {
        count++;
    }
    return count;
}

/* An edge a subtree shares with detail outside it is where a proxy meets a
 * neighbour, so the proxy must have that same edge. */
static void
test_a_proxy_keeps_every_edge_its_subtree_shares_with_the_rest(void) {
    const lit_mesh_t* mesh = fixture();
    tree_facts_t facts = tree_facts(mesh);
    int detail_end = 0;
    while (detail_end < mesh->cluster_count && facts.detail[detail_end]) {
        detail_end++;
    }
    const int detail_edges = 3 * triangles_in(mesh, 0, detail_end);
    edge_t* all = scratch(sizeof(edge_t) * (size_t)detail_edges);
    edge_t* inside = scratch(sizeof(edge_t) * (size_t)detail_edges);
    edge_t* proxy = scratch(sizeof(edge_t) * 3 * (size_t)mesh->triangle_count);
    TEST_ASSERT_EQUAL_INT(detail_edges, collect_edges(mesh, 0, detail_end, all));

    for (int i = 0; i < mesh->node_count; i++) {
        const lit_node_t* node = &mesh->nodes[i];
        if (node->lod_count == 0) {
            continue;
        }
        const int n_inside = collect_edges(mesh, facts.first[i], facts.end[i], inside);
        const int n_proxy = collect_edges(mesh, node->lod_first, node->lod_first + node->lod_count, proxy);
        for (int e = 0; e < n_inside;) {
            int run = 1;
            while (e + run < n_inside && edge_order(&inside[e], &inside[e + run]) == 0) {
                run++;
            }
            if (occurrences(all, detail_edges, inside[e]) > run && occurrences(proxy, n_proxy, inside[e]) == 0) {
                char message[80];
                snprintf(message, sizeof message, "node %d's proxy lost an edge its neighbours use", i);
                TEST_FAIL_MESSAGE(message);
            }
            e += run;
        }
    }
    heap_caps_free(all);
    heap_caps_free(inside);
    heap_caps_free(proxy);
    free_tree_facts(&facts);
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

static lit_view_t
path_view(const lit_mesh_t* mesh, uint32_t t) {
    lit_vec3_t eye, forward;
    camera_path_sample(&sponza_flythrough, t, &eye, &forward);
    lit_view_t view;
    lit_view_look(&view, eye, forward, 0.62f, 6.0f, mesh->position_scale, 368, 448, (int)(t / WALK_STEP_MS) & 3);
    return view;
}

static void
test_with_no_error_allowed_the_walk_keeps_exactly_what_a_flat_test_keeps(void) {
    const lit_mesh_t* mesh = fixture();
    tree_facts_t facts = tree_facts(mesh);
    uint16_t* walked = scratch(sizeof(uint16_t) * (size_t)mesh->cluster_count);
    uint8_t* kept = scratch((size_t)mesh->cluster_count);
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    for (uint32_t t = 0; t < period; t += WALK_STEP_MS) {
        const lit_view_t view = path_view(mesh, t);
        memset(kept, 0, (size_t)mesh->cluster_count);
        const int count = lit_cull_clusters_lod(mesh, &view, 0.0f, walked);
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, kept[walked[i]], "a cluster was listed twice");
            kept[walked[i]] = 1;
        }
        int flat = 0;
        for (int c = 0; c < mesh->cluster_count; c++) {
            const bool in_view = facts.detail[c] && cluster_in_view(&mesh->clusters[c], &view);
            flat += in_view;
            TEST_ASSERT_EQUAL_MESSAGE(in_view, kept[c], "the walk and the flat test disagree on a cluster");
        }
        TEST_ASSERT_EQUAL_INT(flat, count);
    }
    heap_caps_free(walked);
    heap_caps_free(kept);
    free_tree_facts(&facts);
}

/* Counts, for each leaf cluster, how many times the walk drew it: itself,
 * or through a proxy of one of its ancestors. */
static void
count_cover(const lit_mesh_t* mesh, const tree_facts_t* facts, int node, const uint8_t* listed, uint8_t* cover) {
    const lit_node_t* n = &mesh->nodes[node];
    bool proxied = false;
    for (int c = n->lod_first; c < n->lod_first + n->lod_count; c++) {
        proxied |= listed[c] != 0;
    }
    if (proxied) {
        for (int c = facts->first[node]; c < facts->end[node]; c++) {
            cover[c]++;
        }
    }
    if (n->leaf) {
        for (int c = n->first; c < n->first + n->count; c++) {
            cover[c] += listed[c];
        }
        return;
    }
    for (int child = n->first; child < n->first + n->count; child++) {
        count_cover(mesh, facts, child, listed, cover);
    }
}

static void
test_the_lod_walk_draws_each_visible_leaf_cluster_exactly_once(void) {
    const lit_mesh_t* mesh = fixture();
    tree_facts_t facts = tree_facts(mesh);
    uint16_t* walked = scratch(sizeof(uint16_t) * (size_t)mesh->cluster_count);
    uint8_t* listed = scratch((size_t)mesh->cluster_count);
    uint8_t* cover = scratch((size_t)mesh->cluster_count);
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    int proxies_drawn = 0;
    for (uint32_t t = 0; t < period; t += WALK_STEP_MS) {
        const lit_view_t view = path_view(mesh, t);
        memset(listed, 0, (size_t)mesh->cluster_count);
        memset(cover, 0, (size_t)mesh->cluster_count);
        const int count = lit_cull_clusters(mesh, &view, walked);
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, listed[walked[i]], "a cluster was listed twice");
            listed[walked[i]] = 1;
            proxies_drawn += !facts.detail[walked[i]];
        }
        count_cover(mesh, &facts, 0, listed, cover);
        for (int c = 0; c < mesh->cluster_count; c++) {
            if (!facts.detail[c]) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(cover[c] <= 1, "a leaf cluster was drawn both itself and through a proxy");
            if (cluster_in_view(&mesh->clusters[c], &view)) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, cover[c], "a leaf cluster in view was not drawn");
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, proxies_drawn, "no proxy was ever drawn along the flythrough");
    heap_caps_free(walked);
    heap_caps_free(listed);
    heap_caps_free(cover);
    free_tree_facts(&facts);
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

/* In position ticks, so a proxy's error compares without a scale. */
static v3
vertex(const lit_mesh_t* mesh, uint32_t i) {
    return (v3){mesh->positions[i][0], mesh->positions[i][1], mesh->positions[i][2]};
}

static float
box_distance(const lit_cluster_t* c, v3 p) {
    const float point[3] = {p.x, p.y, p.z};
    float sum = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float lo = c->lo[k], hi = c->hi[k];
        const float d = point[k] < lo ? lo - point[k] : (point[k] > hi ? point[k] - hi : 0.0f);
        sum += d * d;
    }
    return sqrtf(sum);
}

/* Ticks from p to the nearest triangle of clusters first..end-1. */
static float
distance_to_clusters(const lit_mesh_t* mesh, int first, int end, v3 p) {
    float best = INFINITY;
    for (int i = first; i < end; i++) {
        const lit_cluster_t* c = &mesh->clusters[i];
        if (box_distance(c, p) >= best) {
            continue;
        }
        for (uint32_t t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const float d =
                point_triangle_distance(p, vertex(mesh, c->vertex_first + tri[0]),
                                        vertex(mesh, c->vertex_first + tri[1]), vertex(mesh, c->vertex_first + tri[2]));
            best = d < best ? d : best;
        }
    }
    return best;
}

static void
test_every_proxy_vertex_lies_within_its_nodes_error_of_the_detail(void) {
    const lit_mesh_t* mesh = fixture();
    tree_facts_t facts = tree_facts(mesh);
    for (int i = 0; i < mesh->node_count; i++) {
        const lit_node_t* n = &mesh->nodes[i];
        for (int ci = n->lod_first; ci < n->lod_first + n->lod_count; ci++) {
            const lit_cluster_t* c = &mesh->clusters[ci];
            for (uint32_t v = c->vertex_first; v < c->vertex_first + c->vertex_count; v++) {
                const float d = distance_to_clusters(mesh, facts.first[i], facts.end[i], vertex(mesh, v));
                if (d > (float)n->lod_error + 0.5f) {
                    char message[96];
                    snprintf(message, sizeof message, "node %d's proxy strays %.1f ticks, its error says %u", i,
                             (double)d, (unsigned)n->lod_error);
                    TEST_FAIL_MESSAGE(message);
                }
            }
        }
    }
    free_tree_facts(&facts);
}

/* Model units from p to the nearest full-detail triangle. */
static float
clearance(const lit_mesh_t* mesh, const tree_facts_t* facts, v3 p) {
    const float s = (float)mesh->position_scale;
    const float d = distance_to_clusters(mesh, facts->first[0], facts->end[0], (v3){p.x * s, p.y * s, p.z * s});
    return d / s;
}

static void
test_the_flythrough_keeps_clear_of_every_triangle(void) {
    const lit_mesh_t* mesh = fixture();
    tree_facts_t facts = tree_facts(mesh);
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    for (uint32_t t = 0; t < period; t += 100) {
        lit_vec3_t eye, forward;
        camera_path_sample(&sponza_flythrough, t, &eye, &forward);
        const float d = clearance(mesh, &facts, (v3){eye.x, eye.y, eye.z});
        if (d < SPONZA_FLYTHROUGH_CLEARANCE) {
            char message[96];
            snprintf(message, sizeof message, "t=%u ms eye (%.0f, %.0f, %.0f) is %.1f from a triangle", (unsigned)t,
                     (double)eye.x, (double)eye.y, (double)eye.z, (double)d);
            TEST_FAIL_MESSAGE(message);
        }
    }
    free_tree_facts(&facts);
}

static void
run_sponza_suite(void) {
    RUN_TEST(test_clusters_tile_both_arrays_in_order);
    RUN_TEST(test_every_triangle_indexes_three_distinct_vertices_of_its_own_cluster);
    RUN_TEST(test_cluster_bounds_hold_their_vertices);
    RUN_TEST(test_the_tree_holds_every_cluster_once_as_a_leafs_or_a_proxy);
    RUN_TEST(test_a_proxy_has_fewer_triangles_than_the_detail_it_replaces);
    RUN_TEST(test_a_proxy_keeps_every_edge_its_subtree_shares_with_the_rest);
    RUN_TEST(test_every_proxy_vertex_lies_within_its_nodes_error_of_the_detail);
    RUN_TEST(test_with_no_error_allowed_the_walk_keeps_exactly_what_a_flat_test_keeps);
    RUN_TEST(test_the_lod_walk_draws_each_visible_leaf_cluster_exactly_once);
    RUN_TEST(test_the_flythrough_keeps_clear_of_every_triangle);
}

SUITE_REGISTER(run_sponza_suite);

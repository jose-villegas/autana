/*
 * r3d_lit_mesh_expect: one Unity check that a baked r3d_lit_mesh_t has the
 * structure render/r3d_pipeline.h relies on, for a suite holding any
 * bake to call once per mesh: clusters tile both arrays in order, every
 * triangle indexes three distinct vertices of its own cluster, bounds hold
 * what they claim, inner nodes fit the walk, and the tree reaches every
 * cluster exactly once.
 *
 * Header-only (static inline) for the reason bbox_extend.h gives.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>

#include "render/r3d_lit_mesh.h"
#include "unity.h"

static inline void
r3d_lit_mesh_expect_cluster(const r3d_lit_mesh_t* mesh, const r3d_lit_cluster_t* c) {
    const int v_end = c->vertex_first + c->vertex_count;
    for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
        const uint16_t* tri = mesh->triangles[t];
        for (int k = 0; k < 3; k++) {
            TEST_ASSERT_TRUE_MESSAGE(tri[k] >= c->vertex_first && tri[k] < v_end,
                                     "a triangle reaches outside its cluster");
        }
        TEST_ASSERT_TRUE_MESSAGE(tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2],
                                 "a triangle repeats a vertex");
    }
    for (int v = c->vertex_first; v < v_end; v++) {
        const int16_t* p = mesh->positions[v];
        TEST_ASSERT_TRUE_MESSAGE(box3s_contains_point(c->bounds, vec3s_from_array(p)),
                                 "a vertex lies outside its cluster's bounds");
    }
}

static inline void
r3d_lit_mesh_expect_subtree(const r3d_lit_mesh_t* mesh, int node, uint8_t* reached) {
    const r3d_lit_node_t* n = &mesh->nodes[node];
    if (n->leaf) {
        TEST_ASSERT_TRUE(n->first + n->count <= mesh->cluster_count);
        for (int c = n->first; c < n->first + n->count; c++) {
            reached[c]++;
            TEST_ASSERT_TRUE_MESSAGE(box3s_contains(n->bounds, mesh->clusters[c].bounds),
                                     "a box sticks out of its parent's");
        }
        return;
    }
    TEST_ASSERT_TRUE(n->first + n->count <= mesh->node_count);
    TEST_ASSERT_TRUE_MESSAGE(n->count <= R3D_LIT_INNER_CHILDREN_MAX,
                             "an inner node has more children than the walk orders");
    for (int child = n->first; child < n->first + n->count; child++) {
        TEST_ASSERT_TRUE_MESSAGE(child > node, "a child must follow its parent");
        TEST_ASSERT_TRUE_MESSAGE(box3s_contains(n->bounds, mesh->nodes[child].bounds),
                                 "a box sticks out of its parent's");
        r3d_lit_mesh_expect_subtree(mesh, child, reached);
    }
}

static inline void
r3d_lit_mesh_expect_valid(const r3d_lit_mesh_t* mesh) {
    TEST_ASSERT_TRUE(mesh->position_scale > 0);
    TEST_ASSERT_TRUE_MESSAGE((mesh->colors == NULL) != (mesh->face_colors == NULL),
                             "a mesh is lit by vertex colours or by face colours, never both or neither");
    int next_vertex = 0, next_triangle = 0;
    for (int i = 0; i < mesh->cluster_count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(next_vertex, c->vertex_first, "clusters must tile the vertices in order");
        TEST_ASSERT_EQUAL_INT_MESSAGE(next_triangle, c->triangle_first, "clusters must tile the triangles in order");
        r3d_lit_mesh_expect_cluster(mesh, c);
        next_vertex += c->vertex_count;
        next_triangle += c->triangle_count;
    }
    TEST_ASSERT_EQUAL_INT(mesh->vertex_count, next_vertex);
    TEST_ASSERT_EQUAL_INT(mesh->triangle_count, next_triangle);

    uint8_t* reached = calloc((size_t)mesh->cluster_count, 1);
    TEST_ASSERT_NOT_NULL(reached);
    r3d_lit_mesh_expect_subtree(mesh, 0, reached);
    for (int c = 0; c < mesh->cluster_count; c++) {
        if (reached[c] != 1) {
            free(reached);
            TEST_FAIL_MESSAGE("every cluster must hang off exactly one leaf");
        }
    }
    free(reached);
}

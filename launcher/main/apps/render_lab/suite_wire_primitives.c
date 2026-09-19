/*
 * Portable suite: wire_primitives_generated.h against the formulas its own
 * #defines encode and the underlying geometry, never against
 * gen_wire_primitives.py's own logic - a generator bug that agreed with
 * itself would otherwise pass unnoticed.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "wire_primitives_generated.h"

/* Heap-allocated and sized to the mesh, not a worst-case constant: the
 * device's main task stack is 3,584 bytes total, too little to spare for a
 * fixed 1024-entry visited/stack pair. */
static bool
is_connected(const wire_mesh_t* mesh) {
    uint8_t* visited = calloc(mesh->vertex_count, sizeof(*visited));
    uint16_t* stack = malloc(mesh->vertex_count * sizeof(*stack));
    int top = 0;
    bool connected = true;

    visited[0] = 1;
    stack[top++] = 0;
    while (top > 0) {
        const uint16_t v = stack[--top];
        for (uint16_t i = 0; i < mesh->edge_count; i++) {
            const wire_edge_t* e = &mesh->edges[i];
            uint16_t other;
            if (e->a == v) {
                other = e->b;
            } else if (e->b == v) {
                other = e->a;
            } else {
                continue;
            }
            if (!visited[other]) {
                visited[other] = 1;
                stack[top++] = other;
            }
        }
    }

    for (uint16_t i = 0; i < mesh->vertex_count; i++) {
        if (!visited[i]) {
            connected = false;
            break;
        }
    }

    free(visited);
    free(stack);
    return connected;
}

static void
check_indices_in_range_and_no_duplicates(const wire_mesh_t* mesh) {
    for (uint16_t i = 0; i < mesh->edge_count; i++) {
        const wire_edge_t* e = &mesh->edges[i];
        TEST_ASSERT_TRUE(e->a < mesh->vertex_count);
        TEST_ASSERT_TRUE(e->b < mesh->vertex_count);
        TEST_ASSERT_NOT_EQUAL(e->a, e->b);

        for (uint16_t j = (uint16_t)(i + 1); j < mesh->edge_count; j++) {
            const wire_edge_t* other = &mesh->edges[j];
            const bool same = (e->a == other->a && e->b == other->b) || (e->a == other->b && e->b == other->a);
            TEST_ASSERT_FALSE_MESSAGE(same, "an edge must not appear twice in either direction");
        }
    }
}

static void
test_plane_indices_are_in_range_with_no_duplicates(void) {
    check_indices_in_range_and_no_duplicates(&wire_plane_mesh);
}

static void
test_cube_indices_are_in_range_with_no_duplicates(void) {
    check_indices_in_range_and_no_duplicates(&wire_cube_mesh);
}

static void
test_sphere_indices_are_in_range_with_no_duplicates(void) {
    check_indices_in_range_and_no_duplicates(&wire_sphere_mesh);
}

static void
test_capsule_indices_are_in_range_with_no_duplicates(void) {
    check_indices_in_range_and_no_duplicates(&wire_capsule_mesh);
}

static void
test_plane_is_one_connected_component(void) {
    TEST_ASSERT_TRUE(is_connected(&wire_plane_mesh));
}

static void
test_cube_is_one_connected_component(void) {
    TEST_ASSERT_TRUE(is_connected(&wire_cube_mesh));
}

static void
test_sphere_is_one_connected_component(void) {
    TEST_ASSERT_TRUE(is_connected(&wire_sphere_mesh));
}

static void
test_capsule_is_one_connected_component(void) {
    TEST_ASSERT_TRUE(is_connected(&wire_capsule_mesh));
}

/* Plane */

static void
test_plane_counts_match_the_defined_density(void) {
    TEST_ASSERT_EQUAL_INT(WIRE_PLANE_N * WIRE_PLANE_N, wire_plane_mesh.vertex_count);
    TEST_ASSERT_EQUAL_INT(2 * WIRE_PLANE_N * (WIRE_PLANE_N - 1), wire_plane_mesh.edge_count);
}

static void
test_plane_edges_join_grid_neighbours_one_step_apart(void) {
    for (uint16_t i = 0; i < wire_plane_mesh.edge_count; i++) {
        const wire_edge_t* e = &wire_plane_mesh.edges[i];
        const int row_a = e->a / WIRE_PLANE_N, col_a = e->a % WIRE_PLANE_N;
        const int row_b = e->b / WIRE_PLANE_N, col_b = e->b % WIRE_PLANE_N;
        const int row_diff = row_a > row_b ? row_a - row_b : row_b - row_a;
        const int col_diff = col_a > col_b ? col_a - col_b : col_b - col_a;
        TEST_ASSERT_EQUAL_INT(1, row_diff + col_diff);
    }
}

static void
test_plane_grid_is_centred(void) {
    int min_x = wire_plane_mesh.vertices[0].x, max_x = min_x;
    int min_z = wire_plane_mesh.vertices[0].z, max_z = min_z;

    for (uint16_t i = 0; i < wire_plane_mesh.vertex_count; i++) {
        const wire_vertex_t* v = &wire_plane_mesh.vertices[i];
        TEST_ASSERT_EQUAL_INT(0, v->y);
        if (v->x < min_x) {
            min_x = v->x;
        }
        if (v->x > max_x) {
            max_x = v->x;
        }
        if (v->z < min_z) {
            min_z = v->z;
        }
        if (v->z > max_z) {
            max_z = v->z;
        }
    }

    TEST_ASSERT_EQUAL_INT(0, min_x + max_x);
    TEST_ASSERT_EQUAL_INT(0, min_z + max_z);
}

/* Cube */

static void
test_cube_has_eight_vertices_and_twelve_edges(void) {
    TEST_ASSERT_EQUAL_INT(8, wire_cube_mesh.vertex_count);
    TEST_ASSERT_EQUAL_INT(12, wire_cube_mesh.edge_count);
}

/* Sphere */

static void
test_sphere_counts_match_the_defined_density(void) {
    TEST_ASSERT_EQUAL_INT(WIRE_SPHERE_RINGS * WIRE_SPHERE_MERIDIANS + 2, wire_sphere_mesh.vertex_count);
    TEST_ASSERT_EQUAL_INT(WIRE_SPHERE_MERIDIANS * (2 * WIRE_SPHERE_RINGS + 1), wire_sphere_mesh.edge_count);
}

/* Unity's double asserts are excluded in this build (see unity_internals.h,
 * UNITY_INCLUDE_DOUBLE never defined here), so the reference is computed in
 * plain double and compared as an int, the same pattern suite_boot_anim.c
 * uses for its own double-precision reference. */
static void
test_sphere_vertices_sit_at_the_radius(void) {
    for (uint16_t i = 0; i < wire_sphere_mesh.vertex_count; i++) {
        const wire_vertex_t* v = &wire_sphere_mesh.vertices[i];
        const double distance = sqrt((double)v->x * v->x + (double)v->y * v->y + (double)v->z * v->z);
        TEST_ASSERT_INT_WITHIN(1, WIRE_SPHERE_RADIUS, (int)(distance + 0.5));
    }
}

/* Capsule */

static void
test_capsule_counts_match_the_defined_density(void) {
    TEST_ASSERT_EQUAL_INT(2 + 2 * WIRE_CAPSULE_RINGS * WIRE_CAPSULE_MERIDIANS, wire_capsule_mesh.vertex_count);
    TEST_ASSERT_EQUAL_INT(WIRE_CAPSULE_MERIDIANS * (4 * WIRE_CAPSULE_RINGS + 1), wire_capsule_mesh.edge_count);
}

static void
test_capsule_vertices_sit_at_the_radius_from_their_hemisphere_centre(void) {
    for (uint16_t i = 0; i < wire_capsule_mesh.vertex_count; i++) {
        const wire_vertex_t* v = &wire_capsule_mesh.vertices[i];
        const double centre_y = v->y >= 0 ? WIRE_CAPSULE_CYLINDER_HALF_LEN : -WIRE_CAPSULE_CYLINDER_HALF_LEN;
        const double dy = v->y - centre_y;
        const double distance = sqrt((double)v->x * v->x + dy * dy + (double)v->z * v->z);
        TEST_ASSERT_INT_WITHIN(1, WIRE_CAPSULE_RADIUS, (int)(distance + 0.5));
    }
}

void
run_wire_primitives_suite(void) {
    RUN_TEST(test_plane_indices_are_in_range_with_no_duplicates);
    RUN_TEST(test_cube_indices_are_in_range_with_no_duplicates);
    RUN_TEST(test_sphere_indices_are_in_range_with_no_duplicates);
    RUN_TEST(test_capsule_indices_are_in_range_with_no_duplicates);

    RUN_TEST(test_plane_is_one_connected_component);
    RUN_TEST(test_cube_is_one_connected_component);
    RUN_TEST(test_sphere_is_one_connected_component);
    RUN_TEST(test_capsule_is_one_connected_component);

    RUN_TEST(test_plane_counts_match_the_defined_density);
    RUN_TEST(test_plane_edges_join_grid_neighbours_one_step_apart);
    RUN_TEST(test_plane_grid_is_centred);

    RUN_TEST(test_cube_has_eight_vertices_and_twelve_edges);

    RUN_TEST(test_sphere_counts_match_the_defined_density);
    RUN_TEST(test_sphere_vertices_sit_at_the_radius);

    RUN_TEST(test_capsule_counts_match_the_defined_density);
    RUN_TEST(test_capsule_vertices_sit_at_the_radius_from_their_hemisphere_centre);
}

SUITE_REGISTER(run_wire_primitives_suite);

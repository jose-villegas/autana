/*
 * Portable suite: primitive_mesh.h. Each shape must have the counts its
 * header names, lie on its unit shape, wind every triangle outward with
 * area, and write nothing when its buffers are short.
 */

#include <math.h>
#include <stdlib.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "math/geometry/primitive_mesh.h"

/* Room for the largest shape a test makes. */
#define MAX_VERTICES  1024
#define MAX_TRIANGLES 2048
/* Tessellations the tests make: a few of each, the smallest included. */
#define GRID_CELLS    7
#define SPHERE_RINGS  9
/* A float's rounding on a unit shape. */
#define ON_SHAPE      1e-5F

static vec3f_t* positions;
static uint16_t (*triangles)[3];

static void
fixture_end(void) {
    free(positions);
    free(triangles);
    positions = NULL;
    triangles = NULL;
}

static primitive_mesh_t
fixture_begin(int vertex_capacity, int triangle_capacity) {
    suite_add_test_cleanup(fixture_end);
    positions = malloc(sizeof(vec3f_t) * MAX_VERTICES);
    triangles = malloc(sizeof(uint16_t[3]) * MAX_TRIANGLES);
    TEST_ASSERT_NOT_NULL(positions);
    TEST_ASSERT_NOT_NULL(triangles);
    return (primitive_mesh_t){positions, vertex_capacity, triangles, triangle_capacity, 0, 0};
}

/* (b - a) x (c - a) of triangle `t`. */
static vec3f_t
face_normal(const primitive_mesh_t* m, int t) {
    const vec3f_t a = m->positions[m->triangles[t][0]];
    const vec3f_t b = m->positions[m->triangles[t][1]];
    const vec3f_t c = m->positions[m->triangles[t][2]];
    return vec3f_cross(vec3f_sub(b, a), vec3f_sub(c, a));
}

static void
assert_indices_in_range(const primitive_mesh_t* m) {
    for (int t = 0; t < m->triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            TEST_ASSERT_LESS_THAN_INT(m->vertex_count, m->triangles[t][k]);
        }
    }
}

static void
test_a_grid_covers_the_unit_square_facing_plus_z(void) {
    primitive_mesh_t m = fixture_begin(MAX_VERTICES, MAX_TRIANGLES);
    for (int cells = 1; cells <= GRID_CELLS; cells++) {
        TEST_ASSERT_TRUE(primitive_grid(cells, &m));
        TEST_ASSERT_EQUAL_INT(primitive_grid_vertex_count(cells), m.vertex_count);
        TEST_ASSERT_EQUAL_INT(primitive_grid_triangle_count(cells), m.triangle_count);
        assert_indices_in_range(&m);
        for (int v = 0; v < m.vertex_count; v++) {
            TEST_ASSERT_TRUE(fabsf(m.positions[v].x) <= 0.5F && fabsf(m.positions[v].y) <= 0.5F);
            TEST_ASSERT_EQUAL_FLOAT(0.0F, m.positions[v].z);
        }
        /* Every triangle faces +z, and together they cover the square once. */
        float area = 0.0F;
        for (int t = 0; t < m.triangle_count; t++) {
            const vec3f_t n = face_normal(&m, t);
            TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, n.z);
            area += 0.5F * n.z;
        }
        TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, 1.0F, area);
    }
}

static void
test_a_sphere_lies_on_the_unit_sphere_wound_outward(void) {
    primitive_mesh_t m = fixture_begin(MAX_VERTICES, MAX_TRIANGLES);
    for (int rings = PRIMITIVE_SPHERE_RINGS_MIN; rings <= SPHERE_RINGS; rings++) {
        TEST_ASSERT_TRUE(primitive_sphere(rings, &m));
        TEST_ASSERT_EQUAL_INT(primitive_sphere_vertex_count(rings), m.vertex_count);
        TEST_ASSERT_EQUAL_INT(primitive_sphere_triangle_count(rings), m.triangle_count);
        assert_indices_in_range(&m);
        for (int v = 0; v < m.vertex_count; v++) {
            TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, 1.0F, sqrtf(vec3f_dot(m.positions[v], m.positions[v])));
        }
        /* Outward: each triangle's normal points away from the centre, through its own centroid. */
        for (int t = 0; t < m.triangle_count; t++) {
            const vec3f_t centroid =
                vec3f_add(m.positions[m.triangles[t][0]],
                          vec3f_add(m.positions[m.triangles[t][1]], m.positions[m.triangles[t][2]]));
            TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, vec3f_dot(face_normal(&m, t), centroid));
        }
    }
}

static void
test_short_buffers_or_too_few_cuts_write_nothing(void) {
    const int cells = GRID_CELLS;
    const int rings = SPHERE_RINGS;
    primitive_mesh_t m = fixture_begin(primitive_grid_vertex_count(cells) - 1, MAX_TRIANGLES);
    TEST_ASSERT_FALSE(primitive_grid(cells, &m));
    m = (primitive_mesh_t){positions, MAX_VERTICES, triangles, primitive_sphere_triangle_count(rings) - 1, 0, 0};
    TEST_ASSERT_FALSE(primitive_sphere(rings, &m));
    m.triangle_capacity = MAX_TRIANGLES;
    TEST_ASSERT_FALSE(primitive_grid(0, &m));
    TEST_ASSERT_FALSE(primitive_sphere(PRIMITIVE_SPHERE_RINGS_MIN - 1, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
    TEST_ASSERT_EQUAL_INT(0, m.triangle_count);
}

/* A shape past what a uint16 index reaches is refused, whatever room the caller gives. */
static void
test_a_shape_past_uint16_indices_is_refused(void) {
    enum { CELLS_PAST_UINT16 = 256 }; /* 257 * 257 vertices */

    primitive_mesh_t m = {NULL, INT32_MAX, NULL, INT32_MAX, 0, 0};
    TEST_ASSERT_GREATER_THAN_INT(UINT16_MAX + 1, primitive_grid_vertex_count(CELLS_PAST_UINT16));
    TEST_ASSERT_FALSE(primitive_grid(CELLS_PAST_UINT16, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
}

void
suite_primitive_mesh(void) {
    RUN_TEST(test_a_grid_covers_the_unit_square_facing_plus_z);
    RUN_TEST(test_a_sphere_lies_on_the_unit_sphere_wound_outward);
    RUN_TEST(test_short_buffers_or_too_few_cuts_write_nothing);
    RUN_TEST(test_a_shape_past_uint16_indices_is_refused);
}

SUITE_REGISTER(suite_primitive_mesh);

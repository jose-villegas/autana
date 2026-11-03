/*
 * Portable suite: iso_surface.h, surface nets. A sphere's field must come
 * back as a closed surface on the sphere, wound and lit outward, wherever
 * the field is placed; a full buffer must leave whole triangles. Parts of
 * the field meshed apart and appended must make the same triangles.
 */

#include <stdlib.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "math/geometry/iso_surface.h"

/* This suite's own sphere: radius 4 samples in a 12-sample cube, so the
 * border is outside all round. */
#define SAMPLES       12
#define RADIUS        4.0F
#define CENTRE        5.5F
#define MAX_VERTICES  1000
#define MAX_TRIANGLES 2000
/* A surface nets vertex sits inside its cell: within one sample's diagonal
 * of the true surface. */
#define CELL_DIAGONAL 1.7320508F
/* Where the parts split the cell slabs: through the sphere. */
#define SPLIT_SLAB    5

static float* values;
static vec3f_t* positions;
static vec3f_t* normals;
static uint16_t (*triangles)[3];
static int32_t* scratch;
static iso_field_t unit; /* one unit a sample, from the origin */

static void
fixture_end(void) {
    free(values);
    free(positions);
    free(normals);
    free(triangles);
    free(scratch);
    values = NULL;
    positions = normals = NULL;
    triangles = NULL;
    scratch = NULL;
}

/* The signed distance inside a sphere at (CENTRE, CENTRE, CENTRE). */
static void
fixture_begin(void) {
    suite_add_test_cleanup(fixture_end);
    values = malloc(sizeof(float) * SAMPLES * SAMPLES * SAMPLES);
    positions = malloc(sizeof(vec3f_t) * MAX_VERTICES);
    normals = malloc(sizeof(vec3f_t) * MAX_VERTICES);
    triangles = malloc(sizeof(uint16_t[3]) * MAX_TRIANGLES);
    scratch = malloc(sizeof(int32_t) * iso_surface_scratch_count(SAMPLES, SAMPLES));
    TEST_ASSERT_NOT_NULL(values);
    TEST_ASSERT_NOT_NULL(scratch);
    for (int x = 0; x < SAMPLES; x++) {
        for (int y = 0; y < SAMPLES; y++) {
            for (int z = 0; z < SAMPLES; z++) {
                const vec3f_t d = {(float)x - CENTRE, (float)y - CENTRE, (float)z - CENTRE};
                values[(((x * SAMPLES) + y) * SAMPLES) + z] = RADIUS - sqrtf(vec3f_dot(d, d));
            }
        }
    }
    unit = (iso_field_t){values, SAMPLES, SAMPLES, SAMPLES, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
}

static iso_mesh_t
mesh(int vertex_capacity, int triangle_capacity) {
    return (iso_mesh_t){positions, normals, vertex_capacity, triangles, triangle_capacity, scratch, 0, 0};
}

static void
test_a_sphere_comes_back_on_the_sphere(void) {
    fixture_begin();
    iso_mesh_t m = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 0.0F, &m));
    TEST_ASSERT_GREATER_THAN_INT(0, m.triangle_count);
    for (int i = 0; i < m.vertex_count; i++) {
        const vec3f_t d = vec3f_sub(positions[i], (vec3f_t){CENTRE, CENTRE, CENTRE});
        TEST_ASSERT_FLOAT_WITHIN(CELL_DIAGONAL, RADIUS, sqrtf(vec3f_dot(d, d)));
        /* Lit outward: the normal leans along the radius. */
        TEST_ASSERT_GREATER_THAN_FLOAT(0.9F, vec3f_dot(normals[i], vec3f_normalize(d)));
    }
}

/* How many triangles hold the edge from `from` to `to`, in that direction. */
static int
edges_from_to(const iso_mesh_t* m, int from, int to) {
    int found = 0;
    for (int u = 0; u < m->triangle_count; u++) {
        for (int k = 0; k < 3; k++) {
            found += triangles[u][k] == from && triangles[u][(k + 1) % 3] == to ? 1 : 0;
        }
    }
    return found;
}

/* Every triangle faces out, and every edge meets its reverse exactly once:
 * the surface is closed and wound one way. */
static void
test_the_sphere_is_closed_and_wound_outward(void) {
    fixture_begin();
    iso_mesh_t m = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 0.0F, &m));
    for (int t = 0; t < m.triangle_count; t++) {
        const vec3f_t a = positions[triangles[t][0]];
        const vec3f_t b = positions[triangles[t][1]];
        const vec3f_t c = positions[triangles[t][2]];
        const vec3f_t facing = vec3f_cross(vec3f_sub(b, a), vec3f_sub(c, a));
        const vec3f_t centroid = vec3f_scale(vec3f_add(vec3f_add(a, b), c), 1.0F / 3.0F);
        TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, vec3f_dot(facing, vec3f_sub(centroid, (vec3f_t){CENTRE, CENTRE, CENTRE})));
        for (int e = 0; e < 3; e++) {
            TEST_ASSERT_EQUAL_INT(1, edges_from_to(&m, triangles[t][(e + 1) % 3], triangles[t][e]));
        }
    }
}

static void
test_origin_and_step_place_the_surface(void) {
    fixture_begin();
    iso_mesh_t first_mesh = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 0.0F, &first_mesh));
    const vec3f_t first = positions[0];
    const vec3f_t origin = {10.0F, -20.0F, 30.0F};
    const vec3f_t step = {2.0F, 3.0F, 0.5F};
    const iso_field_t placed_field = {values, SAMPLES, SAMPLES, SAMPLES, origin, step};
    iso_mesh_t placed = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&placed_field, 0.0F, &placed));
    TEST_ASSERT_EQUAL_INT(first_mesh.vertex_count, placed.vertex_count);
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, origin.x + (first.x * step.x), positions[0].x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, origin.y + (first.y * step.y), positions[0].y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, origin.z + (first.z * step.z), positions[0].z);
}

static void
test_a_full_buffer_keeps_whole_triangles(void) {
    fixture_begin();
    iso_mesh_t m = mesh(MAX_VERTICES, 7);
    TEST_ASSERT_FALSE(iso_surface_nets(&unit, 0.0F, &m));
    TEST_ASSERT_EQUAL_INT(6, m.triangle_count);
    for (int t = 0; t < m.triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            TEST_ASSERT_LESS_THAN_INT(m.vertex_count, triangles[t][k]);
        }
    }
    iso_mesh_t few = mesh(10, MAX_TRIANGLES);
    TEST_ASSERT_FALSE(iso_surface_nets(&unit, 0.0F, &few));
    TEST_ASSERT_EQUAL_INT(10, few.vertex_count);
}

static void
test_a_field_wholly_inside_or_outside_has_no_surface(void) {
    fixture_begin();
    iso_mesh_t m = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, -100.0F, &m));
    TEST_ASSERT_EQUAL_INT(0, m.triangle_count);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 100.0F, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
    iso_field_t thin = unit;
    thin.nx = 1;
    TEST_ASSERT_FALSE(iso_surface_nets(&thin, 0.0F, &m));
}

static void
assert_same_point(vec3f_t expected, vec3f_t actual) {
    TEST_ASSERT_EQUAL_FLOAT(expected.x, actual.x);
    TEST_ASSERT_EQUAL_FLOAT(expected.y, actual.y);
    TEST_ASSERT_EQUAL_FLOAT(expected.z, actual.z);
}

/* The second part meshes into the buffers' upper halves, with scratch of
 * its own, and is appended onto the first: the same triangles, in the same
 * order, at the same points, as the field meshed whole. */
static void
test_parts_appended_make_the_whole_surface(void) {
    fixture_begin();
    iso_mesh_t whole = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 0.0F, &whole));
    const int triangle_count = whole.triangle_count;
    vec3f_t(*expected)[3] = malloc(sizeof(vec3f_t[3]) * (size_t)triangle_count);
    int32_t* second_scratch = malloc(sizeof(int32_t) * iso_surface_scratch_count(SAMPLES, SAMPLES));
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(second_scratch);
    for (int t = 0; t < triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            expected[t][k] = positions[triangles[t][k]];
        }
    }
    iso_mesh_t first = mesh(MAX_VERTICES / 2, MAX_TRIANGLES / 2);
    iso_mesh_t second = {&positions[MAX_VERTICES / 2],
                         &normals[MAX_VERTICES / 2],
                         MAX_VERTICES / 2,
                         &triangles[MAX_TRIANGLES / 2],
                         MAX_TRIANGLES / 2,
                         second_scratch,
                         0,
                         0};
    TEST_ASSERT_TRUE(iso_surface_nets_slabs(&unit, 0.0F, 0, SPLIT_SLAB, &first));
    TEST_ASSERT_TRUE(iso_surface_nets_slabs(&unit, 0.0F, SPLIT_SLAB, SAMPLES - 1, &second));
    first.vertex_capacity = MAX_VERTICES;
    first.triangle_capacity = MAX_TRIANGLES;
    TEST_ASSERT_TRUE(iso_surface_append(&first, &second));
    TEST_ASSERT_EQUAL_INT(triangle_count, first.triangle_count);
    for (int t = 0; t < triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            assert_same_point(expected[t][k], positions[triangles[t][k]]);
        }
    }
    free(expected);
    free(second_scratch);
}

static void
test_an_append_that_does_not_fit_moves_nothing(void) {
    fixture_begin();
    iso_mesh_t into = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 0.0F, &into));
    const int vertex_count = into.vertex_count;
    const int triangle_count = into.triangle_count;
    into.vertex_capacity = vertex_count;
    TEST_ASSERT_FALSE(iso_surface_append(&into, &into));
    TEST_ASSERT_EQUAL_INT(vertex_count, into.vertex_count);
    TEST_ASSERT_EQUAL_INT(triangle_count, into.triangle_count);
}

static void
test_slabs_outside_the_field_mesh_nothing(void) {
    fixture_begin();
    iso_mesh_t m = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_FALSE(iso_surface_nets_slabs(&unit, 0.0F, -1, SPLIT_SLAB, &m));
    TEST_ASSERT_FALSE(iso_surface_nets_slabs(&unit, 0.0F, SPLIT_SLAB, SAMPLES, &m));
    TEST_ASSERT_FALSE(iso_surface_nets_slabs(&unit, 0.0F, SPLIT_SLAB, SPLIT_SLAB, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
}

void
suite_iso_surface(void) {
    RUN_TEST(test_a_sphere_comes_back_on_the_sphere);
    RUN_TEST(test_the_sphere_is_closed_and_wound_outward);
    RUN_TEST(test_origin_and_step_place_the_surface);
    RUN_TEST(test_a_full_buffer_keeps_whole_triangles);
    RUN_TEST(test_a_field_wholly_inside_or_outside_has_no_surface);
    RUN_TEST(test_parts_appended_make_the_whole_surface);
    RUN_TEST(test_an_append_that_does_not_fit_moves_nothing);
    RUN_TEST(test_slabs_outside_the_field_mesh_nothing);
}

SUITE_REGISTER(suite_iso_surface);

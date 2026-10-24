/*
 * Portable suite: iso_surface.h, surface nets. A sphere's field must come
 * back as a closed surface on the sphere, wound and lit outward, wherever
 * the field is placed; a full buffer must leave whole triangles.
 */

#include <stdlib.h>

#include "suites.h"
#include "test_cleanup.h"
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

/* Every triangle faces out, and every edge is shared by exactly two: the
 * surface is closed and wound one way. */
static void
test_the_sphere_is_closed_and_wound_outward(void) {
    fixture_begin();
    iso_mesh_t m = mesh(MAX_VERTICES, MAX_TRIANGLES);
    TEST_ASSERT_TRUE(iso_surface_nets(&unit, 0.0F, &m));
    int balance = 0;
    for (int t = 0; t < m.triangle_count; t++) {
        const vec3f_t a = positions[triangles[t][0]];
        const vec3f_t b = positions[triangles[t][1]];
        const vec3f_t c = positions[triangles[t][2]];
        const vec3f_t facing = vec3f_cross(vec3f_sub(b, a), vec3f_sub(c, a));
        const vec3f_t centroid = vec3f_scale(vec3f_add(vec3f_add(a, b), c), 1.0F / 3.0F);
        TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, vec3f_dot(facing, vec3f_sub(centroid, (vec3f_t){CENTRE, CENTRE, CENTRE})));
        /* Each directed edge must meet its reverse once: count them signed. */
        for (int e = 0; e < 3; e++) {
            const int from = triangles[t][e];
            const int to = triangles[t][(e + 1) % 3];
            int reverse = 0;
            for (int u = 0; u < m.triangle_count; u++) {
                for (int k = 0; k < 3; k++) {
                    reverse += triangles[u][k] == to && triangles[u][(k + 1) % 3] == from ? 1 : 0;
                }
            }
            TEST_ASSERT_EQUAL_INT(1, reverse);
            balance++;
        }
    }
    TEST_ASSERT_EQUAL_INT(3 * m.triangle_count, balance);
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

void
suite_iso_surface(void) {
    RUN_TEST(test_a_sphere_comes_back_on_the_sphere);
    RUN_TEST(test_the_sphere_is_closed_and_wound_outward);
    RUN_TEST(test_origin_and_step_place_the_surface);
    RUN_TEST(test_a_full_buffer_keeps_whole_triangles);
    RUN_TEST(test_a_field_wholly_inside_or_outside_has_no_surface);
}

SUITE_REGISTER(suite_iso_surface);

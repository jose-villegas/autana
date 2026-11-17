/*
 * Portable suite: primitive_mesh.h. Each shape must make the counts its
 * header names, lie on its shape, wind every triangle outward with area,
 * close up where it is a solid (every edge met once each way once its
 * repeated vertices are welded), and write nothing when its parameters are
 * out of range or its buffers short.
 */

#include <math.h>
#include <stdlib.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "math/geometry/primitive_mesh.h"

/* Room for the largest shape a test makes. */
#define MAX_VERTICES  2048
#define MAX_TRIANGLES 4096
/* Tessellations the tests make: a few of each, the smallest included. */
#define MOST_CUTS     6
/* A float's rounding on a unit shape; vertices this close are one point. */
#define ON_SHAPE      1e-5F

static vec3f_t* positions;
static uint16_t (*triangles)[3];
static int* welded;

static void
fixture_end(void) {
    free(positions);
    free(triangles);
    free(welded);
    positions = NULL;
    triangles = NULL;
    welded = NULL;
}

static primitive_mesh_t
fixture_begin(int vertex_capacity, int triangle_capacity) {
    suite_add_test_cleanup(fixture_end);
    positions = malloc(sizeof(vec3f_t) * MAX_VERTICES);
    triangles = malloc(sizeof(uint16_t[3]) * MAX_TRIANGLES);
    welded = malloc(sizeof(int) * MAX_VERTICES);
    TEST_ASSERT_NOT_NULL(positions);
    TEST_ASSERT_NOT_NULL(triangles);
    TEST_ASSERT_NOT_NULL(welded);
    return (primitive_mesh_t){positions, vertex_capacity, triangles, triangle_capacity, 0, 0};
}

static primitive_mesh_t
fixture_full(void) {
    return fixture_begin(MAX_VERTICES, MAX_TRIANGLES);
}

static vec3f_t
corner(const primitive_mesh_t* m, int t, int k) {
    return m->positions[m->triangles[t][k]];
}

/* (b - a) x (c - a) of triangle `t`. */
static vec3f_t
face_normal(const primitive_mesh_t* m, int t) {
    const vec3f_t a = corner(m, t, 0);
    return vec3f_cross(vec3f_sub(corner(m, t, 1), a), vec3f_sub(corner(m, t, 2), a));
}

static void
assert_counts_and_indices(const primitive_mesh_t* m, primitive_counts_t counts) {
    TEST_ASSERT_EQUAL_INT(counts.vertices, m->vertex_count);
    TEST_ASSERT_EQUAL_INT(counts.triangles, m->triangle_count);
    for (int t = 0; t < m->triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            TEST_ASSERT_LESS_THAN_INT(m->vertex_count, m->triangles[t][k]);
        }
    }
}

/* Outward from the origin, which every solid here holds: each triangle's
 * normal points the way its own centroid lies, with area. */
static void
assert_wound_outward(const primitive_mesh_t* m) {
    for (int t = 0; t < m->triangle_count; t++) {
        const vec3f_t centroid = vec3f_add(corner(m, t, 0), vec3f_add(corner(m, t, 1), corner(m, t, 2)));
        TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, vec3f_dot(face_normal(m, t), centroid));
    }
}

/* Each vertex's first twin within ON_SHAPE: the point it stands for. */
static void
weld(const primitive_mesh_t* m) {
    for (int v = 0; v < m->vertex_count; v++) {
        welded[v] = v;
        for (int w = 0; w < v; w++) {
            const vec3f_t d = vec3f_sub(m->positions[v], m->positions[w]);
            if (fabsf(d.x) <= ON_SHAPE && fabsf(d.y) <= ON_SHAPE && fabsf(d.z) <= ON_SHAPE) {
                welded[v] = welded[w];
                break;
            }
        }
    }
}

/* How many triangles hold the welded edge from `a` to `b`, that way round. */
static int
edge_uses(const primitive_mesh_t* m, int a, int b) {
    int uses = 0;
    for (int t = 0; t < m->triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            uses += welded[m->triangles[t][k]] == a && welded[m->triangles[t][(k + 1) % 3]] == b;
        }
    }
    return uses;
}

/* Closed and consistently wound: once welded, every edge is used once each way. */
static void
assert_closed(const primitive_mesh_t* m) {
    weld(m);
    for (int t = 0; t < m->triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            const int a = welded[m->triangles[t][k]];
            const int b = welded[m->triangles[t][(k + 1) % 3]];
            TEST_ASSERT_NOT_EQUAL_INT(a, b);
            TEST_ASSERT_EQUAL_INT(1, edge_uses(m, a, b));
            TEST_ASSERT_EQUAL_INT(1, edge_uses(m, b, a));
        }
    }
}

/* The volume a closed outward mesh encloses, by the divergence theorem. */
static float
enclosed_volume(const primitive_mesh_t* m) {
    float six_times = 0.0F;
    for (int t = 0; t < m->triangle_count; t++) {
        six_times += vec3f_dot(corner(m, t, 0), vec3f_cross(corner(m, t, 1), corner(m, t, 2)));
    }
    return six_times / 6.0F;
}

static void
test_a_plane_covers_the_unit_square_facing_plus_z(void) {
    primitive_mesh_t m = fixture_full();
    for (int n = 1; n <= MOST_CUTS; n++) {
        const primitive_plane_t plane = {.subdivisions = n};
        TEST_ASSERT_TRUE(primitive_plane(&plane, &m));
        assert_counts_and_indices(&m, primitive_plane_counts(&plane));
        TEST_ASSERT_EQUAL_INT(2 * n * n, m.triangle_count);
        for (int v = 0; v < m.vertex_count; v++) {
            TEST_ASSERT_TRUE(fabsf(m.positions[v].x) <= 0.5F && fabsf(m.positions[v].y) <= 0.5F);
            TEST_ASSERT_EQUAL_FLOAT(0.0F, m.positions[v].z);
        }
        /* Every triangle faces +z, and together they cover the square once. */
        float area = 0.0F;
        for (int t = 0; t < m.triangle_count; t++) {
            const vec3f_t normal = face_normal(&m, t);
            TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, normal.z);
            area += 0.5F * normal.z;
        }
        TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, 1.0F, area);
    }
}

static void
test_a_sphere_is_closed_on_the_unit_sphere_wound_outward(void) {
    primitive_mesh_t m = fixture_full();
    for (int rings = PRIMITIVE_SPHERE_RINGS_MIN; rings <= MOST_CUTS; rings++) {
        for (int segments = PRIMITIVE_SEGMENTS_MIN; segments <= MOST_CUTS; segments++) {
            const primitive_sphere_t sphere = {.segments = segments, .rings = rings};
            TEST_ASSERT_TRUE(primitive_sphere(&sphere, &m));
            assert_counts_and_indices(&m, primitive_sphere_counts(&sphere));
            for (int v = 0; v < m.vertex_count; v++) {
                TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, 1.0F, sqrtf(vec3f_dot(m.positions[v], m.positions[v])));
            }
            assert_wound_outward(&m);
            assert_closed(&m);
        }
    }
}

/* Each face a plane on the unit cube's surface, together enclosing exactly its volume. */
static void
test_a_cube_is_closed_on_the_unit_cube_wound_outward(void) {
    primitive_mesh_t m = fixture_full();
    for (int n = 1; n <= MOST_CUTS; n++) {
        const primitive_cube_t cube = {.subdivisions = n};
        TEST_ASSERT_TRUE(primitive_cube(&cube, &m));
        assert_counts_and_indices(&m, primitive_cube_counts(&cube));
        for (int v = 0; v < m.vertex_count; v++) {
            const vec3f_t p = m.positions[v];
            TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, 0.5F, fmaxf(fabsf(p.x), fmaxf(fabsf(p.y), fabsf(p.z))));
        }
        assert_wound_outward(&m);
        assert_closed(&m);
        TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, 1.0F, enclosed_volume(&m));
    }
}

/* Every vertex `radius` from the segment joining the hemispheres' centres. */
static void
test_a_capsule_is_closed_round_its_axis_wound_outward(void) {
    primitive_mesh_t m = fixture_full();
    const float radius = 0.5F;
    const float height = 1.5F;
    for (int rings = 1; rings <= MOST_CUTS; rings++) {
        const primitive_capsule_t capsule = {.radius = radius, .height = height, .segments = MOST_CUTS, .rings = rings};
        TEST_ASSERT_TRUE(primitive_capsule(&capsule, &m));
        assert_counts_and_indices(&m, primitive_capsule_counts(&capsule));
        for (int v = 0; v < m.vertex_count; v++) {
            const vec3f_t p = m.positions[v];
            const float on_axis = fmaxf(-0.5F * height, fminf(0.5F * height, p.y));
            const vec3f_t off = {p.x, p.y - on_axis, p.z};
            TEST_ASSERT_FLOAT_WITHIN(ON_SHAPE, radius, sqrtf(vec3f_dot(off, off)));
        }
        assert_wound_outward(&m);
        assert_closed(&m);
    }
}

/* A cylinder, a frustum and a cone each way up: closed, an end of radius 0 left open as an apex. */
static void
test_a_cylinder_or_cone_is_closed_wound_outward(void) {
    primitive_mesh_t m = fixture_full();
    const float radii[][2] = {{0.5F, 0.5F}, {0.25F, 0.75F}, {0.0F, 0.5F}, {0.5F, 0.0F}};
    for (size_t i = 0; i < sizeof radii / sizeof radii[0]; i++) {
        for (int stacks = 1; stacks <= MOST_CUTS; stacks++) {
            const primitive_cylinder_t cylinder = {.top_radius = radii[i][0],
                                                   .bottom_radius = radii[i][1],
                                                   .height = 2.0F,
                                                   .segments = MOST_CUTS,
                                                   .stacks = stacks};
            TEST_ASSERT_TRUE(primitive_cylinder(&cylinder, &m));
            assert_counts_and_indices(&m, primitive_cylinder_counts(&cylinder));
            for (int v = 0; v < m.vertex_count; v++) {
                TEST_ASSERT_TRUE(fabsf(m.positions[v].y) <= 0.5F * cylinder.height + ON_SHAPE);
            }
            assert_wound_outward(&m);
            assert_closed(&m);
        }
    }
}

static void
test_parameters_out_of_range_write_nothing(void) {
    primitive_mesh_t m = fixture_full();
    TEST_ASSERT_FALSE(primitive_plane(&(primitive_plane_t){.subdivisions = 0}, &m));
    TEST_ASSERT_FALSE(primitive_cube(&(primitive_cube_t){.subdivisions = 0}, &m));
    TEST_ASSERT_FALSE(primitive_sphere(
        &(primitive_sphere_t){.segments = PRIMITIVE_SEGMENTS_MIN - 1, .rings = PRIMITIVE_SPHERE_RINGS_MIN}, &m));
    TEST_ASSERT_FALSE(primitive_sphere(
        &(primitive_sphere_t){.segments = PRIMITIVE_SEGMENTS_MIN, .rings = PRIMITIVE_SPHERE_RINGS_MIN - 1}, &m));
    TEST_ASSERT_FALSE(primitive_capsule(
        &(primitive_capsule_t){.radius = 0.0F, .height = 1.0F, .segments = PRIMITIVE_SEGMENTS_MIN, .rings = 1}, &m));
    TEST_ASSERT_FALSE(primitive_capsule(
        &(primitive_capsule_t){.radius = 1.0F, .height = -1.0F, .segments = PRIMITIVE_SEGMENTS_MIN, .rings = 1}, &m));
    TEST_ASSERT_FALSE(primitive_capsule(
        &(primitive_capsule_t){.radius = 1.0F, .height = 1.0F, .segments = PRIMITIVE_SEGMENTS_MIN, .rings = 0}, &m));
    const primitive_cylinder_t no_ends = {
        .top_radius = 0.0F, .bottom_radius = 0.0F, .height = 1.0F, .segments = PRIMITIVE_SEGMENTS_MIN, .stacks = 1};
    TEST_ASSERT_FALSE(primitive_cylinder(&no_ends, &m));
    const primitive_cylinder_t flat = {
        .top_radius = 1.0F, .bottom_radius = 1.0F, .height = 0.0F, .segments = PRIMITIVE_SEGMENTS_MIN, .stacks = 1};
    TEST_ASSERT_FALSE(primitive_cylinder(&flat, &m));
    const primitive_cylinder_t inside_out = {
        .top_radius = -1.0F, .bottom_radius = 1.0F, .height = 1.0F, .segments = PRIMITIVE_SEGMENTS_MIN, .stacks = 1};
    TEST_ASSERT_FALSE(primitive_cylinder(&inside_out, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
    TEST_ASSERT_EQUAL_INT(0, m.triangle_count);
}

static void
test_short_buffers_write_nothing(void) {
    const primitive_sphere_t sphere = {.segments = MOST_CUTS, .rings = MOST_CUTS};
    primitive_mesh_t m = fixture_begin(primitive_sphere_counts(&sphere).vertices - 1, MAX_TRIANGLES);
    TEST_ASSERT_FALSE(primitive_sphere(&sphere, &m));
    const primitive_cube_t cube = {.subdivisions = MOST_CUTS};
    m = (primitive_mesh_t){positions, MAX_VERTICES, triangles, primitive_cube_counts(&cube).triangles - 1, 0, 0};
    TEST_ASSERT_FALSE(primitive_cube(&cube, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
    TEST_ASSERT_EQUAL_INT(0, m.triangle_count);
}

/* A shape past what a uint16 index reaches is refused whatever room the
 * caller gives, and so is one whose counts would overflow an int. */
static void
test_a_shape_past_uint16_indices_is_refused(void) {
    enum { SUBDIVISIONS_PAST_UINT16 = 256 }; /* 257 * 257 vertices */

    primitive_mesh_t m = {NULL, INT32_MAX, NULL, INT32_MAX, 0, 0};
    const primitive_plane_t past = {.subdivisions = SUBDIVISIONS_PAST_UINT16};
    TEST_ASSERT_GREATER_THAN_INT(UINT16_MAX + 1, primitive_plane_counts(&past).vertices);
    TEST_ASSERT_FALSE(primitive_plane(&past, &m));
    TEST_ASSERT_FALSE(primitive_plane(&(primitive_plane_t){.subdivisions = UINT16_MAX}, &m));
    TEST_ASSERT_FALSE(primitive_sphere(&(primitive_sphere_t){.segments = INT32_MAX / 2, .rings = INT32_MAX / 2}, &m));
    TEST_ASSERT_FALSE(primitive_cube(&(primitive_cube_t){.subdivisions = INT32_MAX - 1}, &m));
    TEST_ASSERT_EQUAL_INT(0, m.vertex_count);
}

void
suite_primitive_mesh(void) {
    RUN_TEST(test_a_plane_covers_the_unit_square_facing_plus_z);
    RUN_TEST(test_a_sphere_is_closed_on_the_unit_sphere_wound_outward);
    RUN_TEST(test_a_cube_is_closed_on_the_unit_cube_wound_outward);
    RUN_TEST(test_a_capsule_is_closed_round_its_axis_wound_outward);
    RUN_TEST(test_a_cylinder_or_cone_is_closed_wound_outward);
    RUN_TEST(test_parameters_out_of_range_write_nothing);
    RUN_TEST(test_short_buffers_write_nothing);
    RUN_TEST(test_a_shape_past_uint16_indices_is_refused);
}

SUITE_REGISTER(suite_primitive_mesh);

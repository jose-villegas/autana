/*
 * Portable suite: stress_preset.h. Every preset's mesh must be whole and
 * repeat byte for byte, fit a dynamic mesh, and move only when the preset
 * says so; and each preset must sit in the corner it is named for, by the
 * renderer's own limits at the size render_lab draws at.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/stress/stress_preset.h"
#include "gfx/gfx.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render/r3d_span.h"

/* Any framing: a preset is laid out in proportion to it. */
static const stress_framing_t framing = {10.0F, 0.5F};

/* The picture's short side at render_lab's default render scale. */
#define DEFAULT_SHORT_PX (GFX_WIDTH * RENDER_LAB_SCALE_UNIT / RENDER_LAB_SCALE_DEFAULT)

/* A per-frame wave's quarter period in: far enough along that every vertex moves. */
#define WAVE_LATER_MS    500

typedef struct {
    stress_buffers_t at[2];
    void* mesh_block;
} fixture_t;

static fixture_t f;

static void
fixture_end(void) {
    for (int i = 0; i < 2; i++) {
        free(f.at[i].positions);
        free(f.at[i].colors);
        free(f.at[i].triangles);
    }
    free(f.mesh_block);
    memset(&f, 0, sizeof f);
}

static void
fixture_begin(const stress_params_t* p) {
    suite_add_test_cleanup(fixture_end);
    const size_t vertices = (size_t)stress_vertex_count(p);
    const size_t triangles = (size_t)stress_triangle_count(p);
    for (int i = 0; i < 2; i++) {
        f.at[i] = (stress_buffers_t){malloc(sizeof(vec3f_t) * vertices), malloc(sizeof(uint8_t[3]) * vertices),
                                     malloc(sizeof(uint16_t[3]) * triangles)};
        TEST_ASSERT_NOT_NULL(f.at[i].positions);
        TEST_ASSERT_NOT_NULL(f.at[i].colors);
        TEST_ASSERT_NOT_NULL(f.at[i].triangles);
    }
}

static void
release(void) {
    suite_run_test_cleanup();
}

static bool
same_source(const r3d_dynamic_source_t* a, const r3d_dynamic_source_t* b) {
    return a->vertex_count == b->vertex_count && a->triangle_count == b->triangle_count
           && memcmp(a->positions, b->positions, sizeof(vec3f_t) * (size_t)a->vertex_count) == 0
           && memcmp(a->colors, b->colors, sizeof(uint8_t[3]) * (size_t)a->vertex_count) == 0
           && memcmp(a->triangles, b->triangles, sizeof(uint16_t[3]) * (size_t)a->triangle_count) == 0;
}

static void
test_every_preset_mesh_is_whole_and_repeats(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        const stress_params_t* p = &stress_presets[i];
        fixture_begin(p);
        r3d_dynamic_source_t a, b;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &a));
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[1], &b));
        TEST_ASSERT_EQUAL_INT(stress_vertex_count(p), a.vertex_count);
        TEST_ASSERT_EQUAL_INT(stress_triangle_count(p), a.triangle_count);
        TEST_ASSERT_TRUE(same_source(&a, &b));
        release();
    }
}

/* One dynamic mesh holds a copy, in as many clusters as its triangles fill. */
static void
test_every_preset_mesh_fits_a_dynamic_mesh(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        const stress_params_t* p = &stress_presets[i];
        fixture_begin(p);
        r3d_dynamic_source_t source;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
        const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(source.vertex_count, source.triangle_count);
        const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
        f.mesh_block = malloc(bytes);
        TEST_ASSERT_NOT_NULL(f.mesh_block);
        r3d_dynamic_mesh_t mesh;
        TEST_ASSERT_TRUE(r3d_dynamic_mesh_init(&mesh, f.mesh_block, bytes, capacity, STRESS_TICKS_PER_UNIT));
        TEST_ASSERT_TRUE(r3d_dynamic_mesh_build(&mesh, &source));
        const int clusters =
            (source.triangle_count + R3D_DYNAMIC_CLUSTER_TRIANGLES - 1) / R3D_DYNAMIC_CLUSTER_TRIANGLES;
        TEST_ASSERT_EQUAL_INT(clusters, mesh.built_clusters);
        release();
    }
}

/* A per-frame preset's mesh moves with time; a still one's never does. */
static void
test_only_a_per_frame_preset_moves(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        const stress_params_t* p = &stress_presets[i];
        fixture_begin(p);
        r3d_dynamic_source_t first, later;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &first));
        TEST_ASSERT_TRUE(stress_source(p, WAVE_LATER_MS, &f.at[1], &later));
        TEST_ASSERT_EQUAL(!p->per_frame, same_source(&first, &later));
        release();
    }
    TEST_ASSERT_TRUE(stress_presets[STRESS_PER_FRAME].per_frame);
}

/* A copy's square on, in the plane it stands in: its centre and half width. */
typedef struct {
    float x, y, z, half;
} copy_box_t;

static copy_box_t
copy_box(const stress_params_t* p, int copy) {
    const transformf_t place = stress_copy_place(p, copy, framing);
    const float unit_half = p->shape == STRESS_SHAPE_GRID ? 0.5F : 1.0F;
    return (copy_box_t){place.position.x, place.position.y, place.position.z, unit_half * place.scale.x};
}

static bool
apart(copy_box_t a, copy_box_t b) {
    return fabsf(a.x - b.x) >= a.half + b.half || fabsf(a.y - b.y) >= a.half + b.half;
}

/* Open: copies side by side, none over another and all in view, so there is nothing to hide or cull. */
static void
test_open_has_nothing_to_hide_or_cull(void) {
    const stress_params_t* p = &stress_presets[STRESS_OPEN];
    const float seen = framing.distance * framing.half_fov_short_tan;
    for (int i = 0; i < p->copies; i++) {
        const copy_box_t a = copy_box(p, i);
        TEST_ASSERT_EQUAL_FLOAT(0.0F, a.z);
        TEST_ASSERT_TRUE(fabsf(a.x) + a.half <= seen && fabsf(a.y) + a.half <= seen);
        for (int j = i + 1; j < p->copies; j++) {
            TEST_ASSERT_TRUE(apart(a, copy_box(p, j)));
        }
    }
}

/* Stack: every copy the same square, each further from the camera: depth complexity is the copy count. */
static void
test_stack_puts_every_copy_behind_the_last(void) {
    const stress_params_t* p = &stress_presets[STRESS_STACK];
    TEST_ASSERT_GREATER_THAN_INT(1, p->copies);
    const copy_box_t front = copy_box(p, 0);
    for (int i = 1; i < p->copies; i++) {
        const copy_box_t b = copy_box(p, i);
        TEST_ASSERT_EQUAL_FLOAT(front.x, b.x);
        TEST_ASSERT_EQUAL_FLOAT(front.y, b.y);
        TEST_ASSERT_EQUAL_FLOAT(front.half, b.half);
        TEST_ASSERT_LESS_THAN_FLOAT(copy_box(p, i - 1).z, b.z);
    }
}

/* Tiny copies: many copies whose triangles are small enough to be filled flat at the default size. */
static void
test_tiny_copies_are_many_and_drawn_flat(void) {
    const stress_params_t* p = &stress_presets[STRESS_TINY_COPIES];
    TEST_ASSERT_GREATER_THAN_INT(stress_presets[STRESS_OPEN].copies, p->copies);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT((float)R3D_SPAN_SMALL_MAX_SIDE, stress_cut_px(p, DEFAULT_SHORT_PX));
}

/* Big tris: one copy of a few triangles, each past the largest small-fill ceiling. */
static void
test_big_tris_are_past_every_small_fill(void) {
    const stress_params_t* p = &stress_presets[STRESS_BIG_TRIS];
    TEST_ASSERT_EQUAL_INT(1, p->copies);
    TEST_ASSERT_GREATER_OR_EQUAL_FLOAT((float)SMALL_FILL_CEILING_MAX, stress_cut_px(p, DEFAULT_SHORT_PX));
}

void
run_stress_preset_suite(void) {
    RUN_TEST(test_every_preset_mesh_is_whole_and_repeats);
    RUN_TEST(test_every_preset_mesh_fits_a_dynamic_mesh);
    RUN_TEST(test_only_a_per_frame_preset_moves);
    RUN_TEST(test_open_has_nothing_to_hide_or_cull);
    RUN_TEST(test_stack_puts_every_copy_behind_the_last);
    RUN_TEST(test_tiny_copies_are_many_and_drawn_flat);
    RUN_TEST(test_big_tris_are_past_every_small_fill);
}

SUITE_REGISTER(run_stress_preset_suite);

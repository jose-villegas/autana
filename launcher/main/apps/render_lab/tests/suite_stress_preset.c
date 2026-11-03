/*
 * Portable suite: stress_preset.h. Every preset's mesh must be whole and
 * repeat byte for byte, fit a dynamic mesh, and move only when the preset
 * says so; and each preset must sit in the corner it is named for, by the
 * renderer's own limits at the size render_lab draws at.
 */

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/stress/stress_preset.h"
#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render/r3d_span.h"

/* Any framing: a preset is laid out in proportion to it. */
static const stress_framing_t framing = {10.0F, 0.5F};
/* Another, nearer and wider, to hold a layout to that proportion. */
static const stress_framing_t other_framing = {3.0F, 1.25F};

/* The picture's short side at render_lab's default render scale. */
#define DEFAULT_SHORT_PX (GFX_WIDTH * RENDER_LAB_SCALE_UNIT / RENDER_LAB_SCALE_DEFAULT)

/* A per-frame wave's quarter period in: far enough along that every vertex moves. */
#define WAVE_LATER_MS    500
/* Steps through a wave period and past its wrap, each a frame's time. */
#define WAVE_STEP_MS     16
#define WAVE_STEPS       160

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

/* A per-frame preset's mesh fits its dynamic mesh at every step of its wave, past the period's wrap. */
static void
test_a_per_frame_mesh_fits_all_through_its_wave(void) {
    const stress_params_t* p = &stress_presets[STRESS_PER_FRAME];
    fixture_begin(p);
    const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(stress_vertex_count(p), stress_triangle_count(p));
    const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
    f.mesh_block = malloc(bytes);
    TEST_ASSERT_NOT_NULL(f.mesh_block);
    r3d_dynamic_mesh_t mesh;
    TEST_ASSERT_TRUE(r3d_dynamic_mesh_init(&mesh, f.mesh_block, bytes, capacity, STRESS_TICKS_PER_UNIT));
    for (int step = 0; step < WAVE_STEPS; step++) {
        r3d_dynamic_source_t source;
        TEST_ASSERT_TRUE(stress_source(p, (uint32_t)(step * WAVE_STEP_MS), &f.at[0], &source));
        TEST_ASSERT_TRUE(r3d_dynamic_mesh_build(&mesh, &source));
    }
    release();
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
        TEST_ASSERT_EQUAL(!stress_per_frame(p), same_source(&first, &later));
        release();
    }
    TEST_ASSERT_TRUE(stress_per_frame(&stress_presets[STRESS_PER_FRAME]));
}

/* A copy's square on, in the plane it stands in: its centre and half width. */
typedef struct {
    float x, y, z, half;
} copy_box_t;

static copy_box_t
copy_box(const stress_params_t* p, int copy) {
    const transformf_t place = stress_copy_place(p, copy, framing);
    const float unit_half = p->shape == STRESS_SHAPE_SPHERE ? 1.0F : 0.5F;
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

/* Every preset fills the same share of the picture however the camera frames it, so its triangles keep their size. */
static void
test_every_layout_scales_with_its_framing(void) {
    const float seen = framing.distance * framing.half_fov_short_tan;
    const float other_seen = other_framing.distance * other_framing.half_fov_short_tan;
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        const stress_params_t* p = &stress_presets[i];
        for (int copy = 0; copy < p->copies; copy++) {
            const transformf_t a = stress_copy_place(p, copy, framing);
            const transformf_t b = stress_copy_place(p, copy, other_framing);
            TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON, a.scale.x / seen, b.scale.x / other_seen);
            TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON, a.position.x / seen, b.position.x / other_seen);
            TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON, a.position.y / seen, b.position.y / other_seen);
        }
    }
}

/* A wall whose copies do not fill its last row still keeps every copy apart and in view. */
static void
test_a_ragged_wall_keeps_copies_apart(void) {
    stress_params_t p = stress_presets[STRESS_OPEN];
    p.copies = 5; /* 3 columns, the second row short */
    const float seen = framing.distance * framing.half_fov_short_tan;
    for (int i = 0; i < p.copies; i++) {
        const copy_box_t a = copy_box(&p, i);
        TEST_ASSERT_TRUE(fabsf(a.x) + a.half <= seen && fabsf(a.y) + a.half <= seen);
        for (int j = i + 1; j < p.copies; j++) {
            TEST_ASSERT_TRUE(apart(a, copy_box(&p, j)));
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

/* Two-sided: a closed cube drawn from both sides, so backface culling has
 * half of it facing away and may skip none; only this preset is two-sided. */
static void
test_two_sided_is_a_closed_shape_drawn_from_both_sides(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        const stress_params_t* p = &stress_presets[i];
        fixture_begin(p);
        r3d_dynamic_source_t source;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
        TEST_ASSERT_EQUAL(i == STRESS_TWO_SIDED, source.double_sided);
        release();
    }
    TEST_ASSERT_EQUAL_INT(STRESS_SHAPE_CUBE, stress_presets[STRESS_TWO_SIDED].shape);
}

/* The widest per-channel spread of a triangle's corners, in RGB565 units, as the small-fill gate measures it. */
static int
corner_spread(const r3d_dynamic_source_t* s, int t) {
    unsigned low[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    unsigned high[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        const uint8_t* c = s->colors[s->triangles[t][k]];
        const uint16_t packed = (uint16_t)RGB565(((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
        const unsigned channels[3] = {rgb565_r5(packed), rgb565_g6(packed), rgb565_b5(packed)};
        for (int ch = 0; ch < 3; ch++) {
            low[ch] = channels[ch] < low[ch] ? channels[ch] : low[ch];
            high[ch] = channels[ch] > high[ch] ? channels[ch] : high[ch];
        }
    }
    int widest = 0;
    for (int ch = 0; ch < 3; ch++) {
        widest = (int)(high[ch] - low[ch]) > widest ? (int)(high[ch] - low[ch]) : widest;
    }
    return widest;
}

/* Noisy tiny: slivers big enough for the small-fill gate to test, each
 * spread past the loosest gate, so the test is paid for and always fails. */
static void
test_noisy_tiny_slivers_are_gated_and_fail_the_gate(void) {
    const stress_params_t* p = &stress_presets[STRESS_NOISY_TINY];
    const float cut = stress_cut_px(p, DEFAULT_SHORT_PX);
    TEST_ASSERT_GREATER_THAN_FLOAT((float)R3D_SPAN_SMALL_MAX_SIDE, cut);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT((float)SMALL_FILL_CEILING_FAST, cut);
    fixture_begin(p);
    r3d_dynamic_source_t source;
    TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
    for (int t = 0; t < source.triangle_count; t++) {
        TEST_ASSERT_GREATER_THAN_INT(SMALL_FILL_GATE_FAST, corner_spread(&source, t));
    }
    release();
}

void
run_stress_preset_suite(void) {
    RUN_TEST(test_every_preset_mesh_is_whole_and_repeats);
    RUN_TEST(test_every_preset_mesh_fits_a_dynamic_mesh);
    RUN_TEST(test_a_per_frame_mesh_fits_all_through_its_wave);
    RUN_TEST(test_only_a_per_frame_preset_moves);
    RUN_TEST(test_every_layout_scales_with_its_framing);
    RUN_TEST(test_a_ragged_wall_keeps_copies_apart);
    RUN_TEST(test_open_has_nothing_to_hide_or_cull);
    RUN_TEST(test_stack_puts_every_copy_behind_the_last);
    RUN_TEST(test_tiny_copies_are_many_and_drawn_flat);
    RUN_TEST(test_big_tris_are_past_every_small_fill);
    RUN_TEST(test_two_sided_is_a_closed_shape_drawn_from_both_sides);
    RUN_TEST(test_noisy_tiny_slivers_are_gated_and_fail_the_gate);
}

SUITE_REGISTER(run_stress_preset_suite);

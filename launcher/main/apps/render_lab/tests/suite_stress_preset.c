/*
 * Portable suite: stress_preset.h. Every preset's mesh must be whole and
 * repeat byte for byte, fit a dynamic mesh, and move only when the preset
 * says so; and each preset must sit in the corner it is named for, by the
 * renderer's own limits at the size render_lab draws at.
 */

#include <float.h>
#include <math.h>
#include <string.h>

#include "suites.h"
#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "anim/anim_binding.h"
#include "anim/anim_transform.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/stress/stress_preset.h"
#include "apps/render_lab/tests/r3d_perf_quarters.h"
#include "asset/asset_store.h"
#include "core/memory.h"
#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/math_const.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"
#include "render/view/r3d_pipeline_count.h"

/* Any framing: a preset is laid out in proportion to it. */
static const stress_framing_t framing = {10.0F, 0.5F};
/* Another, nearer and wider, to hold a layout to that proportion. */
static const stress_framing_t other_framing = {3.0F, 1.25F};

/* The picture's short side at render_lab's default render scale. */
#define DEFAULT_SHORT_PX          (GFX_WIDTH * RENDER_LAB_SCALE_UNIT / RENDER_LAB_SCALE_DEFAULT)

/* A per-frame wave's quarter period in: far enough along that every vertex moves. */
#define WAVE_LATER_MS             500
/* Steps through a wave period and past its wrap, each a frame's time. */
#define WAVE_STEP_MS              16
#define WAVE_STEPS                160
/* Buffer sets a test can hold: two builds of one copy to compare. */
#define FIXTURE_SETS              2
#define TEST_NEAR_Z               0.5F
#define STRESS_TEST_MS_PER_SECOND 1000.0F
#define CULLED_SWEEP_MIN          0.05F
#define CULLED_SWEEP_MAX          0.85F
#define NEAR_BEHIND_MIN           100
#define NEAR_CLIPPED_MIN          100

typedef struct {
    stress_buffers_t at[FIXTURE_SETS];
    void* mesh_block;
    r3d_pipeline_vertex_t* projected;
    uint16_t* clusters;
    r3d_pipeline_count_walk_t* walk;
    r3d_dynamic_mesh_t mesh;
    handle_t pack;
    bool pack_loaded;
    anim_node_tracks_t path;
    uint32_t duration_ms;
} fixture_t;

static fixture_t f;

static void
fixture_end(void) {
    for (int i = 0; i < FIXTURE_SETS; i++) {
        memory_free(f.at[i].positions);
        memory_free(f.at[i].colors);
        memory_free(f.at[i].triangles);
    }
    memory_free(f.mesh_block);
    memory_free(f.projected);
    memory_free(f.clusters);
    memory_free(f.walk);
    if (f.pack_loaded) {
        asset_store_release(f.pack);
    }
    memset(&f, 0, sizeof f);
}

/* `sets` buffer sets for preset `p`'s copy, FIXTURE_SETS only where a test
 * compares two builds: outside an app, PSRAM holds one set of the largest
 * mix copy, not two. */
static void
fixture_begin(const stress_params_t* p, int sets) {
    suite_add_test_cleanup(fixture_end);
    const size_t vertices = (size_t)stress_vertex_count(p);
    const size_t triangles = (size_t)stress_triangle_count(p);
    for (int i = 0; i < sets; i++) {
        f.at[i] = (stress_buffers_t){memory_alloc(sizeof(vec3f_t) * vertices, MEMORY_PSRAM),
                                     memory_alloc(sizeof(uint8_t[3]) * vertices, MEMORY_PSRAM),
                                     memory_alloc(sizeof(uint16_t[3]) * triangles, MEMORY_PSRAM)};
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

/* stress_presets fills each ID STRESS_PRESETS names: a missed row would be all zeros. */
static void
test_every_preset_has_its_row(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        TEST_ASSERT_GREATER_THAN_INT(0, stress_presets[i].cuts);
        TEST_ASSERT_GREATER_THAN_INT(0, stress_presets[i].perf_poses);
        TEST_ASSERT_GREATER_THAN_INT(0, stress_presets[i].copies);
        TEST_ASSERT_GREATER_THAN_INT(0, stress_presets[i].coverage_percent);
    }
}

static void
test_every_preset_mesh_is_whole_and_repeats(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        const stress_params_t* p = &stress_presets[i];
        fixture_begin(p, FIXTURE_SETS);
        r3d_dynamic_source_t a;
        r3d_dynamic_source_t b;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &a));
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[1], &b));
        TEST_ASSERT_EQUAL_INT(stress_vertex_count(p), a.vertex_count);
        TEST_ASSERT_EQUAL_INT(stress_triangle_count(p), a.triangle_count);
        for (int t = 0; t < a.triangle_count; t++) {
            for (int k = 0; k < 3; k++) {
                TEST_ASSERT_LESS_THAN_INT(a.vertex_count, a.triangles[t][k]);
            }
        }
        TEST_ASSERT_TRUE(same_source(&a, &b));
        release();
    }
}

/* One dynamic mesh holds a copy, in as many clusters as its triangles fill. */
static void
mesh_fixture(const stress_params_t* p) {
    fixture_begin(p, 1);
    r3d_dynamic_source_t source;
    TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
    const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(source.vertex_count, source.triangle_count);
    const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
    f.mesh_block = memory_alloc(bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(f.mesh_block);
    TEST_ASSERT_TRUE(r3d_dynamic_mesh_init(&f.mesh, f.mesh_block, bytes, capacity, STRESS_TICKS_PER_UNIT));
    TEST_ASSERT_TRUE(r3d_dynamic_mesh_build(&f.mesh, &source));
}

static void
assert_mesh_fits(const stress_params_t* p) {
    mesh_fixture(p);
    const int triangles = stress_triangle_count(p);
    const int clusters = (triangles + R3D_DYNAMIC_CLUSTER_TRIANGLES - 1) / R3D_DYNAMIC_CLUSTER_TRIANGLES;
    TEST_ASSERT_EQUAL_INT(clusters, f.mesh.built_clusters);
    release();
}

static void
test_every_preset_mesh_fits_a_dynamic_mesh(void) {
    for (int i = 0; i < STRESS_PRESET_COUNT; i++) {
        assert_mesh_fits(&stress_presets[i]);
    }
}

/* A per-frame preset's mesh fits its dynamic mesh at every step of its wave, past the period's wrap. */
static void
test_a_per_frame_mesh_fits_all_through_its_wave(void) {
    const stress_params_t* p = &stress_presets[STRESS_PER_FRAME];
    fixture_begin(p, 1);
    const r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for(stress_vertex_count(p), stress_triangle_count(p));
    const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
    f.mesh_block = memory_alloc(bytes, MEMORY_PSRAM);
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
        fixture_begin(p, FIXTURE_SETS);
        r3d_dynamic_source_t first;
        r3d_dynamic_source_t later;
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
        fixture_begin(p, 1);
        r3d_dynamic_source_t source;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
        TEST_ASSERT_EQUAL(i == STRESS_TWO_SIDED, source.double_sided);
        release();
    }
    TEST_ASSERT_EQUAL_INT(STRESS_SHAPE_CUBE, stress_presets[STRESS_TWO_SIDED].shape);
}

/* The widest per-channel spread of a triangle's corners, in RGB565 units, packed and measured as the small-fill
 * gate does. */
static int
corner_spread(const r3d_dynamic_source_t* s, int t) {
    unsigned low[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    unsigned high[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        const uint8_t* c = s->colors[s->triangles[t][k]];
        const uint16_t packed = rgb565_swap(r3d_span_pack_rgb(c[0], c[1], c[2]));
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

/* Noisy tiny: slivers the small-fill gate tests, each spread past the
 * gate at every side a sliver can take, so the test is paid for and always
 * fails. A fan's slivers run from its centre to two rim points
 * 2pi/segments apart, so the narrowest, at 45 degrees, is still
 * cos(pi/4 - pi/segments) of the radius across. */
static void
test_noisy_tiny_slivers_are_gated_and_fail_the_gate(void) {
    const stress_params_t* p = &stress_presets[STRESS_NOISY_TINY];
    const float radius = stress_cut_px(p, DEFAULT_SHORT_PX);
    const int widest = (int)ceilf(radius);
    const int narrowest = (int)floorf(radius * cosf((MATH_PI / 4.0F) - (MATH_PI / (float)p->cuts)));
    r3d_span_small_fill_t gate;
    r3d_span_small_fill_resolve(&gate, SMALL_FILL_FAST);
    TEST_ASSERT_GREATER_THAN_INT(R3D_SPAN_SMALL_MAX_SIDE, narrowest);
    TEST_ASSERT_LESS_OR_EQUAL_INT(gate.ceiling, widest);
    fixture_begin(p, 1);
    r3d_dynamic_source_t source;
    TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
    for (int t = 0; t < source.triangle_count; t++) {
        for (int side = narrowest; side <= widest; side++) {
            TEST_ASSERT_GREATER_THAN_INT(gate.spread[side], corner_spread(&source, t));
        }
    }
    release();
}

/* Every mix copy alternates uniform and stepped triangles. The scene builds
 * the copies' meshes from its own arena, which run_stress_perf_suite draws. */
static void
test_mixed_copies_split_colour_modes(void) {
    for (int copy = 0; copy < STRESS_MIX_LEVELS; copy++) {
        const stress_params_t p = stress_copy_params(&stress_presets[STRESS_MIX], copy);
        fixture_begin(&p, 1);
        r3d_dynamic_source_t source;
        TEST_ASSERT_TRUE(stress_source(&p, 0, &f.at[0], &source));
        int uniform = 0;
        for (int t = 0; t < source.triangle_count; t++) {
            const uint16_t* tri = source.triangles[t];
            uniform += memcmp(source.colors[tri[0]], source.colors[tri[1]], sizeof source.colors[0]) == 0
                       && memcmp(source.colors[tri[0]], source.colors[tri[2]], sizeof source.colors[0]) == 0;
        }
        TEST_ASSERT_EQUAL_INT(source.triangle_count / STRESS_MIX_UNIFORM_PERIOD, uniform);
        release();
    }
}

static void
test_uniform_presets_have_equal_corner_colours_and_large_coverage(void) {
    const stress_preset_t presets[] = {STRESS_UNIFORM_MID, STRESS_UNIFORM_FULL};
    for (int i = 0; i < (int)(sizeof presets / sizeof presets[0]); i++) {
        const stress_params_t* p = &stress_presets[presets[i]];
        fixture_begin(p, 1);
        r3d_dynamic_source_t source;
        TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
        for (int t = 0; t < source.triangle_count; t++) {
            TEST_ASSERT_EQUAL_INT(0, corner_spread(&source, t));
        }
        TEST_ASSERT_EQUAL_INT(1, p->copies);
        TEST_ASSERT_EQUAL_INT(STRESS_LAYOUT_STACK, p->layout);
        TEST_ASSERT_GREATER_THAN_FLOAT((float)SMALL_FILL_CEILING_MAX, stress_cut_px(p, DEFAULT_SHORT_PX));
        release();
    }
    TEST_ASSERT_GREATER_THAN_INT(stress_presets[STRESS_UNIFORM_MID].coverage_percent,
                                 stress_presets[STRESS_UNIFORM_FULL].coverage_percent);
}

static void
counted_fixture(const stress_params_t* p) {
    mesh_fixture(p);
    f.projected = memory_alloc(sizeof *f.projected * (size_t)f.mesh.mesh.vertex_count, MEMORY_PSRAM);
    f.clusters = memory_alloc(sizeof *f.clusters * (size_t)f.mesh.mesh.cluster_count, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(f.projected);
    TEST_ASSERT_NOT_NULL(f.clusters);
    f.walk = memory_alloc(sizeof *f.walk, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(f.walk);
}

static r3d_pipeline_counts_t
count_copies(const stress_params_t* p, const render_view_t* view) {
    r3d_pipeline_counts_t counts = {0};
    for (int copy = 0; copy < p->copies; copy++) {
        transformf_t place = stress_copy_place(p, copy, framing);
        const mat4f_t model = transformf_matrix(&place);
        r3d_lens_t lens;
        r3d_lens_init(&lens, view, STRESS_TICKS_PER_UNIT);
        r3d_lens_place(&lens, &model, STRESS_TICKS_PER_UNIT);
        const int kept = r3d_pipeline_cull_counted(&f.mesh.mesh, &lens, NULL, f.clusters, f.walk, &counts);
        r3d_pipeline_transform(&f.mesh.mesh, &lens, f.clusters, kept, f.projected, NULL, false);
        r3d_pipeline_count_triangles(&f.mesh.mesh, &lens, f.clusters, kept, f.projected, &counts, NULL);
    }
    return counts;
}

static void
test_card_has_holes_colour_steps_and_scene_size_bins(void) {
    const stress_params_t* p = &stress_presets[STRESS_CARD];
    counted_fixture(p);
    TEST_ASSERT_LESS_THAN_INT(2 * p->cuts * p->cuts, stress_triangle_count(p));
    r3d_dynamic_source_t source;
    TEST_ASSERT_TRUE(stress_source(p, 0, &f.at[0], &source));
    int stepped = 0;
    for (int t = 0; t < source.triangle_count; t++) {
        stepped += corner_spread(&source, t) > SMALL_FILL_GATE_FAST;
    }
    TEST_ASSERT_GREATER_THAN_INT(source.triangle_count / 2, stepped);
    const transformf_t pose = transformf_looking((vec3f_t){0, 0, framing.distance}, vec3f_back(), vec3f_unit_y());
    const render_view_t view =
        render_view_make(&pose, framing.half_fov_short_tan, TEST_NEAR_Z,
                         (viewport_t){DEFAULT_SHORT_PX, GFX_HEIGHT * RENDER_LAB_SCALE_UNIT / RENDER_LAB_SCALE_DEFAULT,
                                      DISPLAY_PORTRAIT});
    const r3d_pipeline_counts_t counts = count_copies(p, &view);
    for (int bin = 0; bin < R3D_SIZES_BINS; bin++) {
        TEST_ASSERT_GREATER_THAN_INT(0, counts.triangles_sized[R3D_SIZES_SHADED][bin]);
    }
    release();
}

static void
camera_fixture(const stress_params_t* p) {
    counted_fixture(p);
    f.pack = asset_store_load(STRESS_SCENE);
    f.pack_loaded = !handle_is_none(f.pack);
    TEST_ASSERT_TRUE(f.pack_loaded);
    const asset_pack_t* pack = asset_store_loaded(f.pack);
    TEST_ASSERT_NOT_NULL(pack);
    const char* clip = strcmp(p->camera, STRESS_CAMERA) == 0 ? STRESS_ORBIT_CLIP : p->camera;
    anim_tracks_t tracks;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_from_pack(pack, clip, &tracks));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find_node(&tracks, name_hash(ANIM_TARGET_SELF), &f.path));
    f.duration_ms = tracks.clip.duration_ms;
}

static r3d_pipeline_counts_t
count_pose(const stress_params_t* p, int pose_index, int quarter) {
    const uint32_t t_ms = (uint32_t)((uint64_t)f.duration_ms * (uint32_t)pose_index / (uint32_t)p->perf_poses);
    const float seconds = (float)t_ms / STRESS_TEST_MS_PER_SECOND;
    const transformf_t pose = anim_transform_sample(&f.path, seconds);
    const render_view_t view = render_view_make(
        &pose, framing.half_fov_short_tan, TEST_NEAR_Z,
        (viewport_t){DEFAULT_SHORT_PX, GFX_HEIGHT * RENDER_LAB_SCALE_UNIT / RENDER_LAB_SCALE_DEFAULT, quarter});
    return count_copies(p, &view);
}

static void
test_culled_fraction_sweeps_from_visible_to_mostly_off_frustum(void) {
    const stress_params_t* p = &stress_presets[STRESS_CULLED];
    camera_fixture(p);
    for (int quarter = 0; quarter < (int)R3D_PERF_QUARTER_COUNT; quarter++) {
        float low = 1.0F;
        float high = 0.0F;
        for (int i = 0; i < p->perf_poses; i++) {
            const r3d_pipeline_counts_t counts = count_pose(p, i, r3d_perf_quarters[quarter].quarter);
            if (counts.clusters_tested == 0) {
                continue;
            }
            const float fraction = (float)counts.clusters_off_frustum / (float)counts.clusters_tested;
            low = fminf(low, fraction);
            high = fmaxf(high, fraction);
        }
        TEST_ASSERT_LESS_THAN_FLOAT(CULLED_SWEEP_MIN, low);
        TEST_ASSERT_GREATER_THAN_FLOAT(CULLED_SWEEP_MAX, high);
    }
    release();
}

static void
test_near_plane_sweeps_behind_and_clipped_triangles(void) {
    const stress_params_t* p = &stress_presets[STRESS_NEAR];
    camera_fixture(p);
    for (int quarter = 0; quarter < (int)R3D_PERF_QUARTER_COUNT; quarter++) {
        const r3d_pipeline_counts_t first = count_pose(p, 0, r3d_perf_quarters[quarter].quarter);
        TEST_ASSERT_EQUAL_INT(0, first.triangles_behind);
        TEST_ASSERT_EQUAL_INT(0, first.triangles_clipped);
        int behind = 0;
        int clipped = 0;
        for (int i = 0; i < p->perf_poses; i++) {
            const r3d_pipeline_counts_t counts = count_pose(p, i, r3d_perf_quarters[quarter].quarter);
            behind = counts.triangles_behind > behind ? counts.triangles_behind : behind;
            clipped = counts.triangles_clipped > clipped ? counts.triangles_clipped : clipped;
        }
        TEST_ASSERT_GREATER_OR_EQUAL_INT(NEAR_BEHIND_MIN, behind);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(NEAR_CLIPPED_MIN, clipped);
    }
    release();
}

void
run_stress_preset_suite(void) {
    RUN_TEST(test_culled_fraction_sweeps_from_visible_to_mostly_off_frustum);
    RUN_TEST(test_near_plane_sweeps_behind_and_clipped_triangles);
    RUN_TEST(test_card_has_holes_colour_steps_and_scene_size_bins);
    RUN_TEST(test_uniform_presets_have_equal_corner_colours_and_large_coverage);
    RUN_TEST(test_mixed_copies_split_colour_modes);
    RUN_TEST(test_every_preset_has_its_row);
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

SUITE_READS(run_stress_preset_suite, STRESS_SCENE);

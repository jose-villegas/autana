/*
 * Portable suite: the counted cull and triangle sort in r3d_pipeline, the
 * per-triangle span writer, and the "cull" debug view built on them. Every
 * mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "box3f_expect.h"
#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "core/checkpoint.h"
#include "core/tune.h"
#include "image/pixel/rgb565.h"
#include "math/linear/mat3f.h"
#include "math/linear/mat4f.h"
#include "render/context/render_context.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/view/r3d_pipeline_count.h"
#include "render/view/raster_cull_view.h"
#ifndef DEVICE_BUILD
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#endif

#define W                64
#define H                48

/* Geometry, in position ticks (one per world unit). */
#define HALF             20   /* half a small triangle's side */
#define FAR_X            1000 /* far enough sideways to be off the screen and outside the frustum */
#define BEHIND_EYE       (-500)
#define DEEPER_Z         100 /* farther from the eye than the plane z = 0 */
#define EYE_Z            (-400)
#define TRIANGLE_CORNERS 3

static const fixture_camera_t looking_at_origin = {{0.0F, 0.0F, EYE_Z}, {0.0F, 0.0F, 1.0F}, 0.5F, 1.0F};
static const fixture_camera_t looking_away = {{0.0F, 0.0F, EYE_Z}, {0.0F, 0.0F, -1.0F}, 0.5F, 1.0F};

static r3d_pipeline_work_t* work;

static void
release_work(void) {
    free(work);
    work = NULL;
}

static r3d_pipeline_work_t*
pipeline_work(void) {
    suite_set_test_cleanup(release_work);
    release_work();
    work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    return work;
}

static r3d_pipeline_count_walk_t* walk;

static void
release_walk(void) {
    free(walk);
    walk = NULL;
    release_work();
}

static r3d_pipeline_count_walk_t*
count_walk(void) {
    suite_set_test_cleanup(release_walk);
    free(walk);
    walk = malloc(sizeof(*walk));
    TEST_ASSERT_NOT_NULL(walk);
    return walk;
}

static r3d_lens_t
lens_for(const fixture_camera_t* camera) {
    r3d_lens_t lens;
    const render_view_t view = render_view_fixture_at(camera, (viewport_t){W, H, 0});
    r3d_lens_init(&lens, &view, 1);
    return lens;
}

/* Three one-triangle clusters under a root: two in front of the camera, the
 * second farther, and one far to the side. */
static const int16_t tree_positions[][3] = {
    {-HALF, -HALF, 0},        {HALF, -HALF, 0},         {HALF, HALF, 0},
    {-HALF, -HALF, DEEPER_Z}, {HALF, -HALF, DEEPER_Z},  {HALF, HALF, DEEPER_Z},
    {FAR_X, -HALF, 0},        {FAR_X + HALF, -HALF, 0}, {FAR_X + HALF, HALF, 0},
};
static const uint16_t tree_triangles[][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
static const uint16_t tree_colors[] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};
static const r3d_lit_cluster_t tree_clusters[] = {
    {0, TRIANGLE_CORNERS, 0, 1, {{-HALF, -HALF, 0}, {HALF, HALF, 0}}, true},
    {3, TRIANGLE_CORNERS, 1, 1, {{-HALF, -HALF, DEEPER_Z}, {HALF, HALF, DEEPER_Z}}, true},
    {6, TRIANGLE_CORNERS, 2, 1, {{FAR_X, -HALF, 0}, {FAR_X + HALF, HALF, 0}}, true},
};
static const r3d_lit_node_t tree_nodes[] = {
    {{{-HALF, -HALF, 0}, {FAR_X + HALF, HALF, DEEPER_Z}}, 1, 3, false},
    {tree_clusters[0].bounds, 0, 1, true},
    {tree_clusters[1].bounds, 1, 1, true},
    {tree_clusters[2].bounds, 2, 1, true},
};

static r3d_lit_mesh_t
tree_mesh(void) {
    return (r3d_lit_mesh_t){.positions = tree_positions,
                            .triangles = tree_triangles,
                            .clusters = tree_clusters,
                            .nodes = tree_nodes,
                            .vertex_count = 9,
                            .triangle_count = 3,
                            .cluster_count = 3,
                            .node_count = 4,
                            .position_scale = 1,
                            .face_colors = tree_colors};
}

/* The same clusters, in any order: the counted walk skips the cull's
 * nearest-first sort. */
static void
assert_same_list(const uint16_t* expected, const uint16_t* actual, int count) {
    for (int i = 0; i < count; i++) {
        bool found = false;
        for (int j = 0; j < count; j++) {
            found = found || expected[i] == actual[j];
        }
        TEST_ASSERT_TRUE(found);
    }
}

static void
test_counted_cull_returns_the_list_cull_returns(void) {
    const r3d_lit_mesh_t mesh = tree_mesh();
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    uint16_t plain[3] = {0};
    uint16_t counted[3] = {0};
    r3d_pipeline_counts_t counts = {0};
    const int expected = r3d_pipeline_cull(&mesh, &lens, NULL, plain, pipeline_work(), NULL);
    const int actual = r3d_pipeline_cull_counted(&mesh, &lens, NULL, counted, count_walk(), &counts);
    TEST_ASSERT_EQUAL_INT(2, expected);
    TEST_ASSERT_EQUAL_INT(expected, actual);
    assert_same_list(plain, counted, expected);
}

static void
test_counted_cull_counts_agree_with_the_list(void) {
    const r3d_lit_mesh_t mesh = tree_mesh();
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    uint16_t out[3];
    r3d_pipeline_counts_t counts = {0};
    const int kept = r3d_pipeline_cull_counted(&mesh, &lens, NULL, out, count_walk(), &counts);
    int submitted = 0;
    for (int i = 0; i < kept; i++) {
        submitted += mesh.clusters[out[i]].triangle_count;
    }
    TEST_ASSERT_EQUAL_INT(kept, counts.clusters_kept);
    TEST_ASSERT_TRUE(counts.clusters_kept <= counts.clusters_tested);
    TEST_ASSERT_EQUAL_INT(submitted, counts.triangles_submitted);
    TEST_ASSERT_EQUAL_INT(mesh.node_count, counts.nodes_visited);
    TEST_ASSERT_EQUAL_INT(1, counts.nodes_cut);
    TEST_ASSERT_EQUAL_INT(2, counts.clusters_tested);

    r3d_pipeline_counts_t twice = counts;
    (void)r3d_pipeline_cull_counted(&mesh, &lens, NULL, out, count_walk(), &twice);
    TEST_ASSERT_EQUAL_INT(2 * counts.nodes_visited, twice.nodes_visited);
    TEST_ASSERT_EQUAL_INT(2 * counts.nodes_cut, twice.nodes_cut);
    TEST_ASSERT_EQUAL_INT(2 * counts.clusters_tested, twice.clusters_tested);
    TEST_ASSERT_EQUAL_INT(2 * counts.clusters_kept, twice.clusters_kept);
    TEST_ASSERT_EQUAL_INT(2 * counts.triangles_submitted, twice.triangles_submitted);
}

static void
test_counted_cull_cuts_a_mesh_wholly_outside_the_frustum(void) {
    const r3d_lit_mesh_t mesh = tree_mesh();
    const r3d_lens_t lens = lens_for(&looking_away);
    uint16_t out[3];
    r3d_pipeline_counts_t counts = {0};
    TEST_ASSERT_EQUAL_INT(0, r3d_pipeline_cull_counted(&mesh, &lens, NULL, out, count_walk(), &counts));
    TEST_ASSERT_TRUE(counts.nodes_cut >= 1);
    TEST_ASSERT_EQUAL_INT(1, counts.nodes_visited);
    TEST_ASSERT_EQUAL_INT(0, counts.clusters_tested);
    TEST_ASSERT_EQUAL_INT(0, counts.clusters_kept);
    TEST_ASSERT_EQUAL_INT(0, counts.triangles_submitted);
}

/* Five single-sided triangles in two clusters, seen from EYE_Z looking at
 * the origin. Cluster 0 holds, in order, one set up, one wound away from the
 * eye and one off the screen; cluster 1 one behind the eye and one with a
 * corner behind it. */
#define SORT_VERTICES 15
static const int16_t sort_positions[SORT_VERTICES][3] = {
    {-HALF, -HALF, 0},       {HALF, -HALF, 0},           {HALF, HALF, 0},           {-HALF, -HALF, 0},
    {HALF, HALF, 0},         {HALF, -HALF, 0},           {FAR_X, -HALF, 0},         {FAR_X + HALF, -HALF, 0},
    {FAR_X + HALF, HALF, 0}, {-HALF, -HALF, BEHIND_EYE}, {HALF, -HALF, BEHIND_EYE}, {HALF, HALF, BEHIND_EYE},
    {-HALF, -HALF, 0},       {HALF, -HALF, 0},           {HALF, HALF, BEHIND_EYE},
};
static const uint16_t sort_triangles[][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {9, 10, 11}, {12, 13, 14}};
static const uint16_t sort_colors[] = {UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX};
static const r3d_lit_cluster_t sort_clusters[] = {
    {0, 9, 0, 3, {{-HALF, -HALF, 0}, {FAR_X + HALF, HALF, 0}}, false},
    {9, 6, 3, 2, {{-HALF, -HALF, BEHIND_EYE}, {HALF, HALF, 0}}, false},
};
static const r3d_lit_node_t sort_nodes[] = {
    {{{-HALF, -HALF, BEHIND_EYE}, {FAR_X + HALF, HALF, 0}}, 0, 2, true},
};

static r3d_lit_mesh_t
sort_mesh(void) {
    return (r3d_lit_mesh_t){.positions = sort_positions,
                            .triangles = sort_triangles,
                            .clusters = sort_clusters,
                            .nodes = sort_nodes,
                            .vertex_count = SORT_VERTICES,
                            .triangle_count = 5,
                            .cluster_count = 2,
                            .node_count = 1,
                            .position_scale = 1,
                            .face_colors = sort_colors};
}

/* The mesh's both clusters transformed into `cs`, and sorted into `counts`. */
static void
sort_sample(r3d_pipeline_counts_t* counts, uint16_t* set_up) {
    const r3d_lit_mesh_t mesh = sort_mesh();
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    const uint16_t clusters[] = {0, 1};
    r3d_pipeline_vertex_t cs[SORT_VERTICES];
    r3d_pipeline_transform(&mesh, &lens, clusters, 2, cs, NULL, false);
    r3d_pipeline_count_triangles(&mesh, &lens, clusters, 2, cs, counts, set_up);
}

static void
test_triangles_are_sorted_into_exactly_one_fate_each(void) {
    r3d_pipeline_counts_t counts = {0};
    sort_sample(&counts, NULL);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_behind);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_clipped);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_missed);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_facing_away);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_set_up);
    TEST_ASSERT_EQUAL_INT(0, counts.triangles_submitted);
}

static void
test_counted_pixels_are_clipped_centres_and_zero_without_setup(void) {
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    r3d_lit_mesh_t mesh = sort_mesh();
    r3d_lit_cluster_t cluster = sort_clusters[0];
    cluster.triangle_count = 1;
    cluster.double_sided = true;
    mesh.clusters = &cluster;
    const uint16_t kept[] = {0};
    const int side = R3D_SIZES_SMALL_MAX * R3D_SUBPIXEL;
    const r3d_pipeline_vertex_t cs[SORT_VERTICES] = {{0, 0, 1.0F}, {side, 0, 1.0F}, {0, side, 1.0F}};
    r3d_pipeline_counts_t counts = {0};
    r3d_pipeline_count_triangles(&mesh, &lens, kept, 0, cs, &counts, NULL);
    TEST_ASSERT_EQUAL_INT(0, counts.pixels[R3D_SIZES_FACE]);
    r3d_pipeline_count_triangles(&mesh, &lens, kept, 1, cs, &counts, NULL);
    const int expected = R3D_SIZES_SMALL_MAX * (R3D_SIZES_SMALL_MAX - 1) / 2;
    TEST_ASSERT_EQUAL_INT(expected, counts.pixels[R3D_SIZES_FACE]);
    TEST_ASSERT_EQUAL_INT(cluster.vertex_count, counts.vertices_transformed);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_sized[R3D_SIZES_FACE][R3D_SIZES_MORE]);
    TEST_ASSERT_EQUAL_INT(0, counts.pixels[R3D_SIZES_UNIFORM]);
    TEST_ASSERT_EQUAL_INT(0, counts.pixels[R3D_SIZES_SHADED]);
    r3d_lens_t clipped = lens;
    clipped.width = R3D_SIZES_SMALL_MAX / 2;
    r3d_pipeline_counts_t partial = {0};
    r3d_pipeline_count_triangles(&mesh, &clipped, kept, 1, cs, &partial, NULL);
    TEST_ASSERT_EQUAL_INT(expected - 1, partial.pixels[R3D_SIZES_FACE]);
}

static void
test_uniform_mode_uses_quantized_colour_steps(void) {
    const int side = R3D_SUBPIXEL * 2 * (UINT8_MAX + 1);
    const r3d_span_vertex_t a = {0, 0, 1.0F, 0, 0, 0};
    r3d_span_vertex_t b = {side, 0, 1.0F, 1, 0, 0};
    const r3d_span_vertex_t c = {0, side, 1.0F, 0, 0, 0};
    TEST_ASSERT_TRUE(r3d_span_uniform(&a, &b, &c));
    b.r = UINT8_MAX;
    TEST_ASSERT_FALSE(r3d_span_uniform(&a, &b, &c));
}

static void
test_off_frustum_clusters_and_subtrees_are_separate(void) {
    r3d_lit_mesh_t mesh = tree_mesh();
    r3d_lit_node_t leaf = tree_nodes[0];
    leaf.first = 0;
    leaf.count = mesh.cluster_count;
    leaf.leaf = true;
    mesh.nodes = &leaf;
    mesh.node_count = 1;
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    uint16_t kept[3];
    r3d_pipeline_counts_t counts = {0};
    TEST_ASSERT_EQUAL_INT(2, r3d_pipeline_cull_counted(&mesh, &lens, NULL, kept, count_walk(), &counts));
    TEST_ASSERT_EQUAL_INT(1, counts.clusters_off_frustum);
    TEST_ASSERT_EQUAL_INT(0, counts.nodes_cut);
    TEST_ASSERT_EQUAL_INT(counts.clusters_tested, counts.clusters_kept + counts.clusters_off_frustum);
}

static void
test_size_pixels_accepts_either_winding(void) {
    enum { SIDE = 4, COVERED = SIDE * (SIDE - 1) / 2 };

    r3d_size_point_t points[] = {{0, 0}, {SIDE, 0}, {0, SIDE}};
    const recti_t box = r3d_size_centre_box(points);
    TEST_ASSERT_EQUAL_INT(COVERED, r3d_size_pixels(points, box, SIDE, SIDE));
    const r3d_size_point_t swap = points[1];
    points[1] = points[2];
    points[2] = swap;
    TEST_ASSERT_EQUAL_INT(COVERED, r3d_size_pixels(points, box, SIDE, SIDE));
    TEST_ASSERT_EQUAL_INT(R3D_SIZES_MORE, r3d_size_bin_of(COVERED));
}

static void
test_size_modes_follow_face_and_corner_colours_in_both_windings(void) {
    const uint16_t kept[] = {0};
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    uint8_t colors[SORT_VERTICES][3] = {0};
    r3d_lit_cluster_t cluster = sort_clusters[0];
    cluster.triangle_count = 1;
    cluster.double_sided = true;
    r3d_lit_mesh_t mesh = sort_mesh();
    mesh.clusters = &cluster;
    mesh.colors = (const uint8_t (*)[3])colors;
    r3d_pipeline_vertex_t cs[SORT_VERTICES];
    r3d_pipeline_transform(&mesh, &lens, kept, 1, cs, NULL, false);
    for (int winding = 0; winding < 2; winding++) {
        for (int mode = 0; mode < R3D_SIZES_MODES; mode++) {
            colors[1][0] = mode == R3D_SIZES_UNIFORM ? 0 : UINT8_MAX;
            mesh.face_colors = mode == R3D_SIZES_FACE ? sort_colors : NULL;
            r3d_pipeline_counts_t counts = {0};
            r3d_pipeline_count_triangles(&mesh, &lens, kept, 1, cs, &counts, NULL);
            TEST_ASSERT_EQUAL_INT(1, counts.triangles_set_up);
            TEST_ASSERT_EQUAL_INT(1, counts.triangles_sized[mode][R3D_SIZES_MORE]);
        }
        const r3d_pipeline_vertex_t swap = cs[0];
        cs[0] = cs[2];
        cs[2] = swap;
    }
}

static void
test_set_up_per_cluster_counts_set_up_and_clipped_triangles(void) {
    enum { UNLISTED = 3, SENTINEL = 0xABCD };

    uint16_t set_up[UNLISTED] = {SENTINEL, SENTINEL, SENTINEL};
    r3d_pipeline_counts_t counts = {0};
    sort_sample(&counts, set_up);
    TEST_ASSERT_EQUAL_UINT16(1, set_up[0]);
    TEST_ASSERT_EQUAL_UINT16(1, set_up[1]);
    TEST_ASSERT_EQUAL_UINT16(SENTINEL, set_up[2]);

    const r3d_lit_mesh_t mesh = sort_mesh();
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    const uint16_t second[] = {1};
    r3d_pipeline_vertex_t cs[SORT_VERTICES];
    r3d_pipeline_transform(&mesh, &lens, second, 1, cs, NULL, false);
    r3d_pipeline_counts_t only = {0};
    uint16_t alone[UNLISTED] = {SENTINEL, SENTINEL, SENTINEL};
    r3d_pipeline_count_triangles(&mesh, &lens, second, 1, cs, &only, alone);
    TEST_ASSERT_EQUAL_UINT16(1, alone[0]); /* the one listed cluster's, by its place in the list */
    TEST_ASSERT_EQUAL_UINT16(SENTINEL, alone[1]);
    TEST_ASSERT_EQUAL_INT(1, only.triangles_behind);
    TEST_ASSERT_EQUAL_INT(1, only.triangles_clipped);
    TEST_ASSERT_EQUAL_INT(0, only.triangles_set_up);
    TEST_ASSERT_EQUAL_INT(0, only.triangles_missed);
    TEST_ASSERT_EQUAL_INT(0, only.triangles_facing_away);
}

/* The triangles a draw fills, each marked once by the writer that sees its spans. */
static bool* filled;

static void
mark_filled(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last, int32_t z,
            int32_t dz) {
    (void)rows;
    (void)y;
    (void)x_first;
    (void)x_last;
    (void)z;
    (void)dz;
    filled[writer->value] = true;
}

static void
release_filled(void) {
    free(filled);
    filled = NULL;
    release_work();
}

/* The draw fills exactly the triangles the count says reach r3d_span: the
 * count's sort and the draw's are one, checked rather than trusted. */
static void
test_the_draw_fills_the_triangles_the_count_sets_up(void) {
    const r3d_lit_mesh_t mesh = sort_mesh();
    const r3d_lens_t lens = lens_for(&looking_at_origin);
    const uint16_t in_front[] = {0}; /* cluster 0: no corner behind the eye */
    r3d_pipeline_vertex_t cs[SORT_VERTICES];
    r3d_pipeline_transform(&mesh, &lens, in_front, 1, cs, NULL, false);
    r3d_pipeline_counts_t counts = {0};
    r3d_pipeline_count_triangles(&mesh, &lens, in_front, 1, cs, &counts, NULL);

    uint16_t* pixels = calloc((size_t)W * H * 2, sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(pixels);
    (void)pipeline_work();
    suite_set_test_cleanup(release_filled);
    filled = calloc((size_t)mesh.triangle_count, sizeof(bool));
    TEST_ASSERT_NOT_NULL(filled);
    const r3d_span_writer_t marker = {.span = mark_filled, .step = R3D_SPAN_STEP_TRIANGLE};
    r3d_span_target_t target = r3d_span_target(pixels, pixels + (size_t)W * H, W, 0, H);
    target.writers = &marker;
    target.writer_count = 1;
    r3d_pipeline_draw(&mesh, &lens, in_front, 1, cs, NULL, &target, work);
    int drawn = 0;
    for (int t = 0; t < mesh.triangle_count; t++) {
        drawn += filled[t];
    }
    free(pixels);
    TEST_ASSERT_EQUAL_INT(1, counts.triangles_set_up);
    TEST_ASSERT_EQUAL_INT(counts.triangles_set_up, drawn);
    TEST_ASSERT_TRUE(filled[0]);
}

static void
test_triangle_sort_adds_to_the_counts_it_is_given(void) {
    r3d_pipeline_counts_t once = {0};
    sort_sample(&once, NULL);
    r3d_pipeline_counts_t twice = once;
    sort_sample(&twice, NULL);
    TEST_ASSERT_EQUAL_INT(2 * once.triangles_behind, twice.triangles_behind);
    TEST_ASSERT_EQUAL_INT(2 * once.triangles_clipped, twice.triangles_clipped);
    TEST_ASSERT_EQUAL_INT(2 * once.triangles_missed, twice.triangles_missed);
    TEST_ASSERT_EQUAL_INT(2 * once.triangles_facing_away, twice.triangles_facing_away);
    TEST_ASSERT_EQUAL_INT(2 * once.triangles_set_up, twice.triangles_set_up);
}

/* The rig's wall and card, both double sided, the card's two triangles side
 * by side over the wall's. */

/* Each instance writes this times its position (1 for the wall, 2 for the
 * card), plus the drawn triangle's index when it asks. */
#define INSTANCE_BASE 100

static void
tags_clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint32_t));
}

static bool
instance_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    out->span = raster_attachment_tag;
    out->value = INSTANCE_BASE * (uint32_t)(instance + 1);
    out->step = self->state != NULL ? R3D_SPAN_STEP_TRIANGLE : R3D_SPAN_STEP_NONE;
    return true;
}

/* The triangle of a double-sided quad of corners 0-1-2 and 0-2-3 under the
 * pixel (x, picture's middle row): the first has the corners right of the
 * diagonal, which side of the picture that is following the view's x axis. */
static int
triangle_under(const render_view_t* view, int x) {
    const bool right = x >= W / 2;
    return right == (view->screen_x.x > 0.0F) ? 0 : 1;
}

/* Every pixel holds one of the four tags, each somewhere, and the writer
 * that did not ask for the index holds the instance's value alone. */
static void
assert_every_pixel_tagged(const uint32_t* tags, const uint32_t* fixed) {
    int seen[2][2] = {{0}};
    for (int p = 0; p < W * H; p++) {
        const uint32_t instance = tags[p] / INSTANCE_BASE;
        const uint32_t triangle = tags[p] % INSTANCE_BASE;
        TEST_ASSERT_TRUE(instance >= 1 && instance <= 2);
        TEST_ASSERT_TRUE(triangle <= 1);
        TEST_ASSERT_EQUAL_UINT32(instance * INSTANCE_BASE, fixed[p]);
        seen[instance - 1][triangle]++;
    }
    for (int i = 0; i < 2; i++) {
        for (int t = 0; t < 2; t++) {
            TEST_ASSERT_TRUE(seen[i][t] > 0);
        }
    }
}

static void
test_per_triangle_writer_tags_the_triangle_that_won_each_pixel(void) {
    int per_triangle = 1;
    const raster_attachment_t tagged = {
        .format = IMAGE_U32, .clear = tags_clear, .writer = instance_writer, .state = &per_triangle};
    const raster_attachment_t constant = {
        .format = IMAGE_U32, .clear = tags_clear, .writer = instance_writer, .state = NULL};
    const raster_attachment_t* const both[] = {&tagged, &constant};
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, 2, 0, W, H, 0);
    raster_rig_attach(r, both, 2);
    r->view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    raster_draw(&r->raster, &r->view);

    const uint32_t* tags = raster_rig_attachment(r, 0);
    const uint32_t* fixed = raster_rig_attachment(r, 1);
    const int row = (H / 2) * W;
    const int near_centre = 3;
    const int wall_edge = 2;
    const int card_left = (W / 2) - near_centre;
    const int card_right = (W / 2) + near_centre;
    const int wall_left = wall_edge;
    const int wall_right = W - 1 - wall_edge;
    TEST_ASSERT_EQUAL_UINT32(2 * INSTANCE_BASE + triangle_under(&r->view, card_left), tags[row + card_left]);
    TEST_ASSERT_EQUAL_UINT32(2 * INSTANCE_BASE + triangle_under(&r->view, card_right), tags[row + card_right]);
    TEST_ASSERT_EQUAL_UINT32(INSTANCE_BASE + triangle_under(&r->view, wall_left), tags[row + wall_left]);
    TEST_ASSERT_EQUAL_UINT32(INSTANCE_BASE + triangle_under(&r->view, wall_right), tags[row + wall_right]);
    TEST_ASSERT_TRUE(tags[row + card_left] != tags[row + card_right]);

    assert_every_pixel_tagged(tags, fixed);
}

/* The cull view's scene, seen from z = -300: a wall behind everything and a
 * card in front of it (drawn), a small quad behind the card (hidden), a
 * single-sided one wound away from the eye (nothing set up) and one flat on
 * y = 0 outside the frustum (culled). Each is one cluster of two triangles. */
#define SCENE_QUADS 5

enum { SCENE_WALL, SCENE_CARD, SCENE_HIDDEN, SCENE_AWAY, SCENE_SIDE };

static const int16_t scene_corners[SCENE_QUADS][4][3] = {
    [SCENE_WALL] = {{-400, -400, 0}, {400, -400, 0}, {400, 400, 0}, {-400, 400, 0}},
    [SCENE_CARD] = {{-40, -40, -100}, {40, -40, -100}, {40, 40, -100}, {-40, 40, -100}},
    [SCENE_HIDDEN] = {{-15, -15, 100}, {15, -15, 100}, {15, 15, 100}, {-15, 15, 100}},
    [SCENE_AWAY] = {{-20, 20, -150}, {20, 20, -150}, {20, -20, -150}, {-20, -20, -150}},
    [SCENE_SIDE] = {{300, 0, -40}, {380, 0, -40}, {380, 0, 40}, {300, 0, 40}},
};
static const fixture_camera_t scene_camera = {{0.0F, 0.0F, -300.0F}, {0.0F, 0.0F, 1.0F}, 0.5F, 1.0F};
static const char* const CULL_VIEW = "cull";
/* The cull view's colours for a culled and a drawn cluster, as raster_cull_view.c paints them. */
static const uint32_t CULLED_RGB = 0x505050;
static const uint32_t DRAWN_RGB = 0x3CC850;
/* What scene_draw() clears to. */
static const uint32_t CLEAR_RGB = 0x000000;
static const int TRIANGLES_PER_QUAD = 2;

typedef struct {
    r3d_quad_t quad[SCENE_QUADS];
    r3d_instance_t instance[SCENE_QUADS];
    render_context_t context;
    render_context_t straight; /* a second context, for a run the observer never touches */
} scene_t;

static scene_t* scene_now;
static int32_t saved_from;
static bool from_changed;
static int32_t saved_paint;

static void
scene_release(void) {
    if (from_changed) {
        (void)tune_set(tune_shared(), TUNE_NAME(cullview, from), saved_from);
        from_changed = false;
    }
    if (scene_now != NULL) {
        (void)tune_set(tune_shared(), TUNE_NAME(cullview, paint), saved_paint);
        render_context_release(&scene_now->context);
        render_context_release(&scene_now->straight);
        free(scene_now);
        scene_now = NULL;
    }
}

static void
set_cull_from(raster_cull_from_t from) {
    const tune_entry_t* entry = tune_find(tune_shared(), TUNE_NAME(cullview, from));
    TEST_ASSERT_NOT_NULL(entry);
    if (!from_changed) {
        saved_from = *entry->value;
        from_changed = true;
    }
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(cullview, from), from));
}

/* The raster's own culling switch, render.cull, as the frame left it. */
static bool
culling_on(void) {
    const tune_entry_t* entry = tune_find(tune_shared(), TUNE_NAME(render, cull));
    TEST_ASSERT_NOT_NULL(entry);
    return *entry->value != 0;
}

static scene_t*
scene_open(void) {
    scene_t* s = calloc(1, sizeof(*s));
    TEST_ASSERT_NOT_NULL(s);
    scene_now = s;
    suite_set_test_cleanup(scene_release);
    /* Flat fates, so a culled cluster's pixels carry its colour to count. */
    const tune_entry_t* paint = tune_find(tune_shared(), TUNE_NAME(cullview, paint));
    TEST_ASSERT_NOT_NULL(paint);
    saved_paint = *paint->value;
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(cullview, paint), RASTER_CULL_PAINT_FATES));
    for (int i = 0; i < SCENE_QUADS; i++) {
        r3d_quad_init(&s->quad[i], scene_corners[i]);
        s->instance[i] = (r3d_instance_t){&s->quad[i].mesh, NULL, NULL, NULL};
    }
    s->quad[SCENE_AWAY].cluster.double_sided = false;
    s->context = s->straight = RENDER_CONTEXT_DEFAULT;
    render_context_set_scale(&s->context, 100);
    render_context_set_scale(&s->straight, 100);
    render_context_set_debug_view(&s->context, render_context_debug_view_named(CULL_VIEW));
    TEST_ASSERT_NOT_NULL(render_context_debug_view_state(&s->context));
    return s;
}

static void
context_draw(scene_t* s, render_context_t* context) {
    const render_view_t view = render_view_fixture_at(&scene_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(context, s->instance, SCENE_QUADS, &view, 0));
}

static void
scene_draw(scene_t* s) {
    context_draw(s, &s->context);
}

static const raster_cull_t*
scene_state(const scene_t* s) {
    return render_context_debug_view_state(&s->context);
}

static int
pixels_of(const scene_t* s, uint32_t rgb) {
    const gfx_color_t* color = raster_color(&s->context.raster);
    int count = 0;
    for (int p = 0; p < W * H; p++) {
        count += color[p] == RGB565BE(rgb);
    }
    return count;
}

static void
test_cull_view_gives_each_cluster_its_fate(void) {
    scene_t* s = scene_open();
    scene_draw(s);
    const raster_cull_t* state = scene_state(s);
    TEST_ASSERT_EQUAL_INT(SCENE_QUADS, state->clusters);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_DRAWN, state->fate[SCENE_WALL]);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_DRAWN, state->fate[SCENE_CARD]);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_HIDDEN, state->fate[SCENE_HIDDEN]);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_NOT_SET_UP, state->fate[SCENE_AWAY]);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_CULLED, state->fate[SCENE_SIDE]);
}

static void
test_cull_view_frame_counts_clusters_by_fate(void) {
    scene_t* s = scene_open();
    scene_draw(s);
    const raster_cull_frame_t frame = raster_cull_frame(scene_state(s));
    TEST_ASSERT_EQUAL_INT(1, frame.clusters[RASTER_FATE_CULLED]);
    TEST_ASSERT_EQUAL_INT(1, frame.clusters[RASTER_FATE_NOT_SET_UP]);
    TEST_ASSERT_EQUAL_INT(1, frame.clusters[RASTER_FATE_HIDDEN]);
    TEST_ASSERT_EQUAL_INT(2, frame.clusters[RASTER_FATE_DRAWN]);
    int total = 0;
    for (int f = 0; f < RASTER_FATE_COUNT; f++) {
        total += frame.clusters[f];
    }
    TEST_ASSERT_EQUAL_INT(SCENE_QUADS, total);
}

static void
test_cull_view_frame_counts_the_walk_and_the_sort_of_every_instance(void) {
    scene_t* s = scene_open();
    scene_draw(s);
    const r3d_pipeline_counts_t c = raster_cull_frame(scene_state(s)).counts;
    int sized = 0;
    for (int mode = 0; mode < R3D_SIZES_MODES; mode++) {
        for (int bin = 0; bin < R3D_SIZES_BINS; bin++) {
            sized += c.triangles_sized[mode][bin];
        }
    }
    TEST_ASSERT_EQUAL_INT(c.triangles_set_up, sized);
    const int survivors = SCENE_QUADS - 1;
    TEST_ASSERT_EQUAL_INT(survivors, c.clusters_kept);
    TEST_ASSERT_EQUAL_INT(survivors, c.clusters_tested);
    TEST_ASSERT_EQUAL_INT(survivors * TRIANGLES_PER_QUAD, c.triangles_submitted);
    TEST_ASSERT_EQUAL_INT(1, c.nodes_cut);
    TEST_ASSERT_EQUAL_INT(SCENE_QUADS, c.nodes_visited);
    TEST_ASSERT_EQUAL_INT(TRIANGLES_PER_QUAD, c.triangles_facing_away);
    TEST_ASSERT_EQUAL_INT(3 * TRIANGLES_PER_QUAD, c.triangles_set_up);
    TEST_ASSERT_EQUAL_INT(c.triangles_submitted, c.triangles_behind + c.triangles_clipped + c.triangles_missed
                                                     + c.triangles_facing_away + c.triangles_set_up);
}

static void
test_cull_view_triangles_covering_counts_only_triangles_owning_pixels(void) {
    scene_t* s = scene_open();
    scene_draw(s);
    const raster_cull_frame_t frame = raster_cull_frame(scene_state(s));

    TEST_ASSERT_EQUAL_INT(2 * TRIANGLES_PER_QUAD, frame.triangles_covering);
    TEST_ASSERT_TRUE(frame.triangles_covering < frame.counts.triangles_set_up);
    TEST_ASSERT_EQUAL_INT(0, pixels_of(s, CULLED_RGB));
}

static void
test_cull_view_from_the_top_draws_again_with_every_cluster_and_the_same_counts(void) {
    scene_t* s = scene_open();
    scene_draw(s);
    const raster_cull_frame_t camera_only = raster_cull_frame(scene_state(s));
    TEST_ASSERT_TRUE(culling_on());
    TEST_ASSERT_EQUAL_INT(0, pixels_of(s, CULLED_RGB));

    set_cull_from(RASTER_CULL_FROM_TOP);
    scene_draw(s);
    const raster_cull_t* state = scene_state(s);
    TEST_ASSERT_EQUAL_INT(RASTER_CULL_FROM_TOP, state->from);
    TEST_ASSERT_TRUE(culling_on());
    TEST_ASSERT_FALSE(state->spectating);

    TEST_ASSERT_TRUE(state->spectator.position.y > state->camera.position.y);
    TEST_ASSERT_TRUE(pixels_of(s, CULLED_RGB) > 0);

    const raster_cull_frame_t from_top = raster_cull_frame(state);
    TEST_ASSERT_EQUAL_MEMORY(&camera_only, &from_top, sizeof from_top);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_CULLED, state->fate[SCENE_SIDE]);
}

/* Pixels of `rgb` in the dashes' on (or off) runs: CULLED_DASH_RUN on, as
 * many off, along x + y. */
#define CULLED_DASH_RUN 2

static int
dash_pixels_of(const scene_t* s, uint32_t rgb, bool on) {
    const gfx_color_t* color = raster_color(&s->context.raster);
    int count = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const bool lit = ((x + y) / CULLED_DASH_RUN) % 2 == 0;
            count += lit == on && color[(y * W) + x] == RGB565BE(rgb);
        }
    }
    return count;
}

/* Shaded, the default paint: a culled cluster is a sparse screen door whose
 * border is dashed, so its grey lies only on the dashes. */
static void
test_shaded_cull_view_dashes_culled_borders(void) {
    scene_t* s = scene_open();
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(cullview, paint), RASTER_CULL_PAINT_SHADED));
    set_cull_from(RASTER_CULL_FROM_TOP);
    scene_draw(s);
    TEST_ASSERT_EQUAL_UINT8(RASTER_FATE_CULLED, scene_state(s)->fate[SCENE_SIDE]);
    TEST_ASSERT_TRUE(dash_pixels_of(s, CULLED_RGB, true) > 0);
    TEST_ASSERT_EQUAL_INT(0, dash_pixels_of(s, CULLED_RGB, false));
    TEST_ASSERT_TRUE(pixels_of(s, CLEAR_RGB) < W * H); /* the screen door still shows */
}

/* A mesh with levels, laid out as the levels bake lays them: LOD_FINEST
 * strips side by side in front of the camera, two triangles each, then
 * LOD_COARSE strips each standing for LOD_MERGE of them, its group's, one
 * triangle each so the levels' clusters differ in size. The root's first
 * child holds the finest, its second the coarse. */
#define LOD_FINEST      4
#define LOD_MERGE       2
#define LOD_COARSE      (LOD_FINEST / LOD_MERGE)
#define LOD_CLUSTERS    (LOD_FINEST + LOD_COARSE)
#define LOD_STRIP       40 /* a finest strip's width, in position ticks */
#define LOD_HALF_HEIGHT 40
#define LOD_LEFT        (-(LOD_FINEST * LOD_STRIP) / 2)
#define LOD_NODES       3 /* the root, the finest leaf, the coarse leaf */
#define LOD_GROUP_ERROR 1
#define QUAD_CORNERS    4
#define LOD_COARSE_TRIS 1
#define LOD_TRIANGLES   ((LOD_FINEST * 2) + (LOD_COARSE * LOD_COARSE_TRIS))
/* Two instances placed this far above and below the middle, apart on screen. */
#define LOD_PAIR        2
#define LOD_APART       45.0F
/* A pick tolerance, in pixels, a group of LOD_GROUP_ERROR is within and one of UINT16_MAX is not. */
#define LOD_TOLERANCE   4.0F
/* Instances of it whose levels together overflow the view's clusters, while
 * the level each draws, with a slot per instance, still fits. */
#define LOD_CROWD       (RASTER_CULL_CLUSTERS_MAX / (LOD_FINEST + 1))

typedef struct {
    int16_t positions[LOD_CLUSTERS * QUAD_CORNERS][3];
    uint8_t colors[LOD_CLUSTERS * QUAD_CORNERS][3];
    uint16_t triangles[LOD_TRIANGLES][3];
    r3d_lit_cluster_t clusters[LOD_CLUSTERS];
    r3d_lit_node_t nodes[LOD_NODES];
    r3d_lit_cluster_lod_t lod[LOD_CLUSTERS];
    r3d_lit_lod_group_t groups[LOD_COARSE];
    r3d_lit_mesh_t mesh;
    r3d_instance_t* instances;
    uint32_t* coarse; /* each instance's coarse bits, R3D_LOD_WORDS(LOD_COARSE) words */
    mat4f_t model[LOD_PAIR];
} lod_scene_t;

static lod_scene_t* lod_now;

static void
lod_release(void) {
    if (lod_now != NULL) {
        free(lod_now->instances);
        free(lod_now->coarse);
        free(lod_now);
        lod_now = NULL;
    }
    scene_release();
}

/* Strip `c` from x0 to x1: its four corners, and its cluster of `triangles`
 * (two, or the first of them alone) from triangle `t`. */
static void
lod_strip(lod_scene_t* l, int c, int x0, int x1, int t, int triangles) {
    const int corner_x[QUAD_CORNERS] = {x0, x1, x1, x0};
    const int corner_y[QUAD_CORNERS] = {-LOD_HALF_HEIGHT, -LOD_HALF_HEIGHT, LOD_HALF_HEIGHT, LOD_HALF_HEIGHT};
    box3s_t bounds = box3s_empty();
    const int v0 = c * QUAD_CORNERS;
    for (int k = 0; k < QUAD_CORNERS; k++) {
        l->positions[v0 + k][0] = (int16_t)corner_x[k];
        l->positions[v0 + k][1] = (int16_t)corner_y[k];
        l->positions[v0 + k][2] = 0;
        memset(l->colors[v0 + k], UINT8_MAX, sizeof l->colors[v0 + k]);
        bounds = box3s_extend(bounds, vec3s_from_array(l->positions[v0 + k]));
    }
    const uint16_t first = (uint16_t)v0;
    for (int k = 0; k < triangles; k++) {
        l->triangles[t + k][0] = first;
        l->triangles[t + k][1] = (uint16_t)(first + 1 + k);
        l->triangles[t + k][2] = (uint16_t)(first + 2 + k);
    }
    l->clusters[c] = (r3d_lit_cluster_t){first, QUAD_CORNERS, (uint16_t)t, (uint16_t)triangles, bounds, true};
}

/* The cull view open, and `count` instances of the levelled mesh, each drawing its finest level. */
static lod_scene_t*
lod_open(int count) {
    (void)scene_open();
    lod_scene_t* l = calloc(1, sizeof *l);
    TEST_ASSERT_NOT_NULL(l);
    lod_now = l;
    suite_set_test_cleanup(lod_release);
    int t = 0;
    for (int f = 0; f < LOD_FINEST; f++) {
        lod_strip(l, f, LOD_LEFT + (f * LOD_STRIP), LOD_LEFT + ((f + 1) * LOD_STRIP), t, 2);
        t += 2;
        l->lod[f] = (r3d_lit_cluster_lod_t){R3D_LOD_NONE, (uint16_t)(f / LOD_MERGE)};
    }
    for (int g = 0; g < LOD_COARSE; g++) {
        const int c = LOD_FINEST + g;
        const int x0 = LOD_LEFT + (g * LOD_MERGE * LOD_STRIP);
        lod_strip(l, c, x0, x0 + (LOD_MERGE * LOD_STRIP), t, LOD_COARSE_TRIS);
        t += LOD_COARSE_TRIS;
        l->lod[c] = (r3d_lit_cluster_lod_t){(uint16_t)g, R3D_LOD_NONE};
        const box3s_t b = l->clusters[c].bounds;
        const int16_t radius = (int16_t)((LOD_MERGE * LOD_STRIP) + LOD_HALF_HEIGHT);
        l->groups[g] = (r3d_lit_lod_group_t){{{(int16_t)((b.lo.x + b.hi.x) / 2), 0, 0}, radius}, LOD_GROUP_ERROR, 0};
    }
    box3s_t all = box3s_empty();
    for (int c = 0; c < LOD_CLUSTERS; c++) {
        all = box3s_union(all, l->clusters[c].bounds);
    }
    l->nodes[0] = (r3d_lit_node_t){all, 1, LOD_NODES - 1, false};
    l->nodes[1] = (r3d_lit_node_t){all, 0, LOD_FINEST, true};
    l->nodes[2] = (r3d_lit_node_t){all, LOD_FINEST, LOD_COARSE, true};
    l->mesh = (r3d_lit_mesh_t){.positions = l->positions,
                               .colors = l->colors,
                               .triangles = l->triangles,
                               .clusters = l->clusters,
                               .nodes = l->nodes,
                               .vertex_count = LOD_CLUSTERS * QUAD_CORNERS,
                               .triangle_count = LOD_TRIANGLES,
                               .cluster_count = LOD_CLUSTERS,
                               .node_count = LOD_NODES,
                               .position_scale = 1,
                               .cluster_lod = l->lod,
                               .lod_groups = l->groups,
                               .lod_group_count = LOD_COARSE};
    r3d_lit_mesh_find_levels(&l->mesh);
    TEST_ASSERT_EQUAL_UINT16(LOD_FINEST, l->mesh.lod_coarse_first);
    l->instances = calloc((size_t)count, sizeof *l->instances);
    TEST_ASSERT_NOT_NULL(l->instances);
    l->coarse = calloc((size_t)count * R3D_LOD_WORDS(LOD_COARSE), sizeof *l->coarse);
    TEST_ASSERT_NOT_NULL(l->coarse);
    for (int i = 0; i < count; i++) {
        l->instances[i] = (r3d_instance_t){&l->mesh, NULL, NULL, &l->coarse[i * R3D_LOD_WORDS(LOD_COARSE)]};
    }
    return l;
}

static const raster_cull_t*
lod_draw(lod_scene_t* l, int count) {
    const render_view_t view = render_view_fixture_at(&scene_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(&scene_now->context, l->instances, count, &view, 0));
    return scene_state(scene_now);
}

static int
fates_total(const raster_cull_frame_t* frame) {
    int total = 0;
    for (int f = 0; f < RASTER_FATE_COUNT; f++) {
        total += frame->clusters[f];
    }
    return total;
}

/* Only the level drawn has fates: the coarse strips the finest stand in for
 * are not culled clusters, and the drawn ones keep theirs. */
static void
test_cull_view_counts_only_the_level_drawn(void) {
    lod_scene_t* l = lod_open(1);
    const raster_cull_t* state = lod_draw(l, 1);
    TEST_ASSERT_EQUAL_INT(LOD_FINEST, state->clusters);
    TEST_ASSERT_EQUAL_INT(LOD_FINEST * TRIANGLES_PER_QUAD, state->triangles);
    const raster_cull_frame_t frame = raster_cull_frame(state);
    TEST_ASSERT_EQUAL_INT(LOD_FINEST, fates_total(&frame));
    TEST_ASSERT_EQUAL_INT(LOD_FINEST, frame.clusters[RASTER_FATE_DRAWN]);
    TEST_ASSERT_EQUAL_INT(LOD_FINEST * TRIANGLES_PER_QUAD, frame.triangles_covering);
}

/* A pick that turns every group coarse: the coarse strips are the level
 * drawn, and from behind and above the camera, where the strips face the
 * spectator, the finest they stand for are not painted culled. */
static void
test_cull_view_paints_the_coarse_level_picked_and_not_the_finest(void) {
    lod_scene_t* l = lod_open(1);
    render_lod_settings_t* lod = &scene_now->context.settings.lod;
    lod->tolerance_px = (float)INT16_MAX; /* every error within it */
    lod->coverage_permille = INT16_MAX;   /* every group small enough */
    lod->pick_every = 1;
    set_cull_from(RASTER_CULL_FROM_CHASE);
    const raster_cull_t* state = lod_draw(l, 1);
    for (int c = 0; c < LOD_CLUSTERS; c++) {
        TEST_ASSERT_EQUAL(c >= LOD_FINEST, r3d_lod_drawn(&l->mesh, c, l->instances[0].lod_coarse));
    }
    TEST_ASSERT_EQUAL_INT(LOD_COARSE, state->clusters);
    TEST_ASSERT_EQUAL_INT(LOD_COARSE * LOD_COARSE_TRIS, state->triangles);
    const raster_cull_frame_t frame = raster_cull_frame(state);
    TEST_ASSERT_EQUAL_INT(LOD_COARSE, fates_total(&frame));
    TEST_ASSERT_EQUAL_INT(frame.counts.clusters_tested, frame.counts.clusters_kept + frame.counts.clusters_off_frustum);
    TEST_ASSERT_EQUAL_INT(0, frame.clusters[RASTER_FATE_CULLED]);
    TEST_ASSERT_TRUE(pixels_of(scene_now, DRAWN_RGB) > 0);
    TEST_ASSERT_EQUAL_INT(0, pixels_of(scene_now, CULLED_RGB));
}

/* Group 0 coarse and group 1 fine: each of two instances draws its coarse
 * strip and group 1's two finest, three clusters of unequal size, so the
 * second instance's triangles follow an odd count; every drawn triangle in
 * sight is counted once. */
static void
test_cull_view_counts_each_drawn_triangle_once_after_an_odd_instance(void) {
    lod_scene_t* l = lod_open(LOD_PAIR);
    l->groups[1].error = UINT16_MAX;
    render_lod_settings_t* lod = &scene_now->context.settings.lod;
    lod->tolerance_px = LOD_TOLERANCE;
    lod->coverage_permille = INT16_MAX; /* every group small enough */
    lod->pick_every = 1;
    for (int i = 0; i < LOD_PAIR; i++) {
        l->model[i] = mat4f_identity();
        l->model[i].m[1][MAT3F_AXES] = i == 0 ? -LOD_APART : LOD_APART;
        l->instances[i].model = &l->model[i];
    }
    const raster_cull_t* state = lod_draw(l, LOD_PAIR);
    for (int i = 0; i < LOD_PAIR; i++) {
        for (int c = 0; c < LOD_CLUSTERS; c++) {
            const bool drawn = c == LOD_FINEST || (c >= LOD_MERGE && c < LOD_FINEST);
            TEST_ASSERT_EQUAL(drawn, r3d_lod_drawn(&l->mesh, c, l->instances[i].lod_coarse));
        }
    }
    const int clusters = LOD_PAIR * (LOD_MERGE + 1);
    const int triangles = LOD_PAIR * ((LOD_MERGE * 2) + LOD_COARSE_TRIS);
    TEST_ASSERT_EQUAL_INT(clusters, state->clusters);
    TEST_ASSERT_EQUAL_INT(triangles, state->triangles);
    const raster_cull_frame_t frame = raster_cull_frame(state);
    TEST_ASSERT_EQUAL_INT(clusters, fates_total(&frame));
    TEST_ASSERT_EQUAL_INT(clusters, frame.clusters[RASTER_FATE_DRAWN]);
    TEST_ASSERT_EQUAL_INT(triangles, frame.triangles_covering);
}

/* The view has room for the level each instance draws, not for every level
 * its mesh holds: a crowd whose levels together overflow it still draws. */
static void
test_cull_view_has_room_for_the_levels_drawn_not_every_level(void) {
    TEST_ASSERT_TRUE(LOD_CROWD * LOD_CLUSTERS > RASTER_CULL_CLUSTERS_MAX);
    lod_scene_t* l = lod_open(LOD_CROWD);
    const raster_cull_t* state = lod_draw(l, LOD_CROWD);
    TEST_ASSERT_EQUAL_INT(LOD_CROWD * LOD_FINEST, state->clusters);
    const raster_cull_frame_t frame = raster_cull_frame(state);
    TEST_ASSERT_EQUAL_INT(LOD_CROWD * LOD_FINEST, fates_total(&frame));
    TEST_ASSERT_TRUE(frame.clusters[RASTER_FATE_DRAWN] > 0);
}

/* The picture a context last drew, its colour then its depth. */
static void
assert_same_picture(const render_context_t* expected, const render_context_t* actual) {
    TEST_ASSERT_EQUAL_MEMORY(raster_color(&expected->raster), raster_color(&actual->raster),
                             sizeof(gfx_color_t) * W * H);
    TEST_ASSERT_EQUAL_MEMORY(raster_depth(&expected->raster), raster_depth(&actual->raster), sizeof(uint16_t) * W * H);
}

/* A pose no view's own spectator takes: above and beside the scene, looking back at it. */
static transformf_t
somewhere_else(void) {
    return transformf_looking((vec3f_t){200.0F, 150.0F, -250.0F}, (vec3f_t){-1.0F, -0.5F, 1.0F}, vec3f_unit_y());
}

static raster_cull_from_t
cull_from(void) {
    const tune_entry_t* entry = tune_find(tune_shared(), TUNE_NAME(cullview, from));
    TEST_ASSERT_NOT_NULL(entry);
    return (raster_cull_from_t)*entry->value;
}

/* With the observer set for one frame and then cleared, every view draws its
 * next frame as a run that never set it; a view with no second draw draws
 * even the observed frame so, and reports no pose. */
static void
observer_off_draws_every_view_as_a_straight_run(scene_t* s) {
    const transformf_t elsewhere = somewhere_else();
    for (int v = RENDER_DEBUG_VIEW_SHADED; v <= RENDER_DEBUG_VIEW_COUNT; v++) {
        const render_debug_view_t* row = render_context_debug_view(v);
        const bool second = row != NULL && row->second_view != NULL;
        render_context_set_debug_view(&s->straight, v);
        render_context_set_debug_view(&s->context, v);
        render_context_set_observer(&s->context, &elsewhere);
        context_draw(s, &s->straight);
        context_draw(s, &s->context);
        transformf_t seen;
        TEST_ASSERT_EQUAL(second, render_context_observer_pose(&s->context, &seen));
        if (!second) {
            assert_same_picture(&s->straight, &s->context);
        }
        render_context_set_observer(&s->context, NULL);
        context_draw(s, &s->straight);
        context_draw(s, &s->context);
        assert_same_picture(&s->straight, &s->context);
        TEST_ASSERT_EQUAL(second && cull_from() != RASTER_CULL_FROM_CAMERA,
                          render_context_observer_pose(&s->context, &seen));
    }
}

/* ... for each cullview.from: the camera, which then draws no second view, and both spectators. */
static void
test_observer_off_draws_every_view_as_a_straight_run(void) {
    scene_t* s = scene_open();
    for (int from = 0; from < RASTER_CULL_FROM_COUNT; from++) {
        set_cull_from((raster_cull_from_t)from);
        observer_off_draws_every_view_as_a_straight_run(s);
    }
}

/* Two poses within rounding of each other: render_view_pose() rebuilds one
 * from a view's unit axes. */
#define POSE_ROUNDING 1e-4F

/* The same place, facing the same way with the same up. */
static void
assert_pose_near(const transformf_t* expected, const transformf_t* actual) {
    vec3f_expect_within(POSE_ROUNDING, expected->position, actual->position);
    vec3f_expect_within(POSE_ROUNDING, quatf_rotate(expected->rotation, vec3f_unit_z()),
                        quatf_rotate(actual->rotation, vec3f_unit_z()));
    vec3f_expect_within(POSE_ROUNDING, quatf_rotate(expected->rotation, vec3f_unit_y()),
                        quatf_rotate(actual->rotation, vec3f_unit_y()));
}

/* The observer at TOP's or CHASE's own pose draws today's TOP or CHASE
 * picture, whatever cullview.from says: CAMERA, which otherwise draws no
 * second view, or the other spectator. */
static void
test_observer_at_a_spectator_pose_draws_that_spectator(void) {
    scene_t* s = scene_open();
    render_context_set_debug_view(&s->straight, render_context_debug_view_named(CULL_VIEW));
    static const raster_cull_from_t spectators[] = {RASTER_CULL_FROM_TOP, RASTER_CULL_FROM_CHASE};
    for (size_t i = 0; i < sizeof spectators / sizeof spectators[0]; i++) {
        set_cull_from(spectators[i]);
        context_draw(s, &s->straight);
        const raster_cull_t* straight = render_context_debug_view_state(&s->straight);
        const transformf_t pose = straight->spectator_pose;
        transformf_t seen;
        TEST_ASSERT_TRUE(render_context_observer_pose(&s->straight, &seen));
        assert_pose_near(&pose, &seen);

        const raster_cull_from_t others[] = {RASTER_CULL_FROM_CAMERA, spectators[1 - i]};
        for (size_t k = 0; k < sizeof others / sizeof others[0]; k++) {
            set_cull_from(others[k]);
            render_context_set_observer(&s->context, &pose);
            scene_draw(s);
            assert_same_picture(&s->straight, &s->context);
            TEST_ASSERT_TRUE(render_context_observer_pose(&s->context, &seen));
            TEST_ASSERT_EQUAL_MEMORY(&pose, &seen, sizeof pose);
            TEST_ASSERT_EQUAL_MEMORY(&straight->frame, &scene_state(s)->frame, sizeof straight->frame);
        }
    }
}

/* A checkpoint holds no observer, and restoring one keeps the observer set
 * since: it is the person's input, not the frame's state. Taken shaded: the
 * cull view's own state outgrows a device's largest PSRAM block. */
static void
test_a_checkpoint_neither_holds_nor_restores_the_observer(void) {
    scene_t* s = scene_open();
    const transformf_t elsewhere = somewhere_else();
    render_context_set_debug_view(&s->context, RENDER_DEBUG_VIEW_SHADED);
    checkpoint_t held[2] = {{0}}; /* taken with the observer set, then with none */
    render_context_set_observer(&s->context, &elsewhere);
    render_context_checkpoint_capture(&held[0], &s->context);
    render_context_set_observer(&s->context, NULL);
    render_context_checkpoint_capture(&held[1], &s->context);
    TEST_ASSERT_FALSE(held[0].failed || held[1].failed);
    for (int round = 0; round < 2; round++) {
        const checkpoint_block_t* a = checkpoint_find(&held[0], &s->context);
        const checkpoint_block_t* b = checkpoint_find(&held[1], &s->context);
        TEST_ASSERT_EQUAL_size_t(a->bytes, b->bytes);
        TEST_ASSERT_EQUAL_MEMORY(a->data, b->data, a->bytes);
        /* Restored under a live observer, it keeps that one, whole. */
        render_context_set_observer(&s->context, &elsewhere);
        TEST_ASSERT_TRUE(render_context_checkpoint_prepare(&held[1], &s->context));
        TEST_ASSERT_TRUE(render_context_checkpoint_prepare(&held[0], &s->context));
        TEST_ASSERT_TRUE(checkpoint_restore(&held[0]));
        TEST_ASSERT_TRUE(s->context.observing);
        TEST_ASSERT_EQUAL_MEMORY(&elsewhere, &s->context.observer, sizeof elsewhere);
    }
    checkpoint_clear(&held[0]);
    checkpoint_clear(&held[1]);
    render_context_set_debug_view(&s->context, render_context_debug_view_named(CULL_VIEW));
    scene_draw(s);
    transformf_t seen;
    TEST_ASSERT_TRUE(render_context_observer_pose(&s->context, &seen));
    TEST_ASSERT_EQUAL_MEMORY(&elsewhere, &seen, sizeof seen);
}

/* A frame generated after an observed one drew no second view, so reports no pose. */
static void
test_a_generated_frame_reports_no_observer_pose(void) {
    scene_t* s = scene_open();
    const transformf_t elsewhere = somewhere_else();
    render_context_set_frame_generation(&s->context, true);
    render_context_set_observer(&s->context, &elsewhere);
    scene_draw(s);
    transformf_t seen;
    TEST_ASSERT_TRUE(render_context_observer_pose(&s->context, &seen));
    render_context_set_debug_view(&s->context, RENDER_DEBUG_VIEW_SHADED);
    scene_draw(s);
    TEST_ASSERT_TRUE(render_context_frame(&s->context).generated);
    TEST_ASSERT_FALSE(render_context_observer_pose(&s->context, &seen));
}

#ifndef DEVICE_BUILD
/* The screen after the context's overlay over a cleared one, W by H. */
static void
overlay_screen(const render_context_t* context, gfx_color_t* out) {
    draw_clear(0);
    render_context_overlay(context, W, H);
    gfx_surface_t surface;
    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
    for (int y = 0; y < H; y++) {
        memcpy(&out[y * W], gfx_target_color(surface.target, y), sizeof(gfx_color_t) * W);
    }
}

static gfx_color_t* overlays;

static void
release_overlays(void) {
    free(overlays);
    overlays = NULL;
}

/* The overlay draws the camera's frustum as the observer sees it: from
 * cullview.from = CAMERA, which draws no frustum alone, the observer at
 * TOP's pose draws TOP's overlay, and elsewhere another. */
static void
test_overlay_draws_the_frustum_from_the_observer(void) {
    scene_t* s = scene_open();
    TEST_ASSERT_TRUE(gfx_init());
    suite_add_test_cleanup(gfx_reset_for_test);
    suite_add_test_cleanup(release_overlays);

    enum { FROM_TOP, OBSERVED, ELSEWHERE, OVERLAYS };

    overlays = malloc(sizeof(gfx_color_t) * W * H * OVERLAYS);
    TEST_ASSERT_NOT_NULL(overlays);
    gfx_color_t* screen[OVERLAYS];
    for (int i = 0; i < OVERLAYS; i++) {
        screen[i] = &overlays[(size_t)i * W * H];
    }
    TEST_ASSERT_TRUE(render_context_has_overlay(&s->context));

    set_cull_from(RASTER_CULL_FROM_TOP);
    scene_draw(s);
    overlay_screen(&s->context, screen[FROM_TOP]);
    const transformf_t top = scene_state(s)->spectator_pose;
    int frustum = 0;
    for (int p = 0; p < W * H; p++) {
        frustum += screen[FROM_TOP][p] != 0;
    }
    TEST_ASSERT_TRUE(frustum > 0);

    set_cull_from(RASTER_CULL_FROM_CAMERA);
    render_context_set_observer(&s->context, &top);
    scene_draw(s);
    overlay_screen(&s->context, screen[OBSERVED]);
    TEST_ASSERT_EQUAL_MEMORY(screen[FROM_TOP], screen[OBSERVED], sizeof(gfx_color_t) * W * H);

    const transformf_t elsewhere = somewhere_else();
    render_context_set_observer(&s->context, &elsewhere);
    scene_draw(s);
    overlay_screen(&s->context, screen[ELSEWHERE]);
    TEST_ASSERT_TRUE(memcmp(screen[FROM_TOP], screen[ELSEWHERE], sizeof(gfx_color_t) * W * H) != 0);
    const raster_cull_t* state = scene_state(s);
    TEST_ASSERT_EQUAL_FLOAT(state->camera.lens.fx, state->spectator.lens.fx);

    /* Cleared, the camera draws no frustum again: nothing over a cleared screen at level 0. */
    render_context_set_observer(&s->context, NULL);
    scene_draw(s);
    overlay_screen(&s->context, screen[OBSERVED]);
    for (int p = 0; p < W * H; p++) {
        TEST_ASSERT_EQUAL_UINT16(0, screen[OBSERVED][p]);
    }
}
#endif

static void
test_compose_counts_can_describe_the_shaded_half_picture(void) {
    raster_t raster = {.width = W / 2,
                       .height = H / 2,
                       .upscale = {.destination_width = W / 2,
                                   .destination_height = H / 2,
                                   .path = UPSCALE_KEPT,
                                   .filter = UPSCALE_FILTER_SURFACE}};
    raster_cull_frame_t frame = {0};
    raster_cull_frame_compose(&frame, &raster);
    TEST_ASSERT_EQUAL_INT(W / 2, frame.render_width);
    TEST_ASSERT_EQUAL_INT(H / 2, frame.render_height);
    TEST_ASSERT_EQUAL_INT(W / 2, frame.output_width);
    TEST_ASSERT_EQUAL_INT(H / 2, frame.output_height);
    TEST_ASSERT_EQUAL_INT(UPSCALE_KEPT, frame.upscale_path);
    TEST_ASSERT_TRUE(frame.upscale_surface);
    raster.upscale.destination_width = W;
    raster.upscale.destination_height = H;
    raster.upscale.path = UPSCALE_DOUBLED;
    raster_cull_frame_compose(&frame, &raster);
    TEST_ASSERT_EQUAL_INT(W, frame.output_width);
    TEST_ASSERT_EQUAL_INT(H, frame.output_height);
    TEST_ASSERT_EQUAL_INT(UPSCALE_DOUBLED, frame.upscale_path);
    TEST_ASSERT_FALSE(frame.upscale_surface);
}

void
run_raster_cull_view_suite(void) {
    RUN_TEST(test_compose_counts_can_describe_the_shaded_half_picture);
    RUN_TEST(test_counted_cull_returns_the_list_cull_returns);
    RUN_TEST(test_counted_cull_counts_agree_with_the_list);
    RUN_TEST(test_counted_cull_cuts_a_mesh_wholly_outside_the_frustum);
    RUN_TEST(test_uniform_mode_uses_quantized_colour_steps);
    RUN_TEST(test_off_frustum_clusters_and_subtrees_are_separate);
    RUN_TEST(test_counted_pixels_are_clipped_centres_and_zero_without_setup);
    RUN_TEST(test_size_pixels_accepts_either_winding);
    RUN_TEST(test_size_modes_follow_face_and_corner_colours_in_both_windings);
    RUN_TEST(test_triangles_are_sorted_into_exactly_one_fate_each);
    RUN_TEST(test_set_up_per_cluster_counts_set_up_and_clipped_triangles);
    RUN_TEST(test_triangle_sort_adds_to_the_counts_it_is_given);
    RUN_TEST(test_the_draw_fills_the_triangles_the_count_sets_up);
    RUN_TEST(test_per_triangle_writer_tags_the_triangle_that_won_each_pixel);
    RUN_TEST(test_cull_view_gives_each_cluster_its_fate);
    RUN_TEST(test_cull_view_frame_counts_clusters_by_fate);
    RUN_TEST(test_cull_view_frame_counts_the_walk_and_the_sort_of_every_instance);
    RUN_TEST(test_cull_view_triangles_covering_counts_only_triangles_owning_pixels);
    RUN_TEST(test_cull_view_from_the_top_draws_again_with_every_cluster_and_the_same_counts);
    RUN_TEST(test_shaded_cull_view_dashes_culled_borders);
    RUN_TEST(test_cull_view_counts_only_the_level_drawn);
    RUN_TEST(test_cull_view_paints_the_coarse_level_picked_and_not_the_finest);
    RUN_TEST(test_cull_view_counts_each_drawn_triangle_once_after_an_odd_instance);
    RUN_TEST(test_cull_view_has_room_for_the_levels_drawn_not_every_level);
    RUN_TEST(test_observer_off_draws_every_view_as_a_straight_run);
    RUN_TEST(test_observer_at_a_spectator_pose_draws_that_spectator);
    RUN_TEST(test_a_checkpoint_neither_holds_nor_restores_the_observer);
    RUN_TEST(test_a_generated_frame_reports_no_observer_pose);
#ifndef DEVICE_BUILD
    RUN_TEST(test_overlay_draws_the_frustum_from_the_observer);
#endif
}

SUITE_REGISTER(run_raster_cull_view_suite);

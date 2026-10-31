/*
 * Portable suite: the counted cull and triangle sort in r3d_pipeline, the
 * per-triangle span writer, and the "cull" debug view built on them. Every
 * mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "core/tune.h"
#include "image/pixel/rgb565.h"
#include "render/context/render_context.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/view/r3d_pipeline_count.h"
#include "render/view/raster_cull_view.h"

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
    const int expected = r3d_pipeline_cull(&mesh, &lens, plain, pipeline_work(), NULL);
    const int actual = r3d_pipeline_cull_counted(&mesh, &lens, counted, count_walk(), &counts);
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
    const int kept = r3d_pipeline_cull_counted(&mesh, &lens, out, count_walk(), &counts);
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
    (void)r3d_pipeline_cull_counted(&mesh, &lens, out, count_walk(), &twice);
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
    TEST_ASSERT_EQUAL_INT(0, r3d_pipeline_cull_counted(&mesh, &lens, out, count_walk(), &counts));
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
    r3d_pipeline_transform(&mesh, &lens, clusters, 2, cs, NULL);
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
    r3d_pipeline_transform(&mesh, &lens, second, 1, cs, NULL);
    r3d_pipeline_counts_t only = {0};
    uint16_t alone[UNLISTED] = {SENTINEL, SENTINEL, SENTINEL};
    r3d_pipeline_count_triangles(&mesh, &lens, second, 1, cs, &only, alone);
    TEST_ASSERT_EQUAL_UINT16(SENTINEL, alone[0]);
    TEST_ASSERT_EQUAL_UINT16(1, alone[1]);
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
    r3d_pipeline_transform(&mesh, &lens, in_front, 1, cs, NULL);
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
/* The cull view's colour for a culled cluster, as raster_cull_view.c paints it. */
static const uint32_t CULLED_RGB = 0x505050;
/* What scene_draw() clears to. */
static const uint32_t CLEAR_RGB = 0x000000;
static const int TRIANGLES_PER_QUAD = 2;

typedef struct {
    r3d_quad_t quad[SCENE_QUADS];
    r3d_instance_t instance[SCENE_QUADS];
    render_context_t context;
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
        s->instance[i] = (r3d_instance_t){&s->quad[i].mesh, NULL};
    }
    s->quad[SCENE_AWAY].cluster.double_sided = false;
    render_context_set_scale(&s->context, 100);
    render_context_set_debug_view(&s->context, render_context_debug_view_named(CULL_VIEW));
    TEST_ASSERT_NOT_NULL(render_context_debug_view_state(&s->context));
    return s;
}

static void
scene_draw(scene_t* s) {
    const render_view_t view = render_view_fixture_at(&scene_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(&s->context, s->instance, SCENE_QUADS, &view, 0));
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

void
run_raster_cull_view_suite(void) {
    RUN_TEST(test_counted_cull_returns_the_list_cull_returns);
    RUN_TEST(test_counted_cull_counts_agree_with_the_list);
    RUN_TEST(test_counted_cull_cuts_a_mesh_wholly_outside_the_frustum);
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
}

SUITE_REGISTER(run_raster_cull_view_suite);

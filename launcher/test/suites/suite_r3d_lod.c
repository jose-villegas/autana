/*
 * Portable suite: the level pick (render/lod/r3d_lod.h) over a camera that
 * flies in to a levelled mesh and back out. The mesh is the levels alone:
 * four finest groups of two clusters each in a row along x, simplified in
 * pairs into two groups, and those into one terminal cluster, every cluster's
 * box its group's. As the levels bake lays them out, the root's first child
 * holds the finest clusters and its second the coarse ones. Nothing is drawn; the pick, r3d_lod_drawn() and the
 * cluster walk are read directly.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "math/linear/transformf.h"
#include "render/context/render_context.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_pipeline.h"
#include "render/view/r3d_pipeline_count.h"

#define W 64
#define H 64

/* The hierarchy: FINEST_GROUPS groups of CLUSTERS_PER_GROUP finest clusters,
 * each level merging MERGE groups into one, up to a single top group. */
enum {
    FINEST_GROUPS = 4,
    CLUSTERS_PER_GROUP = 2,
    MERGE = 2,
    MIDDLE_GROUPS = FINEST_GROUPS / MERGE,
    GROUPS = FINEST_GROUPS + MIDDLE_GROUPS + 1,
    TOP_GROUP = GROUPS - 1,
    FINEST_CLUSTERS = FINEST_GROUPS * CLUSTERS_PER_GROUP,
    CLUSTERS = FINEST_CLUSTERS + FINEST_GROUPS + MIDDLE_GROUPS + 1,
    /* The root, a leaf of the finest clusters, and a leaf of the coarse ones. */
    FINEST_LEAF = 1,
    COARSE_LEAF = 2,
    NODES = 3,
    /* Ticks: a finest group's radius, the spacing of their centres along x, and its error. */
    FINEST_RADIUS = 4,
    FINEST_SPACING = 8,
    FINEST_ERROR = 1,
    /* Each merge's error grows by this factor. */
    ERROR_GROWTH = 4,
};

/* The fly path: eye distances along -z, far to near, each a step closer. */
#define FAR_EYE       2000.0F
#define NEAR_EYE      10.0F
#define STEP          0.97F
/* Nearest eye at which every cluster's box is on screen. */
#define WHOLE_IN_VIEW 64.0F
#define HALF_FOV_TAN  0.5F
#define NEAR_Z        1.0F
#define TOLERANCE     (LOD.tolerance_px)
#define HYSTERESIS    (LOD.hysteresis_px)
#define COVERAGE_MAX  ((float)LOD.coverage_permille / (float)R3D_LOD_PERMILLE)

/* The engine's LOD settings, which every pick here runs under. */
#define LOD           (RENDER_SETTINGS_DEFAULT.lod)
/* A model stretch, and the share of it a group's radius may still shift at FAR_EYE. */
#define STRETCH       3.0F
#define FAR_SHARE     0.05F

typedef struct {
    r3d_lit_cluster_t clusters[CLUSTERS];
    r3d_lit_node_t nodes[NODES];
    r3d_lit_cluster_lod_t lod[CLUSTERS];
    r3d_lit_lod_group_t groups[GROUPS];
    r3d_lit_mesh_t mesh;
} levelled_t;

static levelled_t* levelled;
static void* work;

static void
release(void) {
    free(levelled);
    free(work);
    levelled = NULL;
    work = NULL;
}

static r3d_lit_cluster_t
group_box(const r3d_lit_lod_group_t* g) {
    const vec3s_t c = g->bounds.centre;
    const int16_t r = g->bounds.radius;
    return (r3d_lit_cluster_t){
        0,   0, 0, 0, {{(int16_t)(c.x - r), (int16_t)(c.y - r), c.z}, {(int16_t)(c.x + r), (int16_t)(c.y + r), c.z}},
        true};
}

/* A group holding `count` groups from `first`: their centres' mean, a radius
 * reaching each with R3D_LOD_ENCLOSE_MARGIN to spare, ERROR_GROWTH times the error. */
static r3d_lit_lod_group_t
merged(const r3d_lit_lod_group_t* inner, int first, int count) {
    int sum = 0;
    for (int i = first; i < first + count; i++) {
        sum += inner[i].bounds.centre.x;
    }
    const int16_t x = (int16_t)(sum / count);
    int radius = 0;
    for (int i = first; i < first + count; i++) {
        const int reach = abs(inner[i].bounds.centre.x - x) + inner[i].bounds.radius + R3D_LOD_ENCLOSE_MARGIN;
        radius = reach > radius ? reach : radius;
    }
    return (r3d_lit_lod_group_t){{{x, 0, 0}, (int16_t)radius}, (uint16_t)(inner[first].error * ERROR_GROWTH), 0};
}

static const r3d_lit_mesh_t*
levelled_mesh(void) {
    levelled = calloc(1, sizeof *levelled);
    suite_set_test_cleanup(release);
    TEST_ASSERT_NOT_NULL(levelled);
    r3d_lit_lod_group_t* g = levelled->groups;
    for (int i = 0; i < FINEST_GROUPS; i++) {
        const int16_t x = (int16_t)((2 * i - (FINEST_GROUPS - 1)) * FINEST_SPACING / 2);
        g[i] = (r3d_lit_lod_group_t){{{x, 0, 0}, FINEST_RADIUS}, FINEST_ERROR, 0};
    }
    for (int i = 0; i < MIDDLE_GROUPS; i++) {
        g[FINEST_GROUPS + i] = merged(g, i * MERGE, MERGE);
    }
    g[TOP_GROUP] = merged(g, FINEST_GROUPS, MIDDLE_GROUPS);
    int c = 0;
    for (int f = 0; f < FINEST_CLUSTERS; f++, c++) {
        levelled->lod[c] = (r3d_lit_cluster_lod_t){R3D_LOD_NONE, (uint16_t)(f / CLUSTERS_PER_GROUP)};
        levelled->clusters[c] = group_box(&g[f / CLUSTERS_PER_GROUP]);
    }
    for (int i = 0; i < FINEST_GROUPS; i++, c++) {
        levelled->lod[c] = (r3d_lit_cluster_lod_t){(uint16_t)i, (uint16_t)(FINEST_GROUPS + i / MERGE)};
        levelled->clusters[c] = group_box(&g[i]);
    }
    for (int i = 0; i < MIDDLE_GROUPS; i++, c++) {
        levelled->lod[c] = (r3d_lit_cluster_lod_t){(uint16_t)(FINEST_GROUPS + i), TOP_GROUP};
        levelled->clusters[c] = group_box(&g[FINEST_GROUPS + i]);
    }
    levelled->lod[c] = (r3d_lit_cluster_lod_t){TOP_GROUP, R3D_LOD_NONE};
    levelled->clusters[c] = group_box(&g[TOP_GROUP]);
    const box3s_t all = group_box(&g[TOP_GROUP]).bounds;
    levelled->nodes[0] = (r3d_lit_node_t){all, FINEST_LEAF, NODES - 1, false};
    levelled->nodes[FINEST_LEAF] = (r3d_lit_node_t){all, 0, FINEST_CLUSTERS, true};
    levelled->nodes[COARSE_LEAF] = (r3d_lit_node_t){all, FINEST_CLUSTERS, CLUSTERS - FINEST_CLUSTERS, true};
    levelled->mesh = (r3d_lit_mesh_t){.clusters = levelled->clusters,
                                      .nodes = levelled->nodes,
                                      .cluster_count = CLUSTERS,
                                      .node_count = NODES,
                                      .position_scale = 1,
                                      .cluster_lod = levelled->lod,
                                      .lod_groups = levelled->groups,
                                      .lod_group_count = GROUPS};
    r3d_lit_mesh_find_levels(&levelled->mesh);
    return &levelled->mesh;
}

static r3d_pipeline_work_t*
pipeline_work(void) {
    if (work == NULL) {
        work = malloc(r3d_pipeline_work_bytes());
    }
    TEST_ASSERT_NOT_NULL(work);
    return work;
}

/* The lens of an eye `distance` back from the mesh along -z, looking +z,
 * the mesh placed by `model` (NULL: as it is). */
static r3d_lens_t
lens_at(float distance, const mat4f_t* model) {
    const fixture_camera_t camera = {{0.0F, 0.0F, -distance}, {0.0F, 0.0F, 1.0F}, HALF_FOV_TAN, NEAR_Z};
    const render_view_t view = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
    r3d_lens_t lens;
    r3d_lens_init(&lens, &view, 1);
    if (model != NULL) {
        r3d_lens_place(&lens, model, 1);
    }
    return lens;
}

/* The steps the path takes in, from FAR_EYE until past NEAR_EYE; it then
 * takes them back out. */
static int
steps_in(void) {
    int n = 0;
    for (float d = FAR_EYE; d > NEAR_EYE; d *= STEP) {
        n++;
    }
    return n;
}

/* The eye distance at pose `p` of 2 * steps_in() poses. */
static float
eye_at(int p, int steps) {
    const int in = p < steps ? p : (2 * steps) - 1 - p;
    return FAR_EYE * powf(STEP, (float)in);
}

/* Whether group `g`'s simplified clusters are drawn, which they are all or
 * none; `next` is the group they simplify into. */
static bool
group_drawn(const r3d_lit_mesh_t* mesh, uint16_t g, const uint32_t* coarse, uint16_t* next) {
    int on = 0, off = 0;
    *next = R3D_LOD_NONE;
    for (int c = 0; c < mesh->cluster_count; c++) {
        if (mesh->cluster_lod[c].own == g) {
            const bool shown = r3d_lod_drawn(mesh, c, coarse);
            on += shown ? 1 : 0;
            off += shown ? 0 : 1;
            *next = mesh->cluster_lod[c].parent;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(on == 0 || off == 0, "a group's clusters switched apart");
    return on > 0;
}

/* How many of the levels covering finest cluster `f` are drawn: f itself,
 * then each group's simplified clusters up the chain. */
static int
levels_drawn(const r3d_lit_mesh_t* mesh, int f, const uint32_t* coarse) {
    int drawn = r3d_lod_drawn(mesh, f, coarse) ? 1 : 0;
    for (uint16_t g = mesh->cluster_lod[f].parent; g != R3D_LOD_NONE;) {
        uint16_t next;
        drawn += group_drawn(mesh, g, coarse, &next) ? 1 : 0;
        g = next;
    }
    return drawn;
}

/* One pose's check on the groups picked there. */
typedef void (*pose_check_t)(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* coarse);

/* Picks the groups at every pose along the path, there and back, runs the
 * check on each, and asserts the path shows every group both coarse and fine. */
static void
pick_along_the_path(const r3d_lit_mesh_t* mesh, pose_check_t check) {
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    const int poses = 2 * steps;
    uint32_t ever_coarse = 0, ever_fine = 0;
    for (int p = 0; p < poses; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
        check(mesh, &lens, coarse);
        ever_coarse |= coarse[0];
        ever_fine |= ~coarse[0];
    }
    const uint32_t every = (1U << GROUPS) - 1U;
    TEST_ASSERT_EQUAL_HEX32(every, ever_coarse & every);
    TEST_ASSERT_EQUAL_HEX32(every, ever_fine & every);
}

/* r3d_lod_list_drawn() of `mesh` under `coarse` lists, in order, the clusters
 * r3d_lod_drawn() keeps one by one. */
static void
expect_the_list_is_what_drawn_keeps(const r3d_lit_mesh_t* mesh, const uint32_t* coarse) {
    uint16_t want[CLUSTERS], listed[CLUSTERS];
    int count = 0;
    for (int c = 0; c < mesh->cluster_count; c++) {
        if (r3d_lod_drawn(mesh, c, coarse)) {
            want[count++] = (uint16_t)c;
        }
    }
    TEST_ASSERT_EQUAL_INT(count, r3d_lod_list_drawn(mesh, coarse, NULL, listed, CLUSTERS));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want, listed, count);
}

static void
lists_what_drawn_keeps(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* coarse) {
    (void)lens;
    expect_the_list_is_what_drawn_keeps(mesh, coarse);
}

/* Every cut along the path, nothing coarse (the list's fast path) and no
 * bits at all, with the levels laid out apart and not. */
static void
test_the_drawn_list_holds_what_each_cluster_s_test_draws(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    pick_along_the_path(mesh, lists_what_drawn_keeps);
    const uint32_t clear[BITS32_WORDS(GROUPS)] = {0};
    expect_the_list_is_what_drawn_keeps(mesh, clear);
    expect_the_list_is_what_drawn_keeps(mesh, NULL);
    r3d_lit_mesh_t mixed = *mesh;
    mixed.lod_finest_root = 0; /* one tree for every level: no first coarse cluster either */
    mixed.lod_coarse_first = 0;
    expect_the_list_is_what_drawn_keeps(&mixed, clear);
    expect_the_list_is_what_drawn_keeps(&mixed, NULL);
    r3d_lit_mesh_t plain = *mesh; /* no levels: every cluster */
    plain.cluster_lod = NULL;
    plain.lod_groups = NULL;
    plain.lod_group_count = 0;
    plain.lod_finest_root = 0;
    plain.lod_coarse_first = 0;
    expect_the_list_is_what_drawn_keeps(&plain, NULL);
    expect_the_list_is_what_drawn_keeps(&plain, clear);
}

static void
parents_coarse_keep_children_coarse(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* coarse) {
    (void)lens;
    for (int c = 0; c < CLUSTERS; c++) {
        const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
        if (lod.own != R3D_LOD_NONE && lod.parent != R3D_LOD_NONE && bits32_get(coarse, lod.parent)) {
            TEST_ASSERT_TRUE(bits32_get(coarse, lod.own));
        }
    }
}

static void
test_a_coarse_group_keeps_every_group_simplified_into_it_coarse(void) {
    pick_along_the_path(levelled_mesh(), parents_coarse_keep_children_coarse);
}

/* With every box on screen, the walk keeps exactly the drawn clusters. */
static void
walk_keeps_the_drawn(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* coarse,
                     r3d_pipeline_work_t* w) {
    uint16_t kept[CLUSTERS];
    const int count = r3d_pipeline_cull(mesh, lens, coarse, kept, w, NULL);
    int drawn = 0;
    for (int c = 0; c < CLUSTERS; c++) {
        drawn += r3d_lod_drawn(mesh, c, coarse) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(drawn, count);
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(r3d_lod_drawn(mesh, kept[i], coarse));
    }
}

static void
test_every_finest_cluster_is_drawn_at_exactly_one_level_along_the_path(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_pipeline_work_t* w = pipeline_work();
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    const int poses = 2 * steps;
    bool drew_top = false, drew_finest = false;
    for (int p = 0; p < poses; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
        for (int f = 0; f < FINEST_CLUSTERS; f++) {
            TEST_ASSERT_EQUAL_INT(1, levels_drawn(mesh, f, coarse));
        }
        drew_top |= r3d_lod_drawn(mesh, CLUSTERS - 1, coarse);
        drew_finest |= r3d_lod_drawn(mesh, 0, coarse);
        if (eye_at(p, steps) >= WHOLE_IN_VIEW) {
            walk_keeps_the_drawn(mesh, &lens, coarse, w);
        }
    }
    TEST_ASSERT_TRUE(drew_top);
    TEST_ASSERT_TRUE(drew_finest);
}

static void
test_a_group_s_bit_follows_its_projected_error_within_the_hysteresis_band(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    const int poses = 2 * steps;
    for (int p = 0; p < poses; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
        for (int g = 0; g < GROUPS; g++) {
            const float px = r3d_lod_group_pixels(mesh, &lens, g);
            const bool capped = r3d_lod_group_coverage(mesh, &lens, g) > COVERAGE_MAX;
            if (bits32_get(coarse, (uint16_t)g)) {
                TEST_ASSERT_TRUE(px <= TOLERANCE + HYSTERESIS);
            } else {
                TEST_ASSERT_TRUE(capped || px > TOLERANCE - HYSTERESIS);
            }
        }
    }
}

/* The eye distance at which flying in from FAR_EYE turns the top group fine,
 * its bits left in `coarse`. */
static float
top_turns_fine(const r3d_lit_mesh_t* mesh, uint32_t* coarse) {
    float d = FAR_EYE;
    r3d_lens_t lens = lens_at(d, NULL);
    r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
    TEST_ASSERT_TRUE(bits32_get(coarse, TOP_GROUP));
    while (bits32_get(coarse, TOP_GROUP)) {
        TEST_ASSERT_TRUE(d > NEAR_EYE);
        d *= STEP;
        lens = lens_at(d, NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
    }
    TEST_ASSERT_TRUE(r3d_lod_group_pixels(mesh, &lens, TOP_GROUP) > TOLERANCE + HYSTERESIS);
    return d;
}

/* One step back is inside the band: the top group holds fine, and a still
 * camera keeps every bit. */
static void
test_a_camera_backing_off_a_switch_by_less_than_the_band_does_not_switch_back(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    const r3d_lens_t lens = lens_at(top_turns_fine(mesh, coarse) / STEP, NULL);
    const float back = r3d_lod_group_pixels(mesh, &lens, TOP_GROUP);
    TEST_ASSERT_TRUE(back > TOLERANCE - HYSTERESIS && back <= TOLERANCE + HYSTERESIS);
    r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
    const uint32_t held = coarse[0];
    TEST_ASSERT_FALSE(bits32_get(coarse, TOP_GROUP));
    r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
    TEST_ASSERT_EQUAL_HEX32(held, coarse[0]);
}

/* Flying in from FAR_EYE, a pick that changes a bit says so, and the same
 * pick again under a still camera changes nothing and says so: what partial
 * redraw reads to redraw every row (render/partial). */
static void
test_a_pick_reports_whether_it_changed_a_bit(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    int changes = 0;
    for (float d = FAR_EYE; d > NEAR_EYE; d *= STEP) {
        const r3d_lens_t lens = lens_at(d, NULL);
        uint32_t before[BITS32_WORDS(GROUPS)];
        memcpy(before, coarse, sizeof before);
        const bool changed = r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
        if (memcmp(before, coarse, sizeof before) != 0) {
            TEST_ASSERT_TRUE(changed);
            changes++;
        }
        TEST_ASSERT_FALSE(r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL));
    }
    TEST_ASSERT_TRUE(changes > 1); /* the fly-in crossed switches */
}

/* Through `lens`, no group projects larger than its parent: neither its
 * error nor its radius. */
static void
assert_parents_project_larger(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens) {
    for (int c = 0; c < CLUSTERS; c++) {
        const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
        if (lod.own != R3D_LOD_NONE && lod.parent != R3D_LOD_NONE) {
            TEST_ASSERT_TRUE(r3d_lod_group_pixels(mesh, lens, lod.own) <= r3d_lod_group_pixels(mesh, lens, lod.parent));
            TEST_ASSERT_TRUE(r3d_lod_group_coverage(mesh, lens, lod.own)
                             <= r3d_lod_group_coverage(mesh, lens, lod.parent));
        }
    }
}

/* With no error anywhere, only the coverage cap keeps a group fine: along
 * the path each group is coarse exactly while it covers at most
 * COVERAGE_MAX, every group is seen both ways, and the closing pass
 * never forces a capped group coarse, its parent covering at least as much. */
static void
coarse_exactly_while_small(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* coarse) {
    for (int g = 0; g < GROUPS; g++) {
        const bool small = r3d_lod_group_coverage(mesh, lens, g) <= COVERAGE_MAX;
        TEST_ASSERT_EQUAL(small, bits32_get(coarse, (uint16_t)g));
    }
}

static void
test_a_group_covering_more_than_the_cap_stays_fine_whatever_its_error(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    for (int g = 0; g < GROUPS; g++) {
        levelled->groups[g].error = 0;
    }
    pick_along_the_path(mesh, coarse_exactly_while_small);
}

/* Knowing its least error lets the pick skip a pose where nothing can be
 * coarse; along the path, there and back, it picks exactly what it picks
 * without, the nearest poses included, where every group is fine. */
static void
test_the_least_error_skips_only_poses_that_pick_nothing_coarse(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_lit_mesh_t bounded = *mesh;
    bounded.lod_error_least = FINEST_ERROR;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    uint32_t skipping[BITS32_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    for (int p = 0; p < 2 * steps; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
        r3d_lod_pick(&bounded, &lens, &LOD, skipping, false, NULL);
        TEST_ASSERT_EQUAL_HEX32_ARRAY(coarse, skipping, BITS32_WORDS(GROUPS));
        if (p == steps) {
            TEST_ASSERT_EQUAL_HEX32(0, skipping[0]);
        }
    }
}

static void
test_a_group_never_projects_larger_than_its_parent_under_a_stretched_turned_model(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const vec3f_t scales[] = {{1.0F, 1.0F, 1.0F}, {3.0F, 0.5F, 1.0F}, {0.25F, 2.0F, 4.0F}};
    const float turns[] = {0.0F, 0.7F, 2.1F};
    const int steps = steps_in();
    for (size_t s = 0; s < sizeof scales / sizeof scales[0]; s++) {
        for (size_t t = 0; t < sizeof turns / sizeof turns[0]; t++) {
            transformf_t pose = transformf_identity();
            transformf_set_rotation(&pose,
                                    quatf_from_axis_angle(vec3f_normalize((vec3f_t){0.3F, 1.0F, 0.2F}), turns[t]));
            transformf_set_scale(&pose, scales[s]);
            const mat4f_t model = transformf_compute_matrix(&pose);
            for (int p = 0; p < steps; p++) {
                const r3d_lens_t lens = lens_at(eye_at(p, steps), &model);
                assert_parents_project_larger(mesh, &lens);
            }
        }
    }
}

/* Far off, where a group's radius no longer counts, its error projects at
 * the model's largest scale: a stretch along x by STRETCH reads STRETCH
 * times larger, never smaller. */
static void
test_a_group_s_error_projects_at_the_model_s_largest_scale(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const vec3f_t stretches[] = {{STRETCH, STRETCH, STRETCH}, {STRETCH, 1.0F / STRETCH, 1.0F}};
    for (size_t s = 0; s < sizeof stretches / sizeof stretches[0]; s++) {
        transformf_t pose = transformf_identity();
        transformf_set_scale(&pose, stretches[s]);
        const mat4f_t model = transformf_compute_matrix(&pose);
        const r3d_lens_t plain = lens_at(FAR_EYE, NULL);
        const r3d_lens_t placed = lens_at(FAR_EYE, &model);
        const float ratio =
            r3d_lod_group_pixels(mesh, &placed, TOP_GROUP) / r3d_lod_group_pixels(mesh, &plain, TOP_GROUP);
        TEST_ASSERT_FLOAT_WITHIN(STRETCH * FAR_SHARE, STRETCH, ratio);
    }
}

/* A finest group out of order, projecting larger than its parent as float
 * rounding far off could make it: the parent turns coarse and takes it along. */
static void
test_a_parent_turning_coarse_takes_an_out_of_order_source_with_it(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    levelled->groups[0].error = UINT16_MAX;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    const r3d_lens_t lens = lens_at(FAR_EYE, NULL);
    TEST_ASSERT_TRUE(r3d_lod_group_pixels(mesh, &lens, 0) > TOLERANCE + HYSTERESIS);
    r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
    TEST_ASSERT_TRUE(bits32_get(coarse, TOP_GROUP));
    TEST_ASSERT_TRUE(bits32_get(coarse, 0));
    for (int f = 0; f < FINEST_CLUSTERS; f++) {
        TEST_ASSERT_EQUAL_INT(1, levels_drawn(mesh, f, coarse));
    }
}

/* Levels not laid out apart: a cluster owning group 0 sits first among the
 * finest ones. The closing pass still visits every cluster, so the parent
 * turning coarse takes its source along as on the laid-out mesh. */
static void
test_the_closing_pass_visits_every_cluster_when_levels_are_not_laid_out_apart(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    levelled->groups[0].error = UINT16_MAX;
    const r3d_lens_t lens = lens_at(FAR_EYE, NULL);
    uint32_t apart[BITS32_WORDS(GROUPS)] = {0};
    r3d_lod_pick(mesh, &lens, &LOD, apart, false, NULL);
    TEST_ASSERT_TRUE(bits32_get(apart, 0));
    const r3d_lit_cluster_lod_t owner = levelled->lod[FINEST_CLUSTERS];
    levelled->lod[FINEST_CLUSTERS] = levelled->lod[0];
    levelled->lod[0] = owner;
    r3d_lit_mesh_find_levels(&levelled->mesh);
    TEST_ASSERT_EQUAL_INT(0, mesh->lod_coarse_first);
    TEST_ASSERT_EQUAL_INT(0, mesh->lod_finest_root);
    uint32_t together[BITS32_WORDS(GROUPS)] = {0};
    r3d_lod_pick(mesh, &lens, &LOD, together, false, NULL);
    TEST_ASSERT_TRUE(bits32_get(together, TOP_GROUP));
    TEST_ASSERT_TRUE(bits32_get(together, 0));
    TEST_ASSERT_EQUAL_HEX32_ARRAY(apart, together, BITS32_WORDS(GROUPS));
}

static void
test_without_bits_a_levelled_mesh_walks_only_its_finest_clusters(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    uint16_t kept[CLUSTERS];
    const int count = r3d_pipeline_cull(mesh, &lens, NULL, kept, pipeline_work(), NULL);
    TEST_ASSERT_EQUAL_INT(FINEST_CLUSTERS, count);
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_HEX16(R3D_LOD_NONE, mesh->cluster_lod[kept[i]].own);
    }
}

static void
test_a_mesh_without_levels_walks_every_cluster_whatever_its_bits(void) {
    const r3d_lit_mesh_t* levels = levelled_mesh();
    r3d_lit_mesh_t plain = *levels;
    plain.cluster_lod = NULL;
    plain.lod_groups = NULL;
    plain.lod_group_count = 0;
    plain.lod_finest_root = 0;
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    r3d_pipeline_work_t* w = pipeline_work();
    uint32_t all_coarse[BITS32_WORDS(GROUPS)];
    memset(all_coarse, 0xFF, sizeof all_coarse);
    uint16_t none[CLUSTERS], with[CLUSTERS];
    const int count = r3d_pipeline_cull(&plain, &lens, NULL, none, w, NULL);
    TEST_ASSERT_EQUAL_INT(CLUSTERS, count);
    TEST_ASSERT_EQUAL_INT(count, r3d_pipeline_cull(&plain, &lens, all_coarse, with, w, NULL));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(none, with, count);
}

static void
test_levels_laid_out_apart_start_the_walk_at_the_finest_clusters_tree(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    TEST_ASSERT_EQUAL_INT(FINEST_LEAF, mesh->lod_finest_root);
    TEST_ASSERT_EQUAL_INT(FINEST_CLUSTERS, mesh->lod_coarse_first);
}

/* A finest cluster under the coarse child: the walk keeps starting at the
 * root, so no frame loses it. */
static void
test_a_root_whose_children_mix_finest_and_coarse_clusters_keeps_the_walk_at_the_root(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    levelled->nodes[FINEST_LEAF].count--;
    levelled->nodes[COARSE_LEAF].first--;
    levelled->nodes[COARSE_LEAF].count++;
    r3d_lit_mesh_find_levels(&levelled->mesh);
    TEST_ASSERT_EQUAL_INT(0, mesh->lod_finest_root);
}

/* Coarse clusters out of their groups' order cannot be found by group: the
 * walk keeps starting at the root. */
static void
test_coarse_clusters_out_of_group_order_keep_the_walk_at_the_root(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const r3d_lit_cluster_lod_t first = levelled->lod[FINEST_CLUSTERS];
    levelled->lod[FINEST_CLUSTERS] = levelled->lod[CLUSTERS - 1];
    levelled->lod[CLUSTERS - 1] = first;
    r3d_lit_mesh_find_levels(&levelled->mesh);
    TEST_ASSERT_EQUAL_INT(0, mesh->lod_finest_root);
}

static int
compare_cluster(const void* a, const void* b) {
    return (int)*(const uint16_t*)a - (int)*(const uint16_t*)b;
}

/* Along the path, coarse groups and all, the walk of level 0 and the cut
 * taken from its groups keeps what the walk of the whole tree keeps. */
static void
test_the_finest_tree_walk_and_the_cut_keep_what_the_whole_walk_keeps(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_pipeline_work_t* w = pipeline_work();
    r3d_lit_mesh_t whole = *mesh;
    whole.lod_finest_root = 0;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    bool cut_drawn = false;
    for (int p = 0; p < 2 * steps; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, NULL);
        uint16_t apart[CLUSTERS], together[CLUSTERS];
        const int count = r3d_pipeline_cull(mesh, &lens, coarse, apart, w, NULL);
        TEST_ASSERT_EQUAL_INT(count, r3d_pipeline_cull(&whole, &lens, coarse, together, w, NULL));
        qsort(apart, (size_t)count, sizeof apart[0], compare_cluster);
        qsort(together, (size_t)count, sizeof together[0], compare_cluster);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(together, apart, count);
        cut_drawn |= count > 0 && apart[count - 1] >= FINEST_CLUSTERS;
    }
    TEST_ASSERT_TRUE(cut_drawn);
}

/* Instances of the levelled mesh on the eye's axis, each with its own model,
 * world box and coarse bits, all clear. */
enum { PLACED_MAX = 2 };

typedef struct {
    mat4f_t models[PLACED_MAX];
    box3f_t boxes[PLACED_MAX];
    uint32_t coarse[PLACED_MAX][R3D_LOD_STATE_WORDS(GROUPS)];
    r3d_instance_t instances[PLACED_MAX];
} placed_t;

/* Places `count` instances of `mesh` in `p`, instance i at depth[i]. */
static void
place(placed_t* p, const r3d_lit_mesh_t* mesh, int count, const float* depth) {
    memset(p->coarse, 0, sizeof p->coarse);
    for (int i = 0; i < count; i++) {
        transformf_t pose = transformf_identity();
        transformf_set_position(&pose, (vec3f_t){0.0F, 0.0F, depth[i]});
        p->models[i] = transformf_compute_matrix(&pose);
        p->boxes[i] = r3d_lit_mesh_world_bounds(mesh, &p->models[i]);
        p->instances[i] = (r3d_instance_t){mesh, &p->models[i], &p->boxes[i], p->coarse[i]};
    }
}

/* The view down the axis place() puts instances on. */
static render_view_t
axis_view(void) {
    const fixture_camera_t camera = {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, HALF_FOV_TAN, NEAR_Z};
    return render_view_fixture_at(&camera, (viewport_t){W, H, 0});
}

/* Which of `p`'s `count` instances have their top group coarse: bit i for instance i. */
static unsigned
top_coarse(const placed_t* p, int count) {
    unsigned coarse = 0;
    for (int i = 0; i < count; i++) {
        coarse |= bits32_get(p->coarse[i], TOP_GROUP) ? 1U << i : 0U;
    }
    return coarse;
}

/* Settings that spread the pick over `every` frames. */
static render_settings_t
spread_over(int every) {
    render_settings_t settings = RENDER_SETTINGS_DEFAULT;
    settings.lod.pick_every = every;
    return settings;
}

/* A frame's pick leaves an instance culled whole as it was and picks one in
 * view: both far from the eye, where a pick sets the top group coarse. */
static void
test_the_pick_keeps_the_bits_of_an_instance_culled_whole(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();

    enum { IN_VIEW, BEHIND, INSTANCES };

    const float depth[INSTANCES] = {FAR_EYE, -FAR_EYE};
    placed_t* p = malloc(sizeof *p);
    TEST_ASSERT_NOT_NULL(p);
    place(p, mesh, INSTANCES, depth);
    raster_t raster = {
        .instances = p->instances, .instance_count = INSTANCES, .width = W, .height = H, .picks_levels = true};
    raster.scratch = malloc(raster_scratch_bytes(&raster));
    TEST_ASSERT_NOT_NULL(raster.scratch);
    const render_view_t view = axis_view();
    (void)raster_census(&raster, &view);
    free(raster.scratch);
    const unsigned coarse = top_coarse(p, INSTANCES);
    const uint32_t behind = p->coarse[BEHIND][0];
    free(p);
    TEST_ASSERT_EQUAL_HEX(1U << IN_VIEW, coarse);
    TEST_ASSERT_EQUAL_HEX32(0, behind);
}

/* The scratch picked_in_round() draws in. */
static size_t
picked_scratch_bytes(const r3d_lit_mesh_t* mesh) {
    r3d_instance_t instances[PLACED_MAX] = {{mesh, NULL, NULL, NULL}, {mesh, NULL, NULL, NULL}};
    const raster_t raster = {.instances = instances, .instance_count = PLACED_MAX, .width = W, .height = H};
    return raster_scratch_bytes(&raster);
}

/* PLACED_MAX instances far down the axis, censused with the pick spread over
 * `every` frames at round `round`; `picks` as raster_t.picks_levels. Returns
 * top_coarse(). `p` and `scratch` are the test's. */
static unsigned
picked_in_round(placed_t* p, const r3d_lit_mesh_t* mesh, void* scratch, bool picks, int every, uint32_t round) {
    const float depth[PLACED_MAX] = {FAR_EYE, FAR_EYE};
    place(p, mesh, PLACED_MAX, depth);
    const render_settings_t settings = spread_over(every);
    const raster_t raster = {.instances = p->instances,
                             .instance_count = PLACED_MAX,
                             .width = W,
                             .height = H,
                             .picks_levels = picks,
                             .pick_round = round,
                             .scratch = scratch,
                             .settings = &settings};
    const render_view_t view = axis_view();
    (void)raster_census(&raster, &view);
    return top_coarse(p, PLACED_MAX);
}

/* A second view of the frame draws the levels the frame picked: with
 * picks_levels off its cull sets no bit. */
static void
test_a_cull_that_does_not_pick_levels_leaves_every_bit(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    placed_t* p = malloc(sizeof *p);
    void* scratch = malloc(picked_scratch_bytes(mesh));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(scratch);
    const unsigned picking = picked_in_round(p, mesh, scratch, true, 1, 0);
    const unsigned not_picking = picked_in_round(p, mesh, scratch, false, 1, 0);
    free(scratch);
    free(p);
    TEST_ASSERT_EQUAL_HEX(0x3, picking);
    TEST_ASSERT_EQUAL_HEX(0x0, not_picking);
}

/* Spread over two frames, each frame picks every other instance, in turn. */
static void
test_a_pick_spread_over_frames_picks_each_instance_in_its_own_frame(void) {
    enum { ROUNDS = 4 };

    const r3d_lit_mesh_t* mesh = levelled_mesh();
    placed_t* p = malloc(sizeof *p);
    void* scratch = malloc(picked_scratch_bytes(mesh));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(scratch);
    unsigned rounds[ROUNDS];
    for (uint32_t round = 0; round < ROUNDS; round++) {
        rounds[round] = picked_in_round(p, mesh, scratch, true, 2, round);
    }
    free(scratch);
    free(p);
    /* Round 0, a scene's or view's first frame, picks every instance. */
    TEST_ASSERT_EQUAL_HEX(0x3, rounds[0]);
    TEST_ASSERT_EQUAL_HEX(0x2, rounds[1]);
    TEST_ASSERT_EQUAL_HEX(0x1, rounds[2]);
    TEST_ASSERT_EQUAL_HEX(0x2, rounds[3]);
}

static render_context_t* spread_context;
static placed_t* spread_placed;
static uint16_t* spread_destination; /* W by H, what a partial frame composes into */

static void
release_spread_context(void) {
    if (spread_context != NULL) {
        render_context_release(spread_context);
        free(spread_context);
        spread_context = NULL;
    }
    free(spread_placed);
    free(spread_destination);
    spread_placed = NULL;
    spread_destination = NULL;
    release();
}

/* Two levelled instances at FAR_EYE in a context whose pick is spread over
 * `every` frames, partial redraw as asked, and the destination it composes
 * into; all freed by the cleanup. */
static void
open_spread(int every, bool partial) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    suite_set_test_cleanup(release_spread_context); /* after the mesh's own: it frees the mesh too */
    spread_placed = malloc(sizeof *spread_placed);
    spread_context = malloc(sizeof *spread_context);
    spread_destination = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(spread_placed);
    TEST_ASSERT_NOT_NULL(spread_context);
    TEST_ASSERT_NOT_NULL(spread_destination);
    const float depth[PLACED_MAX] = {FAR_EYE, FAR_EYE};
    place(spread_placed, mesh, PLACED_MAX, depth);
    *spread_context = RENDER_CONTEXT_DEFAULT;
    render_context_set_scale(spread_context, 100);
    render_settings_t settings = spread_over(every);
    settings.partial_redraw = partial;
    render_context_set_settings(spread_context, &settings);
}

/* One frame of the spread context through `view`, drawn and composed. */
static render_context_frame_t
spread_frame(const render_view_t* view) {
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, view, 0));
    (void)render_context_compose(spread_context, spread_destination, NULL, NULL);
    return render_context_frame(spread_context);
}

/* A render context's first frame picks every instance, however far the pick
 * is spread; the frames after it pick their share, and forgetting the scene
 * makes the next frame a first frame again. */
static void
test_a_context_s_first_frame_picks_every_instance_of_a_spread_pick(void) {
    enum { EVERY = 4 };

    open_spread(EVERY, false);
    const render_view_t view = axis_view();
    (void)spread_frame(&view);
    const unsigned first = top_coarse(spread_placed, PLACED_MAX);
    memset(spread_placed->coarse, 0, sizeof spread_placed->coarse);
    /* Round 1 of 4: neither instance's share. */
    (void)spread_frame(&view);
    const unsigned second = top_coarse(spread_placed, PLACED_MAX);
    render_context_forget(spread_context);
    (void)spread_frame(&view);
    TEST_ASSERT_EQUAL_HEX(0x3, first);
    TEST_ASSERT_EQUAL_HEX(0x0, second);
    TEST_ASSERT_EQUAL_HEX(0x3, top_coarse(spread_placed, PLACED_MAX));
}

/* Partial redraw with the pick spread over two frames: the camera flies in
 * to where the top group turns fine, and that frame picks only instance 1.
 * The next frame nothing moves, yet instance 0's pick is owed: it switches
 * cut, and every row redraws. Then nothing is owed and nothing redraws. */
static void
test_a_still_instance_that_switches_cut_on_a_still_frame_redraws_every_row(void) {
    enum { EVERY = 2 };

    open_spread(EVERY, true);
    const render_view_t far = axis_view();
    (void)spread_frame(&far);
    TEST_ASSERT_EQUAL_HEX(0x3, top_coarse(spread_placed, PLACED_MAX));
    const fixture_camera_t camera = {{0.0F, 0.0F, FAR_EYE - WHOLE_IN_VIEW}, {0.0F, 0.0F, 1.0F}, HALF_FOV_TAN, NEAR_Z};
    const render_view_t near = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
    (void)spread_frame(&near);
    TEST_ASSERT_EQUAL_HEX(0x1, top_coarse(spread_placed, PLACED_MAX)); /* instance 0 not yet picked */
    render_context_set_changed(spread_context, NULL, 0);
    const render_context_frame_t owed = spread_frame(&near);
    TEST_ASSERT_EQUAL_HEX(0x0, top_coarse(spread_placed, PLACED_MAX));
    TEST_ASSERT_FALSE(owed.unchanged);
    TEST_ASSERT_EQUAL_INT(H, owed.rows_end - owed.rows_first);
    render_context_set_changed(spread_context, NULL, 0);
    TEST_ASSERT_TRUE(spread_frame(&near).unchanged);
}

/* The counted walk tells the coarse clusters it keeps from the finest: none
 * with every group fine, all with the top group coarse. */
static void
test_the_counted_walk_counts_the_coarse_clusters_it_keeps(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_pipeline_count_walk_t* walk = malloc(sizeof *walk);
    TEST_ASSERT_NOT_NULL(walk);
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    uint16_t out[CLUSTERS];
    r3d_pipeline_counts_t fine = {0};
    TEST_ASSERT_EQUAL_INT(FINEST_CLUSTERS, r3d_pipeline_cull_counted(mesh, &lens, coarse, NULL, out, walk, &fine));
    TEST_ASSERT_EQUAL_INT(0, fine.clusters_coarse);
    coarse[0] = (1U << GROUPS) - 1U;
    r3d_pipeline_counts_t top = {0};
    TEST_ASSERT_EQUAL_INT(1, r3d_pipeline_cull_counted(mesh, &lens, coarse, NULL, out, walk, &top));
    TEST_ASSERT_EQUAL_INT(1, top.clusters_coarse);
    free(walk);
}

/* Geomorph weights (r3d_lod_morph_weight()). A weight is read where a group's
 * error or radius projects a chosen amount, which the eye is searched for: the
 * measure falls as the eye backs away. */
typedef float (*group_measure_t)(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group);

enum {
    SEARCH_ROUNDS = 80,
    /* The group a weight is read from, and the cluster whose parent it is. */
    WEIGHT_GROUP = 0,
    WEIGHT_CLUSTER = 0,
    /* Rounding the weight to a count may land one off the ideal. */
    WEIGHT_SLACK = 1,
};

#define SEARCH_FAR   1.0e6F
/* Past a ramp's end by this share of the ramp, a weight is none whatever the float rounding. */
#define PAST_RAMP    0.01F
/* A share of a span that stays clear of its far end by more than any rounding. */
#define SHARE_INSIDE (1.0F - PAST_RAMP)

/* A radius large enough that one error unit projects well under a pixel where the coverage ramps,
 * and two errors, the first within the leave and the second inside the ramp past it. */
enum { WIDE_RADIUS = 160, ERROR_SMALL = 20 };

/* Shares of a ramp, from whole (0) to none (1), the weights are read at. */
#define SHARE_QUARTER        0.25F
#define SHARE_HALF           0.5F
#define SHARE_THREE_QUARTERS 0.75F

/* A mesh whose vertices carry no morph data but whose pointer says it has some. */
static const int8_t NO_DELTAS[1][3] = {{0, 0, 0}};

static float
eye_for(group_measure_t measure, const r3d_lit_mesh_t* mesh, int group, float target) {
    float near_eye = 0.0F;
    float far_eye = SEARCH_FAR;
    for (int i = 0; i < SEARCH_ROUNDS; i++) {
        const float mid = 0.5F * (near_eye + far_eye);
        const r3d_lens_t lens = lens_at(mid, NULL);
        if (measure(mesh, &lens, group) > target) {
            near_eye = mid;
        } else {
            far_eye = mid;
        }
    }
    return far_eye;
}

static void
blend_state_from_bits(uint32_t* state, const r3d_lit_mesh_t* mesh, const uint32_t* coarse, const uint32_t* arrived) {
    const size_t bytes = sizeof *state * BITS32_WORDS(mesh->lod_group_count);
    memset(state, 0, bytes);
    if (coarse != NULL) {
        memcpy(state, coarse, bytes);
    }
    if (arrived != NULL) {
        memcpy(state + BITS32_WORDS(mesh->lod_group_count), arrived, bytes * (R3D_LOD_STATE_SETS - 1));
    } else {
        memset(state + BITS32_WORDS(mesh->lod_group_count), 0xFF, bytes * (R3D_LOD_STATE_SETS - 1));
    }
}

static int
weight_at(const r3d_lit_mesh_t* mesh, float eye, int cluster, bool enabled) {
    const r3d_lens_t lens = lens_at(eye, NULL);
    r3d_lod_morph_t morph;
    uint32_t state_morph[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_morph, mesh, NULL, NULL);
    r3d_lod_morph_init(&morph, mesh, &lens, &LOD, state_morph, enabled ? RENDER_LOD_BLEND_ON : RENDER_LOD_BLEND_SWITCH);
    return r3d_lod_morph_weight(&morph, mesh, cluster);
}

/* The weight a ramp from whole to none gives `share` of the way down it. */
static int
ramp_weight_at(float share) {
    return (int)((1.0F - share) * (float)R3D_LOD_MORPH_ONE + 0.5F);
}

static float
leave_px(void) {
    return TOLERANCE + HYSTERESIS;
}

static float
error_eye(const r3d_lit_mesh_t* mesh, float px) {
    return eye_for(r3d_lod_group_pixels, mesh, WEIGHT_GROUP, px);
}

static float
cover_eye(const r3d_lit_mesh_t* mesh, float share_of_height) {
    return eye_for(r3d_lod_group_coverage, mesh, WEIGHT_GROUP, share_of_height);
}

static levelled_t*
morphing_levels(void) {
    levelled_mesh();
    levelled->mesh.morph_delta = NO_DELTAS;
    return levelled;
}

static void
test_the_morph_weight_falls_from_whole_to_none_over_a_pixel_past_the_pick_leave_error(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    const float shares[] = {SHARE_QUARTER, SHARE_HALF, SHARE_THREE_QUARTERS};
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_at(mesh, error_eye(mesh, leave_px() * 0.5F), WEIGHT_CLUSTER, true));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE,
                          weight_at(mesh, error_eye(mesh, leave_px() - PAST_RAMP), WEIGHT_CLUSTER, true));
    for (size_t i = 0; i < sizeof shares / sizeof shares[0]; i++) {
        const float px = leave_px() + (shares[i] * LOD.blend_px);
        TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, ramp_weight_at(shares[i]),
                               weight_at(mesh, error_eye(mesh, px), WEIGHT_CLUSTER, true));
    }
    const float past = leave_px() + (LOD.blend_px * (1.0F + PAST_RAMP));
    TEST_ASSERT_EQUAL_INT(0, weight_at(mesh, error_eye(mesh, past), WEIGHT_CLUSTER, true));
    TEST_ASSERT_EQUAL_INT(0, weight_at(mesh, error_eye(mesh, leave_px() * 4.0F), WEIGHT_CLUSTER, true));
}

/* A view past one output pixel's leave but inside the scaled one's. */
static void
test_the_morph_thresholds_scale_with_the_pixels_per_output_pixel(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    const float scale = 2.0F;
    const r3d_lens_t lens = lens_at(error_eye(mesh, scale * leave_px() * 0.75F), NULL);
    r3d_lod_morph_t one, two;
    uint32_t state_one[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_one, mesh, NULL, NULL);
    r3d_lod_morph_init(&one, mesh, &lens, &LOD, state_one, RENDER_LOD_BLEND_ON);
    render_lod_settings_t scaled = LOD;
    scaled.tolerance_px *= scale;
    scaled.hysteresis_px *= scale;
    scaled.blend_px *= scale;
    uint32_t state_two[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_two, mesh, NULL, NULL);
    r3d_lod_morph_init(&two, mesh, &lens, &scaled, state_two, RENDER_LOD_BLEND_ON);
    TEST_ASSERT_EQUAL_INT(0, r3d_lod_morph_weight(&one, mesh, WEIGHT_CLUSTER));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, r3d_lod_morph_weight(&two, mesh, WEIGHT_CLUSTER));
}

static void
test_the_morph_weight_falls_over_a_quarter_of_the_cap_above_the_coverage_cap(void) {
    morphing_levels()->groups[WEIGHT_GROUP].error = 0;
    const r3d_lit_mesh_t* mesh = &levelled->mesh;
    const float shares[] = {SHARE_QUARTER, SHARE_HALF, SHARE_THREE_QUARTERS};
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE,
                          weight_at(mesh, cover_eye(mesh, COVERAGE_MAX * 0.5F), WEIGHT_CLUSTER, true));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE,
                          weight_at(mesh, cover_eye(mesh, COVERAGE_MAX * (1.0F - PAST_RAMP)), WEIGHT_CLUSTER, true));
    for (size_t i = 0; i < sizeof shares / sizeof shares[0]; i++) {
        const float cover = COVERAGE_MAX * (1.0F + (shares[i] * R3D_LOD_MORPH_COVER_RAMP));
        TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, ramp_weight_at(shares[i]),
                               weight_at(mesh, cover_eye(mesh, cover), WEIGHT_CLUSTER, true));
    }
    const float past = COVERAGE_MAX * (1.0F + (R3D_LOD_MORPH_COVER_RAMP * (1.0F + PAST_RAMP)));
    TEST_ASSERT_EQUAL_INT(0, weight_at(mesh, cover_eye(mesh, past), WEIGHT_CLUSTER, true));
}

/* A small error leaves the coverage in charge; a larger one, projecting into its ramp
 * and weighing less, takes over. */
static void
test_the_lesser_of_the_error_and_coverage_weights_is_drawn(void) {
    morphing_levels()->groups[WEIGHT_GROUP].bounds.radius = WIDE_RADIUS;
    const r3d_lit_mesh_t* mesh = &levelled->mesh;
    const float eye = cover_eye(mesh, COVERAGE_MAX * (1.0F + (SHARE_HALF * R3D_LOD_MORPH_COVER_RAMP)));
    const r3d_lens_t lens = lens_at(eye, NULL);
    const int by_cover = ramp_weight_at(SHARE_HALF);
    levelled->groups[WEIGHT_GROUP].error = ERROR_SMALL;
    TEST_ASSERT_TRUE(r3d_lod_group_pixels(mesh, &lens, WEIGHT_GROUP) <= leave_px());
    TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, by_cover, weight_at(mesh, eye, WEIGHT_CLUSTER, true));
    /* The least error past the ramp's middle, where its weight drops below the coverage's. */
    while (r3d_lod_group_pixels(mesh, &lens, WEIGHT_GROUP) <= leave_px() + (SHARE_HALF * LOD.blend_px)) {
        levelled->groups[WEIGHT_GROUP].error++;
    }
    const float px = r3d_lod_group_pixels(mesh, &lens, WEIGHT_GROUP);
    TEST_ASSERT_TRUE(px > leave_px() && px < leave_px() + LOD.blend_px);
    const int by_error = ramp_weight_at((px - leave_px()) / LOD.blend_px);
    TEST_ASSERT_TRUE(by_error < by_cover);
    TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, by_error, weight_at(mesh, eye, WEIGHT_CLUSTER, true));
}

static void
test_there_is_no_morph_weight_without_a_parent_without_morph_data_or_when_disabled(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    const float eye = error_eye(mesh, leave_px() * 0.5F);
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_at(mesh, eye, WEIGHT_CLUSTER, true));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_NONE, mesh->cluster_lod[CLUSTERS - 1].parent);
    TEST_ASSERT_EQUAL_INT(0, weight_at(mesh, eye, CLUSTERS - 1, true));
    TEST_ASSERT_EQUAL_INT(0, weight_at(mesh, eye, WEIGHT_CLUSTER, false));
    levelled->mesh.morph_delta = NULL;
    TEST_ASSERT_EQUAL_INT(0, weight_at(mesh, eye, WEIGHT_CLUSTER, true));
}

/* The arrived bits, then the blending bits, as the pick keeps them. */
enum { BLEND_WORDS = (R3D_LOD_STATE_SETS - 1) * BITS32_WORDS(GROUPS), BLENDING_WORD = BITS32_WORDS(GROUPS) };

static float
enter_px(void) {
    return TOLERANCE - HYSTERESIS;
}

/* Picks at the eye where group WEIGHT_GROUP's error projects `px`, keeping `coarse` and `arrived`. */
static void
pick_at_px(const r3d_lit_mesh_t* mesh, float px, uint32_t* coarse, uint32_t* arrived) {
    const r3d_lens_t lens = lens_at(error_eye(mesh, px), NULL);
    uint32_t state[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state, mesh, coarse, arrived);
    r3d_lod_pick(mesh, &lens, &LOD, state, arrived != NULL, NULL);
    memcpy(coarse, state, sizeof *state * BITS32_WORDS(GROUPS));
    if (arrived != NULL) {
        memcpy(arrived, r3d_lod_arrived(state, GROUPS), sizeof *state * BLEND_WORDS);
    }
}

static int
weight_arrived_at(const r3d_lit_mesh_t* mesh, float px, const uint32_t* arrived) {
    const r3d_lens_t lens = lens_at(error_eye(mesh, px), NULL);
    r3d_lod_morph_t morph;
    uint32_t state_morph[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_morph, mesh, NULL, arrived);
    r3d_lod_morph_init(&morph, mesh, &lens, &LOD, state_morph, RENDER_LOD_BLEND_ON);
    return r3d_lod_morph_weight(&morph, mesh, WEIGHT_CLUSTER);
}

static void
test_a_group_turning_fine_has_its_arrived_bit_set(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    uint32_t arrived[BLEND_WORDS] = {0};
    pick_at_px(mesh, enter_px() * SHARE_HALF, coarse, arrived);
    TEST_ASSERT_TRUE(bits32_get(coarse, WEIGHT_GROUP));
    TEST_ASSERT_FALSE(bits32_get(arrived, WEIGHT_GROUP));
    pick_at_px(mesh, leave_px() + (LOD.blend_px * SHARE_HALF), coarse, arrived);
    TEST_ASSERT_FALSE(bits32_get(coarse, WEIGHT_GROUP));
    TEST_ASSERT_TRUE(bits32_get(arrived, WEIGHT_GROUP));
}

static void
test_the_arrived_bit_clears_once_the_error_projects_past_leave_and_the_morph_ramp(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    uint32_t arrived[BLEND_WORDS] = {0};
    pick_at_px(mesh, enter_px() * SHARE_HALF, coarse, arrived);
    pick_at_px(mesh, leave_px() + (LOD.blend_px * SHARE_HALF), coarse, arrived);
    TEST_ASSERT_TRUE(bits32_get(arrived, WEIGHT_GROUP));
    pick_at_px(mesh, leave_px() + (LOD.blend_px * (1.0F - PAST_RAMP)), coarse, arrived);
    TEST_ASSERT_TRUE(bits32_get(arrived, WEIGHT_GROUP));
    pick_at_px(mesh, leave_px() + (LOD.blend_px * (1.0F + PAST_RAMP)), coarse, arrived);
    TEST_ASSERT_FALSE(bits32_get(coarse, WEIGHT_GROUP));
    TEST_ASSERT_FALSE(bits32_get(arrived, WEIGHT_GROUP));
}

static void
test_a_coarse_group_never_has_its_arrived_bit(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    uint32_t arrived[BLEND_WORDS] = {0};
    arrived[0] = 1U << WEIGHT_GROUP;
    pick_at_px(mesh, enter_px() * SHARE_HALF, coarse, arrived);
    TEST_ASSERT_TRUE(bits32_get(coarse, WEIGHT_GROUP));
    TEST_ASSERT_FALSE(bits32_get(arrived, WEIGHT_GROUP));
}

static void
test_an_arrived_group_blends_whole_up_to_leave_and_any_other_from_enter(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t arrived[BLEND_WORDS] = {0};
    uint32_t none[BLEND_WORDS] = {0};
    arrived[0] = 1U << WEIGHT_GROUP;
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    none[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    TEST_ASSERT_TRUE(leave_px() - enter_px() > LOD.blend_px);
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, enter_px(), arrived));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, enter_px(), none));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, leave_px(), arrived));
    TEST_ASSERT_EQUAL_INT(0, weight_arrived_at(mesh, leave_px(), none));
    const float half = enter_px() + (LOD.blend_px * SHARE_HALF);
    TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, ramp_weight_at(SHARE_HALF), weight_arrived_at(mesh, half, none));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, half, arrived));
    const float past = leave_px() + (LOD.blend_px * SHARE_HALF);
    TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, ramp_weight_at(SHARE_HALF), weight_arrived_at(mesh, past, arrived));
}

/* A pick that flips only an arrived or blending bit reports a change, so the
 * ramping group's rows redraw (render/partial) with the camera still; picked
 * again at the same eye, nothing changes. */
static void
test_a_pick_flipping_only_a_blend_bit_reports_a_change(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    const float ramp_end = LOD.blend_px * (1.0F + R3D_LOD_BLEND_LEAD);
    const float walk[] = {enter_px() * SHARE_HALF, leave_px() + (LOD.blend_px * SHARE_HALF),
                          leave_px() + (ramp_end * SHARE_HALF), leave_px() + (ramp_end * (1.0F + PAST_RAMP)),
                          enter_px() * SHARE_HALF};
    uint32_t state[R3D_LOD_STATE_WORDS(GROUPS)] = {0};
    uint32_t* const coarse = state;
    int blend_only = 0;
    for (size_t i = 0; i < sizeof walk / sizeof walk[0]; i++) {
        const r3d_lens_t lens = lens_at(error_eye(mesh, walk[i]), NULL);
        uint32_t before[R3D_LOD_STATE_WORDS(GROUPS)];
        memcpy(before, state, sizeof before);
        const bool changed = r3d_lod_pick(mesh, &lens, &LOD, coarse, true, NULL);
        TEST_ASSERT_EQUAL(memcmp(before, state, sizeof before) != 0, changed);
        blend_only += changed && memcmp(before, state, sizeof coarse[0] * BITS32_WORDS(GROUPS)) == 0;
        TEST_ASSERT_FALSE(r3d_lod_pick(mesh, &lens, &LOD, coarse, true, NULL));
    }
    TEST_ASSERT_TRUE(blend_only > 0); /* the walk past the ramp flipped blend bits alone */
}

/* Along a walk in and back out over the switches, the pick's blending bit is
 * set wherever the blend, measured without it, weighs more than none, and
 * clear on a coarse group or one whose error projects past its ramp. */
static void
test_the_blending_bit_is_set_where_the_blend_weighs_something(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    uint32_t arrived[BLEND_WORDS] = {0};

    enum { STEPS = 64 };

    /* The bit leads the weight by R3D_LOD_BLEND_LEAD ramps. */
    const float ahead = LOD.blend_px * (1.0F + R3D_LOD_BLEND_LEAD);
    const float far = leave_px() + ahead + (LOD.blend_px * PAST_RAMP);
    for (int i = 0; i <= 2 * STEPS; i++) {
        const float share = (float)(i <= STEPS ? i : (2 * STEPS) - i) / (float)STEPS;
        const float px = far - (share * (far - (enter_px() * SHARE_HALF)));
        pick_at_px(mesh, px, coarse, arrived);
        uint32_t unread[BLEND_WORDS];
        memcpy(unread, arrived, sizeof unread);
        memset(&unread[BLENDING_WORD], 0xFF, sizeof(uint32_t) * BITS32_WORDS(GROUPS));
        const int weight = bits32_get(coarse, WEIGHT_GROUP) ? 0 : weight_arrived_at(mesh, px, unread);
        const bool blending = bits32_get(&arrived[BLENDING_WORD], WEIGHT_GROUP);
        const float whole = bits32_get(arrived, WEIGHT_GROUP) ? leave_px() : enter_px();
        const bool coarse_now = bits32_get(coarse, WEIGHT_GROUP);
        if (weight > 0 || (!coarse_now && px < whole + (ahead * SHARE_INSIDE))) {
            TEST_ASSERT_TRUE(blending);
        } else if (coarse_now || px > whole + ahead + (LOD.blend_px * PAST_RAMP)) {
            TEST_ASSERT_FALSE(blending);
        }
    }
}

/* Without its blending bit a group's clusters weigh none, its error inside the ramp or not. */
static void
test_a_group_without_its_blending_bit_weighs_none(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t arrived[BLEND_WORDS] = {0};
    TEST_ASSERT_EQUAL_INT(0, weight_arrived_at(mesh, enter_px(), arrived));
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, enter_px(), arrived));
}

static void
test_arrived_groups_blend_whole_up_to_leave(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, leave_px(), NULL));
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, weight_arrived_at(mesh, enter_px(), NULL));
    TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, ramp_weight_at(SHARE_HALF),
                           weight_arrived_at(mesh, leave_px() + (LOD.blend_px * SHARE_HALF), NULL));
    TEST_ASSERT_EQUAL_INT(0, weight_arrived_at(mesh, leave_px() + (LOD.blend_px * (1.0F + PAST_RAMP)), NULL));
}

/* The dither (RENDER_LOD_BLEND_ON for a mesh without morph targets). Group WEIGHT_GROUP blends: cluster
 * WEIGHT_CLUSTER is a fine cluster under it, and the cluster simplified from it
 * (whose `own` is the group) is its coarse level. */
enum {
    DITHER_SAMPLES = 48,
    /* The group above the blending one, which never blends here. */
    GROUP_ABOVE = FINEST_GROUPS,
    /* A cluster under a group that does not blend here. */
    OTHER_FINE_CLUSTER = CLUSTERS_PER_GROUP,
    TOP_CLUSTER = CLUSTERS - 1,
};

static int
coarse_level_cluster(const r3d_lit_mesh_t* mesh) {
    for (int c = 0; c < mesh->cluster_count; c++) {
        if (mesh->cluster_lod[c].own == WEIGHT_GROUP) {
            return c;
        }
    }
    TEST_FAIL_MESSAGE("no cluster is simplified from the blending group");
    return -1;
}

/* Whether a band draws at `level`. */
static bool
band_covers(uint8_t band, int level) {
    return level >= r3d_lod_dither_lo(band) && level < r3d_lod_dither_hi(band);
}

static int
band_levels(uint8_t band) {
    int n = 0;
    for (int level = 0; level < R3D_LOD_DITHER_LEVELS; level++) {
        n += band_covers(band, level) ? 1 : 0;
    }
    return n;
}

/* The morph arm's weight for the cluster at `px`, with group WEIGHT_GROUP blending. */
static int
blending_weight_at_px(const r3d_lit_mesh_t* mesh, float px, uint32_t* arrived) {
    levelled->mesh.morph_delta = NO_DELTAS;
    const int weight = weight_arrived_at(mesh, px, arrived);
    levelled->mesh.morph_delta = NULL;
    return weight;
}

static uint8_t
dither_band_at_px(const r3d_lit_mesh_t* mesh, float px, const uint32_t* arrived, int blend, int c) {
    const r3d_lens_t lens = lens_at(error_eye(mesh, px), NULL);
    r3d_lod_morph_t morph;
    uint32_t state_morph[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_morph, mesh, NULL, arrived);
    r3d_lod_morph_init(&morph, mesh, &lens, &LOD, state_morph, blend);
    return r3d_lod_dither_band(&morph, mesh, c);
}

/* From the weight's whole through its ramp to none. */
static float
dither_sample_px(int i) {
    const float first = enter_px() * SHARE_HALF;
    const float last = enter_px() + (LOD.blend_px * (1.0F + PAST_RAMP));
    return first + ((last - first) * (float)i / (float)(DITHER_SAMPLES - 1));
}

/* A group whose coarse level is empty, as a copy's drop-out level is: with
 * nothing simplified from it, its fine cluster alone fades, from every level
 * at no weight to none at the whole one, and never comes back as the weight grows. */
static void
test_a_group_with_an_empty_coarse_level_fades_its_fine_clusters_out(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int coarse = coarse_level_cluster(mesh);
    levelled->lod[coarse].own = R3D_LOD_NONE;
    uint32_t arrived[BLEND_WORDS] = {0};
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    int last = R3D_LOD_DITHER_LEVELS + 1;
    bool saw_gone = false;
    for (int i = DITHER_SAMPLES - 1; i >= 0; i--) {
        const float px = dither_sample_px(i);
        const int weight = blending_weight_at_px(mesh, px, arrived);
        const int levels = band_levels(dither_band_at_px(mesh, px, arrived, RENDER_LOD_BLEND_ON, WEIGHT_CLUSTER));
        TEST_ASSERT_TRUE(levels <= last);
        if (weight == 0) {
            TEST_ASSERT_EQUAL_INT(R3D_LOD_DITHER_LEVELS, levels);
        } else if (weight == R3D_LOD_MORPH_ONE) {
            TEST_ASSERT_EQUAL_INT(0, levels);
            saw_gone = true;
        }
        last = levels;
    }
    TEST_ASSERT_TRUE(saw_gone);
}

/* Under a blending parent and simplified from it, a cluster's bands never
 * share a level and between them cover every one, at every weight. */
static void
test_the_dither_bands_of_a_blending_group_s_two_levels_cover_every_level_exactly_once(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int coarse = coarse_level_cluster(mesh);
    uint32_t arrived[BLEND_WORDS] = {0};
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    bool saw_whole = false;
    bool saw_none = false;
    bool saw_part = false;
    for (int i = 0; i < DITHER_SAMPLES; i++) {
        const float px = dither_sample_px(i);
        const int weight = blending_weight_at_px(mesh, px, arrived);
        const uint8_t fine_band = dither_band_at_px(mesh, px, arrived, RENDER_LOD_BLEND_ON, WEIGHT_CLUSTER);
        const uint8_t coarse_band = dither_band_at_px(mesh, px, arrived, RENDER_LOD_BLEND_ON, coarse);
        for (int level = 0; level < R3D_LOD_DITHER_LEVELS; level++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(
                1, (band_covers(fine_band, level) ? 1 : 0) + (band_covers(coarse_band, level) ? 1 : 0),
                "a level is drawn by both the fine and the coarse cluster, or neither");
        }
        if (weight == R3D_LOD_MORPH_ONE) {
            TEST_ASSERT_EQUAL_INT(R3D_LOD_DITHER_LEVELS, band_levels(coarse_band));
            TEST_ASSERT_EQUAL_INT(0, band_levels(fine_band));
            saw_whole = true;
        } else if (weight == 0) {
            TEST_ASSERT_EQUAL_INT(0, band_levels(coarse_band));
            TEST_ASSERT_EQUAL_INT(R3D_LOD_DITHER_LEVELS, band_levels(fine_band));
            saw_none = true;
        } else {
            saw_part = true;
        }
        TEST_ASSERT_INT_WITHIN(1, (weight * R3D_LOD_DITHER_LEVELS) / R3D_LOD_MORPH_ONE, band_levels(coarse_band));
    }
    TEST_ASSERT_TRUE(saw_whole && saw_none && saw_part);
}

static void
test_the_coarse_cluster_covers_more_levels_as_the_blend_weight_grows(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int coarse = coarse_level_cluster(mesh);
    uint32_t arrived[BLEND_WORDS] = {0};
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    int previous_weight = R3D_LOD_MORPH_ONE + 1;
    int previous_levels = R3D_LOD_DITHER_LEVELS + 1;
    for (int i = 0; i < DITHER_SAMPLES; i++) {
        const float px = dither_sample_px(i);
        const int weight = blending_weight_at_px(mesh, px, arrived);
        const int levels = band_levels(dither_band_at_px(mesh, px, arrived, RENDER_LOD_BLEND_ON, coarse));
        TEST_ASSERT_TRUE(weight <= previous_weight);
        TEST_ASSERT_TRUE(levels <= previous_levels);
        previous_weight = weight;
        previous_levels = levels;
    }
}

/* Without a blending bit on its groups a cluster is drawn at every level. */
static void
test_a_cluster_whose_groups_do_not_blend_has_no_dither_band(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int coarse = coarse_level_cluster(mesh);
    uint32_t arrived[BLEND_WORDS] = {0};
    const float mid_ramp = enter_px() + (LOD.blend_px * SHARE_HALF);
    const int clusters[] = {WEIGHT_CLUSTER, coarse, OTHER_FINE_CLUSTER, TOP_CLUSTER};
    for (size_t i = 0; i < sizeof clusters / sizeof clusters[0]; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, dither_band_at_px(mesh, mid_ramp, arrived, RENDER_LOD_BLEND_ON, clusters[i]));
    }
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    TEST_ASSERT_NOT_EQUAL_UINT8(0, dither_band_at_px(mesh, mid_ramp, arrived, RENDER_LOD_BLEND_ON, WEIGHT_CLUSTER));
    TEST_ASSERT_EQUAL_HEX8(0, dither_band_at_px(mesh, mid_ramp, arrived, RENDER_LOD_BLEND_ON, OTHER_FINE_CLUSTER));
    TEST_ASSERT_EQUAL_HEX8(0, dither_band_at_px(mesh, mid_ramp, arrived, RENDER_LOD_BLEND_ON, TOP_CLUSTER));
}

/* The same blending state: a mesh with morph targets weighs the cluster and has no band, one without has a band and no weight. */
static void
test_the_dither_arm_has_no_morph_weight_and_the_morph_arm_no_dither_band(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    uint32_t arrived[BLEND_WORDS] = {0};
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    const float mid_ramp = enter_px() + (LOD.blend_px * SHARE_HALF);
    const r3d_lens_t lens = lens_at(error_eye(mesh, mid_ramp), NULL);
    r3d_lod_morph_t morph, dither;
    uint32_t state_morph[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_morph, mesh, NULL, arrived);
    r3d_lod_morph_init(&morph, mesh, &lens, &LOD, state_morph, RENDER_LOD_BLEND_ON);
    levelled->mesh.morph_delta = NULL;
    uint32_t state_dither[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_dither, mesh, NULL, arrived);
    r3d_lod_morph_init(&dither, mesh, &lens, &LOD, state_dither, RENDER_LOD_BLEND_ON);
    TEST_ASSERT_GREATER_THAN_INT(0, r3d_lod_morph_weight(&morph, mesh, WEIGHT_CLUSTER));
    TEST_ASSERT_EQUAL_HEX8(0, r3d_lod_dither_band(&morph, mesh, WEIGHT_CLUSTER));
    TEST_ASSERT_EQUAL_INT(0, r3d_lod_morph_weight(&dither, mesh, WEIGHT_CLUSTER));
    TEST_ASSERT_NOT_EQUAL_UINT8(0, r3d_lod_dither_band(&dither, mesh, WEIGHT_CLUSTER));
}

/* The switch arm draws no blend, and the dither needs no morph targets. */
static void
test_the_dither_needs_no_morph_targets_and_the_switch_arm_has_no_band(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    TEST_ASSERT_NULL(mesh->morph_delta);
    uint32_t arrived[BLEND_WORDS] = {0};
    arrived[BLENDING_WORD] = 1U << WEIGHT_GROUP;
    const float mid_ramp = enter_px() + (LOD.blend_px * SHARE_HALF);
    TEST_ASSERT_NOT_EQUAL_UINT8(0, dither_band_at_px(mesh, mid_ramp, arrived, RENDER_LOD_BLEND_ON, WEIGHT_CLUSTER));
    TEST_ASSERT_EQUAL_HEX8(0, dither_band_at_px(mesh, mid_ramp, arrived, RENDER_LOD_BLEND_SWITCH, WEIGHT_CLUSTER));
}

/* The cut with group WEIGHT_GROUP blending: its fine clusters and those
 * simplified from it are both drawn. */
static void
test_a_blending_group_keeps_both_its_fine_clusters_and_those_simplified_from_it(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int coarse = coarse_level_cluster(mesh);
    const uint32_t none[BITS32_WORDS(GROUPS)] = {0};
    const uint32_t blending[BITS32_WORDS(GROUPS)] = {1U << WEIGHT_GROUP};
    TEST_ASSERT_TRUE(r3d_lod_drawn_blending(mesh, WEIGHT_CLUSTER, none, blending));
    TEST_ASSERT_TRUE(r3d_lod_drawn_blending(mesh, coarse, none, blending));
    TEST_ASSERT_FALSE(r3d_lod_drawn_blending(mesh, coarse, none, NULL));
    TEST_ASSERT_FALSE(r3d_lod_drawn_blending(mesh, coarse, none, none));
    for (int c = 0; c < mesh->cluster_count; c++) {
        if (c != coarse) {
            TEST_ASSERT_EQUAL_INT(r3d_lod_drawn(mesh, c, none), r3d_lod_drawn_blending(mesh, c, none, blending));
        }
    }
    const uint32_t above[BITS32_WORDS(GROUPS)] = {(1U << WEIGHT_GROUP) | (1U << GROUP_ABOVE)};
    TEST_ASSERT_FALSE(r3d_lod_drawn_blending(mesh, coarse, above, blending));
    TEST_ASSERT_FALSE(r3d_lod_drawn_blending(mesh, WEIGHT_CLUSTER, above, blending));
    TEST_ASSERT_FALSE(r3d_lod_drawn_blending(mesh, coarse, NULL, blending));
}

static int
cull_sorted(const r3d_lit_mesh_t* mesh, const uint32_t* coarse, const uint32_t* blending, uint16_t* out) {
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    const int count = r3d_pipeline_cull_blending(mesh, &lens, coarse, blending, out, pipeline_work(), NULL);
    qsort(out, (size_t)count, sizeof out[0], compare_cluster);
    return count;
}

static void
test_the_pipeline_cull_keeps_both_levels_of_a_blending_group(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int coarse = coarse_level_cluster(mesh);
    const uint32_t none[BITS32_WORDS(GROUPS)] = {0};
    const uint32_t blending[BITS32_WORDS(GROUPS)] = {1U << WEIGHT_GROUP};
    uint16_t plain[CLUSTERS];
    uint16_t both[CLUSTERS];
    const int fine_count = cull_sorted(mesh, none, NULL, plain);
    TEST_ASSERT_EQUAL_INT(FINEST_CLUSTERS, fine_count);
    const int count = cull_sorted(mesh, none, blending, both);
    TEST_ASSERT_EQUAL_INT(fine_count + 1, count);
    bool has_coarse = false;
    for (int i = 0; i < count; i++) {
        has_coarse = has_coarse || both[i] == coarse;
    }
    TEST_ASSERT_TRUE(has_coarse);
    for (int c = 0; c < FINEST_CLUSTERS; c++) {
        bool kept = false;
        for (int i = 0; i < count; i++) {
            kept = kept || both[i] == c;
        }
        TEST_ASSERT_TRUE(kept);
    }
}

/* A mesh whose levels share one tree, walked whole, keeps both levels of a
 * blending group as the finest tree's walk and its cut do. */
static void
test_the_whole_tree_walk_keeps_both_levels_of_a_blending_group(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_lit_mesh_t whole = *mesh;
    whole.lod_finest_root = 0;
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    const uint32_t none[BITS32_WORDS(GROUPS)] = {0};
    const uint32_t blending[BITS32_WORDS(GROUPS)] = {1U << WEIGHT_GROUP};
    uint16_t apart[CLUSTERS];
    uint16_t together[CLUSTERS];
    const int count = cull_sorted(mesh, none, blending, apart);
    TEST_ASSERT_EQUAL_INT(count,
                          r3d_pipeline_cull_blending(&whole, &lens, none, blending, together, pipeline_work(), NULL));
    qsort(together, (size_t)count, sizeof together[0], compare_cluster);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(apart, together, count);
}

/* The cull view's counted walk keeps what the frame's cull keeps with a
 * group blending: both its levels, counting the coarse one. */
static void
test_the_counted_walk_keeps_both_levels_of_a_blending_group(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_pipeline_count_walk_t* walk = malloc(sizeof *walk);
    TEST_ASSERT_NOT_NULL(walk);
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    const uint32_t none[BITS32_WORDS(GROUPS)] = {0};
    const uint32_t blending[BITS32_WORDS(GROUPS)] = {1U << WEIGHT_GROUP};
    uint16_t culled[CLUSTERS];
    uint16_t counted[CLUSTERS];
    const int count = cull_sorted(mesh, none, blending, culled);
    r3d_pipeline_counts_t counts = {0};
    TEST_ASSERT_EQUAL_INT(count, r3d_pipeline_cull_counted(mesh, &lens, none, blending, counted, walk, &counts));
    qsort(counted, (size_t)count, sizeof counted[0], compare_cluster);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(culled, counted, count);
    TEST_ASSERT_EQUAL_INT(1, counts.clusters_coarse);
    free(walk);
}

static void
test_the_pipeline_cull_without_blending_bits_is_the_plain_cull(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const uint32_t cuts[][BITS32_WORDS(GROUPS)] = {
        {0}, {(1U << WEIGHT_GROUP) | (1U << (WEIGHT_GROUP + 1)) | (1U << GROUP_ABOVE)}};
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    for (size_t k = 0; k < sizeof cuts / sizeof cuts[0]; k++) {
        uint16_t via_blending[CLUSTERS];
        uint16_t plain[CLUSTERS];
        const int count = cull_sorted(mesh, cuts[k], NULL, via_blending);
        TEST_ASSERT_EQUAL_INT(count, r3d_pipeline_cull(mesh, &lens, cuts[k], plain, pipeline_work(), NULL));
        qsort(plain, (size_t)count, sizeof plain[0], compare_cluster);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(plain, via_blending, count);
    }
}

/* Group GROUP_ABOVE and the two groups under it coarse, WEIGHT_GROUP blending
 * and the second group below too: nothing under the coarse group is drawn. */
static void
test_the_pipeline_cull_drops_a_blending_group_s_clusters_under_a_coarse_parent(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    const int second = WEIGHT_GROUP + 1;
    const uint32_t coarse[BITS32_WORDS(GROUPS)] = {(1U << WEIGHT_GROUP) | (1U << second) | (1U << GROUP_ABOVE)};
    const uint32_t blending[BITS32_WORDS(GROUPS)] = {(1U << WEIGHT_GROUP) | (1U << second)};
    uint16_t with[CLUSTERS];
    uint16_t without[CLUSTERS];
    const int count = cull_sorted(mesh, coarse, blending, with);
    TEST_ASSERT_EQUAL_INT(count, cull_sorted(mesh, coarse, NULL, without));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(without, with, count);
    for (int i = 0; i < count; i++) {
        const uint16_t parent = mesh->cluster_lod[with[i]].parent;
        TEST_ASSERT_TRUE(parent == R3D_LOD_NONE || !bits32_get(coarse, parent));
    }
}

/* A mesh of two clusters to transform: the first finest in group 0, with morph
 * targets; the second the coarse cluster simplified from it, with no parent. */
enum {
    MORPH_VERTICES = 6,
    MORPH_FIRST_VERTICES = 3,
    MORPH_CLUSTERS = 2,
    MORPH_GROUP_RADIUS = 8,
    MORPH_GROUP_ERROR = 4,
    /* The first cluster's shift: each delta moves 1 << shift ticks. */
    MORPH_SHIFT = 1,
};

/* The second cluster's deltas are set though its cluster has no parent, so they are never applied. */
typedef struct {
    int16_t positions[MORPH_VERTICES][3];
    int16_t targets[MORPH_VERTICES][3];
    int8_t delta[MORPH_VERTICES][3];
    uint8_t shift[MORPH_CLUSTERS];
    r3d_lit_cluster_t clusters[MORPH_CLUSTERS];
    r3d_lit_node_t nodes[1];
    r3d_lit_cluster_lod_t lod[MORPH_CLUSTERS];
    r3d_lit_lod_group_t groups[1];
    r3d_lit_mesh_t mesh;      /* blends toward `targets` */
    r3d_lit_mesh_t reference; /* sits at `targets`, with no morph data */
} morphing_t;

static morphing_t* morphing;

static void
release_morphing(void) {
    free(morphing);
    morphing = NULL;
}

static const morphing_t*
morphing_mesh(void) {
    morphing = calloc(1, sizeof *morphing);
    suite_set_test_cleanup(release_morphing);
    TEST_ASSERT_NOT_NULL(morphing);
    static const int16_t positions[MORPH_VERTICES][3] = {{-6, -4, 0}, {6, -4, 0}, {0, 6, 0},
                                                         {-5, -3, 1}, {5, -3, 1}, {0, 5, 1}};
    static const int8_t delta[MORPH_VERTICES][3] = {{3, -2, 0}, {-4, 1, 2}, {0, 5, -3},
                                                    {7, 7, 7},  {-7, 7, 7}, {7, -7, 7}};
    memcpy(morphing->positions, positions, sizeof positions);
    memcpy(morphing->delta, delta, sizeof delta);
    morphing->shift[0] = MORPH_SHIFT;
    box3s_t bounds = box3s_empty();
    for (int v = 0; v < MORPH_VERTICES; v++) {
        for (int a = 0; a < 3; a++) {
            const int move = v < MORPH_FIRST_VERTICES ? delta[v][a] * (1 << MORPH_SHIFT) : 0;
            morphing->targets[v][a] = (int16_t)(positions[v][a] + move);
        }
        bounds = box3s_extend(bounds, vec3s_from_array(morphing->targets[v]));
        bounds = box3s_extend(bounds, vec3s_from_array(morphing->positions[v]));
    }
    for (int c = 0; c < MORPH_CLUSTERS; c++) {
        morphing->clusters[c] =
            (r3d_lit_cluster_t){(uint32_t)(c * MORPH_FIRST_VERTICES), MORPH_FIRST_VERTICES, 0, 0, bounds, true};
    }
    morphing->nodes[0] = (r3d_lit_node_t){bounds, 0, MORPH_CLUSTERS, true};
    morphing->lod[0] = (r3d_lit_cluster_lod_t){R3D_LOD_NONE, 0};
    morphing->lod[1] = (r3d_lit_cluster_lod_t){0, R3D_LOD_NONE};
    morphing->groups[0] = (r3d_lit_lod_group_t){{{0, 0, 0}, MORPH_GROUP_RADIUS}, MORPH_GROUP_ERROR, 0};
    morphing->mesh = (r3d_lit_mesh_t){.positions = morphing->positions,
                                      .clusters = morphing->clusters,
                                      .nodes = morphing->nodes,
                                      .vertex_count = MORPH_VERTICES,
                                      .cluster_count = MORPH_CLUSTERS,
                                      .node_count = 1,
                                      .position_scale = 1,
                                      .cluster_lod = morphing->lod,
                                      .lod_groups = morphing->groups,
                                      .lod_group_count = 1,
                                      .morph_shift = morphing->shift,
                                      .morph_delta = morphing->delta};
    morphing->reference = morphing->mesh;
    morphing->reference.positions = morphing->targets;
    morphing->reference.morph_shift = NULL;
    morphing->reference.morph_delta = NULL;
    return morphing;
}

typedef struct {
    r3d_pipeline_vertex_t cs[MORPH_VERTICES];
    r3d_pipeline_rows_t rows[MORPH_CLUSTERS];
} transformed_t;

static const uint16_t BOTH_CLUSTERS[MORPH_CLUSTERS] = {0, 1};

/* The eye at which the first cluster's weight is `share` of the way down its error ramp. */
static float
morph_eye(const morphing_t* m, float share) {
    return error_eye(&m->mesh, leave_px() + (share * LOD.blend_px));
}

static int
transform_morphed_at(const morphing_t* m, float eye, bool boxed, transformed_t* out) {
    const r3d_lens_t lens = lens_at(eye, NULL);
    r3d_lod_morph_t morph;
    uint32_t state_morph[R3D_LOD_STATE_WORDS(GROUPS)];
    blend_state_from_bits(state_morph, &m->mesh, NULL, NULL);
    r3d_lod_morph_init(&morph, &m->mesh, &lens, &LOD, state_morph, RENDER_LOD_BLEND_ON);
    r3d_pipeline_transform_morphed(&m->mesh, &lens, &morph, BOTH_CLUSTERS, MORPH_CLUSTERS, out->cs, out->rows, boxed);
    return r3d_lod_morph_weight(&morph, &m->mesh, 0);
}

static void
transform_plain_at(const r3d_lit_mesh_t* mesh, float eye, bool boxed, transformed_t* out) {
    const r3d_lens_t lens = lens_at(eye, NULL);
    r3d_pipeline_transform(mesh, &lens, BOTH_CLUSTERS, MORPH_CLUSTERS, out->cs, out->rows, boxed);
}

static void
expect_same_vertices(const r3d_pipeline_vertex_t* expected, const r3d_pipeline_vertex_t* actual, int first, int count) {
    for (int v = first; v < first + count; v++) {
        TEST_ASSERT_EQUAL_INT16(expected[v].sx, actual[v].sx);
        TEST_ASSERT_EQUAL_INT16(expected[v].sy, actual[v].sy);
        TEST_ASSERT_EQUAL_FLOAT(expected[v].iz, actual[v].iz);
    }
}

static void
expect_same_rows(const r3d_pipeline_rows_t* expected, const r3d_pipeline_rows_t* actual) {
    TEST_ASSERT_EQUAL_INT16(expected->x0, actual->x0);
    TEST_ASSERT_EQUAL_INT16(expected->y0, actual->y0);
    TEST_ASSERT_EQUAL_INT16(expected->x1, actual->x1);
    TEST_ASSERT_EQUAL_INT16(expected->y1, actual->y1);
    TEST_ASSERT_EQUAL_UINT16(expected->depth, actual->depth);
    TEST_ASSERT_EQUAL(expected->unbounded, actual->unbounded);
}

/* The cluster with no parent stays put, with no weight. */
static void
test_a_cluster_transformed_with_the_whole_weight_lands_on_its_targets(void) {
    const morphing_t* m = morphing_mesh();
    const float eye = morph_eye(m, -SHARE_HALF);
    for (int boxed = 0; boxed <= 1; boxed++) {
        transformed_t blended, direct;
        TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, transform_morphed_at(m, eye, boxed != 0, &blended));
        transform_plain_at(&m->reference, eye, boxed != 0, &direct);
        expect_same_vertices(direct.cs, blended.cs, 0, MORPH_FIRST_VERTICES);
        expect_same_rows(&direct.rows[0], &blended.rows[0]);
        TEST_ASSERT_EQUAL_UINT8(R3D_LOD_MORPH_ONE, blended.rows[0].blend);
        expect_same_vertices(direct.cs, blended.cs, MORPH_FIRST_VERTICES, MORPH_VERTICES - MORPH_FIRST_VERTICES);
        TEST_ASSERT_EQUAL_UINT8(0, blended.rows[1].blend);
    }
}

static void
test_a_cluster_transformed_with_no_weight_matches_the_plain_transform_exactly(void) {
    const morphing_t* m = morphing_mesh();
    const float eye = morph_eye(m, 4.0F);
    for (int boxed = 0; boxed <= 1; boxed++) {
        transformed_t blended, plain;
        memset(&blended, 0, sizeof blended);
        memset(&plain, 0, sizeof plain);
        TEST_ASSERT_EQUAL_INT(0, transform_morphed_at(m, eye, boxed != 0, &blended));
        transform_plain_at(&m->mesh, eye, boxed != 0, &plain);
        TEST_ASSERT_EQUAL_MEMORY(plain.cs, blended.cs, sizeof plain.cs);
        TEST_ASSERT_EQUAL_MEMORY(plain.rows, blended.rows, sizeof plain.rows);
        TEST_ASSERT_EQUAL_UINT8(0, blended.rows[0].blend);
    }
}

static void
test_a_null_morph_transforms_as_the_plain_transform_does(void) {
    const morphing_t* m = morphing_mesh();
    const float eye = morph_eye(m, -SHARE_HALF);
    const r3d_lens_t lens = lens_at(eye, NULL);
    transformed_t blended, plain;
    memset(&blended, 0, sizeof blended);
    memset(&plain, 0, sizeof plain);
    r3d_pipeline_transform_morphed(&m->mesh, &lens, NULL, BOTH_CLUSTERS, MORPH_CLUSTERS, blended.cs, blended.rows,
                                   false);
    transform_plain_at(&m->mesh, eye, false, &plain);
    TEST_ASSERT_EQUAL_MEMORY(plain.cs, blended.cs, sizeof plain.cs);
    TEST_ASSERT_EQUAL_MEMORY(plain.rows, blended.rows, sizeof plain.rows);
}

/* Vertex 0 moves along +x by a few pixels. */
static void
test_a_part_weight_puts_a_vertex_between_where_it_is_and_its_target(void) {
    const morphing_t* m = morphing_mesh();
    const float eye = morph_eye(m, SHARE_HALF);
    transformed_t blended, fine, target;
    const int weight = transform_morphed_at(m, eye, false, &blended);
    TEST_ASSERT_INT_WITHIN(WEIGHT_SLACK, R3D_LOD_MORPH_ONE / 2, weight);
    TEST_ASSERT_EQUAL_UINT8(weight, blended.rows[0].blend);
    transform_plain_at(&m->mesh, eye, false, &fine);
    transform_plain_at(&m->reference, eye, false, &target);
    TEST_ASSERT_TRUE(target.cs[0].sx > fine.cs[0].sx + R3D_SUBPIXEL);
    TEST_ASSERT_TRUE(blended.cs[0].sx > fine.cs[0].sx);
    TEST_ASSERT_TRUE(blended.cs[0].sx < target.cs[0].sx);
    TEST_ASSERT_INT_WITHIN(R3D_SUBPIXEL, (fine.cs[0].sx + target.cs[0].sx) / 2, blended.cs[0].sx);
}

/* Three triangles in a blend: the first finest in group 0, morphing or fading
 * out; the second simplified from group 0, fading in; the third in no group,
 * drawn plain. They overlap at different depths. */
enum {
    DRAWN_CLUSTERS = 3,
    DRAWN_VERTICES = 9,
    DRAWN_PLAIN = 2, /* the cluster in no group */
    DRAWN_HALF = 10, /* ticks: each triangle's half width */
};

typedef struct {
    int16_t positions[DRAWN_VERTICES][3];
    uint8_t colors[DRAWN_VERTICES][3];
    uint16_t triangles[DRAWN_CLUSTERS][3];
    int8_t delta[DRAWN_VERTICES][3];
    uint8_t shift[DRAWN_CLUSTERS];
    r3d_lit_cluster_t clusters[DRAWN_CLUSTERS];
    r3d_lit_node_t nodes[1];
    r3d_lit_cluster_lod_t lod[DRAWN_CLUSTERS];
    r3d_lit_lod_group_t groups[1];
    r3d_lit_mesh_t mesh;
    r3d_pipeline_vertex_t cs[DRAWN_VERTICES];
    r3d_pipeline_rows_t rows[DRAWN_CLUSTERS];
    gfx_color_t color[2][W * H]; /* the whole list through the blend's copies, then split */
    uint16_t depth[2][W * H];
} drawn_blend_t;

static drawn_blend_t* drawn;

static void
release_drawn(void) {
    free(drawn);
    drawn = NULL;
    release();
}

static drawn_blend_t*
drawn_blend(bool morph_targets) {
    drawn = calloc(1, sizeof *drawn);
    suite_set_test_cleanup(release_drawn);
    TEST_ASSERT_NOT_NULL(drawn);
    box3s_t bounds = box3s_empty();
    for (int c = 0; c < DRAWN_CLUSTERS; c++) {
        const int x = (c - 1) * DRAWN_HALF / 2;
        const int16_t corners[3][3] = {
            {(int16_t)(x - DRAWN_HALF), -DRAWN_HALF, (int16_t)c},
            {(int16_t)(x + DRAWN_HALF), -DRAWN_HALF, (int16_t)c},
            {(int16_t)x, DRAWN_HALF, (int16_t)c},
        };
        for (int k = 0; k < 3; k++) {
            const int v = (c * 3) + k;
            memcpy(drawn->positions[v], corners[k], sizeof corners[k]);
            drawn->colors[v][c] = UINT8_MAX;
            drawn->delta[v][0] = (int8_t)(k - 1);
            bounds = box3s_extend(bounds, vec3s_from_array(drawn->positions[v]));
        }
        drawn->triangles[c][0] = (uint16_t)(c * 3);
        drawn->triangles[c][1] = (uint16_t)((c * 3) + 1);
        drawn->triangles[c][2] = (uint16_t)((c * 3) + 2);
        drawn->shift[c] = MORPH_SHIFT;
    }
    for (int c = 0; c < DRAWN_CLUSTERS; c++) {
        drawn->clusters[c] = (r3d_lit_cluster_t){(uint16_t)(c * 3), 3, (uint16_t)c, 1, bounds, true};
    }
    drawn->nodes[0] = (r3d_lit_node_t){bounds, 0, DRAWN_CLUSTERS, true};
    drawn->lod[0] = (r3d_lit_cluster_lod_t){R3D_LOD_NONE, 0};
    drawn->lod[1] = (r3d_lit_cluster_lod_t){0, R3D_LOD_NONE};
    drawn->lod[DRAWN_PLAIN] = (r3d_lit_cluster_lod_t){R3D_LOD_NONE, R3D_LOD_NONE};
    drawn->groups[0] = (r3d_lit_lod_group_t){{{0, 0, 0}, MORPH_GROUP_RADIUS}, MORPH_GROUP_ERROR, 0};
    drawn->mesh = (r3d_lit_mesh_t){.positions = drawn->positions,
                                   .colors = drawn->colors,
                                   .triangles = drawn->triangles,
                                   .clusters = drawn->clusters,
                                   .nodes = drawn->nodes,
                                   .vertex_count = DRAWN_VERTICES,
                                   .triangle_count = DRAWN_CLUSTERS,
                                   .cluster_count = DRAWN_CLUSTERS,
                                   .node_count = 1,
                                   .position_scale = 1,
                                   .cluster_lod = drawn->lod,
                                   .lod_groups = drawn->groups,
                                   .lod_group_count = 1,
                                   .morph_shift = morph_targets ? drawn->shift : NULL,
                                   .morph_delta = morph_targets ? drawn->delta : NULL};
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < W * H; i++) {
            drawn->depth[p][i] = R3D_DEPTH_EMPTY;
        }
    }
    return drawn;
}

/* An instance in a blend draws the same pixels whether its whole list takes
 * the blend's copies or only the clusters that blend do, the rest the plain
 * transform and draw: by morph, and by dither. */
static void
test_a_blending_instance_draws_the_same_pixels_with_its_plain_clusters_split_off(void) {
    for (int morph_targets = 0; morph_targets <= 1; morph_targets++) {
        drawn_blend_t* d = drawn_blend(morph_targets != 0);
        const r3d_lit_mesh_t* mesh = &d->mesh;
        const r3d_lens_t lens = lens_at(error_eye(mesh, leave_px() + (SHARE_HALF * LOD.blend_px)), NULL);
        r3d_lod_morph_t morph;
        uint32_t state_morph[R3D_LOD_STATE_WORDS(GROUPS)];
        blend_state_from_bits(state_morph, mesh, NULL, NULL);
        r3d_lod_morph_init(&morph, mesh, &lens, &LOD, state_morph, RENDER_LOD_BLEND_ON);
        TEST_ASSERT_TRUE(morph.enabled);
        TEST_ASSERT_EQUAL(morph_targets == 0, morph.dither);
        static const uint16_t ORDER[DRAWN_CLUSTERS] = {0, DRAWN_PLAIN, 1};

        r3d_span_target_t whole = r3d_span_target(d->color[0], d->depth[0], W, 0, H);
        r3d_pipeline_transform_morphed(mesh, &lens, &morph, ORDER, DRAWN_CLUSTERS, d->cs, d->rows, false);
        r3d_pipeline_draw_blended(mesh, &lens, ORDER, DRAWN_CLUSTERS, d->cs, d->rows, &whole, pipeline_work(),
                                  morph.dither);

        uint16_t list[DRAWN_CLUSTERS];
        memcpy(list, ORDER, sizeof list);
        const int plain = r3d_lod_morph_partition(&morph, mesh, list, DRAWN_CLUSTERS);
        TEST_ASSERT_EQUAL_INT(1, plain);
        TEST_ASSERT_EQUAL_UINT16(DRAWN_PLAIN, list[0]);
        r3d_span_target_t split = r3d_span_target(d->color[1], d->depth[1], W, 0, H);
        r3d_pipeline_transform(mesh, &lens, list, plain, d->cs, d->rows, false);
        r3d_pipeline_transform_morphed(mesh, &lens, &morph, list + plain, DRAWN_CLUSTERS - plain, d->cs, d->rows,
                                       false);
        r3d_pipeline_draw(mesh, &lens, list, plain, d->cs, d->rows, &split, pipeline_work());
        r3d_pipeline_draw_blended(mesh, &lens, list + plain, DRAWN_CLUSTERS - plain, d->cs, d->rows, &split,
                                  pipeline_work(), morph.dither);

        int covered = 0;
        for (int i = 0; i < W * H; i++) {
            covered += d->depth[0][i] != R3D_DEPTH_EMPTY;
        }
        TEST_ASSERT_GREATER_THAN_INT(0, covered);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(d->depth[0], d->depth[1], W * H);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(d->color[0], d->color[1], W * H);
        release_drawn();
    }
}

/* Triangles a cluster of each level holds in the budget's tests: fewer at
 * each coarser level, though more than half, so a merge still saves. */
enum { FINEST_TRIANGLES = 8, MIDDLE_TRIANGLES = 6, UPPER_TRIANGLES = 5, TOP_TRIANGLES = 4 };

/* The triangles a cluster whose own group is `own` holds. */
static int
level_triangles(uint16_t own) {
    if (own == R3D_LOD_NONE) {
        return FINEST_TRIANGLES;
    }
    return own < FINEST_GROUPS ? MIDDLE_TRIANGLES : (own < TOP_GROUP ? UPPER_TRIANGLES : TOP_TRIANGLES);
}

/* The levelled mesh with its clusters' triangles and each group's saves, as
 * the levels bake writes them: the triangles of the clusters a group holds
 * less those of the clusters simplified from it. */
static const r3d_lit_mesh_t*
counted_mesh(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    for (int c = 0; c < CLUSTERS; c++) {
        levelled->clusters[c].triangle_count = (uint16_t)level_triangles(levelled->lod[c].own);
    }
    for (int g = 0; g < GROUPS; g++) {
        levelled->groups[g].saves = 0;
    }
    for (int c = 0; c < CLUSTERS; c++) {
        const r3d_lit_cluster_lod_t lod = levelled->lod[c];
        const int16_t triangles = (int16_t)levelled->clusters[c].triangle_count;
        if (lod.parent != R3D_LOD_NONE) {
            levelled->groups[lod.parent].saves = (int16_t)(levelled->groups[lod.parent].saves + triangles);
        }
        if (lod.own != R3D_LOD_NONE) {
            levelled->groups[lod.own].saves = (int16_t)(levelled->groups[lod.own].saves - triangles);
        }
    }
    r3d_lit_mesh_find_levels(&levelled->mesh);
    return mesh;
}

/* The triangles `mesh` draws under `coarse`. */
static int
drawn_triangles(const r3d_lit_mesh_t* mesh, const uint32_t* coarse) {
    uint16_t listed[CLUSTERS];
    const int count = r3d_lod_list_drawn(mesh, coarse, NULL, listed, CLUSTERS);
    int triangles = 0;
    for (int i = 0; i < count; i++) {
        triangles += mesh->clusters[listed[i]].triangle_count;
    }
    return triangles;
}

/* A coverage cap tight enough to hold the levelled mesh's groups fine near the floor. */
#define TIGHT_COVERAGE_PERMILLE 10

/* The triangles `histogram` predicts at bin edge `edge`: the finest, less
 * the saves of every bin below it. */
static int
predicted_at(const r3d_lod_histogram_t* histogram, int edge) {
    int triangles = histogram->finest;
    for (int b = 0; b < edge; b++) {
        triangles -= histogram->saves[b];
    }
    return triangles;
}

/* At every pose of the path and every bin edge from the floor up, the
 * steer's whole reach, the triangles the histogram predicts are those a
 * fresh pick at that edge's tolerance and coverage cap draws, hysteresis
 * aside: the histogram is the cut's triangle count as a function of the
 * steered tolerance, the cap rising with it. The risen cap is whole
 * thousandths, so the prediction lies between the picks at the cap rounded
 * down and a thousandth more. */
typedef struct {
    int checked; /* edges whose prediction draws coarse */
    int capped;  /* edges where the floor's cap draws more than the risen one */
} edge_tally_t;

/* Checks the prediction at bin edge `edge` of `histogram`, filled by a pick
 * at `floor` through `lens`, against fresh picks at that edge. */
static void
check_edge(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* floor,
           const r3d_lod_histogram_t* histogram, int edge, edge_tally_t* tally) {
    render_lod_settings_t at = *floor;
    at.tolerance_px = r3d_lod_bin_edge(edge);
    at.hysteresis_px = 0.0F;
    at.coverage_permille = r3d_lod_steer_coverage(at.tolerance_px, floor->tolerance_px, floor->coverage_permille);
    uint32_t fresh[BITS32_WORDS(GROUPS)] = {0};
    r3d_lod_pick(mesh, lens, &at, fresh, false, NULL);
    render_lod_settings_t over = at;
    over.coverage_permille++;
    uint32_t wider[BITS32_WORDS(GROUPS)] = {0};
    r3d_lod_pick(mesh, lens, &over, wider, false, NULL);
    const int predicted = predicted_at(histogram, edge);
    TEST_ASSERT_TRUE_MESSAGE(predicted <= drawn_triangles(mesh, fresh), "edge");
    TEST_ASSERT_TRUE_MESSAGE(predicted >= drawn_triangles(mesh, wider), "edge");
    tally->checked += predicted < histogram->finest ? 1 : 0;
    /* The same edge with the floor's cap draws more: the risen cap let a group turn. */
    at.coverage_permille = floor->coverage_permille;
    uint32_t held[BITS32_WORDS(GROUPS)] = {0};
    r3d_lod_pick(mesh, lens, &at, held, false, NULL);
    tally->capped += drawn_triangles(mesh, held) > drawn_triangles(mesh, fresh) ? 1 : 0;
}

static void
test_the_histogram_predicts_the_triangles_a_pick_at_each_bin_edge_draws(void) {
    const r3d_lit_mesh_t* mesh = counted_mesh();
    const int steps = steps_in();
    /* The engine's cap, and one so tight that it, not the error, holds groups fine near the floor. */
    const int caps[] = {LOD.coverage_permille, TIGHT_COVERAGE_PERMILLE};
    for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
        render_lod_settings_t floor = LOD;
        floor.coverage_permille = caps[c];
        edge_tally_t tally = {0, 0};
        for (int p = 0; p < steps; p++) {
            const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
            uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
            r3d_lod_histogram_t histogram = {{0}, 0, 1.0F};
            r3d_lod_pick(mesh, &lens, &floor, coarse, false, &histogram);
            TEST_ASSERT_EQUAL_INT(FINEST_CLUSTERS * FINEST_TRIANGLES, histogram.finest);
            for (int edge = 1; edge <= R3D_LOD_BINS; edge++) {
                if (r3d_lod_bin_edge(edge) >= floor.tolerance_px) {
                    check_edge(mesh, &lens, &floor, &histogram, edge, &tally);
                }
            }
        }
        TEST_ASSERT_GREATER_THAN_INT(0, tally.checked); /* some edge along the path draws coarse */
        if (caps[c] == TIGHT_COVERAGE_PERMILLE) {
            TEST_ASSERT_GREATER_THAN_INT(0, tally.capped); /* and the tight cap binds somewhere */
        }
    }
}

/* A round's histogram: its finest triangles and saves at two bins, one
 * below the floor and one above it. */
enum { SEEN_FINEST = 1000, BELOW_BIN = 10, BELOW_SAVES = 300, ABOVE_BIN = 20, ABOVE_SAVES = 400, FLOOR_EDGE = 14 };

/* Ends a round of the histogram above, split across both cores, that drew `drawn`. */
static void
steer_round(r3d_lod_steer_t* steer, int budget, int drawn) {
    steer->halves[0].finest = SEEN_FINEST / 2;
    steer->halves[1].finest = SEEN_FINEST - (SEEN_FINEST / 2);
    steer->halves[0].saves[BELOW_BIN] = BELOW_SAVES;
    steer->halves[1].saves[ABOVE_BIN] = ABOVE_SAVES;
    r3d_lod_steer_frame(steer, r3d_lod_bin_edge(FLOOR_EDGE), budget, drawn, true);
}

/* The steer takes the least edge past the floor whose predicted triangles,
 * both cores' histograms summed, fit the budget; the floor when it fits
 * there; the last edge when none does, though never below the floor. What
 * the round drew against what was predicted scales the prediction. */
static void
test_the_steer_takes_the_least_edge_past_the_floor_within_the_budget(void) {
    const float floor_px = r3d_lod_bin_edge(FLOOR_EDGE);
    const int at_floor = SEEN_FINEST - BELOW_SAVES; /* BELOW_BIN is under the floor */
    const int above = at_floor - ABOVE_SAVES;
    r3d_lod_steer_t steer;
    r3d_lod_steer_reset(&steer, floor_px);
    steer_round(&steer, at_floor, at_floor);
    TEST_ASSERT_EQUAL_INT(-1, steer.bin);
    TEST_ASSERT_EQUAL_FLOAT(floor_px, steer.tolerance_px);
    r3d_lod_steer_reset(&steer, floor_px);
    steer_round(&steer, above, at_floor);
    TEST_ASSERT_EQUAL_INT(ABOVE_BIN + 1, steer.bin);
    TEST_ASSERT_EQUAL_FLOAT(r3d_lod_bin_edge(ABOVE_BIN + 1), steer.tolerance_px);
    r3d_lod_steer_reset(&steer, floor_px);
    steer_round(&steer, above - 1, at_floor);
    TEST_ASSERT_EQUAL_INT(R3D_LOD_BINS, steer.bin);
    TEST_ASSERT_EQUAL_FLOAT(r3d_lod_bin_edge(R3D_LOD_BINS), steer.tolerance_px);
    /* Out of reach with the floor past the last edge: the floor. */
    const float high_floor = 2.0F * r3d_lod_bin_edge(R3D_LOD_BINS);
    r3d_lod_steer_reset(&steer, high_floor);
    steer.halves[0].finest = SEEN_FINEST;
    r3d_lod_steer_frame(&steer, high_floor, SEEN_FINEST / 2, SEEN_FINEST, true);
    TEST_ASSERT_EQUAL_FLOAT(high_floor, steer.tolerance_px);
    /* Culling drew half the prediction: the floor's prediction, halved, fits. */
    r3d_lod_steer_reset(&steer, floor_px);
    steer_round(&steer, at_floor / 2, at_floor / 2);
    TEST_ASSERT_EQUAL_INT(-1, steer.bin);
    /* The round's counts start again empty. */
    TEST_ASSERT_EQUAL_INT(0, steer.halves[0].finest + steer.halves[1].finest + steer.frames);
}

/* Over the budget the tolerance rises in one round; under it, it falls a
 * bin a round down to the floor. */
static void
test_the_steer_rises_at_once_and_falls_a_bin_a_round(void) {
    const int above = SEEN_FINEST - BELOW_SAVES - ABOVE_SAVES;
    r3d_lod_steer_t steer;
    r3d_lod_steer_reset(&steer, r3d_lod_bin_edge(FLOOR_EDGE));
    steer_round(&steer, above, SEEN_FINEST - BELOW_SAVES);
    TEST_ASSERT_EQUAL_INT(ABOVE_BIN + 1, steer.bin);
    for (int bin = ABOVE_BIN; bin > FLOOR_EDGE; bin--) {
        steer_round(&steer, SEEN_FINEST, above);
        TEST_ASSERT_EQUAL_INT(bin, steer.bin);
    }
    steer_round(&steer, SEEN_FINEST, above);
    TEST_ASSERT_EQUAL_INT(-1, steer.bin); /* the edge at the floor is the floor */
}

/* A bin lower that only just fits holds the steer; one more below lets it
 * fall the one bin: a camera closing in does not dip a bin and climb back. */
static void
test_the_steer_holds_where_only_the_next_bin_down_fits(void) {
    enum { HOLD_SAVES = 100 };

    const int above = SEEN_FINEST - BELOW_SAVES - ABOVE_SAVES;
    r3d_lod_steer_t steer;
    r3d_lod_steer_reset(&steer, r3d_lod_bin_edge(FLOOR_EDGE));
    steer_round(&steer, above, SEEN_FINEST - BELOW_SAVES);
    TEST_ASSERT_EQUAL_INT(ABOVE_BIN + 1, steer.bin);
    /* HOLD_SAVES a bin further down: there the edge one below fits, the one under it does not. */
    steer.halves[0].saves[ABOVE_BIN - 1] = HOLD_SAVES;
    steer_round(&steer, SEEN_FINEST - BELOW_SAVES - HOLD_SAVES, above - HOLD_SAVES);
    TEST_ASSERT_EQUAL_INT(ABOVE_BIN + 1, steer.bin);
    TEST_ASSERT_TRUE(steer.settled);
    /* And as much again a bin below that: two edges down fit, so it falls one. */
    steer.halves[0].saves[ABOVE_BIN - 1] = HOLD_SAVES;
    steer.halves[0].saves[ABOVE_BIN - 2] = HOLD_SAVES;
    steer_round(&steer, SEEN_FINEST - BELOW_SAVES - HOLD_SAVES, above - (2 * HOLD_SAVES));
    TEST_ASSERT_EQUAL_INT(ABOVE_BIN, steer.bin);
}

/* Rounds the camera holds still for, and the rounds a hold may take to settle. */
enum { HOLD_ROUNDS = 12, SETTLE_ROUNDS = 3 };

/* One steered round of the levelled mesh through `lens`: a pick at the
 * steer's tolerance and coverage cap, which fills the histogram, its drawn triangles, then
 * the steer. Returns the bin it leaves. */
static int
steered_round(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint32_t* coarse, r3d_lod_steer_t* steer,
              int budget) {
    render_lod_settings_t lod = LOD;
    lod.tolerance_px = steer->tolerance_px;
    lod.coverage_permille = r3d_lod_steer_coverage(steer->tolerance_px, LOD.tolerance_px, LOD.coverage_permille);
    steer->halves[0].unit = 1.0F;
    r3d_lod_pick(mesh, lens, &lod, coarse, false, &steer->halves[0]);
    r3d_lod_steer_frame(steer, LOD.tolerance_px, budget, drawn_triangles(mesh, coarse), true);
    return steer->bin;
}

/* Steers along the path in and out; returns how often the bin moved, and
 * counts into `swings` each return to a bin left within SETTLE_ROUNDS rounds. */
static int
steer_along_the_path(const r3d_lit_mesh_t* mesh, uint32_t* coarse, r3d_lod_steer_t* steer, int budget, int* swings) {
    const int steps = steps_in();
    int before = -2, last = -2, held = 0, moved = 0;
    for (int p = 0; p < 2 * steps; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        const int bin = steered_round(mesh, &lens, coarse, steer, budget);
        held++;
        if (bin != last) {
            *swings += bin == before && held <= SETTLE_ROUNDS ? 1 : 0;
            moved++;
            before = last;
            last = bin;
            held = 0;
        }
    }
    return moved;
}

/* Holds the camera at `lens` for HOLD_ROUNDS rounds: the bin settles within
 * SETTLE_ROUNDS and stays. */
static void
hold_settles(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint32_t* coarse, r3d_lod_steer_t* steer, int budget) {
    int settled = -2;
    for (int round = 0; round < HOLD_ROUNDS; round++) {
        const int bin = steered_round(mesh, lens, coarse, steer, budget);
        if (round == SETTLE_ROUNDS) {
            settled = bin;
        }
        if (round > SETTLE_ROUNDS) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(settled, bin, "a held camera's tolerance moved");
            TEST_ASSERT_TRUE(steer->settled);
        }
    }
}

/* Along the path in and out, a steered pick never swings back to a bin it
 * left within SETTLE_ROUNDS rounds (A, B, A), and held still at a pose it
 * settles within SETTLE_ROUNDS rounds and stays. */
static void
test_a_steered_tolerance_settles_and_does_not_swing_along_the_path(void) {
    const r3d_lit_mesh_t* mesh = counted_mesh();
    const int budget = (FINEST_CLUSTERS * FINEST_TRIANGLES) / 2;
    const int steps = steps_in();
    r3d_lod_steer_t steer;
    r3d_lod_steer_reset(&steer, LOD.tolerance_px);
    uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
    int swings = 0;
    const int moved = steer_along_the_path(mesh, coarse, &steer, budget, &swings);
    TEST_ASSERT_EQUAL_INT(0, swings);
    TEST_ASSERT_GREATER_THAN_INT(2, moved); /* the path does steer */
    for (int p = 0; p < steps; p += steps / 8) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        hold_settles(mesh, &lens, coarse, &steer, budget);
    }
}

/* Errors bin in the setting's pixels: a picture drawn at twice the scale,
 * its histogram's unit a half, bins as the setting's picture does, and at
 * unit 2 every save sits a whole octave, two bins, further up. */
static void
test_the_histogram_bins_errors_in_the_setting_s_pixels(void) {
    const r3d_lit_mesh_t* mesh = counted_mesh();
    const int steps = steps_in();
    int compared = 0;
    for (int p = 0; p < steps; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_histogram_t once = {{0}, 0, 1.0F};
        r3d_lod_histogram_t twice = {{0}, 0, 2.0F};
        uint32_t coarse[BITS32_WORDS(GROUPS)] = {0};
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, &once);
        memset(coarse, 0, sizeof coarse);
        r3d_lod_pick(mesh, &lens, &LOD, coarse, false, &twice);
        for (int b = 1; b + 2 < R3D_LOD_BINS; b++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(once.saves[b], twice.saves[b + 2], "bin");
            compared += once.saves[b] != 0 ? 1 : 0;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, compared);
}

/* A raster of the levelled mesh placed twice, far off, steered by `steer`
 * within a budget, at pick round 1; `p` and `scratch` hold its instances. */
static raster_t
steered_raster(const r3d_lit_mesh_t* mesh, placed_t* p, void* scratch, render_settings_t* settings,
               r3d_lod_steer_t* steer) {
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(scratch);
    const float depth[PLACED_MAX] = {FAR_EYE, FAR_EYE};
    place(p, mesh, PLACED_MAX, depth);
    settings->lod.triangle_budget = 1000;
    memset(steer, 0, sizeof *steer);
    return (raster_t){.instances = p->instances,
                      .instance_count = PLACED_MAX,
                      .width = W,
                      .height = H,
                      .picks_levels = true,
                      .pick_round = 1,
                      .steer = steer,
                      .scratch = scratch,
                      .settings = settings};
}

static void
test_a_budgeted_morph_uses_the_pick_limits_before_the_steer_moves(void) {
    const r3d_lit_mesh_t* mesh = &morphing_levels()->mesh;
    placed_t* p = malloc(sizeof *p);
    void* scratch = malloc(picked_scratch_bytes(mesh));
    render_settings_t settings = spread_over(1);
    settings.lod.blend = RENDER_LOD_BLEND_ON;
    r3d_lod_steer_t* steer = malloc(sizeof *steer);
    TEST_ASSERT_NOT_NULL(steer);
    raster_t raster = steered_raster(mesh, p, scratch, &settings, steer);
    settings.lod.triangle_budget = INT32_MAX;
    const float tolerance = r3d_lod_bin_edge(FLOOR_EDGE + 2);
    const float enter = tolerance - settings.lod.hysteresis_px;
    r3d_lod_steer_reset(steer, settings.lod.tolerance_px);
    steer->tolerance_px = tolerance;
    steer->bin = FLOOR_EDGE + 2;
    const float depth[PLACED_MAX] = {error_eye(mesh, enter + settings.lod.blend_px * SHARE_HALF), FAR_EYE};
    place(p, mesh, PLACED_MAX, depth);
    const render_view_t view = axis_view();
    (void)raster_census(&raster, &view);
    TEST_ASSERT_NOT_EQUAL_FLOAT(tolerance, steer->tolerance_px);
    r3d_lens_t lens;
    r3d_lens_init(&lens, &view, 1);
    r3d_lens_fit(&lens, raster.width, raster.height);
    r3d_lens_place(&lens, &p->models[0], 1);
    r3d_lod_morph_t morph;
    raster_lod_morph(&raster, &view, &p->instances[0], &lens, &morph);
    TEST_ASSERT_EQUAL_FLOAT(enter, morph.enter);
    const uint32_t* blending = r3d_lod_blending(r3d_lod_arrived(p->coarse[0], GROUPS), GROUPS);
    TEST_ASSERT_TRUE(bits32_get(blending, WEIGHT_GROUP));
    TEST_ASSERT_EQUAL(bits32_get(blending, WEIGHT_GROUP), r3d_lod_morph_weight(&morph, mesh, WEIGHT_CLUSTER) > 0);
    lens = lens_at(error_eye(mesh, enter), NULL);
    raster_lod_morph(&raster, &view, &p->instances[0], &lens, &morph);
    TEST_ASSERT_EQUAL_INT(R3D_LOD_MORPH_ONE, r3d_lod_morph_weight(&morph, mesh, WEIGHT_CLUSTER));
    free(steer);
    free(scratch);
    free(p);
}

/* A steered census spread over two frames: a steer never started (or below
 * the floor) starts at the floor; round 1 counts only instance 1's finest
 * triangles, and the round ends on round 2, instance 0's turn, empty again. */
static void
test_a_steered_census_counts_each_instance_once_a_round(void) {
    enum { EVERY = 2 };

    const r3d_lit_mesh_t* mesh = counted_mesh();
    placed_t* p = malloc(sizeof *p);
    void* scratch = malloc(picked_scratch_bytes(mesh));
    render_settings_t settings = spread_over(EVERY);
    r3d_lod_steer_t steer;
    raster_t raster = steered_raster(mesh, p, scratch, &settings, &steer);
    const render_view_t view = axis_view();
    (void)raster_census(&raster, &view);
    const float started = steer.tolerance_px;
    const int one_round = steer.halves[0].finest + steer.halves[1].finest;
    const int one_frame = steer.frames;
    raster.pick_round = 2;
    (void)raster_census(&raster, &view);
    free(scratch);
    free(p);
    TEST_ASSERT_EQUAL_FLOAT(LOD.tolerance_px, started);
    TEST_ASSERT_EQUAL_INT(r3d_lod_finest_triangles(mesh), one_round);
    TEST_ASSERT_EQUAL_INT(1, one_frame);
    TEST_ASSERT_EQUAL_INT(0, steer.frames);
    TEST_ASSERT_EQUAL_INT(0, steer.halves[0].finest + steer.halves[1].finest);
}

/* With lod.tolerance_px at 0 a steer starts at the first bin's edge, the
 * least floor the coverage cap can rise from, and its cap there is the
 * setting's own. */
static void
test_a_steer_under_a_zero_tolerance_starts_at_the_first_edge(void) {
    const r3d_lit_mesh_t* mesh = counted_mesh();
    placed_t* p = malloc(sizeof *p);
    void* scratch = malloc(picked_scratch_bytes(mesh));
    render_settings_t settings = spread_over(1);
    settings.lod.tolerance_px = 0.0F;
    r3d_lod_steer_t steer;
    raster_t raster = steered_raster(mesh, p, scratch, &settings, &steer);
    settings.lod.triangle_budget = INT32_MAX; /* within budget: the round leaves it at its floor */
    raster.pick_round = 0;
    const render_view_t view = axis_view();
    (void)raster_census(&raster, &view);
    free(scratch);
    free(p);
    TEST_ASSERT_EQUAL_FLOAT(r3d_lod_bin_edge(0), steer.tolerance_px);
    TEST_ASSERT_EQUAL_INT(
        settings.lod.coverage_permille,
        r3d_lod_steer_coverage(steer.tolerance_px, r3d_lod_bin_edge(0), settings.lod.coverage_permille));
}

/* With partial redraw, a camera held still stops picking once its owed
 * share has run, unless the steer moved its tolerance: then censuses run on
 * until a round keeps it, so the moved tolerance reaches every instance. */
static void
test_a_steer_that_moved_owes_picks_while_the_camera_holds_still(void) {
    enum { EVERY = 2 };

    open_spread(EVERY, true);
    render_settings_t settings = spread_over(EVERY);
    settings.partial_redraw = true;
    settings.lod.triangle_budget = 1000;
    render_context_set_settings(spread_context, &settings);
    const render_view_t view = axis_view();
    (void)spread_frame(&view);
    for (int still = 0; still <= EVERY; still++) {
        render_context_set_changed(spread_context, NULL, 0);
        (void)spread_frame(&view);
    }
    const uint32_t rested = spread_context->raster.pick_round;
    render_context_set_changed(spread_context, NULL, 0);
    (void)spread_frame(&view);
    TEST_ASSERT_EQUAL_UINT32(rested, spread_context->raster.pick_round);
    spread_context->steer.settled = false; /* as a round that moved the tolerance leaves it */
    int censuses = 0;
    for (int still = 0; still < 2 * EVERY; still++) {
        const uint32_t before = spread_context->raster.pick_round;
        render_context_set_changed(spread_context, NULL, 0);
        (void)spread_frame(&view);
        censuses += spread_context->raster.pick_round != before ? 1 : 0;
    }
    /* Picks ran on to the end of the round, which kept the tolerance, and no further. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, censuses);
    TEST_ASSERT_LESS_OR_EQUAL_INT(EVERY, censuses);
    TEST_ASSERT_TRUE(spread_context->steer.settled);
}

void
suite_r3d_lod(void) {
    RUN_TEST(test_a_coarse_group_keeps_every_group_simplified_into_it_coarse);
    RUN_TEST(test_every_finest_cluster_is_drawn_at_exactly_one_level_along_the_path);
    RUN_TEST(test_the_drawn_list_holds_what_each_cluster_s_test_draws);
    RUN_TEST(test_a_group_s_bit_follows_its_projected_error_within_the_hysteresis_band);
    RUN_TEST(test_a_camera_backing_off_a_switch_by_less_than_the_band_does_not_switch_back);
    RUN_TEST(test_a_pick_reports_whether_it_changed_a_bit);
    RUN_TEST(test_a_group_never_projects_larger_than_its_parent_under_a_stretched_turned_model);
    RUN_TEST(test_a_group_covering_more_than_the_cap_stays_fine_whatever_its_error);
    RUN_TEST(test_the_least_error_skips_only_poses_that_pick_nothing_coarse);
    RUN_TEST(test_a_group_s_error_projects_at_the_model_s_largest_scale);
    RUN_TEST(test_a_parent_turning_coarse_takes_an_out_of_order_source_with_it);
    RUN_TEST(test_the_closing_pass_visits_every_cluster_when_levels_are_not_laid_out_apart);
    RUN_TEST(test_without_bits_a_levelled_mesh_walks_only_its_finest_clusters);
    RUN_TEST(test_a_mesh_without_levels_walks_every_cluster_whatever_its_bits);
    RUN_TEST(test_levels_laid_out_apart_start_the_walk_at_the_finest_clusters_tree);
    RUN_TEST(test_a_root_whose_children_mix_finest_and_coarse_clusters_keeps_the_walk_at_the_root);
    RUN_TEST(test_coarse_clusters_out_of_group_order_keep_the_walk_at_the_root);
    RUN_TEST(test_the_finest_tree_walk_and_the_cut_keep_what_the_whole_walk_keeps);
    RUN_TEST(test_the_pick_keeps_the_bits_of_an_instance_culled_whole);
    RUN_TEST(test_a_cull_that_does_not_pick_levels_leaves_every_bit);
    RUN_TEST(test_a_pick_spread_over_frames_picks_each_instance_in_its_own_frame);
    RUN_TEST(test_a_context_s_first_frame_picks_every_instance_of_a_spread_pick);
    RUN_TEST(test_a_still_instance_that_switches_cut_on_a_still_frame_redraws_every_row);
    RUN_TEST(test_the_counted_walk_counts_the_coarse_clusters_it_keeps);
    RUN_TEST(test_the_morph_weight_falls_from_whole_to_none_over_a_pixel_past_the_pick_leave_error);
    RUN_TEST(test_the_morph_thresholds_scale_with_the_pixels_per_output_pixel);
    RUN_TEST(test_the_morph_weight_falls_over_a_quarter_of_the_cap_above_the_coverage_cap);
    RUN_TEST(test_the_lesser_of_the_error_and_coverage_weights_is_drawn);
    RUN_TEST(test_there_is_no_morph_weight_without_a_parent_without_morph_data_or_when_disabled);
    RUN_TEST(test_a_group_turning_fine_has_its_arrived_bit_set);
    RUN_TEST(test_the_arrived_bit_clears_once_the_error_projects_past_leave_and_the_morph_ramp);
    RUN_TEST(test_a_coarse_group_never_has_its_arrived_bit);
    RUN_TEST(test_an_arrived_group_blends_whole_up_to_leave_and_any_other_from_enter);
    RUN_TEST(test_a_pick_flipping_only_a_blend_bit_reports_a_change);
    RUN_TEST(test_the_blending_bit_is_set_where_the_blend_weighs_something);
    RUN_TEST(test_a_group_without_its_blending_bit_weighs_none);
    RUN_TEST(test_arrived_groups_blend_whole_up_to_leave);
    RUN_TEST(test_the_dither_bands_of_a_blending_group_s_two_levels_cover_every_level_exactly_once);
    RUN_TEST(test_a_group_with_an_empty_coarse_level_fades_its_fine_clusters_out);
    RUN_TEST(test_the_coarse_cluster_covers_more_levels_as_the_blend_weight_grows);
    RUN_TEST(test_a_cluster_whose_groups_do_not_blend_has_no_dither_band);
    RUN_TEST(test_the_dither_arm_has_no_morph_weight_and_the_morph_arm_no_dither_band);
    RUN_TEST(test_the_dither_needs_no_morph_targets_and_the_switch_arm_has_no_band);
    RUN_TEST(test_a_blending_group_keeps_both_its_fine_clusters_and_those_simplified_from_it);
    RUN_TEST(test_the_pipeline_cull_keeps_both_levels_of_a_blending_group);
    RUN_TEST(test_the_whole_tree_walk_keeps_both_levels_of_a_blending_group);
    RUN_TEST(test_the_counted_walk_keeps_both_levels_of_a_blending_group);
    RUN_TEST(test_the_pipeline_cull_without_blending_bits_is_the_plain_cull);
    RUN_TEST(test_the_pipeline_cull_drops_a_blending_group_s_clusters_under_a_coarse_parent);
    RUN_TEST(test_a_cluster_transformed_with_the_whole_weight_lands_on_its_targets);
    RUN_TEST(test_a_cluster_transformed_with_no_weight_matches_the_plain_transform_exactly);
    RUN_TEST(test_a_null_morph_transforms_as_the_plain_transform_does);
    RUN_TEST(test_a_part_weight_puts_a_vertex_between_where_it_is_and_its_target);
    RUN_TEST(test_a_blending_instance_draws_the_same_pixels_with_its_plain_clusters_split_off);
    RUN_TEST(test_the_histogram_predicts_the_triangles_a_pick_at_each_bin_edge_draws);
    RUN_TEST(test_the_steer_takes_the_least_edge_past_the_floor_within_the_budget);
    RUN_TEST(test_the_steer_rises_at_once_and_falls_a_bin_a_round);
    RUN_TEST(test_the_steer_holds_where_only_the_next_bin_down_fits);
    RUN_TEST(test_a_steered_tolerance_settles_and_does_not_swing_along_the_path);
    RUN_TEST(test_the_histogram_bins_errors_in_the_setting_s_pixels);
    RUN_TEST(test_a_budgeted_morph_uses_the_pick_limits_before_the_steer_moves);
    RUN_TEST(test_a_steered_census_counts_each_instance_once_a_round);
    RUN_TEST(test_a_steer_under_a_zero_tolerance_starts_at_the_first_edge);
    RUN_TEST(test_a_steer_that_moved_owes_picks_while_the_camera_holds_still);
}

SUITE_REGISTER(suite_r3d_lod);

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
    return (r3d_lit_lod_group_t){{{x, 0, 0}, (int16_t)radius}, (uint16_t)(inner[first].error * ERROR_GROWTH)};
}

static const r3d_lit_mesh_t*
levelled_mesh(void) {
    levelled = calloc(1, sizeof *levelled);
    suite_set_test_cleanup(release);
    TEST_ASSERT_NOT_NULL(levelled);
    r3d_lit_lod_group_t* g = levelled->groups;
    for (int i = 0; i < FINEST_GROUPS; i++) {
        const int16_t x = (int16_t)((2 * i - (FINEST_GROUPS - 1)) * FINEST_SPACING / 2);
        g[i] = (r3d_lit_lod_group_t){{{x, 0, 0}, FINEST_RADIUS}, FINEST_ERROR};
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
    work = malloc(r3d_pipeline_work_bytes());
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
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    const int poses = 2 * steps;
    uint32_t ever_coarse = 0, ever_fine = 0;
    for (int p = 0; p < poses; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse);
        check(mesh, &lens, coarse);
        ever_coarse |= coarse[0];
        ever_fine |= ~coarse[0];
    }
    const uint32_t every = (1U << GROUPS) - 1U;
    TEST_ASSERT_EQUAL_HEX32(every, ever_coarse & every);
    TEST_ASSERT_EQUAL_HEX32(every, ever_fine & every);
}

static void
parents_coarse_keep_children_coarse(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint32_t* coarse) {
    (void)lens;
    for (int c = 0; c < CLUSTERS; c++) {
        const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
        if (lod.own != R3D_LOD_NONE && lod.parent != R3D_LOD_NONE && r3d_lod_coarse(coarse, lod.parent)) {
            TEST_ASSERT_TRUE(r3d_lod_coarse(coarse, lod.own));
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
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    const int poses = 2 * steps;
    bool drew_top = false, drew_finest = false;
    for (int p = 0; p < poses; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse);
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
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    const int poses = 2 * steps;
    for (int p = 0; p < poses; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse);
        for (int g = 0; g < GROUPS; g++) {
            const float px = r3d_lod_group_pixels(mesh, &lens, g);
            const bool capped = r3d_lod_group_coverage(mesh, &lens, g) > COVERAGE_MAX;
            if (r3d_lod_coarse(coarse, (uint16_t)g)) {
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
    r3d_lod_pick(mesh, &lens, &LOD, coarse);
    TEST_ASSERT_TRUE(r3d_lod_coarse(coarse, TOP_GROUP));
    while (r3d_lod_coarse(coarse, TOP_GROUP)) {
        TEST_ASSERT_TRUE(d > NEAR_EYE);
        d *= STEP;
        lens = lens_at(d, NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse);
    }
    TEST_ASSERT_TRUE(r3d_lod_group_pixels(mesh, &lens, TOP_GROUP) > TOLERANCE + HYSTERESIS);
    return d;
}

/* One step back is inside the band: the top group holds fine, and a still
 * camera keeps every bit. */
static void
test_a_camera_backing_off_a_switch_by_less_than_the_band_does_not_switch_back(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    const r3d_lens_t lens = lens_at(top_turns_fine(mesh, coarse) / STEP, NULL);
    const float back = r3d_lod_group_pixels(mesh, &lens, TOP_GROUP);
    TEST_ASSERT_TRUE(back > TOLERANCE - HYSTERESIS && back <= TOLERANCE + HYSTERESIS);
    r3d_lod_pick(mesh, &lens, &LOD, coarse);
    const uint32_t held = coarse[0];
    TEST_ASSERT_FALSE(r3d_lod_coarse(coarse, TOP_GROUP));
    r3d_lod_pick(mesh, &lens, &LOD, coarse);
    TEST_ASSERT_EQUAL_HEX32(held, coarse[0]);
}

/* Flying in from FAR_EYE, a pick that changes a bit says so, and the same
 * pick again under a still camera changes nothing and says so: what partial
 * redraw reads to redraw every row (render/partial). */
static void
test_a_pick_reports_whether_it_changed_a_bit(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    int changes = 0;
    for (float d = FAR_EYE; d > NEAR_EYE; d *= STEP) {
        const r3d_lens_t lens = lens_at(d, NULL);
        uint32_t before[R3D_LOD_WORDS(GROUPS)];
        memcpy(before, coarse, sizeof before);
        const bool changed = r3d_lod_pick(mesh, &lens, &LOD, coarse);
        if (memcmp(before, coarse, sizeof before) != 0) {
            TEST_ASSERT_TRUE(changed);
            changes++;
        }
        TEST_ASSERT_FALSE(r3d_lod_pick(mesh, &lens, &LOD, coarse));
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
        TEST_ASSERT_EQUAL(small, r3d_lod_coarse(coarse, (uint16_t)g));
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
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    uint32_t skipping[R3D_LOD_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    for (int p = 0; p < 2 * steps; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse);
        r3d_lod_pick(&bounded, &lens, &LOD, skipping);
        TEST_ASSERT_EQUAL_HEX32_ARRAY(coarse, skipping, R3D_LOD_WORDS(GROUPS));
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
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    const r3d_lens_t lens = lens_at(FAR_EYE, NULL);
    TEST_ASSERT_TRUE(r3d_lod_group_pixels(mesh, &lens, 0) > TOLERANCE + HYSTERESIS);
    r3d_lod_pick(mesh, &lens, &LOD, coarse);
    TEST_ASSERT_TRUE(r3d_lod_coarse(coarse, TOP_GROUP));
    TEST_ASSERT_TRUE(r3d_lod_coarse(coarse, 0));
    for (int f = 0; f < FINEST_CLUSTERS; f++) {
        TEST_ASSERT_EQUAL_INT(1, levels_drawn(mesh, f, coarse));
    }
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
    uint32_t all_coarse[R3D_LOD_WORDS(GROUPS)];
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
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    const int steps = steps_in();
    bool cut_drawn = false;
    for (int p = 0; p < 2 * steps; p++) {
        const r3d_lens_t lens = lens_at(eye_at(p, steps), NULL);
        r3d_lod_pick(mesh, &lens, &LOD, coarse);
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
    uint32_t coarse[PLACED_MAX][R3D_LOD_WORDS(GROUPS)];
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
        coarse |= r3d_lod_coarse(p->coarse[i], TOP_GROUP) ? 1U << i : 0U;
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

/* A render context's first frame picks every instance, however far the pick
 * is spread; the frames after it pick their share, and forgetting the scene
 * makes the next frame a first frame again. */
static void
test_a_context_s_first_frame_picks_every_instance_of_a_spread_pick(void) {
    enum { EVERY = 4 };

    const r3d_lit_mesh_t* mesh = levelled_mesh();
    suite_set_test_cleanup(release_spread_context);
    spread_placed = malloc(sizeof *spread_placed);
    spread_context = malloc(sizeof *spread_context);
    TEST_ASSERT_NOT_NULL(spread_placed);
    TEST_ASSERT_NOT_NULL(spread_context);
    const float depth[PLACED_MAX] = {FAR_EYE, FAR_EYE};
    place(spread_placed, mesh, PLACED_MAX, depth);
    *spread_context = RENDER_CONTEXT_DEFAULT;
    render_context_set_scale(spread_context, 100);
    const render_settings_t settings = spread_over(EVERY);
    render_context_set_settings(spread_context, &settings);
    const render_view_t view = axis_view();
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &view, 0));
    const unsigned first = top_coarse(spread_placed, PLACED_MAX);
    memset(spread_placed->coarse, 0, sizeof spread_placed->coarse);
    /* Round 1 of 4: neither instance's share. */
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &view, 0));
    const unsigned second = top_coarse(spread_placed, PLACED_MAX);
    render_context_forget(spread_context);
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &view, 0));
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

    const r3d_lit_mesh_t* mesh = levelled_mesh();
    suite_set_test_cleanup(release_spread_context);
    spread_placed = malloc(sizeof *spread_placed);
    spread_context = malloc(sizeof *spread_context);
    TEST_ASSERT_NOT_NULL(spread_placed);
    TEST_ASSERT_NOT_NULL(spread_context);
    const float depth[PLACED_MAX] = {FAR_EYE, FAR_EYE};
    place(spread_placed, mesh, PLACED_MAX, depth);
    *spread_context = RENDER_CONTEXT_DEFAULT;
    render_context_set_scale(spread_context, 100);
    render_settings_t settings = spread_over(EVERY);
    settings.partial_redraw = true;
    render_context_set_settings(spread_context, &settings);
    const render_view_t far = axis_view();
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &far, 0));
    TEST_ASSERT_EQUAL_HEX(0x3, top_coarse(spread_placed, PLACED_MAX));
    const fixture_camera_t camera = {{0.0F, 0.0F, FAR_EYE - WHOLE_IN_VIEW}, {0.0F, 0.0F, 1.0F}, HALF_FOV_TAN, NEAR_Z};
    const render_view_t near = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &near, 0));
    TEST_ASSERT_EQUAL_HEX(0x1, top_coarse(spread_placed, PLACED_MAX)); /* instance 0 not yet picked */
    spread_destination = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(spread_destination);
    render_context_set_changed(spread_context, NULL, 0);
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &near, 0));
    (void)render_context_compose(spread_context, spread_destination, NULL, NULL);
    const render_context_frame_t owed = render_context_frame(spread_context);
    TEST_ASSERT_EQUAL_HEX(0x0, top_coarse(spread_placed, PLACED_MAX));
    TEST_ASSERT_FALSE(owed.unchanged);
    TEST_ASSERT_EQUAL_INT(H, owed.rows_end - owed.rows_first);
    render_context_set_changed(spread_context, NULL, 0);
    TEST_ASSERT_TRUE(render_context_draw(spread_context, spread_placed->instances, PLACED_MAX, &near, 0));
    TEST_ASSERT_TRUE(render_context_frame(spread_context).unchanged);
}

/* The counted walk tells the coarse clusters it keeps from the finest: none
 * with every group fine, all with the top group coarse. */
static void
test_the_counted_walk_counts_the_coarse_clusters_it_keeps(void) {
    const r3d_lit_mesh_t* mesh = levelled_mesh();
    r3d_pipeline_count_walk_t* walk = malloc(sizeof *walk);
    TEST_ASSERT_NOT_NULL(walk);
    const r3d_lens_t lens = lens_at(WHOLE_IN_VIEW, NULL);
    uint32_t coarse[R3D_LOD_WORDS(GROUPS)] = {0};
    uint16_t out[CLUSTERS];
    r3d_pipeline_counts_t fine = {0};
    TEST_ASSERT_EQUAL_INT(FINEST_CLUSTERS, r3d_pipeline_cull_counted(mesh, &lens, coarse, out, walk, &fine));
    TEST_ASSERT_EQUAL_INT(0, fine.clusters_coarse);
    coarse[0] = (1U << GROUPS) - 1U;
    r3d_pipeline_counts_t top = {0};
    TEST_ASSERT_EQUAL_INT(1, r3d_pipeline_cull_counted(mesh, &lens, coarse, out, walk, &top));
    TEST_ASSERT_EQUAL_INT(1, top.clusters_coarse);
    free(walk);
}

void
suite_r3d_lod(void) {
    RUN_TEST(test_a_coarse_group_keeps_every_group_simplified_into_it_coarse);
    RUN_TEST(test_every_finest_cluster_is_drawn_at_exactly_one_level_along_the_path);
    RUN_TEST(test_a_group_s_bit_follows_its_projected_error_within_the_hysteresis_band);
    RUN_TEST(test_a_camera_backing_off_a_switch_by_less_than_the_band_does_not_switch_back);
    RUN_TEST(test_a_pick_reports_whether_it_changed_a_bit);
    RUN_TEST(test_a_group_never_projects_larger_than_its_parent_under_a_stretched_turned_model);
    RUN_TEST(test_a_group_covering_more_than_the_cap_stays_fine_whatever_its_error);
    RUN_TEST(test_the_least_error_skips_only_poses_that_pick_nothing_coarse);
    RUN_TEST(test_a_group_s_error_projects_at_the_model_s_largest_scale);
    RUN_TEST(test_a_parent_turning_coarse_takes_an_out_of_order_source_with_it);
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
}

SUITE_REGISTER(suite_r3d_lod);

/*
 * Portable suite: the three baked Sponza meshes, read from the sponza pack,
 * and the camera loop through them, the scene's camera once it has loaded
 * (sponza_content.h). Each mesh is checked for the structure
 * r3d_pipeline.h relies on, never against the generator; the path and
 * the pictures it sees are checked against the shipped meshes themselves.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "apps/render_lab/sponza_content.h"
#include "asset/asset_store.h"
#include "r3d_lit_mesh_expect.h"
#include "render/r3d.h"
#include "render/r3d_pipeline.h"
#include "scene/scene.h"
#include "sponza_suite.h"
#include "util/runtime/memory.h"

/* The scene, loaded for the suite, its pack, and the three bakes in it. */
static scene_t* sponza;
static const asset_pack_t* pack;
static const r3d_scene_camera_t* flythrough;
#define MESH_FULL (&mesh_full)
#define MESH_LITE (&mesh_lite)
#define MESH_FLAT (&mesh_flat)

static r3d_lit_mesh_t mesh_full;
static r3d_lit_mesh_t mesh_lite;
static r3d_lit_mesh_t mesh_flat;

static void
require_the_scene(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(sponza, "scene sponza did not load: see the log above");
}

static void
open_the_mesh(const char* id, r3d_lit_mesh_t* mesh) {
    TEST_ASSERT_NOT_NULL(id);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, r3d_lit_mesh_open(pack, id, mesh), id);
}

/* The pack id of the mesh `bake`'s entity draws. */
static const char*
mesh_of(sponza_bake_t bake) {
    const scene_entity_t entity = scene_find(sponza, sponza_bakes[bake]);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SCENE_ENTITY_NONE, entity, sponza_bakes[bake]);
    return scene_entity_mesh_id(sponza, entity);
}

static void
open_the_meshes(void) {
    require_the_scene();
    open_the_mesh(mesh_of(SPONZA_BAKE_FULL), &mesh_full);
    open_the_mesh(mesh_of(SPONZA_BAKE_LITE), &mesh_lite);
    open_the_mesh(mesh_of(SPONZA_BAKE_FLAT), &mesh_flat);
}

/* The flat reference: every cluster's eight corners against each plane. */
static bool
cluster_in_view(const r3d_lit_cluster_t* c, const r3d_lens_t* lens) {
    int beyond[5] = {0};
    for (int i = 0; i < 8; i++) {
        const float x = (float)(i & 1 ? c->hi[0] : c->lo[0]);
        const float y = (float)(i & 2 ? c->hi[1] : c->lo[1]);
        const float z = (float)(i & 4 ? c->hi[2] : c->lo[2]);
        const float lx = (lens->m[0][0] * x) + (lens->m[0][1] * y) + (lens->m[0][2] * z) + lens->m[0][3];
        const float ly = (lens->m[1][0] * x) + (lens->m[1][1] * y) + (lens->m[1][2] * z) + lens->m[1][3];
        const float lz = (lens->m[2][0] * x) + (lens->m[2][1] * y) + (lens->m[2][2] * z) + lens->m[2][3];
        beyond[0] += lz < lens->near_z;
        beyond[1] += lx < -lens->center_x * lz;
        beyond[2] += lx > ((float)lens->width - lens->center_x) * lz;
        beyond[3] += ly < -lens->center_y * lz;
        beyond[4] += ly > ((float)lens->height - lens->center_y) * lz;
    }
    for (int p = 0; p < 5; p++) {
        if (beyond[p] == 8) {
            return false;
        }
    }
    return true;
}

static void
check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(const r3d_lit_mesh_t* mesh) {
    const uint32_t period = r3d_scene_camera_period_ms(flythrough);
    uint16_t* walked = malloc(sizeof(*walked) * (size_t)mesh->cluster_count);
    uint8_t* kept = malloc((size_t)mesh->cluster_count);
    TEST_ASSERT_NOT_NULL(walked);
    TEST_ASSERT_NOT_NULL(kept);
    for (uint32_t t = 0; t < period; t += 2500) {
        const camera_t camera = r3d_scene_camera_at(flythrough, t);
        const viewport_t viewport = {SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT, (int)(t / 2500) & 3};
        r3d_lens_t lens;
        r3d_lens_init(&lens, &camera, mesh->position_scale, viewport);

        memset(kept, 0, (size_t)mesh->cluster_count);
        const int count = r3d_pipeline_cull(mesh, &lens, walked);
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, kept[walked[i]], "a cluster was listed twice");
            kept[walked[i]] = 1;
        }
        int flat = 0;
        for (int c = 0; c < mesh->cluster_count; c++) {
            const bool in_view = cluster_in_view(&mesh->clusters[c], &lens);
            flat += in_view;
            TEST_ASSERT_EQUAL_MESSAGE(in_view, kept[c], "the walk and the flat test disagree on a cluster");
        }
        TEST_ASSERT_EQUAL_INT(flat, count);
    }
    free(kept);
    free(walked);
}

typedef struct {
    float x, y, z;
} v3;

static v3
sub(v3 a, v3 b) {
    return (v3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static float
dot(v3 a, v3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

static v3
add_scaled(v3 a, v3 d, float s) {
    return (v3){a.x + (d.x * s), a.y + (d.y * s), a.z + (d.z * s)};
}

/* Ericson, Real-Time Collision Detection 5.1.5. */
static float
point_triangle_distance(v3 p, v3 a, v3 b, v3 c) {
    const v3 ab = sub(b, a);
    const v3 ac = sub(c, a);
    const v3 ap = sub(p, a);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    v3 q;
    if (d1 <= 0 && d2 <= 0) {
        q = a;
    } else {
        const v3 bp = sub(p, b);
        const float d3 = dot(ab, bp);
        const float d4 = dot(ac, bp);
        const v3 cp = sub(p, c);
        const float d5 = dot(ab, cp);
        const float d6 = dot(ac, cp);
        const float vc = (d1 * d4) - (d3 * d2);
        const float vb = (d5 * d2) - (d1 * d6);
        const float va = (d3 * d6) - (d5 * d4);
        if (d3 >= 0 && d4 <= d3) {
            q = b;
        } else if (d6 >= 0 && d5 <= d6) {
            q = c;
        } else if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            q = add_scaled(a, ab, d1 / (d1 - d3));
        } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            q = add_scaled(a, ac, d2 / (d2 - d6));
        } else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            q = add_scaled(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
        } else {
            const float denom = 1.0F / (va + vb + vc);
            q = add_scaled(add_scaled(a, ab, vb * denom), ac, vc * denom);
        }
    }
    const v3 d = sub(p, q);
    return sqrtf(dot(d, d));
}

static v3
vertex(const r3d_lit_mesh_t* mesh, int i) {
    const float s = 1.0F / (float)mesh->position_scale;
    return (v3){(float)mesh->positions[i][0] * s, (float)mesh->positions[i][1] * s, (float)mesh->positions[i][2] * s};
}

static float
box_distance(const r3d_lit_mesh_t* mesh, const r3d_lit_cluster_t* c, v3 p) {
    const float s = 1.0F / (float)mesh->position_scale;
    const float point[3] = {p.x, p.y, p.z};
    float sum = 0.0F;
    for (int k = 0; k < 3; k++) {
        const float lo = (float)c->lo[k] * s;
        const float hi = (float)c->hi[k] * s;
        const float d = point[k] < lo ? lo - point[k] : (point[k] > hi ? point[k] - hi : 0.0F);
        sum += d * d;
    }
    return sqrtf(sum);
}

static float
clearance(const r3d_lit_mesh_t* mesh, v3 p) {
    float best = INFINITY;
    for (int i = 0; i < mesh->cluster_count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[i];
        if (box_distance(mesh, c, p) >= best) {
            continue;
        }
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const float d =
                point_triangle_distance(p, vertex(mesh, tri[0]), vertex(mesh, tri[1]), vertex(mesh, tri[2]));
            if (d < best) {
                best = d;
            }
        }
    }
    return best;
}

static void
check_the_flythrough_keeps_clear_of_every_triangle(const r3d_lit_mesh_t* mesh) {
    const uint32_t period = r3d_scene_camera_period_ms(flythrough);
    for (uint32_t t = 0; t < period; t += 100) {
        vec3f_t eye;
        vec3f_t forward;
        r3d_scene_camera_sample(flythrough, t, &eye, &forward);
        const float d = clearance(mesh, (v3){eye.x, eye.y, eye.z});
        if (d < SPONZA_FLYTHROUGH_CLEARANCE) {
            char message[96];
            TEST_ASSERT_TRUE(snprintf(message, sizeof message, "t=%u ms eye (%.0f, %.0f, %.0f) is %.1f from a triangle",
                                      (unsigned)t, (double)eye.x, (double)eye.y, (double)eye.z, (double)d)
                             > 0);
            TEST_FAIL_MESSAGE(message);
        }
    }
}

/* Every check holds for both bakes. */
static void
test_both_bakes_have_the_structure_the_pipeline_relies_on(void) {
    open_the_meshes();
    r3d_lit_mesh_expect_valid(MESH_FULL);
    r3d_lit_mesh_expect_valid(MESH_LITE);
    r3d_lit_mesh_expect_valid(MESH_FLAT);
}

/* The baked clusters are meshlets: none over 32 triangles, and sharing
 * vertices enough that a cluster holds fewer vertices than triangles. */
static void
check_the_clusters_are_meshlets(const r3d_lit_mesh_t* mesh) {
    for (int c = 0; c < mesh->cluster_count; c++) {
        TEST_ASSERT_TRUE_MESSAGE(mesh->clusters[c].triangle_count <= 32, "a cluster is bigger than a meshlet");
    }
    TEST_ASSERT_TRUE_MESSAGE(mesh->vertex_count < mesh->triangle_count, "meshlets do not share their vertices");
}

static void
test_both_bakes_are_cut_into_meshlets(void) {
    open_the_meshes();
    check_the_clusters_are_meshlets(MESH_FULL);
    check_the_clusters_are_meshlets(MESH_LITE);
    check_the_clusters_are_meshlets(MESH_FLAT);
}

static void
test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(void) {
    open_the_meshes();
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(MESH_FULL);
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(MESH_LITE);
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(MESH_FLAT);
}

static void
test_the_flythrough_keeps_clear_of_every_triangle(void) {
    open_the_meshes();
    check_the_flythrough_keeps_clear_of_every_triangle(MESH_FULL);
    check_the_flythrough_keeps_clear_of_every_triangle(MESH_LITE);
    check_the_flythrough_keeps_clear_of_every_triangle(MESH_FLAT);
}

/* At the pace of a slow walk, with the seam between a lap's end and its start
 * as smooth as anywhere else. */
static void
test_the_flythrough_moves_smoothly_and_closes_its_loop(void) {
    require_the_scene();
    const uint32_t period = r3d_scene_camera_period_ms(flythrough);
    vec3f_t previous;
    vec3f_t forward;
    r3d_scene_camera_sample(flythrough, 0, &previous, &forward);
    for (uint32_t t = 10; t <= period + 100; t += 10) {
        vec3f_t eye;
        r3d_scene_camera_sample(flythrough, t, &eye, &forward);
        const vec3f_t step = vec3f_sub(eye, previous);
        TEST_ASSERT_TRUE_MESSAGE(vec3f_dot(step, step) < 2.0F * 2.0F, "the eye jumped between two samples 10 ms apart");
        TEST_ASSERT_FLOAT_WITHIN(0.001F, 1.0F, sqrtf(vec3f_dot(forward, forward)));
        previous = eye;
    }
}

/* The fraction of the picture covered at `t_ms` into the flythrough. */
static float
share_covered_at(const raster_t* raster, uint32_t t_ms) {
    const camera_t camera = r3d_scene_camera_at(flythrough, t_ms);
    raster_draw(raster, &camera, 0);
    const int pixels = raster->width * raster->height;
    const uint16_t* depth = raster_depth(raster);
    int covered = 0;
    for (int i = 0; i < pixels; i++) {
        covered += depth[i] != 0;
    }
    return (float)covered / (float)pixels;
}

/* The camera stays inside the building, so on average walls, floor and
 * galleries fill well over nine tenths of the picture (0.72 at the least,
 * looking up at the sky). Wound the wrong way round, the same bake shows
 * only the building's far sides, about 0.6. */
static void
check_the_flythrough_sees_mostly_building(const r3d_lit_mesh_t* mesh) {
    const r3d_instance_t instance = {mesh, NULL};
    raster_t raster = {
        .instances = &instance, .instance_count = 1, .width = SPONZA_RENDER_WIDTH, .height = SPONZA_RENDER_HEIGHT};
    void* scratch = memory_alloc(raster_scratch_bytes(&raster), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(scratch);
    raster.scratch = scratch;
    const uint32_t period = r3d_scene_camera_period_ms(flythrough);
    float sum = 0.0F;
    int samples = 0;
    for (uint32_t t = 0; t < period; t += SPONZA_POSE_EVERY_MS) {
        sum += share_covered_at(&raster, t);
        samples++;
    }
    memory_free(scratch);
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(0.85F, sum / (float)samples, "the flythrough sees mostly sky");
}

static void
test_the_flythrough_sees_mostly_building(void) {
    open_the_meshes();
    check_the_flythrough_sees_mostly_building(MESH_FULL);
    check_the_flythrough_sees_mostly_building(MESH_LITE);
    check_the_flythrough_sees_mostly_building(MESH_FLAT);
}

/* The pack the build wrote for the scene: on the device, the one flashed
 * to the assets partition. The scene loads from it, and each bake's mesh
 * opens. */
static void
test_the_scene_loads_from_its_pack_with_a_lit_mesh_for_each_bake_and_its_path(void) {
    require_the_scene();
    for (int i = 0; i < (int)SPONZA_BAKE_COUNT; i++) {
        r3d_lit_mesh_t mesh;
        open_the_mesh(mesh_of((sponza_bake_t)i), &mesh);
        TEST_ASSERT_GREATER_THAN_INT(0, mesh.triangle_count);
    }
    TEST_ASSERT_NOT_NULL(flythrough);
    TEST_ASSERT_GREATER_THAN_UINT32(0, r3d_scene_camera_period_ms(flythrough));
}

static void
run_sponza_suite(void) {
    sponza_suite_t loaded = sponza_suite_load();
    sponza = loaded.scene;
    pack = loaded.pack;
    flythrough = loaded.path;
    RUN_TEST(test_the_scene_loads_from_its_pack_with_a_lit_mesh_for_each_bake_and_its_path);
    RUN_TEST(test_both_bakes_have_the_structure_the_pipeline_relies_on);
    RUN_TEST(test_both_bakes_are_cut_into_meshlets);
    RUN_TEST(test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps);
    RUN_TEST(test_the_flythrough_moves_smoothly_and_closes_its_loop);
    RUN_TEST(test_the_flythrough_keeps_clear_of_every_triangle);
    RUN_TEST(test_the_flythrough_sees_mostly_building);
    loaded = (sponza_suite_t){sponza, pack, flythrough};
    sponza_suite_release(&loaded);
    sponza = NULL;
    pack = NULL;
    flythrough = NULL;
}

SUITE_REGISTER(run_sponza_suite);

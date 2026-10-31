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
#include "core/memory.h"
#include "display/viewport.h"
#include "r3d_lit_mesh_expect.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"
#include "render/raster.h"
#include "scene/scene.h"
#include "sponza_suite.h"

/* The scene, loaded for the suite, its pack, and the three bakes in it. */
static scene_t* sponza;
static const asset_pack_t* pack;
static const camera_t* flythrough;
static scene_entity_t camera_entity;
static const char* camera_name;
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
mesh_of(scene_baked_variant_t bake) {
    const scene_entity_t entity = scene_find(sponza, name_hash(sponza_bakes[bake]));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SCENE_ENTITY_NONE, entity, sponza_bakes[bake]);
    return scene_entity_mesh_id(sponza, entity);
}

static void
open_the_meshes(void) {
    require_the_scene();
    open_the_mesh(mesh_of(SCENE_BAKED_FULL), &mesh_full);
    open_the_mesh(mesh_of(SCENE_BAKED_LITE), &mesh_lite);
    open_the_mesh(mesh_of(SCENE_BAKED_FLAT), &mesh_flat);
}

/* Hand-written planes keep this an independent check of the cull. */
static bool
cluster_in_view(const r3d_lit_cluster_t* c, const r3d_lens_t* lens) {
    int beyond[5] = {0};
    for (int i = 0; i < 8; i++) {
        const vec3f_t l = mat4f_apply(&lens->m, vec3f_from_vec3s(box3s_corner(c->bounds, i), 1.0F));
        beyond[0] += l.z < lens->project.near_z;
        beyond[1] += l.x < -lens->project.cx * l.z;
        beyond[2] += l.x > ((float)lens->width - lens->project.cx) * l.z;
        beyond[3] += l.y < -lens->project.cy * l.z;
        beyond[4] += l.y > ((float)lens->height - lens->project.cy) * l.z;
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
    const uint32_t period = scene_animator_clip_duration_ms(sponza, camera_entity, 0);
    r3d_pipeline_work_t* work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    uint16_t* walked = malloc(sizeof(*walked) * (size_t)mesh->cluster_count);
    uint8_t* kept = malloc((size_t)mesh->cluster_count);
    TEST_ASSERT_NOT_NULL(walked);
    TEST_ASSERT_NOT_NULL(kept);
    for (uint32_t t = 0; t < period; t += 2500) {
        const viewport_t viewport = {SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT, (int)(t / 2500) & 3};
        (void)scene_animator_seek(sponza, camera_entity, SCENE_ANIMATOR_CURRENT, t);
        const render_view_t view = sponza_suite_view(flythrough, sponza, camera_entity, viewport);
        r3d_lens_t lens;
        r3d_lens_init(&lens, &view, mesh->position_scale);

        memset(kept, 0, (size_t)mesh->cluster_count);
        const int count = r3d_pipeline_cull(mesh, &lens, walked, work, NULL);
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
    free(work);
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
    const vec3f_t point = {p.x, p.y, p.z};
    const vec3f_t closest = box3f_closest_point(box3f_from_box3s(c->bounds, s), point);
    const vec3f_t delta = vec3f_sub(closest, point);
    return sqrtf(vec3f_dot(delta, delta));
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
    const uint32_t period = scene_animator_clip_duration_ms(sponza, camera_entity, 0);
    for (uint32_t t = 0; t < period; t += 100) {
        (void)scene_animator_seek(sponza, camera_entity, SCENE_ANIMATOR_CURRENT, t);
        const vec3f_t eye = scene_entity_local(sponza, camera_entity).position;
        const float d = clearance(mesh, (v3){eye.x, eye.y, eye.z});
        if (d < SPONZA_FLYTHROUGH_CLEARANCE) {
            char message[96];
            TEST_ASSERT_TRUE(snprintf(message, sizeof message,
                                      "%s t=%u ms eye (%.0f, %.0f, %.0f) is %.1f from a triangle", camera_name,
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

/* Points the suite's camera statics at camera `name`, which must have a path. */
static void
select_camera(const char* name) {
    camera_name = name;
    flythrough = scene_camera_lens(sponza, name_hash(name));
    camera_entity = scene_find(sponza, name_hash(name));
    TEST_ASSERT_NOT_NULL_MESSAGE(flythrough, name);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, scene_animator_clip_duration_ms(sponza, camera_entity, 0), name);
}

/* Kept out of line: the stack gate follows its calls through `check`. */
static __attribute__((noinline)) void
check_camera_paths(void (*check)(const r3d_lit_mesh_t*)) {
    for (int camera = 0; camera < SPONZA_CAMERA_COUNT; camera++) {
        select_camera(sponza_cameras[camera]);
        check(MESH_FULL);
        check(MESH_LITE);
        check(MESH_FLAT);
    }
    select_camera(sponza_cameras[0]);
}

static void
test_the_flythrough_keeps_clear_of_every_triangle(void) {
    open_the_meshes();
    check_camera_paths(check_the_flythrough_keeps_clear_of_every_triangle);
}

/* At the pace of a slow walk, with the seam between a lap's end and its start
 * as smooth as anywhere else. */
static void
test_the_flythrough_moves_smoothly_and_closes_its_loop(void) {
    require_the_scene();
    for (int camera = 0; camera < SPONZA_CAMERA_COUNT; camera++) {
        select_camera(sponza_cameras[camera]);
        const uint32_t period = scene_animator_clip_duration_ms(sponza, camera_entity, 0);
        const vec3f_t forward_axis = {0.0F, 0.0F, 1.0F};
        (void)scene_animator_seek(sponza, camera_entity, SCENE_ANIMATOR_CURRENT, 0);
        vec3f_t previous = scene_entity_local(sponza, camera_entity).position;
        for (uint32_t t = 10; t <= period + 100; t += 10) {
            (void)scene_animator_seek(sponza, camera_entity, SCENE_ANIMATOR_CURRENT, t);
            const transformf_t pose = scene_entity_local(sponza, camera_entity);
            const vec3f_t eye = pose.position;
            const vec3f_t forward = quatf_rotate(pose.rotation, forward_axis);
            const vec3f_t step = vec3f_sub(eye, previous);
            TEST_ASSERT_TRUE_MESSAGE(vec3f_dot(step, step) < 2.0F * 2.0F, camera_name);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001F, 1.0F, sqrtf(vec3f_dot(forward, forward)), camera_name);
            previous = eye;
        }
    }
    select_camera(sponza_cameras[0]);
}

/* The fraction of the picture covered at `t_ms` into the flythrough. */
static float
share_covered_at(const raster_t* raster, uint32_t t_ms) {
    (void)scene_animator_seek(sponza, camera_entity, SCENE_ANIMATOR_CURRENT, t_ms);
    const render_view_t view =
        sponza_suite_view(flythrough, sponza, camera_entity, (viewport_t){raster->width, raster->height, 0});
    raster_draw(raster, &view);
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
    const uint32_t period = scene_animator_clip_duration_ms(sponza, camera_entity, 0);
    float sum = 0.0F;
    int samples = 0;
    for (uint32_t t = 0; t < period; t += SPONZA_POSE_EVERY_MS) {
        sum += share_covered_at(&raster, t);
        samples++;
    }
    memory_free(scratch);
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(0.85F, sum / (float)samples, camera_name);
}

static void
test_the_flythrough_sees_mostly_building(void) {
    open_the_meshes();
    check_camera_paths(check_the_flythrough_sees_mostly_building);
}

/* The pack the build wrote for the scene: on the device, the one flashed
 * to the assets partition. The scene loads from it, and each bake's mesh
 * opens. */
static void
test_the_scene_loads_from_its_pack_with_a_lit_mesh_for_each_bake_and_its_path(void) {
    require_the_scene();
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        r3d_lit_mesh_t mesh;
        open_the_mesh(mesh_of((scene_baked_variant_t)i), &mesh);
        TEST_ASSERT_GREATER_THAN_INT(0, mesh.triangle_count);
    }
    TEST_ASSERT_NOT_NULL(flythrough);
    TEST_ASSERT_GREATER_THAN_UINT32(0, scene_animator_clip_duration_ms(sponza, camera_entity, 0));
}

static void
run_sponza_suite(void) {
    sponza_suite_t loaded = sponza_suite_load();
    sponza = loaded.scene;
    pack = asset_store_loaded(loaded.load);
    flythrough = loaded.lens;
    camera_entity = sponza == NULL ? SCENE_ENTITY_NONE : scene_find(sponza, name_hash(sponza_cameras[0]));
    RUN_TEST(test_the_scene_loads_from_its_pack_with_a_lit_mesh_for_each_bake_and_its_path);
    RUN_TEST(test_both_bakes_have_the_structure_the_pipeline_relies_on);
    RUN_TEST(test_both_bakes_are_cut_into_meshlets);
    RUN_TEST(test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps);
    RUN_TEST(test_the_flythrough_moves_smoothly_and_closes_its_loop);
    RUN_TEST(test_the_flythrough_keeps_clear_of_every_triangle);
    RUN_TEST(test_the_flythrough_sees_mostly_building);
    loaded = (sponza_suite_t){sponza, loaded.load, flythrough, loaded.arena_mark};
    sponza_suite_release(&loaded);
    sponza = NULL;
    pack = NULL;
    flythrough = NULL;
}

SUITE_REGISTER(run_sponza_suite);
SUITE_READS(run_sponza_suite, SPONZA_SCENE);

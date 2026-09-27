/*
 * Portable suite: the two baked Sponza meshes (sponza_mesh_generated.h,
 * sponza_lite_mesh_generated.h) and the camera loop through them
 * (sponza_flythrough.h). Each mesh is checked for the structure
 * r3d_lit_pipeline.h relies on, never against the generator; the path and
 * the pictures it sees are checked against the shipped meshes themselves.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "esp_heap_caps.h"

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_lite_mesh_generated.h"
#include "apps/render_lab/sponza_mesh_generated.h"
#include "r3d_lit_mesh_expect.h"
#include "render/r3d_lit_frame.h"

_Static_assert(SPONZA_VERTEX_COUNT <= 65535 && SPONZA_LITE_VERTEX_COUNT <= 65535,
               "triangles index vertices with uint16_t");
_Static_assert(SPONZA_LITE_CLUSTER_COUNT <= SPONZA_CLUSTER_COUNT,
               "the working arrays below are sized for the larger bake");

/* The flat reference: every cluster's eight corners against each plane. */
static bool
cluster_in_view(const r3d_lit_cluster_t* c, const r3d_lit_view_t* view) {
    int beyond[5] = {0};
    for (int i = 0; i < 8; i++) {
        const float x = (float)(i & 1 ? c->hi[0] : c->lo[0]);
        const float y = (float)(i & 2 ? c->hi[1] : c->lo[1]);
        const float z = (float)(i & 4 ? c->hi[2] : c->lo[2]);
        const float lx = (view->m[0][0] * x) + (view->m[0][1] * y) + (view->m[0][2] * z) + view->m[0][3];
        const float ly = (view->m[1][0] * x) + (view->m[1][1] * y) + (view->m[1][2] * z) + view->m[1][3];
        const float lz = (view->m[2][0] * x) + (view->m[2][1] * y) + (view->m[2][2] * z) + view->m[2][3];
        beyond[0] += lz < view->near_z;
        beyond[1] += lx < -view->center_x * lz;
        beyond[2] += lx > ((float)view->width - view->center_x) * lz;
        beyond[3] += ly < -view->center_y * lz;
        beyond[4] += ly > ((float)view->height - view->center_y) * lz;
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
    const uint32_t period = r3d_path_period_ms(&sponza_flythrough);
    static uint16_t walked[SPONZA_CLUSTER_COUNT];
    static uint8_t kept[SPONZA_CLUSTER_COUNT];
    for (uint32_t t = 0; t < period; t += 2500) {
        r3d_lit_view_t view;
        sponza_view_at(&view, t, mesh->position_scale, (int)(t / 2500) & 3);

        memset(kept, 0, sizeof kept);
        const int count = r3d_lit_cull_clusters(mesh, &view, walked);
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, kept[walked[i]], "a cluster was listed twice");
            kept[walked[i]] = 1;
        }
        int flat = 0;
        for (int c = 0; c < mesh->cluster_count; c++) {
            const bool in_view = cluster_in_view(&mesh->clusters[c], &view);
            flat += in_view;
            TEST_ASSERT_EQUAL_MESSAGE(in_view, kept[c], "the walk and the flat test disagree on a cluster");
        }
        TEST_ASSERT_EQUAL_INT(flat, count);
    }
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
    const uint32_t period = r3d_path_period_ms(&sponza_flythrough);
    for (uint32_t t = 0; t < period; t += 100) {
        r3d_vec3f_t eye;
        r3d_vec3f_t forward;
        r3d_path_sample(&sponza_flythrough, t, &eye, &forward);
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
    r3d_lit_mesh_expect_valid(&sponza_mesh);
    r3d_lit_mesh_expect_valid(&sponza_lite_mesh);
}

static void
test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(void) {
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(&sponza_mesh);
    check_the_tree_walk_keeps_exactly_what_a_flat_test_keeps(&sponza_lite_mesh);
}

static void
test_the_flythrough_keeps_clear_of_every_triangle(void) {
    check_the_flythrough_keeps_clear_of_every_triangle(&sponza_mesh);
    check_the_flythrough_keeps_clear_of_every_triangle(&sponza_lite_mesh);
}

/* The fraction of the picture covered at `t_ms` into the flythrough. */
static float
share_covered_at(const r3d_lit_frame_t* frame, uint32_t t_ms) {
    r3d_lit_view_t view;
    sponza_view_at(&view, t_ms, frame->mesh->position_scale, 0);
    r3d_lit_frame_render(frame, &view);
    const int pixels = frame->width * frame->height;
    int covered = 0;
    for (int i = 0; i < pixels; i++) {
        covered += frame->depth[i] != 0;
    }
    return (float)covered / (float)pixels;
}

/* The camera stays inside the building, so on average walls, floor and
 * galleries fill well over nine tenths of the picture (0.72 at the least,
 * looking up at the sky). Wound the wrong way round, the same bake shows
 * only the building's far sides, about 0.6. */
static void
check_the_flythrough_sees_mostly_building(const r3d_lit_mesh_t* mesh) {
    r3d_lit_frame_t frame = {.mesh = mesh, .width = SPONZA_RENDER_WIDTH, .height = SPONZA_RENDER_HEIGHT};
    void* scratch = heap_caps_malloc(r3d_lit_frame_scratch_bytes(mesh, frame.width, frame.height),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(scratch);
    r3d_lit_frame_use_scratch(&frame, scratch);
    const uint32_t period = r3d_path_period_ms(&sponza_flythrough);
    float sum = 0.0F;
    int samples = 0;
    for (uint32_t t = 0; t < period; t += 5000) {
        sum += share_covered_at(&frame, t);
        samples++;
    }
    heap_caps_free(scratch);
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(0.85F, sum / (float)samples, "the flythrough sees mostly sky");
}

static void
test_the_flythrough_sees_mostly_building(void) {
    check_the_flythrough_sees_mostly_building(&sponza_mesh);
    check_the_flythrough_sees_mostly_building(&sponza_lite_mesh);
}

static void
run_sponza_suite(void) {
    RUN_TEST(test_both_bakes_have_the_structure_the_pipeline_relies_on);
    RUN_TEST(test_the_tree_walk_keeps_exactly_what_a_flat_test_keeps);
    RUN_TEST(test_the_flythrough_keeps_clear_of_every_triangle);
    RUN_TEST(test_the_flythrough_sees_mostly_building);
}

SUITE_REGISTER(run_sponza_suite);

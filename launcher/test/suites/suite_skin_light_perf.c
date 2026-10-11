/* Device lighting measurements over the demo pack at a fixed sampled pose. */
#include <string.h>
#include "anim/anim_skeleton.h"
#include "asset/asset_store.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_skin.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"

#define PACK_ID "capybara"

static const char*
entry_name(const asset_pack_t* pack, uint32_t type) {
    const char* name = NULL;
    for (uint32_t i = 0; i < pack->count; i++) {
        asset_entry_t entry;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_entry(pack, i, &entry));
        if (entry.type == type) {
            TEST_ASSERT_NULL_MESSAGE(name, "skin_light_perf: ambiguous model entry");
            name = entry.name;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(name, "skin_light_perf: model entry missing; flash demo pack");
    return name;
}

static void
open_model(const asset_pack_t* pack, anim_skeleton_t* skeleton, r3d_skin_t* skin, r3d_lit_mesh_t* bind) {
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_skeleton_open(pack, entry_name(pack, ANIM_SKELETON_ASSET), skeleton));
    const char* skin_id = entry_name(pack, R3D_SKIN_ASSET);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_skin_open(pack, skin_id, skin));
    const size_t skin_length = strlen(skin_id);
    const size_t suffix_length = sizeof R3D_SKIN_ID_SUFFIX - 1U;
    TEST_ASSERT_TRUE_MESSAGE(skin_length > suffix_length, "skin_light_perf: invalid skin id");
    const size_t mesh_length = skin_length - suffix_length;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(R3D_SKIN_ID_SUFFIX, skin_id + mesh_length, "skin_light_perf: invalid skin id");
    char mesh_id[ASSET_NAME_MAX];
    memcpy(mesh_id, skin_id, mesh_length);
    mesh_id[mesh_length] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, r3d_lit_mesh_open(pack, mesh_id, bind),
                                  "skin_light_perf: model mesh missing; flash demo assets");
}

#ifdef DEVICE_BUILD
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include "anim/pose/anim_pose.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "perf_stats.h"
#include "render/skin/r3d_skin_light_internal.h"

#define CLIP_ID      "gallop"
#define POSE_SECONDS 0.2F
#define SAMPLES      100
#define WARMUP       8
static const char* TAG = "skin_light_perf";
static const vec3f_t DIRECTIONS[] = {
    {0.45F, 0.80F, 0.40F}, {-0.70F, 0.35F, 0.30F}, {0.10F, 0.40F, -0.90F}, {0, -1, 0.15F}};
static const vec3f_t COLORS[] = {
    {0.80F, 0.72F, 0.60F}, {0.25F, 0.32F, 0.45F}, {0.35F, 0.35F, 0.30F}, {0.20F, 0.16F, 0.10F}};

typedef struct {
    const asset_pack_t* pack;
    anim_skeleton_t skeleton;
    r3d_skin_t skin;
    r3d_lit_mesh_t bind;
    anim_tracks_t clip;
    transformf_t* local;
    anim_joint_bound_t* curves;
    mat4f_t* model;
    mat4f_t* palette;
    mat3f_t* normals;
    void* positions;
    r3d_skin_output_t posed;
    uint8_t (*light)[3];
    r3d_skin_light_transfer_t* transfer;
    int32_t* samples;
    int32_t* micros;
    r3d_skin_lights_t lights;
} fixture_t;

static fixture_t* active;

static void
release(void) {
    if (active == NULL) {
        return;
    }
    memory_free(active->local);
    memory_free(active->curves);
    memory_free(active->model);
    memory_free(active->palette);
    memory_free(active->normals);
    memory_free(active->positions);
    memory_free(active->light);
    memory_free(active->transfer);
    memory_free(active->samples);
    memory_free(active->micros);
    if (active->pack != NULL) {
        asset_store_release(PACK_ID);
    }
    memory_free(active);
    active = NULL;
}

static void
heap_report(const char* stage) {
    ESP_LOGI(TAG, "HEAP %s internal_free=%u largest=%u", stage, (unsigned)memory_free_bytes(MEMORY_INTERNAL),
             (unsigned)memory_largest_block(MEMORY_INTERNAL));
}

static void*
fixture_block(size_t bytes) {
    void* block = memory_alloc(bytes, MEMORY_INTERNAL);
    TEST_ASSERT_NOT_NULL_MESSAGE(block, "skin_light_perf: insufficient internal RAM");
    memset(block, 0, bytes);
    return block;
}

static fixture_t*
fixture(int lights) {
    heap_report("before");
    suite_set_test_cleanup(release);
    active = fixture_block(sizeof *active);
    fixture_t* f = active;
    f->pack = asset_store_pack(PACK_ID);
    TEST_ASSERT_NOT_NULL_MESSAGE(f->pack, "skin_light_perf: capybara pack missing; flash demo assets");
    open_model(f->pack, &f->skeleton, &f->skin, &f->bind);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, anim_tracks_from_pack(f->pack, CLIP_ID, &f->clip),
                                  "skin_light_perf: gallop clip missing");
    TEST_ASSERT_EQUAL_INT(f->skeleton.joint_count, f->skin.joint_count);
    TEST_ASSERT_EQUAL_INT(f->bind.vertex_count, f->skin.vertex_count);
    const int joints = f->skeleton.joint_count;
    f->local = fixture_block((size_t)joints * sizeof *f->local);
    f->curves = fixture_block((size_t)f->clip.count * sizeof *f->curves);
    f->model = fixture_block((size_t)joints * sizeof *f->model);
    f->palette = fixture_block((size_t)joints * sizeof *f->palette);
    f->normals = fixture_block((size_t)joints * sizeof *f->normals);
    f->positions = fixture_block(r3d_skin_output_bytes(&f->bind));
    f->light = fixture_block((size_t)f->bind.vertex_count * sizeof *f->light);
    f->transfer = fixture_block(sizeof *f->transfer);
    f->samples = fixture_block(SAMPLES * sizeof *f->samples);
    f->micros = fixture_block(SAMPLES * sizeof *f->micros);
    TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, f->positions, r3d_skin_output_bytes(&f->bind), &f->posed));
    anim_pose_t pose = {.local = f->local, .joint_count = joints};
    anim_pose_rest(&f->skeleton, &pose);
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind_skeleton(&f->clip, &f->skeleton, f->curves, f->clip.count, NULL));
    anim_sample_pose(f->curves, f->clip.count, POSE_SECONDS, &pose);
    anim_pose_model(&f->skeleton, &pose, f->model);
    r3d_skin_palette(&f->skin, f->model, f->bind.position_scale, f->palette);
    r3d_skin_normal_palette(f->palette, joints, mat4f_identity(), f->normals);
    ESP_LOGI(TAG,
             "RAM renderer_colors=%u renderer_normals=%u renderer_light_copy=%u scene_transfer_internal=%u "
             "scene_world_lights_psram=%u",
             (unsigned)((size_t)f->bind.vertex_count * sizeof *f->light),
             (unsigned)((size_t)joints * sizeof *f->normals), 0U, (unsigned)sizeof *f->transfer,
             (unsigned)sizeof f->lights);
    f->lights.count = lights;
    f->lights.ambient = (vec3f_t){0.22F, 0.22F, 0.25F};
    for (int i = 0; i < lights; i++) {
        f->lights.direction[i] = vec3f_normalize(DIRECTIONS[i]);
        f->lights.color[i] = COLORS[i];
    }
    r3d_skin_light_transfer_build(0.0F, f->transfer);
    f->lights.transfer = f->transfer;
    heap_report("allocated");
    ESP_LOGI(TAG, "POSE clip=%s seconds=%.3f vertices=%d influences=%d lights=%d", CLIP_ID, (double)POSE_SECONDS,
             f->bind.vertex_count, f->skin.influences, lights);
    return f;
}

/* Exact normalization reference blends the packed bind normal with the world-space normal palette. */
static vec3f_t
reference_normal(const fixture_t* f, int vertex) {
    const uint8_t* r = f->skin.vertices + (size_t)vertex * f->skin.vertex_stride;
    const int influences = f->skin.influences;
    const int8_t* bind = (const int8_t*)(r + 2 * influences);
    float normal[R3D_SKIN_NORMAL_WIDTH] = {0};
    for (int i = 0; i < influences; i++) {
        const mat3f_t* m = &f->normals[r[i]];
        const float weight = (float)r[influences + i] / R3D_SKIN_WEIGHT_SUM;
        for (int axis = 0; axis < R3D_SKIN_NORMAL_WIDTH; axis++) {
            normal[axis] += weight * (m->m[axis][0] * bind[0] + m->m[axis][1] * bind[1] + m->m[axis][2] * bind[2]);
        }
    }
    return vec3f_normalize((vec3f_t){normal[0], normal[1], normal[2]});
}

static void
reference_light(const fixture_t* f, int vertex, uint8_t out[3]) {
    const vec3f_t n = reference_normal(f, vertex);
    light_evaluate(n, &f->lights, f->bind.colors[vertex], out);
}

typedef enum { SKIN, FUSED_NL, FUSED_RSQRT, FUSED, EXACT_SEPARATE, NORMAL_PALETTE } kernel_t;

static kernel_t stage;

/* Reuse the shipped vertex transform and bounds loop; diagnostic stages add no switches to that kernel. */
static void
stage_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, const uint8_t albedo[3], uint8_t out[3]) {
    (void)albedo;
    const vec3f_t sum = light_irradiance(stage == FUSED_RSQRT ? light_renormalise(normal) : normal, lights);
    const float channels[] = {sum.x, sum.y, sum.z};
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        out[k] = (uint8_t)mathf_clamp(channels[k] * R3D_SKIN_LIGHT_BYTE_SCALE + 0.5F, 0.0F, R3D_SKIN_LIGHT_BYTE_SCALE);
    }
}

#define light_evaluate            stage_evaluate
#define light_renormalise(normal) (normal)
#define r3d_skin_output_bytes     stage_output_bytes
#define r3d_skin_output_init      stage_output_init
#define r3d_skin_normal_palette   stage_normal_palette
#define r3d_skin_palette          stage_palette
#define r3d_skin_clusters         stage_clusters
#define r3d_skin_clusters_lit     stage_clusters_lit
#define r3d_skin_refit_nodes      stage_refit_nodes
#include "render/skin/r3d_skinning.c"
#undef light_evaluate
#undef light_renormalise
#undef r3d_skin_output_bytes
#undef r3d_skin_output_init
#undef r3d_skin_normal_palette
#undef r3d_skin_palette
#undef r3d_skin_clusters
#undef r3d_skin_clusters_lit
#undef r3d_skin_refit_nodes
#undef LIT_RANGE

static void
kernel(fixture_t* f, kernel_t kind) {
    if (kind == NORMAL_PALETTE) {
        r3d_skin_normal_palette(f->palette, f->skin.joint_count, mat4f_identity(), f->normals);
        return;
    }
    if (kind == FUSED_NL || kind == FUSED_RSQRT) {
        stage = kind;
        stage_clusters_lit(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, &f->posed, f->normals, &f->lights,
                           f->light);
        return;
    }
    if (kind == FUSED) {
        r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, &f->posed, f->normals,
                              &f->lights, f->light);
        return;
    }
    r3d_skin_clusters(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, &f->posed);
    if (kind == EXACT_SEPARATE) {
        for (int v = 0; v < f->bind.vertex_count; v++) {
            reference_light(f, v, f->light[v]);
        }
    }
}

static void
quality(fixture_t* f, kernel_t kind) {
    if (kind == NORMAL_PALETTE) {
        return;
    }
    const size_t positions = (size_t)f->bind.vertex_count * sizeof *f->posed.positions;
    const uint32_t position_crc = asset_crc32(f->posed.positions, positions);
    if (kind == SKIN || kind == FUSED_NL || kind == FUSED_RSQRT) {
        ESP_LOGI(TAG, "CHECK positions=%08" PRIx32, position_crc);
        return;
    }
    const uint32_t checksum = asset_crc32(f->light, (size_t)f->bind.vertex_count * sizeof *f->light);
    kernel(f, SKIN);
    TEST_ASSERT_EQUAL_HEX32(position_crc, asset_crc32(f->posed.positions, positions));
    uint64_t error_sum = 0;
    int error_max = 0;
    for (int v = 0; v < f->bind.vertex_count; v++) {
        uint8_t reference[3];
        reference_light(f, v, reference);
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const int error = abs((int)f->light[v][k] - reference[k]);
            error_sum += error;
            error_max = error > error_max ? error : error_max;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, error_max);
    ESP_LOGI(TAG, "CHECK fused_positions=%08" PRIx32, position_crc);
    ESP_LOGI(TAG, "QUALITY max=%d mean=%.6f checksum=%08" PRIx32, error_max,
             (double)error_sum / (f->bind.vertex_count * R3D_SKIN_LIGHT_CHANNELS), checksum);
}

static void
measure(kernel_t kind, int lights, const char* label) {
    fixture_t* f = fixture(lights);
    for (int i = 0; i < WARMUP; i++) {
        kernel(f, kind);
    }
    for (int i = 0; i < SAMPLES; i++) {
        const int64_t start_us = timing_now_us();
        const uint32_t start = esp_cpu_get_cycle_count();
        kernel(f, kind);
        f->samples[i] = (int32_t)(esp_cpu_get_cycle_count() - start);
        f->micros[i] = (int32_t)(timing_now_us() - start_us);
        timing_yield();
    }
    const perf_stats_t stats = perf_stats_compute(f->samples, SAMPLES);
    const perf_stats_t micros = perf_stats_compute(f->micros, SAMPLES);
    ESP_LOGI(TAG, "%s min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus", label, (long long)micros.min,
             (long long)micros.max, (long long)micros.avg, (long long)micros.med, (long long)micros.p95);
    ESP_LOGI(TAG, "%s min=%lldcycles max=%lldcycles avg=%lldcycles med=%lldcycles p95=%lldcycles", label,
             (long long)stats.min, (long long)stats.max, (long long)stats.avg, (long long)stats.med,
             (long long)stats.p95);
    ESP_LOGI(TAG, "COST cycles_per_%s=%.3f", kind == NORMAL_PALETTE ? "build" : "vertex",
             (double)stats.med / (kind == NORMAL_PALETTE ? 1 : f->bind.vertex_count));
    quality(f, kind);
    release();
    suite_clear_test_cleanup();
    heap_report("released");
}

#define PERF_TEST(name, kind, lights)                                                                                  \
    static void name(void) { measure(kind, lights, #name); }
#define LIGHT_COUNT_TESTS(prefix, kind)                                                                                \
    PERF_TEST(test_##prefix##_1, kind, 1)                                                                              \
    PERF_TEST(test_##prefix##_2, kind, 2)                                                                              \
    PERF_TEST(test_##prefix##_4, kind, 4)
PERF_TEST(test_normal_palette_build, NORMAL_PALETTE, 1)
PERF_TEST(test_skin_only, SKIN, 1)
LIGHT_COUNT_TESTS(fused_nl, FUSED_NL)
LIGHT_COUNT_TESTS(fused_rsqrt, FUSED_RSQRT)
LIGHT_COUNT_TESTS(fused_renormalised, FUSED)
LIGHT_COUNT_TESTS(exact_separate, EXACT_SEPARATE)
#undef LIGHT_COUNT_TESTS
#undef PERF_TEST

void
suite_skin_light_perf(void) {
    RUN_TEST(test_normal_palette_build);
    RUN_TEST(test_skin_only);
    RUN_TEST(test_fused_nl_1);
    RUN_TEST(test_fused_nl_2);
    RUN_TEST(test_fused_nl_4);
    RUN_TEST(test_fused_rsqrt_1);
    RUN_TEST(test_fused_rsqrt_2);
    RUN_TEST(test_fused_rsqrt_4);
    RUN_TEST(test_fused_renormalised_1);
    RUN_TEST(test_fused_renormalised_2);
    RUN_TEST(test_fused_renormalised_4);
    RUN_TEST(test_exact_separate_1);
    RUN_TEST(test_exact_separate_2);
    RUN_TEST(test_exact_separate_4);
}
#else
static void
release_pack(void) {
    asset_store_release(PACK_ID);
}

static void
test_demo_skin_mesh_lookup(void) {
    const asset_pack_t* pack = asset_store_pack(PACK_ID);
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "skin_light_perf: capybara pack missing; flash demo assets");
    suite_set_test_cleanup(release_pack);
    anim_skeleton_t skeleton;
    r3d_skin_t skin;
    r3d_lit_mesh_t bind;
    open_model(pack, &skeleton, &skin, &bind);
    TEST_ASSERT_EQUAL_INT(skeleton.joint_count, skin.joint_count);
    TEST_ASSERT_EQUAL_INT(bind.vertex_count, skin.vertex_count);
    release_pack();
    suite_clear_test_cleanup();
}

void
suite_skin_light_perf(void) {
    RUN_TEST(test_demo_skin_mesh_lookup);
}
#endif
SUITE_REGISTER(suite_skin_light_perf);
SUITE_READS(suite_skin_light_perf, PACK_ID);

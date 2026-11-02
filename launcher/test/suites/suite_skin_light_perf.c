/* Device lighting measurements over the demo pack at a fixed sampled pose. */
#include <string.h>
#include "anim/anim_skeleton.h"
#include "asset/asset_store.h"
#include "hash/crc32.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_skin.h"
#include "skin_light_reference.h"
#include "suites.h"
#include "testkit/cleanup.h"
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
#include "anim/anim_skeleton_format.h"
#include "anim/pose/anim_pose.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "profile/frame_cost.h"
#include "render/r3d_skin_format.h"
#include "render/skin/r3d_skin_light_internal.h"
#include "testkit/bench.h"

#define CLIP_ID           "gallop"
#define POSE_SECONDS      0.2F
#define SAMPLES           100
#define WARMUP            8
#define XTPERF_ITERATIONS 100
static const char* TAG = "skin_light_perf";
static const char TEST_PREFIX[] = "test_";
static const vec3f_t DIRECTIONS[] = {
    {0.45F, 0.80F, 0.40F}, {-0.70F, 0.35F, 0.30F}, {0.10F, 0.40F, -0.90F}, {0, -1, 0.15F}};
static const vec3f_t COLORS[] = {
    {0.80F, 0.72F, 0.60F}, {0.25F, 0.32F, 0.45F}, {0.35F, 0.35F, 0.30F}, {0.20F, 0.16F, 0.10F}};

typedef struct {
    const asset_pack_t* pack;
    handle_t load;
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
    r3d_skin_light_table_t* table;
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
    void* blocks[] = {active->local,   active->curves,    active->model, active->palette,
                      active->normals, active->positions, active->light, active->transfer,
                      active->table,   active->samples,   active->micros};
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        memory_free(blocks[i]);
    }
    asset_store_release(active->load);
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
    f->load = asset_store_load(PACK_ID);
    f->pack = asset_store_loaded(f->load);
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

/* GCC's tree-sink moves normalisation past the ambient loads and spills across the
 * reciprocal call; the exact_separate rows time this function. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("no-tree-sink")
#endif
/* Packed bind normals keep quality checks independent of source-normal quantisation. */
static void
reference_light(const fixture_t* f, int vertex, uint8_t out[3]) {
    skin_light_reference(skin_light_reference_normal(&f->skin, f->normals, vertex, true), &f->lights,
                         f->bind.colors[vertex], 0, out);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

typedef enum { SKIN, DIRECT, RENORMALISED, TABLE, EXACT_SEPARATE, NORMAL_PALETTE, TABLE_BUILD } kernel_t;

static void
kernel(fixture_t* f, kernel_t kind) {
    if (kind == NORMAL_PALETTE) {
        r3d_skin_normal_palette(f->palette, f->skin.joint_count, mat4f_identity(), f->normals);
        return;
    }
    if (kind == TABLE_BUILD) {
        f->lights.table_scale = r3d_skin_light_table_build(&f->lights, f->table);
        return;
    }
    if (kind == DIRECT || kind == RENORMALISED || kind == TABLE) {
        const r3d_skin_light_kernel_t function = kind == DIRECT         ? r3d_skin_clusters_lit
                                                 : kind == RENORMALISED ? r3d_skin_clusters_renormalised
                                                                        : r3d_skin_clusters_table;
        function(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, &f->posed, f->normals, &f->lights, f->light);
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
assert_direct_quality(const fixture_t* f) {
    int direct_max = 0;
    for (int v = 0; v < f->bind.vertex_count; v++) {
        uint8_t reference[R3D_SKIN_LIGHT_CHANNELS];
        skin_light_reference(skin_light_reference_normal(&f->skin, f->normals, v, false), &f->lights, f->bind.colors[v],
                             0, reference);
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const int error = abs((int)f->light[v][k] - reference[k]);
            direct_max = error > direct_max ? error : direct_max;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND, direct_max);
}

static void
quality(fixture_t* f, kernel_t kind) {
    if (kind == NORMAL_PALETTE || kind == TABLE_BUILD) {
        return;
    }
    const size_t positions = (size_t)f->bind.vertex_count * sizeof *f->posed.positions;
    const uint32_t position_crc = crc32_update(0, f->posed.positions, positions);
    if (kind == SKIN) {
        ESP_LOGI(TAG, "CHECK positions=%08" PRIx32, position_crc);
        return;
    }
    const uint32_t checksum = crc32_update(0, f->light, (size_t)f->bind.vertex_count * sizeof *f->light);
    kernel(f, SKIN);
    TEST_ASSERT_EQUAL_HEX32(position_crc, crc32_update(0, f->posed.positions, positions));
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
    if (kind == DIRECT) {
        assert_direct_quality(f);
    } else {
        TEST_ASSERT_LESS_OR_EQUAL_INT(
            kind == TABLE ? R3D_SKIN_LIGHT_TABLE_ERROR_BOUND : R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND, error_max);
    }
    ESP_LOGI(TAG, "CHECK fused_positions=%08" PRIx32, position_crc);
    ESP_LOGI(TAG, "QUALITY max=%d mean=%.6f checksum=%08" PRIx32, error_max,
             (double)error_sum / (f->bind.vertex_count * R3D_SKIN_LIGHT_CHANNELS), checksum);
}

static fixture_t*
fixture_for_kernel(kernel_t kind, int lights) {
    fixture_t* f = fixture(lights);
    const bool table_mode = kind == TABLE || kind == TABLE_BUILD;
    if (table_mode) {
        f->table = fixture_block(sizeof *f->table);
        f->lights.table = f->table;
        f->lights.table_scale = r3d_skin_light_table_build(&f->lights, f->table);
    }
    return f;
}

static void
measure(kernel_t kind, int lights, const char* label) {
    fixture_t* f = fixture_for_kernel(kind, lights);
    const bool table_mode = kind == TABLE || kind == TABLE_BUILD;
    const char* mode = table_mode ? "table" : kind == DIRECT ? "direct" : "renormalised";
    ESP_LOGI(
        TAG,
        "RAM mode=%s renderer_bytes=%u scene_bytes=%u renderer_colors=%u renderer_normals=%u scene_table=%u "
        "scene_metadata_psram=%u",
        mode,
        (unsigned)((size_t)f->bind.vertex_count * sizeof *f->light + (size_t)f->skin.joint_count * sizeof *f->normals),
        (unsigned)(sizeof *f->transfer + (table_mode ? sizeof *f->table : 0)),
        (unsigned)((size_t)f->bind.vertex_count * sizeof *f->light),
        (unsigned)((size_t)f->skin.joint_count * sizeof *f->normals), (unsigned)(table_mode ? sizeof *f->table : 0),
        (unsigned)sizeof f->lights);
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
    const bench_stats_t stats = bench_stats_compute(f->samples, SAMPLES);
    const bench_stats_t micros = bench_stats_compute(f->micros, SAMPLES);
    ESP_LOGI(TAG, "%s one core: mean %lldus", label + sizeof TEST_PREFIX - 1, (long long)micros.avg);
    ESP_LOGI(TAG, "%s min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus", label, (long long)micros.min,
             (long long)micros.max, (long long)micros.avg, (long long)micros.med, (long long)micros.p95);
    ESP_LOGI(TAG, "%s min=%lldcycles max=%lldcycles avg=%lldcycles med=%lldcycles p95=%lldcycles", label,
             (long long)stats.min, (long long)stats.max, (long long)stats.avg, (long long)stats.med,
             (long long)stats.p95);
    ESP_LOGI(TAG, "COST cycles_per_%s=%.3f", kind == NORMAL_PALETTE || kind == TABLE_BUILD ? "build" : "vertex",
             (double)stats.med / (kind == NORMAL_PALETTE || kind == TABLE_BUILD ? 1 : f->bind.vertex_count));
    quality(f, kind);
    release();
    suite_clear_test_cleanup();
    heap_report("released");
}

/* Events share a warmed, fixed pose; a zero count is valid for an unused hardware path. */
static void
measure_xtperf_kernel(kernel_t kind, const char* row) {
    fixture_t* f = fixture_for_kernel(kind, 1);
    uint64_t total_cycles = 0;
    uint64_t total_insn = 0;
    for (int e = 0; e < frame_cost_event_count(); e++) {
        for (int i = 0; i < WARMUP; i++) {
            kernel(f, kind);
        }
        const frame_cost_event_t* const event = frame_cost_event_at(e);
        TEST_ASSERT_TRUE_MESSAGE(frame_cost_count_begin(e), "disarm frame_cost before measuring counters");
        for (int i = 0; i < XTPERF_ITERATIONS; i++) {
            kernel(f, kind);
        }
        uint32_t cycles = 0;
        uint32_t value = 0;
        const bool counted = frame_cost_count_end(&cycles, &value);
        ESP_LOGI("xtperf", "scene=%s event=%s cycles_per_step=%u value_per_step=%u steps=%d%s", row, event->name,
                 (unsigned)(cycles / XTPERF_ITERATIONS), (unsigned)(value / XTPERF_ITERATIONS), XTPERF_ITERATIONS,
                 counted ? "" : " overflow=counters");
        total_cycles += cycles;
        if (strcmp(event->name, FRAME_COST_DEFAULT_EVENT) == 0) {
            total_insn += value;
        }
        timing_yield();
    }
    release();
    suite_clear_test_cleanup();
    TEST_ASSERT_TRUE_MESSAGE(total_cycles > 0, "the cycle counter never moved");
    TEST_ASSERT_TRUE_MESSAGE(total_insn > 0, "the retired-instruction counter never moved");
}

static void
test_skin_light_xtensa_counters(void) {
#if CONFIG_LAUNCHER_QEMU
    TEST_IGNORE_MESSAGE("QEMU does not model the performance monitor");
#endif
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, esp_cpu_get_core_id(), "perfmon counters must be measured on core 0");
    measure_xtperf_kernel(DIRECT, "fused_direct_1");
    measure_xtperf_kernel(RENORMALISED, "fused_renormalised_1");
    measure_xtperf_kernel(TABLE, "fused_table_1");
}

#define PERF_TEST(name, kind, lights)                                                                                  \
    static void name(void) { measure(kind, lights, #name); }
#define LIGHT_COUNT_TESTS(prefix, kind)                                                                                \
    PERF_TEST(test_##prefix##_1, kind, 1)                                                                              \
    PERF_TEST(test_##prefix##_2, kind, 2)                                                                              \
    PERF_TEST(test_##prefix##_4, kind, 4)
PERF_TEST(test_normal_palette_build, NORMAL_PALETTE, 1)
PERF_TEST(test_skin_only, SKIN, 1)
LIGHT_COUNT_TESTS(fused_direct, DIRECT)
LIGHT_COUNT_TESTS(fused_renormalised, RENORMALISED)
LIGHT_COUNT_TESTS(fused_table, TABLE)
LIGHT_COUNT_TESTS(table_build, TABLE_BUILD)
LIGHT_COUNT_TESTS(exact_separate, EXACT_SEPARATE)
#undef LIGHT_COUNT_TESTS
#undef PERF_TEST

void
suite_skin_light_perf(void) {
    RUN_TEST(test_skin_light_xtensa_counters);
    RUN_TEST(test_normal_palette_build);
    RUN_TEST(test_skin_only);
    RUN_TEST(test_fused_direct_1);
    RUN_TEST(test_fused_direct_2);
    RUN_TEST(test_fused_direct_4);
    RUN_TEST(test_fused_table_1);
    RUN_TEST(test_fused_table_2);
    RUN_TEST(test_fused_table_4);
    RUN_TEST(test_table_build_1);
    RUN_TEST(test_table_build_2);
    RUN_TEST(test_table_build_4);
    RUN_TEST(test_fused_renormalised_1);
    RUN_TEST(test_fused_renormalised_2);
    RUN_TEST(test_fused_renormalised_4);
    RUN_TEST(test_exact_separate_1);
    RUN_TEST(test_exact_separate_2);
    RUN_TEST(test_exact_separate_4);
}
#else
static handle_t load;

static void
release_pack(void) {
    asset_store_release(load);
}

static void
test_demo_skin_mesh_lookup(void) {
    load = asset_store_load(PACK_ID);
    const asset_pack_t* pack = asset_store_loaded(load);
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

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
#include "render/skin/r3d_skin_light.h"

#define CLIP_ID      "gallop"
#define POSE_SECONDS 0.2F
#define SAMPLES      100
#define WARMUP       8
#define TABLE_CELLS  (R3D_SKIN_LIGHT_TABLE_SIZE * R3D_SKIN_LIGHT_TABLE_SIZE)
static const char* TAG = "skin_light_perf";
static const vec3f_t DIRECTIONS[] = {
    {0.45F, 0.80F, 0.40F}, {-0.70F, 0.35F, 0.30F}, {0.10F, 0.40F, -0.90F}, {0, -1, 0.15F}};
static const vec3f_t COLOURS[] = {
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
    void* positions;
    r3d_skin_output_t posed;
    uint8_t (*light)[3];
    r3d_skin_light_table_t* table;
    vec3f_t* directions;
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
    memory_free(active->positions);
    memory_free(active->light);
    memory_free(active->table);
    memory_free(active->directions);
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
    f->positions = fixture_block(r3d_skin_output_bytes(&f->bind));
    f->light = fixture_block((size_t)f->bind.vertex_count * sizeof *f->light);
    f->table = fixture_block(sizeof *f->table);
    f->directions = fixture_block(TABLE_CELLS * sizeof *f->directions);
    f->samples = fixture_block(SAMPLES * sizeof *f->samples);
    f->micros = fixture_block(SAMPLES * sizeof *f->micros);
    TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, f->positions, r3d_skin_output_bytes(&f->bind), &f->posed));
    anim_pose_t pose = {.local = f->local, .joint_count = joints};
    anim_pose_rest(&f->skeleton, &pose);
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind_skeleton(&f->clip, &f->skeleton, f->curves, f->clip.count, NULL));
    anim_sample_pose(f->curves, f->clip.count, POSE_SECONDS, &pose);
    anim_pose_model(&f->skeleton, &pose, f->model);
    r3d_skin_palette(&f->skin, f->model, f->bind.position_scale, f->palette);
    f->lights.count = lights;
    f->lights.ambient = (vec3f_t){0.22F, 0.22F, 0.25F};
    for (int i = 0; i < lights; i++) {
        f->lights.direction[i] = vec3f_normalize(DIRECTIONS[i]);
        f->lights.colour[i] = COLOURS[i];
    }
    r3d_skin_light_directions(R3D_SKIN_LIGHT_TABLE_SIZE, f->directions);
    r3d_skin_light_build(f->directions, &f->lights, f->table);
    heap_report("allocated");
    ESP_LOGI(TAG, "POSE clip=%s seconds=%.3f vertices=%d influences=%d lights=%d", CLIP_ID, (double)POSE_SECONDS,
             f->bind.vertex_count, f->skin.influences, lights);
    return f;
}

/* Independent float reference uses the packed bind normal, normalized after weighted rotation. */
static vec3f_t
reference_normal(const fixture_t* f, int vertex) {
    const uint8_t* r = f->skin.vertices + (size_t)vertex * f->skin.vertex_stride;
    const int influences = f->skin.influences;
    const int8_t* bind = (const int8_t*)(r + 2 * influences);
    float normal[R3D_SKIN_NORMAL_WIDTH] = {0};
    for (int i = 0; i < influences; i++) {
        const mat4f_t* m = &f->palette[r[i]];
        const float weight = (float)r[influences + i] / R3D_SKIN_WEIGHT_SUM;
        for (int axis = 0; axis < R3D_SKIN_NORMAL_WIDTH; axis++) {
            normal[axis] += weight * (m->m[axis][0] * bind[0] + m->m[axis][1] * bind[1] + m->m[axis][2] * bind[2]);
        }
    }
    return vec3f_normalize((vec3f_t){normal[0], normal[1], normal[2]});
}

static void
reference_light(vec3f_t n, const r3d_skin_lights_t* lights, uint8_t out[3]) {
    float sum[] = {lights->ambient.x, lights->ambient.y, lights->ambient.z};
    for (int i = 0; i < lights->count; i++) {
        const vec3f_t d = lights->direction[i];
        const float facing = fmaxf(0, n.x * d.x + n.y * d.y + n.z * d.z);
        sum[0] += lights->colour[i].x * facing;
        sum[1] += lights->colour[i].y * facing;
        sum[2] += lights->colour[i].z * facing;
    }
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        out[k] = (uint8_t)fminf(R3D_SKIN_LIGHT_BYTE_SCALE, fmaxf(0, sum[k] * R3D_SKIN_LIGHT_BYTE_SCALE + 0.5F));
    }
}

typedef enum { SKIN, DIRECT, RENORMALISED, LOOKUP, BUILD } kernel_t;

static void
kernel(fixture_t* f, kernel_t kind) {
    switch (kind) {
        case SKIN: r3d_skin_clusters(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, &f->posed); break;
        case DIRECT:
        case RENORMALISED:
            r3d_skin_light_direct(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, &f->lights,
                                  kind == RENORMALISED, f->light);
            break;
        case LOOKUP:
            r3d_skin_light_table(&f->skin, &f->bind, f->palette, 0, f->bind.cluster_count, f->table, f->light);
            break;
        case BUILD: r3d_skin_light_build(f->directions, &f->lights, f->table); break;
    }
}

static void
quality(const fixture_t* f, kernel_t kind) {
    if (kind == SKIN) {
        ESP_LOGI(TAG, "CHECK positions=%08" PRIx32,
                 asset_crc32(f->posed.positions, (size_t)f->bind.vertex_count * sizeof *f->posed.positions));
        return;
    }
    const int count = kind == BUILD ? TABLE_CELLS : f->bind.vertex_count;
    uint64_t error_sum = 0;
    int error_max = 0;
    for (int v = 0; v < count; v++) {
        uint8_t reference[3];
        reference_light(kind == BUILD ? f->directions[v] : reference_normal(f, v), &f->lights, reference);
        const uint8_t* got = kind == BUILD ? f->table->rgb[v] : f->light[v];
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const int error = abs((int)got[k] - reference[k]);
            error_sum += error;
            error_max = error > error_max ? error : error_max;
        }
    }
    uint32_t checksum;
    if (kind == BUILD) {
        /* Padding is defined zero, so the table checksum is reproducible. */
        checksum = asset_crc32(f->table->rgb, sizeof f->table->rgb);
    } else {
        checksum = asset_crc32(f->light, (size_t)count * sizeof *f->light);
    }
    ESP_LOGI(TAG, "QUALITY max=%d mean=%.6f checksum=%08" PRIx32, error_max,
             (double)error_sum / (count * R3D_SKIN_LIGHT_CHANNELS), checksum);
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
    ESP_LOGI(TAG, "COST cycles_per_%s=%.3f", kind == BUILD ? "build" : "vertex",
             (double)stats.med / (kind == BUILD ? 1 : f->bind.vertex_count));
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
PERF_TEST(test_skin_only, SKIN, 1)
LIGHT_COUNT_TESTS(direct, DIRECT)
LIGHT_COUNT_TESTS(renormalised, RENORMALISED)
PERF_TEST(test_table_lookup, LOOKUP, 4)
LIGHT_COUNT_TESTS(table_build, BUILD)
#undef LIGHT_COUNT_TESTS
#undef PERF_TEST

void
suite_skin_light_perf(void) {
    RUN_TEST(test_skin_only);
    RUN_TEST(test_direct_1);
    RUN_TEST(test_direct_2);
    RUN_TEST(test_direct_4);
    RUN_TEST(test_renormalised_1);
    RUN_TEST(test_renormalised_2);
    RUN_TEST(test_renormalised_4);
    RUN_TEST(test_table_lookup);
    RUN_TEST(test_table_build_1);
    RUN_TEST(test_table_build_2);
    RUN_TEST(test_table_build_4);
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

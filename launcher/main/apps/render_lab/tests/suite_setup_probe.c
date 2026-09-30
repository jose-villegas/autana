/*
 * Measurement probe, never ships: the lit-mesh draw's setup split into
 * sub-stages, per core, at the flythrough's poses, with the mesh and the
 * transformed vertices optionally copied into internal SRAM, and the Xtensa
 * performance counters read around each core's draw.
 */
#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_lite_mesh_generated.h"
#include "apps/render_lab/sponza_mesh_generated.h"
#include "render/r3d_lit_frame.h"
#include "util/job.h"

static const char* TAG = "setup_probe";

#define PARTS         4
#define SPLIT_BUCKETS 64

enum { PLACE_CS = 1, PLACE_TRI = 2, PLACE_COL = 4 };

typedef struct {
    const char* name;
    uint16_t select, mask;
} event_t;

static const event_t EVENTS[] = {
    {"insn", XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL},
    {"dstall", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL},
    {"dbusy", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_BUSY},
    {"dmiss", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_CACHE_MISS},
};
#define EVENT_COUNT ((int)(sizeof(EVENTS) / sizeof(EVENTS[0])))

typedef struct {
    const char* name;
    int lit_stop, span_stop;
} stop_t;

#if R3D_LIT_PROBE
static const stop_t STOPS[] = {
    {"walk", R3D_LIT_PROBE_WALK, 0},
    {"fetch", R3D_LIT_PROBE_FETCH, 0},
    {"reject", R3D_LIT_PROBE_REJECT, 0},
    {"color", R3D_LIT_PROBE_COLOR, 0},
    {"setup", 0, 1},
    {"rows", 0, 2},
    {"spanset", 0, 3},
    {"whole", 0, 0},
};
#else
static const stop_t STOPS[] = {{"setup", 0, 1}, {"whole", 0, 0}};
#endif
#define STOP_COUNT ((int)(sizeof(STOPS) / sizeof(STOPS[0])))

typedef struct {
    r3d_lit_mesh_t mesh[PARTS];
    r3d_lit_vertex_t* cs[PARTS];
    uint16_t* visible[PARTS];
    int count[PARTS];
    int first_cluster[PARTS + 1];
    void* blocks[3 * PARTS];
} parts_t;

typedef struct {
    const r3d_lit_frame_t* frame;
    const r3d_lit_view_t* view;
    const parts_t* parts;
    const event_t* event;
    int row0, row1;
    uint32_t* cycles;
    uint32_t* value;
} core_job_t;

static void
free_parts(parts_t* p) {
    for (int i = 0; i < 3 * PARTS; i++) {
        heap_caps_free(p->blocks[i]);
        p->blocks[i] = NULL;
    }
    for (int i = 0; i < PARTS; i++) {
        heap_caps_free(p->visible[i]);
        p->visible[i] = NULL;
    }
}

static void*
internal_copy(const void* src, size_t bytes) {
    void* dst = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dst != NULL && src != NULL) {
        memcpy(dst, src, bytes);
    }
    return dst;
}

static int
vertex_first(const r3d_lit_mesh_t* m, int cluster) {
    return cluster == m->cluster_count ? m->vertex_count : m->clusters[cluster].vertex_first;
}

static int
triangle_first(const r3d_lit_mesh_t* m, int cluster) {
    return cluster == m->cluster_count ? m->triangle_count : m->clusters[cluster].triangle_first;
}

/* False when a copy does not fit; everything allocated so far is freed. */
static bool
make_parts(parts_t* p, const r3d_lit_mesh_t* m, r3d_lit_vertex_t* psram_cs, int place) {
    memset(p, 0, sizeof *p);
    int cluster = 0;
    for (int i = 0; i < PARTS; i++) {
        p->first_cluster[i] = cluster;
        const int target = (int)(((int64_t)m->vertex_count * (i + 1)) / PARTS);
        while (cluster < m->cluster_count && (i == PARTS - 1 || vertex_first(m, cluster) < target)) {
            cluster++;
        }
    }
    p->first_cluster[PARTS] = m->cluster_count;
    for (int i = 0; i < PARTS; i++) {
        const int c0 = p->first_cluster[i];
        const int c1 = p->first_cluster[i + 1];
        const int v0 = vertex_first(m, c0);
        const int v1 = vertex_first(m, c1);
        const int t0 = triangle_first(m, c0);
        const int t1 = triangle_first(m, c1);
        p->mesh[i] = *m;
        p->cs[i] = psram_cs;
        p->visible[i] = heap_caps_malloc(sizeof(uint16_t) * (size_t)m->cluster_count, MALLOC_CAP_SPIRAM);
        TEST_ASSERT_NOT_NULL(p->visible[i]);
        if (place & PLACE_CS) {
            r3d_lit_vertex_t* b = internal_copy(NULL, sizeof(r3d_lit_vertex_t) * (size_t)(v1 - v0));
            p->blocks[3 * i] = b;
            if (b == NULL) {
                goto fail;
            }
            p->cs[i] = b - v0;
        }
        if (place & PLACE_TRI) {
            uint16_t(*b)[3] = internal_copy(m->triangles + t0, sizeof(uint16_t[3]) * (size_t)(t1 - t0));
            p->blocks[(3 * i) + 1] = b;
            if (b == NULL) {
                goto fail;
            }
            p->mesh[i].triangles = (const uint16_t(*)[3])(b - t0);
        }
        if (place & PLACE_COL) {
            uint8_t(*b)[3] = internal_copy(m->colors + v0, sizeof(uint8_t[3]) * (size_t)(v1 - v0));
            p->blocks[(3 * i) + 2] = b;
            if (b == NULL) {
                goto fail;
            }
            p->mesh[i].colors = (const uint8_t(*)[3])(b - v0);
        }
    }
    return true;
fail:
    free_parts(p);
    return false;
}

static void
partition(parts_t* p, const uint16_t* visible, int n) {
    for (int i = 0; i < PARTS; i++) {
        p->count[i] = 0;
    }
    for (int k = 0; k < n; k++) {
        int i = 0;
        while (visible[k] >= p->first_cluster[i + 1]) {
            i++;
        }
        p->visible[i][p->count[i]++] = visible[k];
    }
}

/* r3d_lit_frame.c's split, copied so this probe drives the halves itself. */
static int
balanced_split_row(const r3d_lit_frame_t* frame, const uint16_t* visible, int n) {
    uint32_t weight[SPLIT_BUCKETS] = {0};
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        const r3d_lit_rows_t* r = &frame->rows[visible[i]];
        const float middle = r->unbounded ? 0.5F * (float)frame->height : 0.5F * (r->y0 + r->y1);
        int row = (int)middle;
        row = row < 0 ? 0 : (row >= frame->height ? frame->height - 1 : row);
        const uint32_t tris = frame->mesh->clusters[visible[i]].triangle_count;
        weight[row * SPLIT_BUCKETS / frame->height] += tris;
        total += tris;
    }
    uint32_t sum = 0;
    for (int bucket = 0; bucket < SPLIT_BUCKETS; bucket++) {
        sum += weight[bucket];
        if (2 * sum >= total) {
            const int row = (2 * bucket + 1) * frame->height / (2 * SPLIT_BUCKETS);
            return row < 1 ? 1 : row;
        }
    }
    return frame->height / 2;
}

static void
core_draw(void* ctx) {
    const core_job_t* j = ctx;
    const r3d_lit_frame_t* f = j->frame;
    const size_t offset = (size_t)j->row0 * (size_t)f->width;
    memset(f->depth + offset, 0, (size_t)(j->row1 - j->row0) * (size_t)f->width * sizeof(uint16_t));
    const r3d_span_target_t target = {f->color + offset, f->depth + offset, f->width, j->row0, j->row1};
    xtensa_perfmon_stop();
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 0xffff, 0, -1);
    xtensa_perfmon_init(1, j->event->select, j->event->mask, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_reset(1);
    xtensa_perfmon_start();
    for (int i = 0; i < PARTS; i++) {
        r3d_lit_draw(&j->parts->mesh[i], j->view, j->parts->visible[i], j->parts->count[i], j->parts->cs[i], f->rows,
                     &target);
    }
    xtensa_perfmon_stop();
    *j->cycles = xtensa_perfmon_value(0);
    *j->value = xtensa_perfmon_value(1);
}

/* Triangles in the clusters a core's rows do not skip. */
static int
submitted(const r3d_lit_frame_t* f, const uint16_t* visible, int n, int row0, int row1) {
    int tris = 0;
    for (int i = 0; i < n; i++) {
        const r3d_lit_rows_t* r = &f->rows[visible[i]];
        if (r->unbounded || !(r->y1 < (float)row0 || r->y0 > (float)row1)) {
            tris += f->mesh->clusters[visible[i]].triangle_count;
        }
    }
    return tris;
}

static uint32_t
median4(uint32_t v[EVENT_COUNT]) {
    uint32_t s[EVENT_COUNT];
    memcpy(s, v, sizeof s);
    for (int i = 1; i < EVENT_COUNT; i++) {
        for (int k = i; k > 0 && s[k - 1] > s[k]; k--) {
            const uint32_t t = s[k];
            s[k] = s[k - 1];
            s[k - 1] = t;
        }
    }
    return (s[(EVENT_COUNT - 1) / 2] + s[EVENT_COUNT / 2]) / 2;
}

static void
measure_pose(const char* variant, r3d_lit_frame_t* frame, parts_t* parts, uint32_t t_ms) {
    static r3d_lit_view_t view; /* statics here: the main task's stack is tight */
    sponza_view_at(&view, t_ms, frame->mesh->position_scale, 0);
    const int n = r3d_lit_cull_clusters(frame->mesh, &view, frame->visible);
    partition(parts, frame->visible, n);
    uint32_t start = esp_cpu_get_cycle_count();
    for (int i = 0; i < PARTS; i++) {
        r3d_lit_transform(&parts->mesh[i], &view, parts->visible[i], parts->count[i], parts->cs[i], frame->rows);
    }
    const uint32_t transform = esp_cpu_get_cycle_count() - start;
    const int mid = balanced_split_row(frame, frame->visible, n);
    const int sub0 = submitted(frame, frame->visible, n, mid, frame->height);
    const int sub1 = submitted(frame, frame->visible, n, 0, mid);
    ESP_LOGI(TAG, "SPT v=%s t=%u clusters=%d transform_cyc=%u mid=%d sub0=%d sub1=%d", variant, (unsigned)(t_ms / 1000),
             n, (unsigned)transform, mid, sub0, sub1);
    for (int s = 0; s < STOP_COUNT; s++) {
        r3d_lit_probe_stop = STOPS[s].lit_stop;
        r3d_span_stop_after = STOPS[s].span_stop;
        static uint32_t cyc[2][EVENT_COUNT];
        static uint32_t val[2][EVENT_COUNT];
        for (int e = 0; e < EVENT_COUNT; e++) {
            static core_job_t c1;
            static core_job_t c0;
            c1 = (core_job_t){frame, &view, parts, &EVENTS[e], 0, mid, &cyc[1][e], &val[1][e]};
            c0 = (core_job_t){frame, &view, parts, &EVENTS[e], mid, frame->height, &cyc[0][e], &val[0][e]};
            TEST_ASSERT_TRUE(job_try_core1(core_draw, &c1, sizeof c1));
            core_draw(&c0);
            TEST_ASSERT_TRUE(job_wait(2000));
        }
        ESP_LOGI(TAG,
                 "SP v=%s t=%u stop=%s c0_cyc=%u c1_cyc=%u c0_insn=%u c1_insn=%u c0_dstall=%u c1_dstall=%u "
                 "c0_dbusy=%u c1_dbusy=%u c0_dmiss=%u c1_dmiss=%u",
                 variant, (unsigned)(t_ms / 1000), STOPS[s].name, (unsigned)median4(cyc[0]), (unsigned)median4(cyc[1]),
                 (unsigned)val[0][0], (unsigned)val[1][0], (unsigned)val[0][1], (unsigned)val[1][1],
                 (unsigned)val[0][2], (unsigned)val[1][2], (unsigned)val[0][3], (unsigned)val[1][3]);
    }
    r3d_lit_probe_stop = 0;
    r3d_span_stop_after = 0;
}

static void
run_variant(const char* variant, const r3d_lit_mesh_t* mesh, int place) {
    gfx_color_t* panel =
        heap_caps_malloc(sizeof(gfx_color_t) * (size_t)GFX_WIDTH * GFX_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r3d_lit_frame_t frame = {
        .mesh = mesh, .width = SPONZA_RENDER_WIDTH, .height = SPONZA_RENDER_HEIGHT, .doubled = panel};
    void* scratch = heap_caps_malloc(r3d_lit_frame_scratch_bytes(mesh, frame.width, frame.height),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(panel);
    TEST_ASSERT_NOT_NULL(scratch);
    r3d_lit_frame_use_scratch(&frame, scratch);
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    parts_t* parts = calloc(1, sizeof(parts_t));
    TEST_ASSERT_NOT_NULL(parts);
    const bool fits = make_parts(parts, mesh, frame.cs, place);
    ESP_LOGI(TAG,
             "SPV v=%s place=%d fits=%d internal_free_before=%u largest_before=%u internal_free_after=%u "
             "largest_after=%u",
             variant, place, fits, (unsigned)free_before, (unsigned)largest_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (fits) {
        for (uint32_t t_ms = 0; t_ms < sponza_flythrough_period_ms(); t_ms += SPONZA_POSE_EVERY_MS) {
            measure_pose(variant, &frame, parts, t_ms);
        }
        free_parts(parts);
    }
    free(parts);
    heap_caps_free(scratch);
    heap_caps_free(panel);
}

static void
test_setup_split_full(void) {
    run_variant("full_base", &sponza_mesh, 0);
    TEST_PASS();
}

static void
test_setup_split_lite(void) {
    run_variant("lite_base", &sponza_lite_mesh, 0);
    run_variant("lite_cs", &sponza_lite_mesh, PLACE_CS);
    run_variant("lite_tri", &sponza_lite_mesh, PLACE_TRI);
    run_variant("lite_col", &sponza_lite_mesh, PLACE_COL);
    run_variant("lite_tricol", &sponza_lite_mesh, PLACE_TRI | PLACE_COL);
    run_variant("lite_cstri", &sponza_lite_mesh, PLACE_CS | PLACE_TRI);
    run_variant("lite_cscol", &sponza_lite_mesh, PLACE_CS | PLACE_COL);
    run_variant("lite_all", &sponza_lite_mesh, PLACE_CS | PLACE_TRI | PLACE_COL);
    run_variant("lite_base2", &sponza_lite_mesh, 0);
    TEST_PASS();
}

static void
run_setup_probe_suite(void) {
    RUN_TEST(test_setup_split_full);
    RUN_TEST(test_setup_split_lite);
}

#else /* !DEVICE_BUILD */

static void
run_setup_probe_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER_ON_REQUEST(run_setup_probe_suite);

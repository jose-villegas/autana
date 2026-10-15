/* Fixed workloads share kernels between hardware counters and the instruction ratchet. */
#ifdef DEVICE_BUILD
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "core/timing.h"
#include "esp32s3/rom/cache.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/cache_utils.h"
#include "math/linear/transformf.h"
#include "math/linear/transformx.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathf_rsqrt.h"
#include "math/scalar/trig.h"
#include "perf_stats.h"
#include "profile/frame_cost.h"
#include "services/build_id.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"

#define MATH_BENCH_BATCH            64
#define MATH_BENCH_WARM_REPS        31
#define MATH_BENCH_COLD_REPS        9
#define MATH_BENCH_ICOUNT_CALLS     4096
#define MATH_BENCH_ICOUNT_BATCHES   3
#define MATH_BENCH_DOT_SIZE         256
#define MATH_BENCH_NAME_SIZE        64
#define MATH_BENCH_INPUT_PERIOD     17
#define MATH_BENCH_INPUT_SCALE      0.125F
#define MATH_BENCH_LERP_NUMERATOR   3
#define MATH_BENCH_LERP_DENOMINATOR 8
#define MATH_BENCH_LERP_F           ((float)MATH_BENCH_LERP_NUMERATOR / MATH_BENCH_LERP_DENOMINATOR)
#define MATH_BENCH_LERP_X           (MATHX_ONE * MATH_BENCH_LERP_NUMERATOR / MATH_BENCH_LERP_DENOMINATOR)
#define MATH_BENCH_WRAP_LIMIT       1

typedef struct {
    vec3f_t af, bf;
    vec3i_t ai, bi;
    vec3s_t as, bs;
    vec3x_t ax, bx;
    quatf_t qf, rf;
    quatx_t qx, rx;
    mat4f_t mf, nf;
    mat4x_t mx, nx;
    transformf_t tf;
    transformx_t tx;
    float angle;
    uint16_t phase;
} input_t;

typedef union {
    vec3f_t vf;
    vec3i_t vi;
    vec3s_t vs;
    vec3x_t vx;
    quatf_t qf;
    quatx_t qx;
    mat4f_t mf;
    mat4x_t mx;
    float f;
    int64_t i;
} result_t;

typedef struct {
    input_t input[MATH_BENCH_BATCH];
    volatile result_t output[MATH_BENCH_BATCH];
    float dot_a[MATH_BENCH_DOT_SIZE], dot_b[MATH_BENCH_DOT_SIZE];
} fixture_t;

static fixture_t* active_fixture;

static void
release(void) {
    free(active_fixture);
    active_fixture = NULL;
}

typedef void (*kernel_t)(fixture_t*);

typedef struct {
    const char* name;
    const char* type;
    kernel_t fn;
    bool ratchet;
} row_t;

/* Volatile result stores preserve every component, including matrix columns. */
#define MATH_KERNEL(name, type, member, expression)                                                                    \
    static __attribute__((noinline)) void math_bench_##name##_##type(fixture_t* f) {                                   \
        for (int j = 0; j < MATH_BENCH_BATCH; j++) {                                                                   \
            const input_t* p = &f->input[j];                                                                           \
            f->output[j].member = (expression);                                                                        \
        }                                                                                                              \
    }

#define MATH_VECTOR_KERNELS(type, member, scalar)                                                                      \
    MATH_KERNEL(add, type, member, vec3##type##_add(p->a##type, p->b##type))                                           \
    MATH_KERNEL(scale, type, member, vec3##type##_scale(p->a##type, p->b##type.x))                                     \
    MATH_KERNEL(dot, type, scalar, vec3##type##_dot(p->a##type, p->b##type))                                           \
    MATH_KERNEL(mul, type, scalar, math##type##_mul(p->a##type.x, p->b##type.x))

MATH_VECTOR_KERNELS(f, vf, f)
MATH_VECTOR_KERNELS(i, vi, i)
MATH_VECTOR_KERNELS(s, vs, i)
MATH_VECTOR_KERNELS(x, vx, i)
MATH_KERNEL(normalize, f, vf, vec3f_normalize(p->af))
MATH_KERNEL(normalize, x, vx, vec3x_normalize(p->ax))
MATH_KERNEL(lerp, f, vf, vec3f_lerp(p->af, p->bf, MATH_BENCH_LERP_F))
MATH_KERNEL(lerp, x, vx, vec3x_lerp(p->ax, p->bx, MATH_BENCH_LERP_X))
MATH_KERNEL(lerp, i, vi, vec3i_lerp(p->ai, p->bi, MATH_BENCH_LERP_NUMERATOR, MATH_BENCH_LERP_DENOMINATOR))
MATH_KERNEL(lerp, s, vs, vec3s_lerp(p->as, p->bs, MATH_BENCH_LERP_NUMERATOR, MATH_BENCH_LERP_DENOMINATOR))
MATH_KERNEL(wrap, f, f, mathf_wrap(p->angle, -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))
MATH_KERNEL(wrap, i, i, mathi_wrap(p->ai.x, -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))
MATH_KERNEL(wrap, x, i, mathx_wrap(p->ax.x, -MATH_BENCH_WRAP_LIMIT* MATHX_ONE, MATH_BENCH_WRAP_LIMIT* MATHX_ONE))
MATH_KERNEL(convert, f_to_x, vx, vec3x_from_vec3f(p->af))
MATH_KERNEL(convert, x_to_f, vf, vec3f_from_vec3x(p->ax))
MATH_KERNEL(convert, f_to_s, vs, vec3s_from_vec3f(p->af, MATH_BENCH_INPUT_SCALE))
MATH_KERNEL(convert, s_to_f, vf, vec3f_from_vec3s(p->as, MATH_BENCH_INPUT_SCALE))
MATH_KERNEL(convert, i_to_f, vf, vec3f_from_vec3i(p->ai))
MATH_KERNEL(convert, f_to_i, vi, vec3i_from_vec3f(p->af))
MATH_KERNEL(slerp, f, qf, quatf_slerp(p->qf, p->rf, MATH_BENCH_LERP_F))
MATH_KERNEL(nlerp, f, qf, quatf_nlerp(p->qf, p->rf, MATH_BENCH_LERP_F))
MATH_KERNEL(nlerp, x, qx, quatx_nlerp(p->qx, p->rx, MATH_BENCH_LERP_X))
MATH_KERNEL(quat_mul, f, qf, quatf_mul(p->qf, p->rf))
MATH_KERNEL(quat_mul, x, qx, quatx_mul(p->qx, p->rx))
MATH_KERNEL(mat4_mul, f, mf, mat4f_mul(p->mf, p->nf))
MATH_KERNEL(mat4_mul, x, mx, mat4x_mul(p->mx, p->nx))
MATH_KERNEL(transform_compute_matrix, f, mf, transformf_compute_matrix(&p->tf))
MATH_KERNEL(transform_compute_matrix, x, mx, transformx_compute_matrix(&p->tx))
MATH_KERNEL(mathf_recip, f, f, mathf_recip(p->af.x))
MATH_KERNEL(mathf_rsqrt, f, f, mathf_rsqrt(p->af.x))
MATH_KERNEL(div_recip, f, f, 1.0F / p->af.x)
MATH_KERNEL(div_rsqrt, f, f, 1.0F / sqrtf(p->af.x))
MATH_KERNEL(sinf, f, f, sinf(p->angle))
MATH_KERNEL(cosf, f, f, cosf(p->angle))
MATH_KERNEL(atan2f, f, f, atan2f(p->angle, p->bf.x))
MATH_KERNEL(sqrtf, f, f, sqrtf(p->af.x))
MATH_KERNEL(trig_sin, i, i, trig_sin(p->phase))

static __attribute__((noinline)) void
math_bench_dot_f32_n256_f(fixture_t* f) {
    float acc = 0.0F;
    for (int j = 0; j < MATH_BENCH_DOT_SIZE; j++) {
        acc += f->dot_a[j] * f->dot_b[j];
    }
    f->output[0].f = acc;
}

#define MATH_ROW(name, type, ratchet) {#name, #type, math_bench_##name##_##type, ratchet}
static const row_t rows[] = {
    MATH_ROW(add, f, false),
    MATH_ROW(scale, f, false),
    MATH_ROW(dot, f, true),
    MATH_ROW(mul, f, false),
    MATH_ROW(lerp, f, false),
    MATH_ROW(add, i, false),
    MATH_ROW(scale, i, false),
    MATH_ROW(dot, i, false),
    MATH_ROW(mul, i, false),
    MATH_ROW(lerp, i, false),
    MATH_ROW(add, s, false),
    MATH_ROW(scale, s, false),
    MATH_ROW(dot, s, false),
    MATH_ROW(mul, s, false),
    MATH_ROW(lerp, s, false),
    MATH_ROW(add, x, false),
    MATH_ROW(scale, x, false),
    MATH_ROW(dot, x, false),
    MATH_ROW(mul, x, false),
    MATH_ROW(lerp, x, false),
    MATH_ROW(normalize, f, true),
    MATH_ROW(normalize, x, false),
    MATH_ROW(wrap, f, false),
    MATH_ROW(wrap, i, false),
    MATH_ROW(wrap, x, false),
    MATH_ROW(convert, f_to_x, false),
    MATH_ROW(convert, x_to_f, false),
    MATH_ROW(convert, f_to_s, false),
    MATH_ROW(convert, s_to_f, false),
    MATH_ROW(convert, i_to_f, false),
    MATH_ROW(convert, f_to_i, false),
    MATH_ROW(slerp, f, true),
    MATH_ROW(nlerp, f, false),
    MATH_ROW(nlerp, x, false),
    MATH_ROW(quat_mul, f, false),
    MATH_ROW(quat_mul, x, false),
    MATH_ROW(mat4_mul, f, true),
    MATH_ROW(mat4_mul, x, false),
    MATH_ROW(transform_compute_matrix, f, false),
    MATH_ROW(transform_compute_matrix, x, false),
    MATH_ROW(mathf_recip, f, true),
    MATH_ROW(mathf_rsqrt, f, true),
    MATH_ROW(div_recip, f, false),
    MATH_ROW(div_rsqrt, f, false),
    MATH_ROW(sinf, f, false),
    MATH_ROW(cosf, f, false),
    MATH_ROW(atan2f, f, false),
    MATH_ROW(sqrtf, f, false),
    MATH_ROW(trig_sin, i, false),
    MATH_ROW(dot_f32_n256, f, false),
};

static fixture_t*
fixture(void) {
    fixture_t* f = malloc(sizeof *f);
    active_fixture = f;
    suite_set_test_cleanup(release);
    TEST_ASSERT_NOT_NULL(f);
    for (int j = 0; j < MATH_BENCH_BATCH; j++) {
        input_t* p = &f->input[j];
        const float v = (float)(j % MATH_BENCH_INPUT_PERIOD + 1) * MATH_BENCH_INPUT_SCALE;
        const float turn = (float)j / MATH_BENCH_BATCH;
        p->af = (vec3f_t){v, v + 1.0F, v - 1.0F};
        p->bf = (vec3f_t){v + MATH_BENCH_INPUT_SCALE, -v, 1.0F};
        p->ai = vec3i_from_vec3f(p->af);
        p->bi = vec3i_from_vec3f(p->bf);
        p->as = vec3s_from_vec3f(p->af, MATH_BENCH_INPUT_SCALE);
        p->bs = vec3s_from_vec3f(p->bf, MATH_BENCH_INPUT_SCALE);
        p->ax = vec3x_from_vec3f(p->af);
        p->bx = vec3x_from_vec3f(p->bf);
        p->angle = (turn - 0.5F) * MATH_TAU;
        p->phase = (uint16_t)(j * TRIG_TURN / MATH_BENCH_BATCH);
        p->qf = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, p->angle);
        p->rf = quatf_from_axis_angle((vec3f_t){1.0F, 0.0F, 0.0F}, p->angle + MATH_PI / 2.0F);
        p->qx = quatx_from_axis_angle((vec3x_t){0, MATHX_ONE, 0}, mathf_to_x(turn - 0.5F));
        p->rx = quatx_from_axis_angle((vec3x_t){MATHX_ONE, 0, 0}, mathf_to_x(turn - 0.25F));
        p->tf = (transformf_t){.position = p->af, .rotation = p->qf, .scale = p->bf};
        p->tx = (transformx_t){.position = p->ax, .rotation = p->qx, .scale = p->bx};
        p->mf = transformf_compute_matrix(&p->tf);
        p->nf = mat4f_from_trs(p->bf, p->rf, p->af);
        p->mx = transformx_compute_matrix(&p->tx);
        p->nx = mat4x_from_trs(p->bx, p->rx, p->ax);
    }
    for (int j = 0; j < MATH_BENCH_DOT_SIZE; j++) {
        f->dot_a[j] = (float)(j % MATH_BENCH_INPUT_PERIOD + 1) * MATH_BENCH_INPUT_SCALE;
        f->dot_b[j] = 1.0F - f->dot_a[j];
    }
    return f;
}

/* Both cores must be cache-safe while dirty PSRAM lines are written back and invalidated. */
static void IRAM_ATTR
cold_cache(void) {
    spi_flash_op_lock();
    spi_flash_disable_interrupts_caches_and_other_cpu();
    Cache_WriteBack_All();
    Cache_Invalidate_DCache_All();
    Cache_Invalidate_ICache_All();
    spi_flash_enable_interrupts_caches_and_other_cpu();
    spi_flash_op_unlock();
}

static int
ops_per_call(const row_t* row) {
    return row->fn == math_bench_dot_f32_n256_f ? 1 : MATH_BENCH_BATCH;
}

static float
median_cycles(const row_t* row, fixture_t* f, bool cold) {
    _Static_assert(MATH_BENCH_COLD_REPS <= MATH_BENCH_WARM_REPS, "cold samples must fit warm sample storage");
    int32_t samples[MATH_BENCH_WARM_REPS];
    const int reps = cold ? MATH_BENCH_COLD_REPS : MATH_BENCH_WARM_REPS;
    row->fn(f);
    for (int j = 0; j < reps; j++) {
        if (cold) {
            cold_cache();
        }
        const uint32_t start = esp_cpu_get_cycle_count();
        row->fn(f);
        samples[j] = (int32_t)(esp_cpu_get_cycle_count() - start);
    }
    return (float)perf_stats_compute(samples, reps).med / (float)ops_per_call(row);
}

static void
test_math_board_rows(void) {
#if CONFIG_LAUNCHER_QEMU
    TEST_IGNORE_MESSAGE("QEMU has no hardware cache timing or performance monitor");
#endif
    TEST_ASSERT_EQUAL_INT(0, esp_cpu_get_core_id());
    TEST_ASSERT_TRUE(frame_cost_counters_idle());
    const int insn = frame_cost_event_index(FRAME_COST_DEFAULT_EVENT);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, insn);
    printf("BUILD_ID=%s\n", build_id());
    fixture_t* f = fixture();
    for (size_t j = 0; j < sizeof rows / sizeof *rows; j++) {
        const row_t* row = &rows[j];
        const float warm = median_cycles(row, f, false);
        const float cold = median_cycles(row, f, true);
        row->fn(f);
        TEST_ASSERT_TRUE(frame_cost_count_begin(insn));
        row->fn(f);
        uint32_t cycles = 0, value = 0;
        TEST_ASSERT_TRUE(frame_cost_count_end(&cycles, &value));
        printf("MATHBENCH name=%s type=%s warm=%.1f cold=%.1f insn=%.1f\n", row->name, row->type, (double)warm,
               (double)cold, (double)((float)value / (float)ops_per_call(row)));
        timing_yield();
    }
    release();
    suite_clear_test_cleanup();
}

static void
test_math_instruction_rows(void) {
    fixture_t* f = fixture();
    for (size_t j = 0; j < sizeof rows / sizeof *rows; j++) {
        const row_t* row = &rows[j];
        if (!row->ratchet) {
            continue;
        }
        row->fn(f);
        int64_t us = 0;
        for (int batch = 0; batch < MATH_BENCH_ICOUNT_BATCHES; batch++) {
            const int64_t start = timing_now_us();
            for (int call = 0; call < MATH_BENCH_ICOUNT_CALLS; call++) {
                row->fn(f);
            }
            us += timing_now_us() - start;
            timing_yield();
        }
        char name[MATH_BENCH_NAME_SIZE];
        snprintf(name, sizeof name, "math_%s_%s", row->name, row->type);
        perf_icount_report(name, us, MATH_BENCH_ICOUNT_BATCHES * MATH_BENCH_ICOUNT_CALLS * ops_per_call(row),
                           MATH_BENCH_ICOUNT_BATCHES);
    }
    release();
    suite_clear_test_cleanup();
}

void
run_math_perf_suite(void) {
    RUN_TEST(test_math_board_rows);
}

void
run_math_icount_suite(void) {
    RUN_TEST(test_math_instruction_rows);
}

SUITE_REGISTER_ON_REQUEST(run_math_perf_suite);
SUITE_REGISTER_ON_REQUEST(run_math_icount_suite);
#endif

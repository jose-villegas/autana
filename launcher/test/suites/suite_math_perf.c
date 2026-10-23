/* Fixed workloads share kernels between hardware counters and the instruction ratchet. */
#ifdef DEVICE_BUILD
#include <math.h>
#include "core/memory.h"
#include "core/timing.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "math/linear/transformf.h"
#include "math/linear/transformx.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"
#include "math/scalar/trig.h"
#include "perf_stats.h"
#include "profile/frame_cost.h"
#include "services/build_id.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"

#define MATH_BENCH_BATCH             64
#define MATH_BENCH_ICOUNT_CALLS      4096
#define MATH_BENCH_ICOUNT_INFO_CALLS 256
#define MATH_BENCH_ICOUNT_BATCHES    3
#define MATH_BENCH_DOT_SIZE          256
#define MATH_BENCH_INPUT_PERIOD      17
#define MATH_BENCH_INPUT_SCALE       0.125F
#define MATH_BENCH_LERP_NUMERATOR    3
#define MATH_BENCH_LERP_DENOMINATOR  8
#define MATH_BENCH_LERP_F            ((float)MATH_BENCH_LERP_NUMERATOR / MATH_BENCH_LERP_DENOMINATOR)
#define MATH_BENCH_LERP_X            (MATHX_ONE * MATH_BENCH_LERP_NUMERATOR / MATH_BENCH_LERP_DENOMINATOR)
#define MATH_BENCH_WRAP_LIMIT        1

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
    result_t output[MATH_BENCH_BATCH];
    float dot_a[MATH_BENCH_DOT_SIZE], dot_b[MATH_BENCH_DOT_SIZE];
} fixture_t;

static fixture_t* active_fixture;

static void
release(void) {
    memory_free(active_fixture);
    active_fixture = NULL;
}

typedef void (*kernel_t)(fixture_t*);

typedef struct {
    const char* icount_name;
    const char* name;
    const char* type;
    kernel_t fn;
    bool ratchet;
    int ops;
} row_t;

#define MATH_VECTOR_ROWS(X, type, vmember, smember, dot_ratchet, ...)                                                  \
    X(add, "add (vec3)", type, false, vmember, vec3##type##_add(p->a##type, p->b##type))                               \
    X(scale, "scale (vec3)", type, false, vmember, vec3##type##_scale(p->a##type, p->b##type.x))                       \
    X(dot, "dot (vec3)", type, dot_ratchet, smember, vec3##type##_dot(p->a##type, p->b##type))                         \
    X(mul, "mul (scalar)", type, false, smember, math##type##_mul(p->a##type.x, p->b##type.x))                         \
    X(lerp, "lerp (vec3)", type, false, vmember, vec3##type##_lerp(p->a##type, p->b##type, __VA_ARGS__))

#define MATH_ROWS(X, DOT)                                                                                              \
    MATH_VECTOR_ROWS(X, f, vf, f, true, MATH_BENCH_LERP_F)                                                             \
    MATH_VECTOR_ROWS(X, i, vi, i, false, MATH_BENCH_LERP_NUMERATOR, MATH_BENCH_LERP_DENOMINATOR)                       \
    MATH_VECTOR_ROWS(X, s, vs, i, false, MATH_BENCH_LERP_NUMERATOR, MATH_BENCH_LERP_DENOMINATOR)                       \
    MATH_VECTOR_ROWS(X, x, vx, i, false, MATH_BENCH_LERP_X)                                                            \
    X(normalize, "normalize (vec3)", f, true, vf, vec3f_normalize(p->af))                                              \
    X(normalize, "normalize (vec3)", x, false, vx, vec3x_normalize(p->ax))                                             \
    X(wrap, "wrap", f, false, f, mathf_wrap(p->angle, -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))                  \
    X(wrap, "wrap", i, false, i, mathi_wrap(p->ai.x, -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))                   \
    X(wrap, "wrap", x, false, i,                                                                                       \
      mathx_wrap(p->ax.x, -MATH_BENCH_WRAP_LIMIT* MATHX_ONE, MATH_BENCH_WRAP_LIMIT* MATHX_ONE))                        \
    X(convert, "convert", f_to_x, false, vx, vec3x_from_vec3f(p->af))                                                  \
    X(convert, "convert", x_to_f, false, vf, vec3f_from_vec3x(p->ax))                                                  \
    X(convert, "convert", f_to_s, false, vs, vec3s_from_vec3f(p->af, MATH_BENCH_INPUT_SCALE))                          \
    X(convert, "convert", s_to_f, false, vf, vec3f_from_vec3s(p->as, MATH_BENCH_INPUT_SCALE))                          \
    X(convert, "convert", i_to_f, false, vf, vec3f_from_vec3i(p->ai))                                                  \
    X(convert, "convert", f_to_i, false, vi, vec3i_from_vec3f(p->af))                                                  \
    X(slerp, "slerp", f, true, qf, quatf_slerp(p->qf, p->rf, MATH_BENCH_LERP_F))                                       \
    X(nlerp, "nlerp", f, false, qf, quatf_nlerp(p->qf, p->rf, MATH_BENCH_LERP_F))                                      \
    X(nlerp, "nlerp", x, false, qx, quatx_nlerp(p->qx, p->rx, MATH_BENCH_LERP_X))                                      \
    X(quat_mul, "quat mul", f, false, qf, quatf_mul(p->qf, p->rf))                                                     \
    X(quat_mul, "quat mul", x, false, qx, quatx_mul(p->qx, p->rx))                                                     \
    X(mat4_mul, "mat4 mul", f, true, mf, mat4f_mul(p->mf, p->nf))                                                      \
    X(mat4_mul, "mat4 mul", x, false, mx, mat4x_mul(p->mx, p->nx))                                                     \
    X(transform_compute_matrix, "transform compute_matrix", f, false, mf, transformf_compute_matrix(&p->tf))           \
    X(transform_compute_matrix, "transform compute_matrix", x, false, mx, transformx_compute_matrix(&p->tx))           \
    X(mathf_recip, "mathf_recip", f, false, f, mathf_recip(p->af.x))                                                   \
    X(mathf_rsqrt, "mathf_rsqrt", f, false, f, mathf_rsqrt(p->af.x))                                                   \
    X(mathf_rsqrt_approx, "mathf_rsqrt_approx", f, false, f, mathf_rsqrt_approx(p->af.x))                              \
    X(div_recip, "div_recip", f, false, f, 1.0F / p->af.x)                                                             \
    X(div_rsqrt, "div_rsqrt", f, false, f, 1.0F / sqrtf(p->af.x))                                                      \
    X(sinf, "sinf", f, false, f, sinf(p->angle))                                                                       \
    X(cosf, "cosf", f, false, f, cosf(p->angle))                                                                       \
    X(atan2f, "atan2f", f, false, f, atan2f(p->angle, p->bf.x))                                                        \
    X(sqrtf, "sqrtf", f, false, f, sqrtf(p->af.x))                                                                     \
    X(trig_sin, "trig_sin", i, false, i, trig_sin(p->phase))                                                           \
    DOT(dot_f32_n256, "dot_f32_n256", f, false, 1)

#define MATH_KERNEL(name, label, type, ratchet, member, expression)                                                    \
    static __attribute__((noinline)) void math_bench_##name##_##type(fixture_t* f) {                                   \
        for (int j = 0; j < MATH_BENCH_BATCH; j++) {                                                                   \
            const input_t* p = &f->input[j];                                                                           \
            f->output[j].member = (expression);                                                                        \
        }                                                                                                              \
        __asm__ volatile("" : : "r"(f->output) : "memory");                                                            \
    }

#define MATH_DOT_KERNEL(name, label, type, ratchet, ops)                                                               \
    static __attribute__((noinline)) void math_bench_##name##_##type(fixture_t* f) {                                   \
        float acc = 0.0F;                                                                                              \
        for (int j = 0; j < MATH_BENCH_DOT_SIZE; j++) {                                                                \
            acc += f->dot_a[j] * f->dot_b[j];                                                                          \
        }                                                                                                              \
        f->output[0].f = acc;                                                                                          \
        __asm__ volatile("" : : "r"(f->output) : "memory");                                                            \
    }
MATH_ROWS(MATH_KERNEL, MATH_DOT_KERNEL)

#define MATH_ROW_OPS(name, label, type, hot, count)                                                                    \
    {"math_" #name "_" #type, label, #type, math_bench_##name##_##type, hot, count},
#define MATH_ROW(name, label, type, hot, member, expression) MATH_ROW_OPS(name, label, type, hot, MATH_BENCH_BATCH)
static const row_t rows[] = {MATH_ROWS(MATH_ROW, MATH_ROW_OPS)};

#define MATH_STRING(value) #value
#define MATH_MANIFEST_OPS(name, label, type, hot, count)                                                               \
    #name "_" #type "\t" label "\t" #type "\t" MATH_STRING(count) "\n"
#define MATH_MANIFEST(name, label, type, hot, member, expression)                                                      \
    MATH_MANIFEST_OPS(name, label, type, hot, MATH_BENCH_BATCH)
/* The ELF retains row identity for reports made before a board capture exists. */
static const char math_bench_manifest[] = MATH_ROWS(MATH_MANIFEST, MATH_MANIFEST_OPS);

static fixture_t*
fixture(void) {
    fixture_t* f = memory_alloc(sizeof *f, MEMORY_INTERNAL);
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

static float
median_cycles(const row_t* row, fixture_t* f, bool cold) {
    _Static_assert(PERF_CYCLE_COLD_REPS <= PERF_CYCLE_WARM_REPS, "cold samples must fit warm sample storage");
    int32_t samples[PERF_CYCLE_WARM_REPS];
    const int reps = cold ? PERF_CYCLE_COLD_REPS : PERF_CYCLE_WARM_REPS;
    row->fn(f);
    for (int j = 0; j < reps; j++) {
        if (cold) {
            perf_cache_cold();
        }
        const uint32_t start = esp_cpu_get_cycle_count();
        row->fn(f);
        samples[j] = (int32_t)(esp_cpu_get_cycle_count() - start);
    }
    return (float)perf_stats_compute(samples, reps).med / (float)(cold ? 1 : row->ops);
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
        printf("MATHBENCH name=%s type=%s ops=%d warm=%.1f cold=%.1f insn=%.1f\n", row->name, row->type, row->ops,
               (double)warm, (double)cold, (double)((float)value / (float)row->ops));
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
        row->fn(f);
        const int calls = row->ratchet ? MATH_BENCH_ICOUNT_CALLS : MATH_BENCH_ICOUNT_INFO_CALLS;
        int reps = 0;
        int64_t us = 0;
        for (int batch = 0; batch < MATH_BENCH_ICOUNT_BATCHES; batch++) {
            const int64_t start = timing_now_us();
            int64_t elapsed_us;
            do {
                for (int call = 0; call < calls; call++) {
                    row->fn(f);
                }
                reps += calls;
                elapsed_us = timing_now_us() - start;
            } while (!row->ratchet && !perf_icount_long_enough(elapsed_us, 1));
            us += elapsed_us;
            timing_yield();
        }
        perf_icount_report_row(row->ratchet ? PERF_ICOUNT_PREFIX : PERF_ICOUNT_INFO_PREFIX, row->icount_name, us, reps,
                               MATH_BENCH_ICOUNT_BATCHES);
    }
    release();
    suite_clear_test_cleanup();
}

void
run_math_perf_suite(void) {
    __asm__ volatile("" : : "r"(math_bench_manifest));
    RUN_TEST(test_math_board_rows);
}

void
run_math_icount_suite(void) {
    RUN_TEST(test_math_instruction_rows);
}

SUITE_REGISTER_ON_REQUEST(run_math_perf_suite);
SUITE_REGISTER_ON_REQUEST(run_math_icount_suite);
#endif

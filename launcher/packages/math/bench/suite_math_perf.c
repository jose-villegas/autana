/* Fixed workloads share kernels between hardware counters and the instruction ratchet. */
#include <math.h>
#include "math/linear/transformf.h"
#include "math/linear/transformx.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"
#include "math/scalar/maths.h"
#include "math/scalar/trig.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "testkit/suite.h"
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

#define MATH_BENCH_ARRAYS(PAIR, ARRAY)                                                                                 \
    PAIR(vf, vec3f_t)                                                                                                  \
    PAIR(vi, vec3i_t)                                                                                                  \
    PAIR(vs, vec3s_t)                                                                                                  \
    PAIR(vx, vec3x_t)                                                                                                  \
    PAIR(qf, quatf_t)                                                                                                  \
    PAIR(qx, quatx_t)                                                                                                  \
    PAIR(mf, mat4f_t)                                                                                                  \
    PAIR(mx, mat4x_t)                                                                                                  \
    ARRAY(tf, transformf_t, MATH_BENCH_BATCH)                                                                          \
    ARRAY(tx, transformx_t, MATH_BENCH_BATCH)                                                                          \
    ARRAY(angle, float, MATH_BENCH_BATCH)                                                                              \
    ARRAY(saturate, int32_t, MATH_BENCH_BATCH)                                                                         \
    ARRAY(phase, uint16_t, MATH_BENCH_BATCH)                                                                           \
    ARRAY(output, result_t, MATH_BENCH_BATCH)                                                                          \
    ARRAY(dot_a, float, MATH_BENCH_DOT_SIZE)                                                                           \
    ARRAY(dot_b, float, MATH_BENCH_DOT_SIZE)

#define MATH_BENCH_PAIR_TYPE(name, type)                                                                               \
    typedef struct {                                                                                                   \
        type a, b;                                                                                                     \
    } pair_##name##_t;
#define MATH_BENCH_SKIP(name, type, count)
MATH_BENCH_ARRAYS(MATH_BENCH_PAIR_TYPE, MATH_BENCH_SKIP)

#define MATH_BENCH_FIELD(name, type, count) type* name;
#define MATH_BENCH_PAIR_FIELD(name, type)   MATH_BENCH_FIELD(name, pair_##name##_t, MATH_BENCH_BATCH)

typedef struct {
    MATH_BENCH_ARRAYS(MATH_BENCH_PAIR_FIELD, MATH_BENCH_FIELD)
} fixture_t;

static fixture_t* active_fixture;

static void
release(void) {
    if (active_fixture == NULL) {
        return;
    }
#define MATH_BENCH_FREE(name, type, count) free(active_fixture->name);
#define MATH_BENCH_PAIR_FREE(name, type)   MATH_BENCH_FREE(name, pair_##name##_t, MATH_BENCH_BATCH)
    MATH_BENCH_ARRAYS(MATH_BENCH_PAIR_FREE, MATH_BENCH_FREE)
    free(active_fixture);
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

static float
wrap_fmodf(float v, float lo, float hi) {
    const float len = hi - lo;
    float r = fmodf(v - lo, len);
    if (r < 0.0F) {
        r += len;
    }
    const float wrapped = lo + (r < len ? r : 0.0F);
    return wrapped < hi ? wrapped : lo;
}

static vec3x_t
normalize_div(vec3x_t a) {
    enum {
        COMPONENT_SHIFT = 29,
        TOP_BIT = 31,
        RECIPROCAL_SHIFT = COMPONENT_SHIFT + TOP_BIT,
        OUTPUT_SHIFT = RECIPROCAL_SHIFT - MATHX_SHIFT,
    };

    const int32_t components[] = {a.x, a.y, a.z};
    uint32_t magnitude = 0;
    for (size_t i = 0; i < sizeof components / sizeof components[0]; i++) {
        const uint32_t absolute = components[i] < 0 ? (uint32_t)-(int64_t)components[i] : (uint32_t)components[i];
        if (absolute > magnitude) {
            magnitude = absolute;
        }
    }
    if (magnitude == 0) {
        return a;
    }
    const int shift = COMPONENT_SHIFT - (TOP_BIT - __builtin_clz(magnitude));
    int64_t scaled[3];
    uint64_t square = 0;
    for (size_t i = 0; i < sizeof scaled / sizeof scaled[0]; i++) {
        scaled[i] = shift >= 0 ? (int64_t)components[i] * (INT64_C(1) << shift) : fx_round_shift(components[i], -shift);
        square += (uint64_t)(scaled[i] * scaled[i]);
    }
    uint64_t length = mathi_isqrt64(square);
    length += square - (length * length) > length;
    const int32_t reciprocal = mathx_saturate((int64_t)(((UINT64_C(1) << RECIPROCAL_SHIFT) + length / 2) / length));
    return (vec3x_t){(int32_t)fx_round_shift(scaled[0] * reciprocal, OUTPUT_SHIFT),
                     (int32_t)fx_round_shift(scaled[1] * reciprocal, OUTPUT_SHIFT),
                     (int32_t)fx_round_shift(scaled[2] * reciprocal, OUTPUT_SHIFT)};
}

static quatx_t
nlerp_div(quatx_t a, quatx_t b, int32_t t) {
    const int32_t dot = mathx_add(mathx_add(mathx_add(mathx_mul(a.x, b.x), mathx_mul(a.y, b.y)), mathx_mul(a.z, b.z)),
                                  mathx_mul(a.w, b.w));
    if (dot < 0) {
        b = (quatx_t){mathx_neg(b.x), mathx_neg(b.y), mathx_neg(b.z), mathx_neg(b.w)};
    }
    const int32_t wa = mathx_sub(MATHX_ONE, t);
    const quatx_t blend = {
        mathx_add(mathx_mul(a.x, wa), mathx_mul(b.x, t)), mathx_add(mathx_mul(a.y, wa), mathx_mul(b.y, t)),
        mathx_add(mathx_mul(a.z, wa), mathx_mul(b.z, t)), mathx_add(mathx_mul(a.w, wa), mathx_mul(b.w, t))};
    const int32_t square = mathx_add(
        mathx_add(mathx_add(mathx_mul(blend.x, blend.x), mathx_mul(blend.y, blend.y)), mathx_mul(blend.z, blend.z)),
        mathx_mul(blend.w, blend.w));
    const int32_t reciprocal = mathx_div(MATHX_ONE, (int32_t)mathi_isqrt64((uint64_t)square << MATHX_SHIFT));
    return (quatx_t){mathx_mul(blend.x, reciprocal), mathx_mul(blend.y, reciprocal), mathx_mul(blend.z, reciprocal),
                     mathx_mul(blend.w, reciprocal)};
}

/* MATH_PLAIN(div_recip_f): 1.0F / x */

/* MATH_PLAIN(div_rsqrt_f): 1.0F / sqrtf(x) */

/* MATH_PLAIN(wrap_fmodf_f): fmodf(v - lo, hi - lo), adjusted into [lo, hi) */

/* MATH_PLAIN(normalize_div_x): isqrt64 squared length, then 64-bit divide and component scaling */

/* MATH_PLAIN(nlerp_div_x): linear quaternion blend, isqrt64 length, then 64-bit divide normalization */

/* MATH_PLAIN(sinf_f): sinf(phase * (MATH_TAU / TRIG_TURN)) */

#define MATH_VECTOR_ROWS(X, type, vmember, smember, dot_ratchet, ...)                                                  \
    X(add, "add (vec3)", type, "vector", false, vmember, vec3##type##_add(f->v##type[j].a, f->v##type[j].b))           \
    X(scale, "scale (vec3)", type, "vector", false, vmember, vec3##type##_scale(f->v##type[j].a, f->v##type[j].b.x))   \
    X(dot, "dot (vec3)", type, "vector", dot_ratchet, smember, vec3##type##_dot(f->v##type[j].a, f->v##type[j].b))     \
    X(mul, "mul (scalar)", type, "scalar", false, smember, math##type##_mul(f->v##type[j].a.x, f->v##type[j].b.x))     \
    X(lerp, "lerp (vec3)", type, "vector", false, vmember,                                                             \
      vec3##type##_lerp(f->v##type[j].a, f->v##type[j].b, __VA_ARGS__))

#define MATH_QUATERNION_ROWS(X, type, lerp)                                                                            \
    X(nlerp, "nlerp", type, "quaternion", false, q##type, quat##type##_nlerp(f->q##type[j].a, f->q##type[j].b, lerp))  \
    X(quat_mul, "quat mul", type, "quaternion", false, q##type, quat##type##_mul(f->q##type[j].a, f->q##type[j].b))

/* A float scalar function of the first input lane. */
#define MATH_SCALAR_F_ROW(X, fn) X(fn, #fn, f, "scalar", false, f, fn(f->vf[j].a.x))

#define MATH_ROWS(X, DOT)                                                                                              \
    MATH_VECTOR_ROWS(X, f, vf, f, true, MATH_BENCH_LERP_F)                                                             \
    MATH_VECTOR_ROWS(X, i, vi, i, false, MATH_BENCH_LERP_NUMERATOR, MATH_BENCH_LERP_DENOMINATOR)                       \
    MATH_VECTOR_ROWS(X, s, vs, i, false, MATH_BENCH_LERP_NUMERATOR, MATH_BENCH_LERP_DENOMINATOR)                       \
    MATH_VECTOR_ROWS(X, x, vx, i, false, MATH_BENCH_LERP_X)                                                            \
    X(normalize, "normalize (vec3)", f, "vector", true, vf, vec3f_normalize(f->vf[j].a))                               \
    X(normalize, "normalize (vec3)", x, "vector", false, vx, vec3x_normalize(f->vx[j].a))                              \
    X(wrap, "wrap", f, "scalar", false, f, mathf_wrap(f->angle[j], -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))     \
    X(wrap, "wrap", i, "scalar", false, i, mathi_wrap(f->vi[j].a.x, -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))    \
    X(wrap, "wrap", x, "scalar", false, i,                                                                             \
      mathx_wrap(f->vx[j].a.x, -MATH_BENCH_WRAP_LIMIT* MATHX_ONE, MATH_BENCH_WRAP_LIMIT* MATHX_ONE))                   \
    X(convert, "convert", f_to_x, "vector", false, vx, vec3x_from_vec3f(f->vf[j].a))                                   \
    X(convert, "convert", x_to_f, "vector", false, vf, vec3f_from_vec3x(f->vx[j].a))                                   \
    X(convert, "convert", f_to_s, "vector", false, vs, vec3s_from_vec3f(f->vf[j].a, MATH_BENCH_INPUT_SCALE))           \
    X(convert, "convert", s_to_f, "vector", false, vf, vec3f_from_vec3s(f->vs[j].a, MATH_BENCH_INPUT_SCALE))           \
    X(convert, "convert", i_to_f, "vector", false, vf, vec3f_from_vec3i(f->vi[j].a))                                   \
    X(convert, "convert", f_to_i, "vector", false, vi, vec3i_from_vec3f(f->vf[j].a))                                   \
    X(slerp, "slerp", f, "quaternion", true, qf, quatf_slerp(f->qf[j].a, f->qf[j].b, MATH_BENCH_LERP_F))               \
    MATH_QUATERNION_ROWS(X, f, MATH_BENCH_LERP_F)                                                                      \
    MATH_QUATERNION_ROWS(X, x, MATH_BENCH_LERP_X)                                                                      \
    X(mat4_mul, "mat4 mul", f, "matrix", true, mf, mat4f_mul(f->mf[j].a, f->mf[j].b))                                  \
    X(mat4_mul, "mat4 mul", x, "matrix", false, mx, mat4x_mul(f->mx[j].a, f->mx[j].b))                                 \
    X(transform_compute_matrix, "transform compute_matrix", f, "matrix", false, mf,                                    \
      transformf_compute_matrix(&f->tf[j]))                                                                            \
    X(transform_compute_matrix, "transform compute_matrix", x, "matrix", false, mx,                                    \
      transformx_compute_matrix(&f->tx[j]))                                                                            \
    X(maths_saturate, "maths_saturate", s, "scalar", false, i, maths_saturate(f->saturate[j]))                         \
    MATH_SCALAR_F_ROW(X, mathf_recip)                                                                                  \
    MATH_SCALAR_F_ROW(X, mathf_recip_approx)                                                                           \
    MATH_SCALAR_F_ROW(X, mathf_rsqrt)                                                                                  \
    MATH_SCALAR_F_ROW(X, mathf_rsqrt_approx)                                                                           \
    X(wrap_fmodf, "wrap_fmodf", f, "libm", false, f,                                                                   \
      wrap_fmodf(f->angle[j], -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT))                                          \
    X(normalize_div, "normalize_div", x, "vector", false, vx, normalize_div(f->vx[j].a))                               \
    X(nlerp_div, "nlerp_div", x, "quaternion", false, qx, nlerp_div(f->qx[j].a, f->qx[j].b, MATH_BENCH_LERP_X))        \
    X(mathx_rsqrt_q30, "mathx_rsqrt_q30", x, "scalar", false, i,                                                       \
      mathx_rsqrt_q30(MATHX_RSQRT_MIN + (uint32_t)f->vx[j].a.x))                                                       \
    X(div_recip, "div_recip", f, "scalar", false, f, 1.0F / f->vf[j].a.x)                                              \
    X(div_rsqrt, "div_rsqrt", f, "scalar", false, f, 1.0F / sqrtf(f->vf[j].a.x))                                       \
    X(sinf, "sinf", f, "libm", false, f, sinf((float)f->phase[j] * (MATH_TAU / TRIG_TURN)))                            \
    X(cosf, "cosf", f, "libm", false, f, cosf(f->angle[j]))                                                            \
    X(atan2f, "atan2f", f, "libm", false, f, atan2f(f->angle[j], f->vf[j].b.x))                                        \
    X(sqrtf, "sqrtf", f, "libm", false, f, sqrtf(f->vf[j].a.x))                                                        \
    X(trig_sin, "trig_sin", i, "scalar", false, i, trig_sin(f->phase[j]))                                              \
    DOT(dot_f32_n256, "dot_f32_n256", f, "calibration", false, 1)

#define MATH_KERNEL(name, label, type, family, ratchet, member, expression)                                            \
    static __attribute__((noinline)) void math_bench_##name##_##type(fixture_t* f) {                                   \
        for (int j = 0; j < MATH_BENCH_BATCH; j++) {                                                                   \
            f->output[j].member = (expression);                                                                        \
        }                                                                                                              \
        __asm__ volatile("" : : "r"(f->output) : "memory");                                                            \
    }

#define MATH_DOT_KERNEL(name, label, type, family, ratchet, ops)                                                       \
    static __attribute__((noinline)) void math_bench_##name##_##type(fixture_t* f) {                                   \
        float acc = 0.0F;                                                                                              \
        for (int j = 0; j < MATH_BENCH_DOT_SIZE; j++) {                                                                \
            acc += f->dot_a[j] * f->dot_b[j];                                                                          \
        }                                                                                                              \
        f->output[0].f = acc;                                                                                          \
        __asm__ volatile("" : : "r"(f->output) : "memory");                                                            \
    }
MATH_ROWS(MATH_KERNEL, MATH_DOT_KERNEL)

#define MATH_ROW_OPS(name, label, type, family, hot, count)                                                            \
    {"math_" #name "_" #type, label, #type, math_bench_##name##_##type, hot, count},
#define MATH_ROW(name, label, type, family, hot, member, expression)                                                   \
    MATH_ROW_OPS(name, label, type, family, hot, MATH_BENCH_BATCH)
static const row_t rows[] = {MATH_ROWS(MATH_ROW, MATH_ROW_OPS)};

#define MATH_STRING(value) #value
#define MATH_MANIFEST_OPS(name, label, type, family, hot, count)                                                       \
    #name "_" #type "\t" label "\t" #type "\t" MATH_STRING(count) "\t" family "\n"
#define MATH_MANIFEST(name, label, type, family, hot, member, expression)                                              \
    MATH_MANIFEST_OPS(name, label, type, family, hot, MATH_BENCH_BATCH)
/* The ELF retains row identity for reports made before a board capture exists. */
static const char math_bench_manifest[] = MATH_ROWS(MATH_MANIFEST, MATH_MANIFEST_OPS);

static fixture_t*
fixture(void) {
    fixture_t* f = bench_alloc_internal(sizeof *f, "fixture");
    *f = (fixture_t){0};
    active_fixture = f;
    suite_set_test_cleanup(release);
#define MATH_BENCH_ALLOC(name, type, count) f->name = bench_alloc_internal(sizeof *f->name * (count), #name);
#define MATH_BENCH_PAIR_ALLOC(name, type)   MATH_BENCH_ALLOC(name, pair_##name##_t, MATH_BENCH_BATCH)
    MATH_BENCH_ARRAYS(MATH_BENCH_PAIR_ALLOC, MATH_BENCH_ALLOC)
    static const int32_t saturate_inputs[] = {INT32_MIN, INT16_MIN - 1, INT16_MIN, 0,
                                              INT16_MAX, INT16_MAX + 1, INT32_MAX};
    for (int j = 0; j < MATH_BENCH_BATCH; j++) {
        const float v = (float)((j % MATH_BENCH_INPUT_PERIOD) + 1) * MATH_BENCH_INPUT_SCALE;
        const float turn = (float)j / MATH_BENCH_BATCH;
        f->vf[j].a = (vec3f_t){v, v + 1.0F, v - 1.0F};
        f->vf[j].b = (vec3f_t){v + MATH_BENCH_INPUT_SCALE, -v, 1.0F};
        f->vi[j].a = vec3i_from_vec3f(f->vf[j].a);
        f->vi[j].b = vec3i_from_vec3f(f->vf[j].b);
        f->vs[j].a = vec3s_from_vec3f(f->vf[j].a, MATH_BENCH_INPUT_SCALE);
        f->vs[j].b = vec3s_from_vec3f(f->vf[j].b, MATH_BENCH_INPUT_SCALE);
        f->vx[j].a = vec3x_from_vec3f(f->vf[j].a);
        f->vx[j].b = vec3x_from_vec3f(f->vf[j].b);
        f->angle[j] = (turn - 0.5F) * MATH_TAU;
        f->saturate[j] = saturate_inputs[j % (sizeof saturate_inputs / sizeof saturate_inputs[0])];
        f->phase[j] = (uint16_t)(j * TRIG_TURN / MATH_BENCH_BATCH);
        f->qf[j].a = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, f->angle[j]);
        f->qf[j].b = quatf_from_axis_angle((vec3f_t){1.0F, 0.0F, 0.0F}, f->angle[j] + (MATH_PI / 2.0F));
        f->qx[j].a = quatx_from_axis_angle((vec3x_t){0, MATHX_ONE, 0}, mathf_to_x(turn - 0.5F));
        f->qx[j].b = quatx_from_axis_angle((vec3x_t){MATHX_ONE, 0, 0}, mathf_to_x(turn - 0.25F));
        f->tf[j] = (transformf_t){.position = f->vf[j].a, .rotation = f->qf[j].a, .scale = f->vf[j].b};
        f->tx[j] = (transformx_t){.position = f->vx[j].a, .rotation = f->qx[j].a, .scale = f->vx[j].b};
        f->mf[j].a = transformf_compute_matrix(&f->tf[j]);
        f->mf[j].b = mat4f_from_trs(f->vf[j].b, f->qf[j].b, f->vf[j].a);
        f->mx[j].a = transformx_compute_matrix(&f->tx[j]);
        f->mx[j].b = mat4x_from_trs(f->vx[j].b, f->qx[j].b, f->vx[j].a);
    }
    for (int j = 0; j < MATH_BENCH_DOT_SIZE; j++) {
        f->dot_a[j] = (float)((j % MATH_BENCH_INPUT_PERIOD) + 1) * MATH_BENCH_INPUT_SCALE;
        f->dot_b[j] = 1.0F - f->dot_a[j];
    }
    return f;
}

typedef struct {
    const row_t* row;
    fixture_t* fixture;
} cycle_context_t;

static void
run_cycles(void* ctx) {
    cycle_context_t* c = ctx;
    c->row->fn(c->fixture);
}

static void
test_math_board_rows(void) {
    if (bench_is_qemu()) {
        TEST_IGNORE_MESSAGE("QEMU has no hardware cache timing or performance monitor");
    }
    bench_target_report("MATHTARGET");
    fixture_t* f = fixture();
    for (size_t j = 0; j < sizeof rows / sizeof *rows; j++) {
        const row_t* row = &rows[j];
        cycle_context_t ctx = {.row = row, .fixture = f};
        bench_report_cycles("MATHBENCH", row->name, row->type, row->ops, run_cycles, &ctx);
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
            const int64_t start = bench_now_us();
            int64_t elapsed_us;
            do {
                for (int call = 0; call < calls; call++) {
                    row->fn(f);
                }
                reps += calls;
                elapsed_us = bench_now_us() - start;
            } while (!bench_icount_long_enough(elapsed_us, 1));
            us += elapsed_us;
            bench_yield();
        }
        bench_icount_report_row(row->ratchet ? BENCH_ICOUNT_PREFIX : BENCH_ICOUNT_INFO_PREFIX, row->icount_name, us,
                                reps, MATH_BENCH_ICOUNT_BATCHES);
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

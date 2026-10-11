/*
 * vec_ops_template: component-wise arithmetic and lerp for vectors of every
 * dimension and number type. Each operation is written once so dimensions
 * share the scalar policy; see math_template.h for the macro arguments.
 */
#pragma once

#include <stdint.h>

#define MATH_VEC_EACH_2(F, ...)              F(__VA_ARGS__, x), F(__VA_ARGS__, y)
#define MATH_VEC_EACH_3(F, ...)              MATH_VEC_EACH_2(F, __VA_ARGS__), F(__VA_ARGS__, z)
#define MATH_VEC_COMPONENT2(OPS, OP, c)      OPS##_##OP(a.c, b.c)
#define MATH_VEC_COMPONENT3(OPS, OP, c)      OPS##_##OP(a.c, b.c, d.c)
#define MATH_VEC_COMPONENT_LERP(OPS, c, ...) OPS##_lerp(a.c, b.c, __VA_ARGS__)
#define MATH_VEC_LERP_EACH_2(OPS, ...)                                                                                 \
    MATH_VEC_COMPONENT_LERP(OPS, x, __VA_ARGS__), MATH_VEC_COMPONENT_LERP(OPS, y, __VA_ARGS__)
#define MATH_VEC_LERP_EACH_3(OPS, ...)                                                                                 \
    MATH_VEC_LERP_EACH_2(OPS, __VA_ARGS__), MATH_VEC_COMPONENT_LERP(OPS, z, __VA_ARGS__)

#define MATH_VEC_OP2(P, OPS, N, OP)                                                                                    \
    static inline P##_t P##_##OP(P##_t a, P##_t b) { return (P##_t){MATH_VEC_EACH_##N(MATH_VEC_COMPONENT2, OPS, OP)}; }
#define MATH_VEC_OP3(P, OPS, N, OP)                                                                                    \
    static inline P##_t P##_##OP(P##_t a, P##_t b, P##_t d) {                                                          \
        return (P##_t){MATH_VEC_EACH_##N(MATH_VEC_COMPONENT3, OPS, OP)};                                               \
    }

#define MATH_DEFINE_VEC_OPS(P, OPS, N)                                                                                 \
    /* add_c = a_c + b_c. Arguments are by value, so results may reuse either. */                                      \
    MATH_VEC_OP2(P, OPS, N, add)                                                                                       \
    /* sub_c = a_c - b_c. */                                                                                           \
    MATH_VEC_OP2(P, OPS, N, sub)                                                                                       \
    /* min_c = min(a_c, b_c). */                                                                                       \
    MATH_VEC_OP2(P, OPS, N, min)                                                                                       \
    /* max_c = max(a_c, b_c). */                                                                                       \
    MATH_VEC_OP2(P, OPS, N, max)                                                                                       \
    /* clamp_c = clamp(v_c, lo_c, hi_c). */                                                                            \
    MATH_VEC_OP3(P, OPS, N, clamp)                                                                                     \
    /* repeat_c = repeat(v_c, len_c). */                                                                               \
    MATH_VEC_OP2(P, OPS, N, repeat)                                                                                    \
    /* wrap_c = wrap(v_c, lo_c, hi_c). */                                                                              \
    MATH_VEC_OP3(P, OPS, N, wrap)

#define MATH_DEFINE_VEC_LERP(P, T, OPS, N)                                                                             \
    /* lerp_c = a_c + (b_c - a_c) * t. */                                                                              \
    static inline P##_t P##_lerp(P##_t a, P##_t b, T t) { return (P##_t){MATH_VEC_LERP_EACH_##N(OPS, t)}; }
#define MATH_DEFINE_VEC_LERP_RATIO(P, OPS, N)                                                                          \
    /* lerp_c = a_c + (b_c - a_c) * t / n; truncates toward a. */                                                      \
    static inline P##_t P##_lerp(P##_t a, P##_t b, int32_t t, int32_t n) {                                             \
        return (P##_t){MATH_VEC_LERP_EACH_##N(OPS, t, n)};                                                             \
    }

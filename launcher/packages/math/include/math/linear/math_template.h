/*
 * math_template: how math/linear/ writes one algorithm once for several number
 * types. A family is a header holding one macro per group of functions
 * (vec3_template.h's MATH_DEFINE_VEC3, quat_template.h's MATH_DEFINE_QUAT,
 * and mat4_ and transform_template.h likewise). An instantiation header
 * (vec3f.h, quatx.h, ...) is the macro's arguments and nothing else:
 *
 *   P    the type being made, e.g. vec3f (its functions are vec3f_add, ...)
 *   V, Q, M   the vector, quaternion and matrix types a family builds on
 *   T    the scalar (float, int32_t, int16_t)
 *   W    the type a dot product sums in (wider than T for an integer)
 *   OPS  the scalar operations' prefix (mathf, mathi, maths, mathx): the
 *        static inline functions OPS_add, OPS_mul, OPS_one and the rest in
 *        mathf.h, mathi.h, maths.h and mathx.h. A family names only the ones
 *        it uses, so a type without OPS_div or OPS_sqrt gets no normalize.
 *   D, S, N   a swizzle's result and source vector types, and the source's
 *        component count (vec_swizzle_template.h)
 *
 * A reader looks at the instantiation's plain names (vec3f_add, quatx_mul)
 * and the tables in docs/math/README.md, and can ignore the macros.
 * Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#define MATH_DEFINE_VECTOR_BINARY(P, OPS, OP, APPLY)                                                                   \
    static inline P##_t P##_##OP(P##_t a, P##_t b) { return (P##_t){APPLY(OPS##_##OP, a, b)}; }

#define MATH_DEFINE_VECTOR_TERNARY(P, OPS, OP, APPLY)                                                                  \
    static inline P##_t P##_##OP(P##_t v, P##_t lo, P##_t hi) { return (P##_t){APPLY(OPS##_##OP, v, lo, hi)}; }

#define MATH_DEFINE_VECTOR_RANGES(P, OPS, BINARY, TERNARY)                                                             \
    /* min/max = (op(ax, bx), ...); clamp/wrap = (op(vx, lox, hix), ...). */                                           \
    MATH_DEFINE_VECTOR_BINARY(P, OPS, min, BINARY)                                                                     \
    MATH_DEFINE_VECTOR_BINARY(P, OPS, max, BINARY)                                                                     \
    MATH_DEFINE_VECTOR_TERNARY(P, OPS, clamp, TERNARY)                                                                 \
    /* repeat = (repeat(vx, lenx), ...). */                                                                            \
    MATH_DEFINE_VECTOR_BINARY(P, OPS, repeat, BINARY)                                                                  \
    MATH_DEFINE_VECTOR_TERNARY(P, OPS, wrap, TERNARY)

#define MATH_DEFINE_VECTOR_LERP(P, T, OPS, COMPONENTS)                                                                 \
    /* lerp = a + (b - a) * t, per component. */                                                                       \
    static inline P##_t P##_lerp(P##_t a, P##_t b, T t) { return (P##_t){COMPONENTS(OPS, a, b, t)}; }

#define MATH_DEFINE_VECTOR_LERP_RATIO(P, OPS, COMPONENTS)                                                              \
    /* lerp = a + (b - a) * t / n; truncates toward a, per component. */                                               \
    static inline P##_t P##_lerp(P##_t a, P##_t b, int32_t t, int32_t n) {                                             \
        return (P##_t){COMPONENTS(OPS, a, b, t, n)};                                                                   \
    }

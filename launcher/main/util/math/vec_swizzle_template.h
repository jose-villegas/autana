/*
 * vec_swizzle_template: swizzles, a new vector made of named components of
 * another (vec3f_zyx, vec2f_yx), for every number type. C has no member
 * swizzle syntax, so each is a function named by its component order that
 * takes the vector by value; it is a pure move, with no arithmetic.
 *
 * A same-dimension family is part of vec2_template.h and vec3_template.h.
 * The two families that change dimension need both vector types of a number
 * type, so a vec3 header instantiates MATH_DEFINE_VEC_SWIZZLE after
 * including its vec2; that also gives it P_from_xy.
 *
 * The 48 names of a number type come from nested iterators over the
 * component letters, not a generator: about twenty lines of macros against a
 * script and a checked-in file to keep in step. A macro does not expand
 * inside its own expansion, so each nesting depth has its own iterator.
 */
#pragma once

/* Apply a macro to each of 2 component letters at nesting depth 1. */
#define MATH_SWIZZLE_1OF2(F, ...)              F(__VA_ARGS__, x) F(__VA_ARGS__, y)
/* Apply a macro to each of 3 component letters at nesting depth 1. */
#define MATH_SWIZZLE_1OF3(F, ...)              F(__VA_ARGS__, x) F(__VA_ARGS__, y) F(__VA_ARGS__, z)
/* Apply a macro to each of 2 component letters at nesting depth 2. */
#define MATH_SWIZZLE_2OF2(F, ...)              F(__VA_ARGS__, x) F(__VA_ARGS__, y)
/* Apply a macro to each of 3 component letters at nesting depth 2. */
#define MATH_SWIZZLE_2OF3(F, ...)              F(__VA_ARGS__, x) F(__VA_ARGS__, y) F(__VA_ARGS__, z)
/* Apply a macro to each of 2 component letters at nesting depth 3. */
#define MATH_SWIZZLE_3OF2(F, ...)              F(__VA_ARGS__, x) F(__VA_ARGS__, y)
/* Apply a macro to each of 3 component letters at nesting depth 3. */
#define MATH_SWIZZLE_3OF3(F, ...)              F(__VA_ARGS__, x) F(__VA_ARGS__, y) F(__VA_ARGS__, z)

/* F(D, S, a, b) for every pair of letters from the first N of x, y, z. */
#define MATH_SWIZZLE_EACH2(N, F, D, S)         MATH_SWIZZLE_1OF##N(MATH_SWIZZLE_EACH2_B, N, F, D, S)
/* Expand the second component of every two-letter swizzle. */
#define MATH_SWIZZLE_EACH2_B(N, F, D, S, a)    MATH_SWIZZLE_2OF##N(F, D, S, a)

/* F(D, S, a, b, c) for every triple of letters from the first N of x, y, z. */
#define MATH_SWIZZLE_EACH3(N, F, D, S)         MATH_SWIZZLE_1OF##N(MATH_SWIZZLE_EACH3_B, N, F, D, S)
/* Expand the second component of every three-letter swizzle. */
#define MATH_SWIZZLE_EACH3_B(N, F, D, S, a)    MATH_SWIZZLE_2OF##N(MATH_SWIZZLE_EACH3_C, N, F, D, S, a)
/* Expand the third component of every three-letter swizzle. */
#define MATH_SWIZZLE_EACH3_C(N, F, D, S, a, b) MATH_SWIZZLE_3OF##N(F, D, S, a, b)

/* S_ab(v) = (v.a, v.b), a D. */
#define MATH_SWIZZLE2(D, S, a, b)                                                                                      \
    static inline D##_t S##_##a##b(S##_t v) { return (D##_t){v.a, v.b}; }

/* S_abc(v) = (v.a, v.b, v.c), a D. */
#define MATH_SWIZZLE3(D, S, a, b, c)                                                                                   \
    static inline D##_t S##_##a##b##c(S##_t v) { return (D##_t){v.a, v.b, v.c}; }

/* Every two-letter swizzle of the N-component S, each returning a D. */
#define MATH_DEFINE_SWIZZLE2(D, S, N) MATH_SWIZZLE_EACH2(N, MATH_SWIZZLE2, D, S)

/* Every three-letter swizzle of the N-component S, each returning a D. */
#define MATH_DEFINE_SWIZZLE3(D, S, N) MATH_SWIZZLE_EACH3(N, MATH_SWIZZLE3, D, S)

/* P is a vec3 type, V the vec2 of the same number type and T its scalar. */
#define MATH_DEFINE_VEC_SWIZZLE(P, V, T)                                                                               \
    /* P_c1c2(v) = (v.c1, v.c2), a V, each c one of x, y, z. */                                                        \
    MATH_DEFINE_SWIZZLE2(V, P, 3)                                                                                      \
    /* V_c1c2c3(v) = (v.c1, v.c2, v.c3), a P, each c one of x, y. */                                                   \
    MATH_DEFINE_SWIZZLE3(P, V, 2)                                                                                      \
                                                                                                                       \
    /* from_xy = (xy.x, xy.y, z) */                                                                                    \
    static inline P##_t P##_from_xy(V##_t xy, T z) { return (P##_t){xy.x, xy.y, z}; }

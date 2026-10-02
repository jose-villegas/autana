/*
 * math_template: how util/math/ writes one algorithm once for several number
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
 *
 * A reader looks at the instantiation's plain names (vec3f_add, quatx_mul)
 * and the tables in docs/math/README.md, and can ignore the macros.
 * Header-only, static inline and ESP-IDF-free.
 */
#pragma once

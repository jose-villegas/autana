/* math_text: canonical, allocation-free text for math values. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "math/geometry/rectf.h"
#include "math/geometry/recti.h"
#include "math/geometry/rects.h"
#include "math/geometry/rectx.h"
#include "math/linear/mat3f.h"
#include "math/linear/mat4f.h"
#include "math/linear/mat4x.h"
#include "math/linear/quatf.h"
#include "math/linear/quatx.h"
#include "math/linear/vec2f.h"
#include "math/linear/vec2i.h"
#include "math/linear/vec2s.h"
#include "math/linear/vec2x.h"
#include "math/linear/vec3f.h"
#include "math/linear/vec3i.h"
#include "math/linear/vec3s.h"
#include "math/linear/vec3x.h"

enum {
    MATH_TEXT_RADIX = 10,
    MATH_TEXT_HEX_RADIX = 16,
    MATH_TEXT_SIGN = 1,
    MATH_TEXT_NUL = 1,
    MATH_TEXT_POINT = 1,
    MATHI_TEXT_DIGITS = 10,
    MATHS_TEXT_DIGITS = 5,
    MATHF_TEXT_PRECISION = 9,
    MATHF_TEXT_EXPONENT = 4,
    MATHX_TEXT_FRACTION = 5,
    MATHU_TEXT_DIGITS = 10,
    MATHU_TEXT_MAX = MATHU_TEXT_DIGITS + MATH_TEXT_NUL,
    MATHI_TEXT_MAX = MATH_TEXT_SIGN + MATHI_TEXT_DIGITS + MATH_TEXT_NUL,
    MATHS_TEXT_MAX = MATH_TEXT_SIGN + MATHS_TEXT_DIGITS + MATH_TEXT_NUL,
    MATHF_TEXT_MAX = MATH_TEXT_SIGN + MATHF_TEXT_PRECISION + MATH_TEXT_POINT + MATHF_TEXT_EXPONENT + MATH_TEXT_NUL,
    MATHX_TEXT_MAX = MATH_TEXT_SIGN + MATHS_TEXT_DIGITS + MATH_TEXT_POINT + MATHX_TEXT_FRACTION + MATH_TEXT_NUL,
    MATH_TEXT_RECT_OFFSETS = 2, /* the two offset signs of WxH+X+Y */
};

/* A composite holds each component's scalar text; its NUL slots pay for the separators. */
enum {
    VEC2F_TEXT_MAX = 2 * MATHF_TEXT_MAX,
    VEC3F_TEXT_MAX = 3 * MATHF_TEXT_MAX,
    RECTF_TEXT_MAX = (4 * MATHF_TEXT_MAX) + MATH_TEXT_RECT_OFFSETS,
    VEC2I_TEXT_MAX = 2 * MATHI_TEXT_MAX,
    VEC3I_TEXT_MAX = 3 * MATHI_TEXT_MAX,
    RECTI_TEXT_MAX = (4 * MATHI_TEXT_MAX) + MATH_TEXT_RECT_OFFSETS,
    VEC2S_TEXT_MAX = 2 * MATHS_TEXT_MAX,
    VEC3S_TEXT_MAX = 3 * MATHS_TEXT_MAX,
    RECTS_TEXT_MAX = (4 * MATHS_TEXT_MAX) + MATH_TEXT_RECT_OFFSETS,
    VEC2X_TEXT_MAX = 2 * MATHX_TEXT_MAX,
    VEC3X_TEXT_MAX = 3 * MATHX_TEXT_MAX,
    RECTX_TEXT_MAX = (4 * MATHX_TEXT_MAX) + MATH_TEXT_RECT_OFFSETS,
    QUATF_TEXT_MAX = 4 * MATHF_TEXT_MAX,
    QUATX_TEXT_MAX = 4 * MATHX_TEXT_MAX,
    MAT3F_TEXT_MAX = MAT3F_AXES * MAT3F_AXES * MATHF_TEXT_MAX,
    MAT4F_TEXT_MAX = 4 * 4 * MATHF_TEXT_MAX,
    MAT4X_TEXT_MAX = 4 * 4 * MATHX_TEXT_MAX
};

/* Format returns the required length excluding NUL and terminates buf when cap > 0.
 * Each *_TEXT_MAX includes NUL and guarantees no truncation.
 * Parse leaves out and end untouched on failure. NULL end requires the whole
 * string; otherwise *end receives the position just after the value. */
#include "math/text/text_template.h"
/* mathu: an unsigned 32-bit count, decimal without a sign. mathu_parse_hex reads
 * 0x-prefixed hexadecimal; both take the same end contract. */
MATH_DECLARE_TEXT(mathu, uint32_t)
MATH_DEFINE_TEXT_VALUE(mathu, MATHU_TEXT_MAX, uint32_t)
bool mathu_parse_hex(const char* s, const char** end, uint32_t* out);
MATH_DEFINE_TEXT_FAMILY(float, mathf, MATHF_TEXT_MAX, vec2f, VEC2F_TEXT_MAX, vec3f, VEC3F_TEXT_MAX, rectf,
                        RECTF_TEXT_MAX)
MATH_DEFINE_TEXT_FAMILY(int32_t, mathi, MATHI_TEXT_MAX, vec2i, VEC2I_TEXT_MAX, vec3i, VEC3I_TEXT_MAX, recti,
                        RECTI_TEXT_MAX)
MATH_DEFINE_TEXT_FAMILY(int16_t, maths, MATHS_TEXT_MAX, vec2s, VEC2S_TEXT_MAX, vec3s, VEC3S_TEXT_MAX, rects,
                        RECTS_TEXT_MAX)
MATH_DEFINE_TEXT_FAMILY(int32_t, mathx, MATHX_TEXT_MAX, vec2x, VEC2X_TEXT_MAX, vec3x, VEC3X_TEXT_MAX, rectx,
                        RECTX_TEXT_MAX)
MATH_DEFINE_VECTOR_TEXT(quatf, QUATF_TEXT_MAX, float, mathf, MATHF_TEXT_MAX, 4, MATH_TEXT_EACH_4)
MATH_DEFINE_VECTOR_TEXT(quatx, QUATX_TEXT_MAX, int32_t, mathx, MATHX_TEXT_MAX, 4, MATH_TEXT_EACH_4)
MATH_DEFINE_MATRIX_TEXT(mat3f, MAT3F_TEXT_MAX, float, mathf, MATHF_TEXT_MAX, MAT3F_AXES)
MATH_DEFINE_MATRIX_TEXT(mat4f, MAT4F_TEXT_MAX, float, mathf, MATHF_TEXT_MAX, 4)
MATH_DEFINE_MATRIX_TEXT(mat4x, MAT4X_TEXT_MAX, int32_t, mathx, MATHX_TEXT_MAX, 4)

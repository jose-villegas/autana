/* text_template: canonical component and geometry text over scalar OPS. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "math/linear/vec_ops_template.h"

static inline int
math_text_copy(char* buf, size_t cap, const char* text, int length) {
    if (cap != 0) {
        size_t count = (size_t)length < cap ? (size_t)length : cap - 1;
        memcpy(buf, text, count);
        buf[count] = '\0';
    }
    return length;
}

#define MATH_DEFINE_TEXT_VALUE(P, MAX, T)                                                                              \
    typedef struct {                                                                                                   \
        char chars[MAX];                                                                                               \
    } P##_text_t;                                                                                                      \
    static inline P##_text_t P##_text(T v) {                                                                           \
        P##_text_t text;                                                                                               \
        P##_format(text.chars, sizeof text.chars, v);                                                                  \
        return text;                                                                                                   \
    }

#define MATH_DEFINE_TEXT(P, MAX, T, OPS, SCALAR_MAX, N, COLS, PACK, UNPACK)                                            \
    enum { MAX = (N) * SCALAR_MAX };                                                                                   \
    static inline int P##_format(char* buf, size_t cap, P##_t v) {                                                     \
        T a[N];                                                                                                        \
        PACK;                                                                                                          \
        char text[MAX];                                                                                                \
        int length = 0;                                                                                                \
        for (int i = 0; i < (N); ++i) {                                                                                \
            if (i != 0) {                                                                                              \
                text[length++] = i % (COLS) == 0 ? ';' : ',';                                                          \
            }                                                                                                          \
            length += OPS##_format(text + length, sizeof text - (size_t)length, a[i]);                                 \
        }                                                                                                              \
        return math_text_copy(buf, cap, text, length);                                                                 \
    }                                                                                                                  \
    MATH_DEFINE_TEXT_VALUE(P, MAX, P##_t)                                                                              \
    static inline bool P##_parse(const char* s, const char** end, P##_t* out) {                                        \
        T a[N];                                                                                                        \
        const char* cursor = s;                                                                                        \
        for (int i = 0; i < (N); ++i) {                                                                                \
            if (i != 0 && *cursor++ != (i % (COLS) == 0 ? ';' : ',')) {                                                \
                return false;                                                                                          \
            }                                                                                                          \
            if (!OPS##_parse(cursor, &cursor, &a[i])) {                                                                \
                return false;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
        if (end == NULL && *cursor != '\0') {                                                                          \
            return false;                                                                                              \
        }                                                                                                              \
        P##_t v;                                                                                                       \
        UNPACK;                                                                                                        \
        *out = v;                                                                                                      \
        if (end != NULL) {                                                                                             \
            *end = cursor;                                                                                             \
        }                                                                                                              \
        return true;                                                                                                   \
    }

#define MATH_DEFINE_RECT_TEXT(P, MAX, T, OPS, SCALAR_MAX)                                                              \
    enum { MAX = 4 * SCALAR_MAX + 2 };                                                                                 \
    static inline int P##_format(char* buf, size_t cap, P##_t v) {                                                     \
        T a[] = {v.w, v.h, v.x, v.y};                                                                                  \
        char text[MAX];                                                                                                \
        int length = 0;                                                                                                \
        for (int i = 0; i < 4; ++i) {                                                                                  \
            char scalar[SCALAR_MAX];                                                                                   \
            int count = OPS##_format(scalar, sizeof scalar, a[i]);                                                     \
            if (i == 1) {                                                                                              \
                text[length++] = 'x';                                                                                  \
            }                                                                                                          \
            if (i >= 2 && scalar[0] != '-') {                                                                          \
                text[length++] = '+';                                                                                  \
            }                                                                                                          \
            memcpy(text + length, scalar, (size_t)count + 1);                                                          \
            length += count;                                                                                           \
        }                                                                                                              \
        return math_text_copy(buf, cap, text, length);                                                                 \
    }                                                                                                                  \
    MATH_DEFINE_TEXT_VALUE(P, MAX, P##_t)                                                                              \
    static inline bool P##_parse(const char* s, const char** end, P##_t* out) {                                        \
        T x = 0, y = 0, w, h;                                                                                          \
        const char* cursor;                                                                                            \
        if (!OPS##_parse(s, &cursor, &w) || *cursor != 'x' || !OPS##_parse(cursor + 1, &cursor, &h)) {                 \
            return false;                                                                                              \
        }                                                                                                              \
        if (*cursor == '+' || *cursor == '-') {                                                                        \
            if (!OPS##_parse(cursor, &cursor, &x) || (*cursor != '+' && *cursor != '-')                                \
                || !OPS##_parse(cursor, &cursor, &y)) {                                                                \
                return false;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
        if (end == NULL && *cursor != '\0') {                                                                          \
            return false;                                                                                              \
        }                                                                                                              \
        *out = (P##_t){x, y, w, h};                                                                                    \
        if (end != NULL) {                                                                                             \
            *end = cursor;                                                                                             \
        }                                                                                                              \
        return true;                                                                                                   \
    }

#define MATH_DEFINE_MATRIX_TEXT(P, MAX, T, OPS, SCALAR_MAX, AXES)                                                      \
    MATH_DEFINE_TEXT(                                                                                                  \
        P, MAX, T, OPS, SCALAR_MAX, (AXES) * (AXES), AXES,                                                             \
        for (int j = 0; j < (AXES) * (AXES); ++j) { a[j] = v.m[j / (AXES)][j % (AXES)]; },                             \
        for (int j = 0; j < (AXES) * (AXES); ++j) { v.m[j / (AXES)][j % (AXES)] = a[j]; })

#define MATH_DECLARE_TEXT(P, T)                                                                                        \
    int P##_format(char* buf, size_t cap, T v);                                                                        \
    bool P##_parse(const char* s, const char** end, T* out);

#define MATH_TEXT_PACK(C, V)   a[component++] = V.C
#define MATH_TEXT_UNPACK(C, V) V.C = a[component++]

#define MATH_DEFINE_VECTOR_TEXT(P, MAX, T, OPS, SCALAR_MAX, N, EACH)                                                   \
    MATH_DEFINE_TEXT(P, MAX, T, OPS, SCALAR_MAX, N, N, int component = 0; EACH(MATH_TEXT_PACK, v), int component = 0;  \
                     EACH(MATH_TEXT_UNPACK, v))

#define MATH_TEXT_EACH_4(F, ...) MATH_VEC_EACH_3(F, __VA_ARGS__), F(w, __VA_ARGS__)

#define MATH_DEFINE_TEXT_FAMILY(T, OPS, SCALAR_MAX, V2, V2_MAX, V3, V3_MAX, R, R_MAX)                                  \
    MATH_DECLARE_TEXT(OPS, T)                                                                                          \
    MATH_DEFINE_TEXT_VALUE(OPS, SCALAR_MAX, T)                                                                         \
    MATH_DEFINE_VECTOR_TEXT(V2, V2_MAX, T, OPS, SCALAR_MAX, 2, MATH_VEC_EACH_2)                                        \
    MATH_DEFINE_VECTOR_TEXT(V3, V3_MAX, T, OPS, SCALAR_MAX, 3, MATH_VEC_EACH_3)                                        \
    MATH_DEFINE_RECT_TEXT(R, R_MAX, T, OPS, SCALAR_MAX)

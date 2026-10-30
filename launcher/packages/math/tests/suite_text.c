#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "math/scalar/rng.h"
#include "math/text/math_text.h"
#include "testkit/suite.h"
#include "unity.h"

enum { TEXT_RANDOM_SAMPLES = 20000 };

#define TEXT_RANDOM_SEED UINT32_C(1013904223)

static void
test_scalar_round_trips(void) {
    char* text = malloc(MATHX_TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
    for (int32_t i = INT16_MIN; i <= INT16_MAX; ++i) {
        int16_t got;
        TEST_ASSERT_LESS_THAN(MATHS_TEXT_MAX, maths_format(text, MATHS_TEXT_MAX, (int16_t)i));
        TEST_ASSERT_TRUE(maths_parse(text, NULL, &got));
        TEST_ASSERT_EQUAL_INT(i, got);
    }
    static const int32_t parts[] = {INT16_MIN, -1, 0, 1, INT16_MAX};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i) {
        for (int32_t fraction = 0; fraction < MATHX_ONE; ++fraction) {
            int32_t raw = (int32_t)((int64_t)parts[i] * MATHX_ONE + fraction), got;
            TEST_ASSERT_LESS_THAN(MATHX_TEXT_MAX, mathx_format(text, MATHX_TEXT_MAX, raw));
            TEST_ASSERT_TRUE(mathx_parse(text, NULL, &got));
            TEST_ASSERT_EQUAL_INT32(raw, got);
        }
    }
    free(text);
}

static void
check_float(float value, char* text, char* shorter) {
    float got;
    TEST_ASSERT_LESS_THAN(MATHF_TEXT_MAX, mathf_format(text, MATHF_TEXT_MAX, value));
    TEST_ASSERT_TRUE(mathf_parse(text, NULL, &got));
    if (isnan(value)) {
        TEST_ASSERT_TRUE(isnan(got));
        return;
    }
    TEST_ASSERT_EQUAL_MEMORY(&value, &got, sizeof value);
    for (int precision = 1; precision < MATHF_TEXT_PRECISION; ++precision) {
        snprintf(shorter, MATHF_TEXT_MAX, "%.*g", precision, (double)value);
        float parsed = strtof(shorter, NULL);
        if (strcmp(shorter, text) == 0) {
            break;
        }
        TEST_ASSERT_TRUE(memcmp(&value, &parsed, sizeof value) != 0);
    }
}

static void
test_float_round_trips(void) {
    char* text = malloc(2 * MATHF_TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
    static const float values[] = {0.0F,    -0.0F,    FLT_TRUE_MIN, FLT_MIN - FLT_TRUE_MIN,
                                   FLT_MAX, -FLT_MAX, INFINITY,     -INFINITY,
                                   NAN,     0.1F,     1.0F / 3.0F};
    for (size_t i = 0; i < sizeof values / sizeof *values; ++i) {
        check_float(values[i], text, text + MATHF_TEXT_MAX);
    }
    static const uint32_t seeds[] = {TEXT_RANDOM_SEED, ~TEXT_RANDOM_SEED, 0};
    for (size_t seed = 0; seed < sizeof seeds / sizeof *seeds; ++seed) {
        rng_t random;
        rng_seed(&random, seeds[seed]);
        for (int i = 0; i < TEXT_RANDOM_SAMPLES; ++i) {
            uint32_t bits = rng_next(&random);
            float value;
            memcpy(&value, &bits, sizeof value);
            check_float(value, text, text + MATHF_TEXT_MAX);
        }
    }
    free(text);
}

/* Keep returned text temporaries out of the aggregate test's device stack frame. */
#define DEFINE_TEXT_CHECK(P, T)                                                                                        \
    static __attribute__((noinline)) void check_##P##_text(T value, const char* expected) {                            \
        TEST_ASSERT_EQUAL_STRING(expected, P##_text(value).chars);                                                     \
    }

#define DEFINE_TEXT_FAMILY_CHECK(S)                                                                                    \
    DEFINE_TEXT_CHECK(vec2##S, vec2##S##_t)                                                                            \
    DEFINE_TEXT_CHECK(vec3##S, vec3##S##_t)                                                                            \
    DEFINE_TEXT_CHECK(rect##S, rect##S##_t)

DEFINE_TEXT_CHECK(mathf, float)
DEFINE_TEXT_CHECK(mathi, int32_t)
DEFINE_TEXT_CHECK(maths, int16_t)
DEFINE_TEXT_CHECK(mathx, int32_t)
DEFINE_TEXT_FAMILY_CHECK(f)
DEFINE_TEXT_FAMILY_CHECK(i)
DEFINE_TEXT_FAMILY_CHECK(s)
DEFINE_TEXT_FAMILY_CHECK(x)
DEFINE_TEXT_CHECK(quatf, quatf_t)
DEFINE_TEXT_CHECK(quatx, quatx_t)
DEFINE_TEXT_CHECK(mat3f, mat3f_t)
DEFINE_TEXT_CHECK(mat4f, mat4f_t)
DEFINE_TEXT_CHECK(mat4x, mat4x_t)

#define CHECK_TEXT(P, MAX, T, ...)                                                                                     \
    do {                                                                                                               \
        T value = __VA_ARGS__, parsed;                                                                                 \
        int length = P##_format(text, MAX, value);                                                                     \
        TEST_ASSERT_LESS_THAN(MAX, length);                                                                            \
        check_##P##_text(value, text);                                                                                 \
        TEST_ASSERT_TRUE(P##_parse(text, NULL, &parsed));                                                              \
        TEST_ASSERT_EQUAL_MEMORY(&value, &parsed, sizeof value);                                                       \
        for (size_t cap = 1; cap <= (size_t)length; ++cap) {                                                           \
            TEST_ASSERT_EQUAL_INT(length, P##_format(short_text, cap, value));                                         \
            TEST_ASSERT_EQUAL_INT(0, short_text[cap - 1]);                                                             \
            if (cap > 1) {                                                                                             \
                TEST_ASSERT_EQUAL_MEMORY(text, short_text, cap - 1);                                                   \
            }                                                                                                          \
        }                                                                                                              \
        TEST_ASSERT_EQUAL_INT(length, P##_format(NULL, 0, value));                                                     \
        T saved = parsed;                                                                                              \
        TEST_ASSERT_FALSE(P##_parse("", NULL, &parsed));                                                               \
        TEST_ASSERT_EQUAL_MEMORY(&saved, &parsed, sizeof saved);                                                       \
        text[length] = ' ';                                                                                            \
        text[length + 1] = '\0';                                                                                       \
        TEST_ASSERT_FALSE(P##_parse(text, NULL, &parsed));                                                             \
        TEST_ASSERT_EQUAL_MEMORY(&saved, &parsed, sizeof saved);                                                       \
        const char* end;                                                                                               \
        TEST_ASSERT_TRUE(P##_parse(text, &end, &parsed));                                                              \
        TEST_ASSERT_EQUAL_STRING(" ", end);                                                                            \
    } while (0)

#define CHECK_TEXT_CASES(P, MAX, T, ...)                                                                               \
    do {                                                                                                               \
        static const T cases[] = {__VA_ARGS__};                                                                        \
        for (size_t sample = 0; sample < sizeof cases / sizeof *cases; ++sample) {                                     \
            CHECK_TEXT(P, MAX, T, cases[sample]);                                                                      \
        }                                                                                                              \
    } while (0)

#define CHECK_TEXT_FAMILY(S, U, LOW, HIGH)                                                                             \
    do {                                                                                                               \
        CHECK_TEXT_CASES(vec2##S, VEC2##U##_TEXT_MAX, vec2##S##_t, ((vec2##S##_t){0}), ((vec2##S##_t){LOW, HIGH}),     \
                         ((vec2##S##_t){1, -2}));                                                                      \
        CHECK_TEXT_CASES(vec3##S, VEC3##U##_TEXT_MAX, vec3##S##_t, ((vec3##S##_t){0}), ((vec3##S##_t){LOW, HIGH, 1}),  \
                         ((vec3##S##_t){1, -2, 3}));                                                                   \
        CHECK_TEXT_CASES(rect##S, RECT##U##_TEXT_MAX, rect##S##_t, ((rect##S##_t){0}),                                 \
                         ((rect##S##_t){LOW, HIGH, HIGH, LOW}), ((rect##S##_t){1, -2, 3, 4}));                         \
    } while (0)

static void
test_all_types(void) {
    char* text = malloc(MAT4F_TEXT_MAX + sizeof " ");
    char* short_text = malloc(MAT4F_TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_NOT_NULL(short_text);
    CHECK_TEXT_CASES(mathf, MATHF_TEXT_MAX, float, 0.0F, -0.0F, -FLT_MAX, FLT_MAX);
    CHECK_TEXT_CASES(mathi, MATHI_TEXT_MAX, int32_t, 0, INT32_MIN, INT32_MAX);
    CHECK_TEXT_CASES(maths, MATHS_TEXT_MAX, int16_t, 0, INT16_MIN, INT16_MAX);
    CHECK_TEXT_CASES(mathx, MATHX_TEXT_MAX, int32_t, 0, INT32_MIN, INT32_MAX);
    CHECK_TEXT_FAMILY(f, F, -FLT_MAX, FLT_MAX);
    CHECK_TEXT_FAMILY(i, I, INT32_MIN, INT32_MAX);
    CHECK_TEXT_FAMILY(s, S, INT16_MIN, INT16_MAX);
    CHECK_TEXT_FAMILY(x, X, INT32_MIN, INT32_MAX);
    CHECK_TEXT_CASES(quatf, QUATF_TEXT_MAX, quatf_t, ((quatf_t){0}), ((quatf_t){0, 0, 0, 1}),
                     ((quatf_t){-FLT_MAX, FLT_MAX, 1, -2}));
    CHECK_TEXT_CASES(quatx, QUATX_TEXT_MAX, quatx_t, ((quatx_t){0}), ((quatx_t){0, 0, 0, 1}),
                     ((quatx_t){INT32_MIN, INT32_MAX, 1, -2}));
    CHECK_TEXT_CASES(mat3f, MAT3F_TEXT_MAX, mat3f_t, ((mat3f_t){0}), ((mat3f_t){{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}),
                     ((mat3f_t){{{-FLT_MAX, FLT_MAX, 1}}}));
    CHECK_TEXT_CASES(mat4f, MAT4F_TEXT_MAX, mat4f_t, ((mat4f_t){0}),
                     ((mat4f_t){{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}),
                     ((mat4f_t){{{-FLT_MAX, FLT_MAX, 1}}}));
    CHECK_TEXT_CASES(mat4x, MAT4X_TEXT_MAX, mat4x_t, ((mat4x_t){0}),
                     ((mat4x_t){{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}),
                     ((mat4x_t){{{INT32_MIN, INT32_MAX, 1}}}));
    free(short_text);
    free(text);
}

static void
test_unsigned_text(void) {
    char text[MATHU_TEXT_MAX];
    static const uint32_t samples[] = {0, 1, UINT8_MAX, UINT16_MAX, UINT32_MAX};
    for (size_t i = 0; i < sizeof samples / sizeof *samples; ++i) {
        uint32_t got = 7;
        TEST_ASSERT_LESS_THAN(MATHU_TEXT_MAX, mathu_format(text, sizeof text, samples[i]));
        TEST_ASSERT_TRUE(mathu_parse(text, NULL, &got));
        TEST_ASSERT_EQUAL_UINT32(samples[i], got);
    }
    static const char* const bad[] = {"", " 1", "1 ", "-1", "+1", "4294967296", "99999999999999999999", "1x", "0x10"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        uint32_t got = 7;
        TEST_ASSERT_FALSE(mathu_parse(bad[i], NULL, &got));
        TEST_ASSERT_EQUAL_UINT32(7, got);
    }
    uint32_t got = 7;
    const char* end = NULL;
    TEST_ASSERT_TRUE(mathu_parse("12 34", &end, &got));
    TEST_ASSERT_EQUAL_UINT32(12, got);
    TEST_ASSERT_EQUAL_STRING(" 34", end);
    TEST_ASSERT_TRUE(mathu_parse_hex("0xFf", NULL, &got));
    TEST_ASSERT_EQUAL_UINT32(255, got);
    TEST_ASSERT_TRUE(mathu_parse_hex("0XFFFFFFFF", NULL, &got));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, got);
    static const char* const bad_hex[] = {"",     "10",   "0x",   "0xg",   "0x100000000",
                                          "-0x1", "0x1 ", " 0x1", "0x0x5", "0x0X1"};
    for (size_t i = 0; i < sizeof bad_hex / sizeof *bad_hex; ++i) {
        got = 7;
        TEST_ASSERT_FALSE(mathu_parse_hex(bad_hex[i], NULL, &got));
        TEST_ASSERT_EQUAL_UINT32(7, got);
    }
    TEST_ASSERT_TRUE(mathu_parse_hex("0x1Z", &end, &got));
    TEST_ASSERT_EQUAL_STRING("Z", end);
}

static void
test_scalar_grammar(void) {
    static const char* const invalid[] = {"", " ", " 1", "1 ", "--1", "+", ".", "1junk"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        float f = 1;
        int32_t x = 1, integer = 1;
        int16_t small = 1;
        TEST_ASSERT_FALSE(mathf_parse(invalid[i], NULL, &f));
        TEST_ASSERT_FALSE(mathx_parse(invalid[i], NULL, &x));
        TEST_ASSERT_FALSE(mathi_parse(invalid[i], NULL, &integer));
        TEST_ASSERT_FALSE(maths_parse(invalid[i], NULL, &small));
        TEST_ASSERT_EQUAL_FLOAT(1, f);
        TEST_ASSERT_EQUAL_INT(1, x);
        TEST_ASSERT_EQUAL_INT(1, integer);
        TEST_ASSERT_EQUAL_INT(1, small);
    }
    int32_t fixed = 0;
    TEST_ASSERT_TRUE(mathx_parse("0.00000762939453125", NULL, &fixed));
    TEST_ASSERT_EQUAL_INT(1, fixed);
    TEST_ASSERT_TRUE(mathx_parse("-0.00000762939453125", NULL, &fixed));
    TEST_ASSERT_EQUAL_INT(-1, fixed);
    TEST_ASSERT_TRUE(mathx_parse("0.00000762939453124999999999999", NULL, &fixed));
    TEST_ASSERT_EQUAL_INT(0, fixed);
    TEST_ASSERT_FALSE(mathx_parse("32767.99999", NULL, &fixed));
    TEST_ASSERT_FALSE(mathx_parse("-32768.000000000001", NULL, &fixed));
    TEST_ASSERT_FALSE(mathx_parse("999999999999999999999999", NULL, &fixed));
    float f;
    TEST_ASSERT_FALSE(mathf_parse("1e999", NULL, &f));
    TEST_ASSERT_FALSE(mathf_parse("1e-999", NULL, &f));
    TEST_ASSERT_FALSE(mathf_parse("0x1p0", NULL, &f));
    rectf_t rect;
    TEST_ASSERT_TRUE(rectf_parse("0x48", NULL, &rect));
    TEST_ASSERT_EQUAL_FLOAT(0, rect.w);
    TEST_ASSERT_EQUAL_FLOAT(48, rect.h);
    recti_t r = {1, 2, 3, 4};
    static const char* const bad_rect[] = {"1", "1x", "1x2+3", "1x2+3+", "1x2+3+4+5", "1x2 3 4"};
    for (size_t i = 0; i < sizeof bad_rect / sizeof *bad_rect; ++i) {
        TEST_ASSERT_FALSE(recti_parse(bad_rect[i], NULL, &r));
        TEST_ASSERT_EQUAL_INT(1, r.x);
        TEST_ASSERT_EQUAL_INT(2, r.y);
        TEST_ASSERT_EQUAL_INT(3, r.w);
        TEST_ASSERT_EQUAL_INT(4, r.h);
    }
}

static void
test_examples_and_failure(void) {
    char* text = malloc(MAT4F_TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
    vec2i_format(text, VEC2I_TEXT_MAX, (vec2i_t){12, -3});
    TEST_ASSERT_EQUAL_STRING("12,-3", text);
    vec3f_format(text, VEC3F_TEXT_MAX, (vec3f_t){0.1F, 1, -2.5F});
    TEST_ASSERT_EQUAL_STRING("0.1,1,-2.5", text);
    quatf_format(text, QUATF_TEXT_MAX, (quatf_t){0, 0, 0, 1});
    TEST_ASSERT_EQUAL_STRING("0,0,0,1", text);
    mat3f_format(text, MAT3F_TEXT_MAX, (mat3f_t){{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}});
    TEST_ASSERT_EQUAL_STRING("1,0,0;0,1,0;0,0,1", text);
    mat3f_format(text, MAT3F_TEXT_MAX, (mat3f_t){{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}});
    TEST_ASSERT_EQUAL_STRING("1,2,3;4,5,6;7,8,9", text);
    recti_format(text, RECTI_TEXT_MAX, (recti_t){10, 20, 64, 48});
    TEST_ASSERT_EQUAL_STRING("64x48+10+20", text);
    recti_format(text, RECTI_TEXT_MAX, (recti_t){-10, 20, 64, 48});
    TEST_ASSERT_EQUAL_STRING("64x48-10+20", text);
    recti_t rect;
    TEST_ASSERT_TRUE(recti_parse("640x480", NULL, &rect));
    TEST_ASSERT_EQUAL_INT(0, rect.x);
    TEST_ASSERT_EQUAL_INT(640, rect.w);
    mathx_format(text, MATHX_TEXT_MAX, MATHX_ONE + MATHX_ONE / 2);
    TEST_ASSERT_EQUAL_STRING("1.5", text);
    mathx_format(text, MATHX_TEXT_MAX, 1);
    TEST_ASSERT_EQUAL_STRING("0.00002", text);
    static const char* const invalid[] = {"", " 1,2", "1,2 ", "1", "1,", "1,2,3", "1,2junk", "2147483648,0"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        vec2i_t out = {12, -3};
        const char* end = NULL;
        TEST_ASSERT_FALSE(vec2i_parse(invalid[i], NULL, &out));
        TEST_ASSERT_EQUAL_INT(12, out.x);
        TEST_ASSERT_EQUAL_INT(-3, out.y);
        if (i == 0 || i == 1 || i == 3 || i == 4 || i == 7) {
            TEST_ASSERT_FALSE(vec2i_parse(invalid[i], &end, &out));
            TEST_ASSERT_NULL(end);
        }
    }
    const char* end;
    vec2i_t out;
    TEST_ASSERT_TRUE(vec2i_parse("1,2tail", &end, &out));
    TEST_ASSERT_EQUAL_STRING("tail", end);
    TEST_ASSERT_EQUAL_INT(5, vec2i_format(text, 2, (vec2i_t){12, -3}));
    TEST_ASSERT_EQUAL_STRING("1", text);
    TEST_ASSERT_EQUAL_INT(5, vec2i_format(NULL, 0, (vec2i_t){12, -3}));
    int16_t small = 1;
    TEST_ASSERT_FALSE(maths_parse("32768", NULL, &small));
    TEST_ASSERT_EQUAL_INT(1, small);
    int32_t fixed = 1;
    TEST_ASSERT_FALSE(mathx_parse("32768", NULL, &fixed));
    TEST_ASSERT_EQUAL_INT(1, fixed);
    TEST_ASSERT_FALSE(mathx_parse("-32768.1", NULL, &fixed));
    free(text);
}

void
run_text_suite(void) {
    RUN_TEST(test_scalar_round_trips);
    RUN_TEST(test_float_round_trips);
    RUN_TEST(test_all_types);
    RUN_TEST(test_unsigned_text);
    RUN_TEST(test_scalar_grammar);
    RUN_TEST(test_examples_and_failure);
}

SUITE_REGISTER(run_text_suite);

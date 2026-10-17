#include "suites.h"
#include "unity.h"

#include "math/linear/affine2f.h"
#include "math/linear/affine2x.h"

enum {
    AFFINE_LEFT = -3,
    AFFINE_TOP = 5,
    AFFINE_WIDTH = 10,
    AFFINE_HEIGHT = 4,
    AFFINE_TRANSLATE_X = 7,
    AFFINE_TRANSLATE_Y = -2,
    AFFINE_SCALE = 2,
    AFFINE_QUARTERS = 4,
};

#define AFFINE_F(v) ((float)(v))
#define AFFINE_X(v) ((int32_t)(v) * MATHX_ONE)

#define DEFINE_AFFINE_TESTS(P, V, R, C, ASSERT)                                                                        \
    static void test_##P##_identity(void) {                                                                            \
        const P##_t t = P##_identity();                                                                                \
        ASSERT(C(1), t.x_axis.x);                                                                                      \
        ASSERT(C(0), t.x_axis.y);                                                                                      \
        ASSERT(C(0), t.y_axis.x);                                                                                      \
        ASSERT(C(1), t.y_axis.y);                                                                                      \
        ASSERT(C(0), t.origin.x);                                                                                      \
        ASSERT(C(0), t.origin.y);                                                                                      \
    }                                                                                                                  \
    static void test_##P##_mul_applies_b_first(void) {                                                                 \
        const P##_t a = {{C(1), C(1)}, {C(0), C(1)}, {C(AFFINE_TRANSLATE_X), C(AFFINE_TRANSLATE_Y)}};                  \
        const P##_t b = {{C(AFFINE_SCALE), C(0)}, {C(1), C(1)}, {C(AFFINE_LEFT), C(AFFINE_TOP)}};                      \
        const P##_t t = P##_mul(a, b);                                                                                 \
        ASSERT(C(AFFINE_SCALE), t.x_axis.x);                                                                           \
        ASSERT(C(AFFINE_SCALE), t.x_axis.y);                                                                           \
        ASSERT(C(1), t.y_axis.x);                                                                                      \
        ASSERT(C(2), t.y_axis.y);                                                                                      \
        ASSERT(C(AFFINE_LEFT + AFFINE_TRANSLATE_X), t.origin.x);                                                       \
        ASSERT(C(AFFINE_LEFT + AFFINE_TOP + AFFINE_TRANSLATE_Y), t.origin.y);                                          \
    }                                                                                                                  \
    static void test_##P##_inverse_and_determinant(void) {                                                             \
        const P##_t t = {{C(1), C(1)}, {C(1), C(2)}, {C(AFFINE_TRANSLATE_X), C(AFFINE_TRANSLATE_Y)}};                  \
        ASSERT(C(1), P##_determinant(t));                                                                              \
        P##_t inv;                                                                                                     \
        TEST_ASSERT_TRUE(P##_invert(t, &inv));                                                                         \
        ASSERT(C(2), inv.x_axis.x);                                                                                    \
        ASSERT(C(-1), inv.x_axis.y);                                                                                   \
        ASSERT(C(-1), inv.y_axis.x);                                                                                   \
        ASSERT(C(1), inv.y_axis.y);                                                                                    \
        ASSERT(C(AFFINE_TRANSLATE_Y - 2 * AFFINE_TRANSLATE_X), inv.origin.x);                                          \
        ASSERT(C(AFFINE_TRANSLATE_X - AFFINE_TRANSLATE_Y), inv.origin.y);                                              \
        const V##_t p = {C(AFFINE_LEFT), C(AFFINE_TOP)};                                                               \
        const V##_t back = P##_map_point(&inv, P##_map_point(&t, p));                                                  \
        ASSERT(p.x, back.x);                                                                                           \
        ASSERT(p.y, back.y);                                                                                           \
    }                                                                                                                  \
    static void test_##P##_singular_keeps_output(void) {                                                               \
        const P##_t t = {{C(1), C(1)}, {C(2), C(2)}, {C(AFFINE_LEFT), C(AFFINE_TOP)}};                                 \
        P##_t out = P##_identity();                                                                                    \
        const P##_t before = out;                                                                                      \
        TEST_ASSERT_FALSE(P##_invert(t, &out));                                                                        \
        TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);                                                           \
    }                                                                                                                  \
    static void test_##P##_point_and_vector(void) {                                                                    \
        const P##_t t = {{C(1), C(1)}, {C(0), C(2)}, {C(AFFINE_TRANSLATE_X), C(AFFINE_TRANSLATE_Y)}};                  \
        const V##_t p = {C(AFFINE_LEFT), C(AFFINE_TOP)};                                                               \
        const V##_t point = P##_map_point(&t, p), vector = P##_map_vector(&t, p);                                      \
        ASSERT(C(AFFINE_LEFT + AFFINE_TRANSLATE_X), point.x);                                                          \
        ASSERT(C(AFFINE_LEFT + 2 * AFFINE_TOP + AFFINE_TRANSLATE_Y), point.y);                                         \
        ASSERT(C(AFFINE_LEFT), vector.x);                                                                              \
        ASSERT(C(AFFINE_LEFT + 2 * AFFINE_TOP), vector.y);                                                             \
    }                                                                                                                  \
    static void test_##P##_rect_quarter_and_shear(void) {                                                              \
        const R##_t r = {C(AFFINE_LEFT), C(AFFINE_TOP), C(AFFINE_WIDTH), C(AFFINE_HEIGHT)};                            \
        const P##_t quarter = {{C(0), C(1)}, {C(-1), C(0)}, {C(0), C(0)}};                                             \
        const R##_t turned = P##_map_rect(&quarter, r);                                                                \
        ASSERT(C(-AFFINE_TOP - AFFINE_HEIGHT), turned.x);                                                              \
        ASSERT(r.x, turned.y);                                                                                         \
        ASSERT(r.h, turned.w);                                                                                         \
        ASSERT(r.w, turned.h);                                                                                         \
        const P##_t shear = {{C(1), C(0)}, {C(-1), C(1)}, {C(AFFINE_TRANSLATE_X), C(AFFINE_TRANSLATE_Y)}};             \
        const R##_t box = P##_map_rect(&shear, r);                                                                     \
        ASSERT(C(AFFINE_LEFT - AFFINE_TOP - AFFINE_HEIGHT + AFFINE_TRANSLATE_X), box.x);                               \
        ASSERT(C(AFFINE_TOP + AFFINE_TRANSLATE_Y), box.y);                                                             \
        ASSERT(C(AFFINE_WIDTH + AFFINE_HEIGHT), box.w);                                                                \
        ASSERT(r.h, box.h);                                                                                            \
        const recti_t pixels = {AFFINE_LEFT, AFFINE_TOP, AFFINE_WIDTH, AFFINE_HEIGHT};                                 \
        const recti_t mapped = P##_map_recti(&shear, pixels);                                                          \
        TEST_ASSERT_EQUAL_INT(AFFINE_LEFT - AFFINE_TOP - AFFINE_HEIGHT + AFFINE_TRANSLATE_X, mapped.x);                \
        TEST_ASSERT_EQUAL_INT(AFFINE_TOP + AFFINE_TRANSLATE_Y, mapped.y);                                              \
        TEST_ASSERT_EQUAL_INT(AFFINE_WIDTH + AFFINE_HEIGHT, mapped.w);                                                 \
        TEST_ASSERT_EQUAL_INT(AFFINE_HEIGHT, mapped.h);                                                                \
    }                                                                                                                  \
    static void test_##P##_equal_checks_every_scalar(void) {                                                           \
        const P##_t a = {{C(1), C(2)}, {C(3), C(4)}, {C(5), C(6)}};                                                    \
        TEST_ASSERT_TRUE(P##_equal(a, a));                                                                             \
        for (int i = 0; i < 6; i++) {                                                                                  \
            P##_t b = a;                                                                                               \
            switch (i) {                                                                                               \
                case 0: b.x_axis.x = C(0); break;                                                                      \
                case 1: b.x_axis.y = C(0); break;                                                                      \
                case 2: b.y_axis.x = C(0); break;                                                                      \
                case 3: b.y_axis.y = C(0); break;                                                                      \
                case 4: b.origin.x = C(0); break;                                                                      \
                default: b.origin.y = C(0); break;                                                                     \
            }                                                                                                          \
            TEST_ASSERT_FALSE(P##_equal(a, b));                                                                        \
        }                                                                                                              \
    }

DEFINE_AFFINE_TESTS(affine2f, vec2f, rectf, AFFINE_F, TEST_ASSERT_EQUAL_FLOAT)
DEFINE_AFFINE_TESTS(affine2x, vec2x, rectx, AFFINE_X, TEST_ASSERT_EQUAL_INT32)

static void
test_pixel_rounding_ties_and_single_round(void) {
    affine2x_t x = affine2x_identity();
    affine2f_t f = affine2f_identity();
    x.x_axis.x = x.y_axis.x = x.x_axis.y = x.y_axis.y = MATHX_ONE / AFFINE_SCALE;
    f.x_axis.x = f.y_axis.x = f.x_axis.y = f.y_axis.y = 1.0F / AFFINE_SCALE;
    for (int sign = -1; sign <= 1; sign += AFFINE_SCALE) {
        const vec2i_t p = {sign, 0};
        const vec2i_t xp = affine2x_map_pixel(&x, p), fp = affine2f_map_pixel(&f, p);
        TEST_ASSERT_EQUAL_INT(sign, xp.x);
        TEST_ASSERT_EQUAL_INT(sign, xp.y);
        TEST_ASSERT_EQUAL_INT(sign, fp.x);
        TEST_ASSERT_EQUAL_INT(sign, fp.y);
        const vec2i_t sum = affine2x_map_pixel(&x, (vec2i_t){sign, sign});
        TEST_ASSERT_EQUAL_INT(sign, sum.x);
        TEST_ASSERT_EQUAL_INT(sign, sum.y);
        const vec2i_t float_sum = affine2f_map_pixel(&f, (vec2i_t){sign, sign});
        TEST_ASSERT_EQUAL_INT(sign, float_sum.x);
        TEST_ASSERT_EQUAL_INT(sign, float_sum.y);
    }
    x.origin = (vec2x_t){MATHX_ONE / AFFINE_SCALE, -MATHX_ONE / AFFINE_SCALE};
    f.origin = (vec2f_t){1.0F / AFFINE_SCALE, -1.0F / AFFINE_SCALE};
    const vec2i_t xp = affine2x_map_pixel(&x, (vec2i_t){0, 0});
    const vec2i_t fp = affine2f_map_pixel(&f, (vec2i_t){0, 0});
    TEST_ASSERT_EQUAL_INT(1, xp.x);
    TEST_ASSERT_EQUAL_INT(-1, xp.y);
    TEST_ASSERT_EQUAL_INT(1, fp.x);
    TEST_ASSERT_EQUAL_INT(-1, fp.y);
}

static void
test_quarter_pixels_and_recti_agree(void) {
    affine2x_t x = affine2x_identity();
    affine2f_t f = affine2f_identity();
    const affine2x_t qx = {{0, MATHX_ONE}, {-MATHX_ONE, 0}, {0, 0}};
    const affine2f_t qf = {{0.0F, 1.0F}, {-1.0F, 0.0F}, {0.0F, 0.0F}};
    const vec2i_t points[] = {
        {AFFINE_LEFT, AFFINE_TOP}, {-AFFINE_TOP, AFFINE_LEFT}, {-AFFINE_LEFT, -AFFINE_TOP}, {AFFINE_TOP, -AFFINE_LEFT}};
    const recti_t r = {AFFINE_LEFT, AFFINE_TOP, AFFINE_WIDTH, AFFINE_HEIGHT};
    const recti_t boxes[] = {r,
                             {-AFFINE_TOP - AFFINE_HEIGHT, AFFINE_LEFT, AFFINE_HEIGHT, AFFINE_WIDTH},
                             {-AFFINE_LEFT - AFFINE_WIDTH, -AFFINE_TOP - AFFINE_HEIGHT, AFFINE_WIDTH, AFFINE_HEIGHT},
                             {AFFINE_TOP, -AFFINE_LEFT - AFFINE_WIDTH, AFFINE_HEIGHT, AFFINE_WIDTH}};
    for (int i = 0; i < AFFINE_QUARTERS; i++) {
        const vec2i_t xp = affine2x_map_pixel(&x, points[0]), fp = affine2f_map_pixel(&f, points[0]);
        TEST_ASSERT_EQUAL_INT(points[i].x, xp.x);
        TEST_ASSERT_EQUAL_INT(points[i].y, xp.y);
        TEST_ASSERT_EQUAL_INT(points[i].x, fp.x);
        TEST_ASSERT_EQUAL_INT(points[i].y, fp.y);
        const recti_t xr = affine2x_map_recti(&x, r), fr = affine2f_map_recti(&f, r);
        TEST_ASSERT_EQUAL_MEMORY(&boxes[i], &xr, sizeof xr);
        TEST_ASSERT_EQUAL_MEMORY(&boxes[i], &fr, sizeof fr);
        x = affine2x_mul(qx, x);
        f = affine2f_mul(qf, f);
    }
}

static void
test_fixed_fractional_products_and_inverse(void) {
    const affine2x_t a = {{MATHX_ONE / 3, -(MATHX_ONE / 5)},
                          {MATHX_ONE / 7, 2 * MATHX_ONE / 3},
                          {7 * MATHX_ONE / 3, -(2 * MATHX_ONE / 5)}};
    const affine2x_t b = {{-(MATHX_ONE / 2), MATHX_ONE / 3},
                          {MATHX_ONE / 5, 3 * MATHX_ONE / 7},
                          {-(5 * MATHX_ONE / 3), 7 * MATHX_ONE / 5}};
    const affine2x_t expected_product = {{-7802, 21117}, {8381, 16103}, {129616, 56797}};
    const affine2x_t expected_inverse = {{174218, 52265}, {-37332, 87109}, {-421441, -87108}};
    const affine2x_t product = affine2x_mul(a, b);
    TEST_ASSERT_EQUAL_MEMORY(&expected_product, &product, sizeof product);
    affine2x_t inverse;
    TEST_ASSERT_TRUE(affine2x_invert(a, &inverse));
    TEST_ASSERT_EQUAL_MEMORY(&expected_inverse, &inverse, sizeof inverse);
}

void
suite_affine2(void) {
    RUN_TEST(test_affine2f_identity);
    RUN_TEST(test_affine2f_mul_applies_b_first);
    RUN_TEST(test_affine2f_inverse_and_determinant);
    RUN_TEST(test_affine2f_singular_keeps_output);
    RUN_TEST(test_affine2f_point_and_vector);
    RUN_TEST(test_affine2f_rect_quarter_and_shear);
    RUN_TEST(test_affine2f_equal_checks_every_scalar);
    RUN_TEST(test_affine2x_identity);
    RUN_TEST(test_affine2x_mul_applies_b_first);
    RUN_TEST(test_affine2x_inverse_and_determinant);
    RUN_TEST(test_affine2x_singular_keeps_output);
    RUN_TEST(test_affine2x_point_and_vector);
    RUN_TEST(test_affine2x_rect_quarter_and_shear);
    RUN_TEST(test_affine2x_equal_checks_every_scalar);
    RUN_TEST(test_fixed_fractional_products_and_inverse);
    RUN_TEST(test_pixel_rounding_ties_and_single_round);
    RUN_TEST(test_quarter_pixels_and_recti_agree);
}

SUITE_REGISTER(suite_affine2);

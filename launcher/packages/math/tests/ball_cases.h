/* ball_cases: shared containment cases for circle and sphere instantiations. */
#pragma once

#define DEFINE_BALL_CONTAINMENT_TEST(P, V, T)                                                                          \
    static void test_##P##_contains_ball(void) {                                                                       \
        const struct {                                                                                                 \
            T x, y, radius, margin;                                                                                    \
            bool contained;                                                                                            \
        } cases[] = {                                                                                                  \
            {3, 4, 4, 0, true},  {3, 4, 5, 0, true},   {0, 6, 5, 0, false},  {0, 0, 4, 0, true},                       \
            {0, 0, 10, 0, true}, {1, 0, 10, 0, false}, {3, 4, 4, 1, true},   {3, 4, 5, 1, false},                      \
            {0, 6, 5, -1, true}, {0, 0, 4, 11, false}, {0, 0, 11, 0, false}, {0, 0, 11, -1, true},                     \
        };                                                                                                             \
        V##_t centre = {0};                                                                                            \
        centre.x = 2;                                                                                                  \
        centre.y = -3;                                                                                                 \
        const P##_t outer = {centre, 10};                                                                              \
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {                                                  \
            V##_t offset = {0};                                                                                        \
            offset.x = cases[k].x;                                                                                     \
            offset.y = cases[k].y;                                                                                     \
            const P##_t inner = {V##_add(centre, offset), cases[k].radius};                                            \
            TEST_ASSERT_EQUAL(cases[k].contained, P##_contains_ball(outer, inner, cases[k].margin));                   \
        }                                                                                                              \
    }

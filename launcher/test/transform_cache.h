/* The shared cache-invalidation contract, instantiated for each numeric transform type. */
#pragma once

#include "unity.h"

#define ASSERT_TRANSFORM_SETTERS(prefix, t, vector, turn, rebuilds, check_cached)                                      \
    do {                                                                                                               \
        TEST_ASSERT_FALSE(rebuilds(&(t)));                                                                             \
        prefix##_set_position(&(t), vector(1.0F, 0.0F, 0.0F));                                                         \
        TEST_ASSERT_TRUE(rebuilds(&(t)));                                                                              \
        if (check_cached) {                                                                                            \
            TEST_ASSERT_FALSE(rebuilds(&(t)));                                                                         \
        }                                                                                                              \
        prefix##_set_rotation(&(t), (t).rotation);                                                                     \
        TEST_ASSERT_TRUE(rebuilds(&(t)));                                                                              \
        prefix##_set_scale(&(t), vector(2.0F, 2.0F, 2.0F));                                                            \
        TEST_ASSERT_TRUE(rebuilds(&(t)));                                                                              \
        prefix##_translate(&(t), vector(1.0F, 0.0F, 0.0F));                                                            \
        TEST_ASSERT_TRUE(rebuilds(&(t)));                                                                              \
        prefix##_rotate(&(t), (turn));                                                                                 \
        TEST_ASSERT_TRUE(rebuilds(&(t)));                                                                              \
    } while (0)

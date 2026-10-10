/* The shared cache-invalidation contract, instantiated for each numeric transform type. */
#pragma once

#include "unity.h"

#define ASSERT_TRANSFORM_SETTERS(prefix, t, vector, turn, rebuilds)                                                    \
    do {                                                                                                               \
        TEST_ASSERT_FALSE_MESSAGE(rebuilds(&(t)), #prefix " initial matrix cached");                                   \
        prefix##_set_position(&(t), vector(1.0F, 0.0F, 0.0F));                                                         \
        TEST_ASSERT_TRUE_MESSAGE(rebuilds(&(t)), #prefix " set_position invalidates");                                 \
        TEST_ASSERT_FALSE_MESSAGE(rebuilds(&(t)), #prefix " set_position matrix cached");                              \
        prefix##_set_rotation(&(t), (t).rotation);                                                                     \
        TEST_ASSERT_TRUE_MESSAGE(rebuilds(&(t)), #prefix " set_rotation invalidates");                                 \
        prefix##_set_scale(&(t), vector(2.0F, 2.0F, 2.0F));                                                            \
        TEST_ASSERT_TRUE_MESSAGE(rebuilds(&(t)), #prefix " set_scale invalidates");                                    \
        prefix##_translate(&(t), vector(1.0F, 0.0F, 0.0F));                                                            \
        TEST_ASSERT_TRUE_MESSAGE(rebuilds(&(t)), #prefix " translate invalidates");                                    \
        prefix##_rotate(&(t), (turn));                                                                                 \
        TEST_ASSERT_TRUE_MESSAGE(rebuilds(&(t)), #prefix " rotate invalidates");                                       \
    } while (0)

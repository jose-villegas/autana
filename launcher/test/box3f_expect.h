/*
 * box3f_expect: assertions that a box3f_t or vec3f_t matches, shared by
 * every suite that checks a world or model box.
 *
 * Header-only (static inline) so a suite anywhere under launcher/main/ or
 * launcher/test/ can pull it in without a new compile-list entry in either
 * run_tests.sh or the device test build.
 */
#pragma once

#include "math/geometry/box3f.h"
#include "unity.h"

/* Fails unless every coordinate of got is within tolerance of want's. */
static inline void
vec3f_expect_within(float tolerance, vec3f_t want, vec3f_t got) {
    TEST_ASSERT_FLOAT_WITHIN(tolerance, want.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, want.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, want.z, got.z);
}

/* Fails unless both corners of got are within tolerance of want's. */
static inline void
box3f_expect_within(float tolerance, box3f_t want, box3f_t got) {
    vec3f_expect_within(tolerance, want.lo, got.lo);
    vec3f_expect_within(tolerance, want.hi, got.hi);
}

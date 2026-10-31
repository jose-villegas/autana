#include <math.h>
#include <stdint.h>

#include "math/projection/inverse_depth.h"
#include "testkit/suite.h"
#include "unity.h"

static const float NEAR_Z = 6.0F;
static const float CODE_MAX = (float)UINT16_MAX;

/* code = scale / z * code_max steps by code_max scale / z^2 per unit; one
 * unit on from z the code falls by about that much. */
static void
test_steps_per_unit_is_the_code_slope(void) {
    const float z = 3000.0F;
    const float scale = 512.0F;
    const float slope = CODE_MAX * scale / (z * z);
    TEST_ASSERT_EQUAL_FLOAT(slope, inverse_depth_steps_per_unit(scale, CODE_MAX, z));
    /* In double: the two codes agree to more digits than a float holds. */
    const double fall = ((double)scale / (double)z - (double)scale / ((double)z + 1.0)) * (double)CODE_MAX;
    TEST_ASSERT_FLOAT_WITHIN(slope / z, slope, (float)fall);
    /* A larger scale gives proportionally more steps at the same depth. */
    TEST_ASSERT_EQUAL_FLOAT(slope * 2.0F, inverse_depth_steps_per_unit(scale * 2.0F, CODE_MAX, z));
}

/* The fit is a power of two at or under the nearest depth, and near_z when
 * nothing is past the clip plane. */
static void
test_scale_fit_floors_to_a_power_of_two_never_under_near(void) {
    TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, inverse_depth_scale_fit(NEAR_Z, NEAR_Z));
    TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, inverse_depth_scale_fit(NEAR_Z, NEAR_Z * 0.5F));
    TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, inverse_depth_scale_fit(NEAR_Z, INFINITY * 0.0F));
    /* Just past near_z, the power of two below would fall under it. */
    TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, inverse_depth_scale_fit(NEAR_Z, NEAR_Z + 1.0F));
    const float power = 2048.0F;
    TEST_ASSERT_EQUAL_FLOAT(power, inverse_depth_scale_fit(NEAR_Z, power));
    TEST_ASSERT_EQUAL_FLOAT(power, inverse_depth_scale_fit(NEAR_Z, power * 1.5F));
    TEST_ASSERT_EQUAL_FLOAT(power, inverse_depth_scale_fit(NEAR_Z, nextafterf(power * 2.0F, 0.0F)));
    TEST_ASSERT_EQUAL_FLOAT(power * 2.0F, inverse_depth_scale_fit(NEAR_Z, power * 2.0F));
}

void
suite_inverse_depth(void) {
    RUN_TEST(test_steps_per_unit_is_the_code_slope);
    RUN_TEST(test_scale_fit_floors_to_a_power_of_two_never_under_near);
}

SUITE_REGISTER(suite_inverse_depth);

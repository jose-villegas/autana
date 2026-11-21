/* The image usage examples (image_examples/) compiled and run. */
#include "suites.h"
#include "unity.h"

#include "../image_examples/colour.c"

#define GREY_RGB        0x777777U
#define LIGHTEN_STEP    0.1F
#define GREY_BYTE_SLACK 1
#define CHANNEL_MASK    0xFFU
#define WHITE_RGB       0xFFFFFFU

static void
test_documented_colour_lightens_a_grey_and_keeps_it_grey(void) {
    TEST_ASSERT_EQUAL_HEX32(GREY_RGB, colour_example_lighten(GREY_RGB, 0.0F));
    const uint32_t lighter = colour_example_lighten(GREY_RGB, LIGHTEN_STEP);
    const int r = (int)((lighter >> 16) & CHANNEL_MASK);
    const int g = (int)((lighter >> 8) & CHANNEL_MASK);
    const int b = (int)(lighter & CHANNEL_MASK);
    TEST_ASSERT_GREATER_THAN_INT((int)(GREY_RGB & CHANNEL_MASK), r);
    TEST_ASSERT_INT_WITHIN(GREY_BYTE_SLACK, r, g);
    TEST_ASSERT_INT_WITHIN(GREY_BYTE_SLACK, r, b);
    TEST_ASSERT_EQUAL_HEX32(WHITE_RGB, colour_example_lighten(GREY_RGB, 1.0F));
}

void
suite_image_examples(void) {
    RUN_TEST(test_documented_colour_lightens_a_grey_and_keeps_it_grey);
}

SUITE_REGISTER(suite_image_examples);

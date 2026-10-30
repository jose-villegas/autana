/* Codec parsing rejects incomplete, signed and out-of-range byte arguments. */
#include "audio/console/console_codec_parse.h"
#include "suites.h"
#include "unity.h"

static console_codec_args_t
fixture(void) {
    return (console_codec_args_t){0};
}

static void
test_codec_parse_valid(void) {
    console_codec_args_t args = fixture();
    TEST_ASSERT_TRUE(console_codec_parse("0", &args));
    TEST_ASSERT_EQUAL_UINT8(0, args.reg);
    TEST_ASSERT_FALSE(args.write);
    TEST_ASSERT_TRUE(console_codec_parse(" 0xFF 255 ", &args));
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, args.reg);
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, args.value);
    TEST_ASSERT_TRUE(args.write);
    TEST_ASSERT_TRUE(console_codec_parse("010 0X0a", &args));
    TEST_ASSERT_EQUAL_UINT8(10, args.reg);
    TEST_ASSERT_EQUAL_UINT8(10, args.value);
    TEST_ASSERT_TRUE(console_codec_parse("\t255\r\n", &args));
    TEST_ASSERT_FALSE(args.write);
}

static void
test_codec_parse_invalid(void) {
    static const char* const invalid[] = {"",
                                          " ",
                                          "-1",
                                          "+1",
                                          "256",
                                          "0x100",
                                          "0x",
                                          "0xffg",
                                          "1 -1",
                                          "1 +1",
                                          "1 256",
                                          "1 0x100",
                                          "1 0x",
                                          "0x0x5",
                                          "1 2 3",
                                          "1x",
                                          "1.0",
                                          "1 2x",
                                          "99999999999999999999999999"};
    console_codec_args_t args = fixture();
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(console_codec_parse(invalid[i], &args), invalid[i]);
    }
}

void
run_console_codec_suite(void) {
    RUN_TEST(test_codec_parse_valid);
    RUN_TEST(test_codec_parse_invalid);
}
SUITE_REGISTER(run_console_codec_suite)

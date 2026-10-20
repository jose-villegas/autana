/* Tone command accepts complete decimal arguments within audio limits. */
#include <stdio.h>
#include "console/verbs/console_tone_parse.h"
#include "suites.h"
#include "unity.h"
#define TEST_TARGET_HZ   1000u
#define TEST_DURATION_MS 100u

static console_tone_args_t
fixture(void) {
    return (console_tone_args_t){
        .hz = TEST_TARGET_HZ, .ms = TEST_DURATION_MS, .volume_pct = CONSOLE_TONE_DEFAULT_VOLUME_PCT};
}

static void
test_tone_parse_valid(void) {
    console_tone_args_t args = fixture();
    TEST_ASSERT_TRUE(console_tone_parse("1000 100", &args));
    TEST_ASSERT_EQUAL_UINT16(TEST_TARGET_HZ, args.hz);
    TEST_ASSERT_EQUAL_UINT16(TEST_DURATION_MS, args.ms);
    TEST_ASSERT_EQUAL_UINT8(CONSOLE_TONE_DEFAULT_VOLUME_PCT, args.volume_pct);
    TEST_ASSERT_TRUE(console_tone_parse(" 300 250 0 ", &args));
    TEST_ASSERT_EQUAL_UINT8(0, args.volume_pct);
    TEST_ASSERT_TRUE(console_tone_parse("\t001000\t000100\t030\t", &args));
    TEST_ASSERT_EQUAL_UINT16(TEST_TARGET_HZ, args.hz);
    TEST_ASSERT_EQUAL_UINT16(TEST_DURATION_MS, args.ms);
    TEST_ASSERT_EQUAL_UINT8(CONSOLE_TONE_DEFAULT_VOLUME_PCT, args.volume_pct);
    char limit[64];
    snprintf(limit, sizeof limit, "%u %u %u", AUDIO_OUT_NYQUIST_HZ - 1, AUDIO_OUT_MAX_MS, AUDIO_OUT_VOLUME_MAX_PCT);
    TEST_ASSERT_TRUE(console_tone_parse(limit, &args));
}

static void
test_tone_parse_invalid(void) {
    static const char* const invalid[] = {"",         " \t ",         "1000",
                                          "x 100",    "1000 x",       "1000 100 x",
                                          "-1 100",   "1000 -1",      "0 100",
                                          "1000 0",   "1000 100 -1",  "1000 100 1 extra",
                                          "1.5 100",  "1000 100 1.0", "1000 10000000000000000000000000000000",
                                          "+1000 100"};
    console_tone_args_t args = fixture();
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        const console_tone_args_t before = args;
        TEST_ASSERT_FALSE_MESSAGE(console_tone_parse(invalid[i], &args), invalid[i]);
        TEST_ASSERT_EQUAL_UINT16(before.hz, args.hz);
        TEST_ASSERT_EQUAL_UINT16(before.ms, args.ms);
        TEST_ASSERT_EQUAL_UINT8(before.volume_pct, args.volume_pct);
    }
    char limit[64];
    snprintf(limit, sizeof limit, "%u 100", AUDIO_OUT_NYQUIST_HZ);
    TEST_ASSERT_FALSE(console_tone_parse(limit, &args));
    snprintf(limit, sizeof limit, "1000 %u", AUDIO_OUT_MAX_MS + 1);
    TEST_ASSERT_FALSE(console_tone_parse(limit, &args));
    snprintf(limit, sizeof limit, "1000 100 %u", AUDIO_OUT_VOLUME_MAX_PCT + 1);
    TEST_ASSERT_FALSE(console_tone_parse(limit, &args));
}

void
run_console_tone_suite(void) {
    RUN_TEST(test_tone_parse_valid);
    RUN_TEST(test_tone_parse_invalid);
}
SUITE_REGISTER(run_console_tone_suite)

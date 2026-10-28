#include "audio/audio_out.h"
#include "sound/console/console_mic_parse.h"
#include "suites.h"
#include "unity.h"

static void
test_record_framing(void) {
    char line[128];
    TEST_ASSERT_TRUE(console_mic_begin(line, sizeof line, AUDIO_OUT_SAMPLE_RATE_HZ, 441));
    TEST_ASSERT_EQUAL_STRING("MIC_BEGIN {\"rate\":22050,\"samples\":441}", line);
    TEST_ASSERT_FALSE(console_mic_begin(line, 1, AUDIO_OUT_SAMPLE_RATE_HZ, 441));
}

static void
test_record_arguments(void) {
    console_mic_args_t args;
    TEST_ASSERT_TRUE(console_mic_parse("record 1000 at 0 0x32 40 at 900 0x32 0", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1000 0x32 0", &args));
    TEST_ASSERT_EQUAL_UINT32(2, args.write_count);
    TEST_ASSERT_EQUAL_UINT32(900, args.writes[1].ms);
    TEST_ASSERT_EQUAL_HEX8(0x32, args.writes[0].reg);
    TEST_ASSERT_EQUAL_HEX8(40, args.writes[0].value);
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1001 0x32 0", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1 256 0", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1 0 256", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1 0 0 at 2 0 0 at 3 0 0 at 4 0 0 at 5 0 0", &args));
    TEST_ASSERT_TRUE(console_mic_parse("record 1000 at 0 050 040", &args));
    TEST_ASSERT_EQUAL_UINT8(50, args.writes[0].reg);
    TEST_ASSERT_EQUAL_UINT8(40, args.writes[0].value);
    TEST_ASSERT_TRUE(console_mic_parse("record 1000", &args));
    TEST_ASSERT_EQUAL_UINT32(AUDIO_LOOPBACK_SETTLE_MS, args.settle_ms);
    TEST_ASSERT_TRUE(console_mic_parse("record 10000 0", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 10001", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 0", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record -1", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 100 0 extra", &args));
    TEST_ASSERT_FALSE(console_mic_parse("record 100 9999999999999999999999", &args));
}

void
suite_mic_record(void) {
    RUN_TEST(test_record_framing);
    RUN_TEST(test_record_arguments);
}
SUITE_REGISTER(suite_mic_record)

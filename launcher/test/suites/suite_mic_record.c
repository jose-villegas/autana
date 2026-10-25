#include "audio/audio_codec_eq.h"
#include "console/verbs/console_mic_parse.h"
#include "suites.h"
#include "unity.h"

static void
test_eq_words(void) {
    uint8_t bytes[4];
    TEST_ASSERT_EQUAL_HEX32(0x10000000, audio_codec_eq_encode(1.0));
    TEST_ASSERT_EQUAL_HEX32(0x30000000, audio_codec_eq_encode(-1.0));
    audio_codec_eq_bytes(audio_codec_eq_encode(-0.5), bytes);
    const uint8_t expected[] = {0x38, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(expected, bytes, sizeof expected);
}

static void
test_eq_presets(void) {
    /* https://github.com/hicaoc/NRL-ESP32/blob/main/src/lib/wifi_config_portal_audio.html */
    const double nrl[] = {0.8464592509, -1.6692031398, 0.7166338712, -1.6929185092, 0.8464592509};
    const uint32_t words[] = {227219675, 625668518, 192369940, 619302472, 227219675};
    const double butterworth[] = {0.9702283766983296, -1.9404567533966592, 0.9702283766983296, -1.9395702073516703,
                                  0.9413432994416484};
    const uint32_t hpf[] = {0x0F860E31, 0x20F3E39F, 0x0F860E31, 0x20F7853B, 0x0F0FBDFE};
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
        TEST_ASSERT_EQUAL_HEX32(words[i], audio_codec_eq_encode(nrl[i]));
        TEST_ASSERT_EQUAL_HEX32(hpf[i], audio_codec_eq_encode(butterworth[i]));
    }
}

static void
test_record_framing(void) {
    char line[128];
    TEST_ASSERT_TRUE(console_mic_begin(line, sizeof line, 22050, 441));
    TEST_ASSERT_EQUAL_STRING("MIC_BEGIN {\"rate\":22050,\"samples\":441}", line);
    TEST_ASSERT_FALSE(console_mic_begin(line, 1, 22050, 441));
}

static void
test_record_arguments(void) {
    console_mic_args_t args;
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
    RUN_TEST(test_eq_words);
    RUN_TEST(test_eq_presets);
    RUN_TEST(test_record_arguments);
}
SUITE_REGISTER(suite_mic_record)

#include "audio/audio_codec_drc.h"
#include "suites.h"
#include "unity.h"

static audio_codec_drc_t
fixture(void) {
    return (audio_codec_drc_t){0};
}

static void
test_drc_register_fields(void) {
    audio_codec_drc_t config = fixture();
    TEST_ASSERT_EQUAL_HEX(0, audio_codec_drc_enable_reg(&config));
    TEST_ASSERT_EQUAL_HEX(0, audio_codec_drc_level_reg(&config));
    config.on = true;
    for (unsigned field = 0; field <= AUDIO_CODEC_DRC_FIELD_MAX; field++) {
        config.winsize = config.max_level = config.min_level = config.ramp = (uint8_t)field;
        TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_DRC_ENABLE_MASK | field, audio_codec_drc_enable_reg(&config));
        for (unsigned minimum = 0; minimum <= AUDIO_CODEC_DRC_FIELD_MAX; minimum++) {
            config.min_level = (uint8_t)minimum;
            TEST_ASSERT_EQUAL_HEX((field << AUDIO_CODEC_DRC_LEVEL_SHIFT) | minimum, audio_codec_drc_level_reg(&config));
        }
        config.on = false;
        TEST_ASSERT_EQUAL_HEX(field, audio_codec_drc_enable_reg(&config));
        config.on = true;
        for (unsigned old = 0; old <= UINT8_MAX; old++) {
            TEST_ASSERT_EQUAL_HEX((field << AUDIO_CODEC_DRC_RAMP_SHIFT) | (old & AUDIO_CODEC_DRC_FIELD_MAX),
                                  audio_codec_drc_ramp_reg(old, &config));
        }
    }
}

void
run_audio_drc_suite(void) {
    RUN_TEST(test_drc_register_fields);
}
SUITE_REGISTER(run_audio_drc_suite)

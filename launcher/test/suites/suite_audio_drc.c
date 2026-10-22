#include "audio/audio_codec_drc.h"
#include "suites.h"
#include "unity.h"

static audio_codec_drc_t
fixture(void) {
    return (audio_codec_drc_t){.on = true, .winsize = 4u, .max_level = AUDIO_CODEC_DRC_FIELD_MAX, .ramp = 2u};
}

static void
test_drc_register_fields(void) {
    audio_codec_drc_t config = fixture();
    TEST_ASSERT_EQUAL_HEX(0x84, audio_codec_drc_enable_reg(&config));
    TEST_ASSERT_EQUAL_HEX(0xF0, audio_codec_drc_level_reg(&config));
    TEST_ASSERT_EQUAL_HEX(0x28, audio_codec_drc_ramp_reg(0x08, &config));
    config.on = false;
    config.winsize = 0;
    TEST_ASSERT_EQUAL_HEX(0x00, audio_codec_drc_enable_reg(&config));
}

void
run_audio_drc_suite(void) {
    RUN_TEST(test_drc_register_fields);
}
SUITE_REGISTER(run_audio_drc_suite)

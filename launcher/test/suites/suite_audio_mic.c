#include "audio/audio_codec_mic.h"
#include "suites.h"
#include "unity.h"

#define TEST_IO_ERROR (-1)

typedef struct {
    int values[AUDIO_CODEC_MIC_REGISTER_COUNT];
    unsigned reads, writes, fail_write;
    bool fail_read;
} fake_codec_t;

static fake_codec_t
fixture(void) {
    fake_codec_t fake = {0};
    for (unsigned i = 0; i < AUDIO_CODEC_MIC_REGISTER_COUNT; i++) {
        fake.values[i] = AUDIO_CODEC_MIC_VOLUME_DEFAULT + (int)i;
    }
    return fake;
}

static int
get_reg(void* context, int reg, int* value) {
    fake_codec_t* fake = context;
    fake->reads++;
    *value = fake->values[reg - AUDIO_CODEC_MIC_VOLUME_REG];
    return fake->fail_read ? TEST_IO_ERROR : 0;
}

static int
set_reg(void* context, int reg, int value) {
    fake_codec_t* fake = context;
    fake->values[reg - AUDIO_CODEC_MIC_VOLUME_REG] = value;
    fake->writes++;
    return fake->writes == fake->fail_write ? TEST_IO_ERROR : 0;
}

static void
test_mic_register_fields(void) {
    audio_codec_mic_t config = AUDIO_CODEC_MIC_DEFAULT;
    TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_MIC_VOLUME_DEFAULT, audio_codec_mic_volume_reg(&config));
    TEST_ASSERT_EQUAL_HEX(0, audio_codec_mic_alc_reg(&config));
    TEST_ASSERT_EQUAL_HEX(0, audio_codec_mic_level_reg(&config));
    TEST_ASSERT_EQUAL_HEX(0, audio_codec_mic_gate_reg(&config));
    TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_MIC_HPF_DEFAULT, audio_codec_mic_hpf1_reg(&config));
    TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_MIC_EQ_BYPASS | AUDIO_CODEC_MIC_HPF_DYNAMIC | AUDIO_CODEC_MIC_HPF_DEFAULT,
                          audio_codec_mic_hpf2_reg(&config));
    config.alc = config.gate = true;
    config.hpf = false;
    config.alc_max = config.gate_win = AUDIO_CODEC_MIC_NIBBLE_MAX;
    config.alc_min = config.alc_win = config.gate_ng = AUDIO_CODEC_MIC_NIBBLE_MAX;
    config.gate_depth = AUDIO_CODEC_MIC_DEPTH_MAX;
    config.hpf_s1 = config.hpf_s2 = AUDIO_CODEC_MIC_HPF_MAX;
    TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_MIC_ALC_ENABLE | AUDIO_CODEC_MIC_GATE_ENABLE | AUDIO_CODEC_MIC_NIBBLE_MAX,
                          audio_codec_mic_alc_reg(&config));
    TEST_ASSERT_EQUAL_HEX(UINT8_MAX, audio_codec_mic_level_reg(&config));
    TEST_ASSERT_EQUAL_HEX(UINT8_MAX, audio_codec_mic_gate_reg(&config));
    TEST_ASSERT_EQUAL_HEX(UINT8_MAX, audio_codec_mic_hpf1_reg(&config));
    TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_MIC_EQ_BYPASS | AUDIO_CODEC_MIC_HPF_MAX, audio_codec_mic_hpf2_reg(&config));
}

static void
test_mic_restore_transaction(void) {
    for (unsigned failure = 0; failure <= AUDIO_CODEC_MIC_REGISTER_COUNT * 2; failure++) {
        fake_codec_t fake = fixture();
        const fake_codec_t original = fake;
        audio_codec_saved_t saved = {0};
        audio_codec_mic_t config = AUDIO_CODEC_MIC_DEFAULT;
        config.alc = config.gate = true;
        config.hpf = false;
        fake.fail_write = failure;
        TEST_ASSERT_EQUAL(failure == 0 || failure > AUDIO_CODEC_MIC_REGISTER_COUNT,
                          audio_codec_mic_apply(&saved, &fake, get_reg, set_reg, &config));
        const unsigned reads = fake.reads;
        if (failure == 0) {
            config.alc = false;
            TEST_ASSERT_TRUE(audio_codec_mic_apply(&saved, &fake, get_reg, set_reg, &config));
            TEST_ASSERT_EQUAL_UINT(reads, fake.reads);
        }
        if (!audio_codec_saved_restore(&saved, &fake, set_reg)) {
            TEST_ASSERT_TRUE(saved.restore_pending);
            TEST_ASSERT_TRUE(audio_codec_saved_restore(&saved, &fake, set_reg));
        }
        TEST_ASSERT_FALSE(saved.restore_pending);
        TEST_ASSERT_EQUAL_INT_ARRAY(original.values, fake.values, AUDIO_CODEC_MIC_REGISTER_COUNT);
    }
    fake_codec_t fake = fixture();
    fake.fail_read = true;
    audio_codec_saved_t saved = {0};
    audio_codec_mic_t config = AUDIO_CODEC_MIC_DEFAULT;
    TEST_ASSERT_FALSE(audio_codec_mic_apply(&saved, &fake, get_reg, set_reg, &config));
    TEST_ASSERT_EQUAL_UINT(0, fake.writes);
    TEST_ASSERT_FALSE(saved.restore_pending);
}

void
run_audio_mic_suite(void) {
    RUN_TEST(test_mic_register_fields);
    RUN_TEST(test_mic_restore_transaction);
}
SUITE_REGISTER(run_audio_mic_suite)

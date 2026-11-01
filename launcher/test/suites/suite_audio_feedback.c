#include "audio/audio_codec_feedback.h"
#include "audio/audio_out.h"
#include "suites.h"
#include "unity.h"

#define TEST_REGISTER_OLD 0x9bu
#define TEST_IO_ERROR     (-1)

typedef struct {
    int value;
    unsigned reads;
    unsigned writes;
    unsigned fail_write;
    bool fail_read;
} fake_codec_t;

static fake_codec_t
fixture(void) {
    return (fake_codec_t){.value = TEST_REGISTER_OLD};
}

static int
get_reg(void* context, int reg, int* value) {
    fake_codec_t* fake = context;
    TEST_ASSERT_EQUAL_INT(AUDIO_CODEC_FEEDBACK_REG, reg);
    fake->reads++;
    *value = fake->value;
    return fake->fail_read ? TEST_IO_ERROR : 0;
}

static int
set_reg(void* context, int reg, int value) {
    fake_codec_t* fake = context;
    TEST_ASSERT_EQUAL_INT(AUDIO_CODEC_FEEDBACK_REG, reg);
    fake->value = value;
    fake->writes++;
    return fake->writes == fake->fail_write ? TEST_IO_ERROR : 0;
}

static void
test_feedback_restore_errors(void) {
    for (unsigned failure = 0; failure <= 3; failure++) {
        fake_codec_t fake = fixture();
        fake.fail_read = failure == 1;
        fake.fail_write = failure == 2 ? 1 : failure == 3 ? 2 : 0;
        audio_codec_feedback_t state = {0};
        const bool enabled = audio_codec_feedback_set(&state, &fake, get_reg, set_reg, true);
        TEST_ASSERT_EQUAL(failure != 1 && failure != 2, enabled);
        if (failure == 1) {
            TEST_ASSERT_EQUAL_UINT(0, fake.writes);
        } else {
            TEST_ASSERT_EQUAL_HEX(TEST_REGISTER_OLD & ~AUDIO_CODEC_ADCDAT_MASK, fake.value & ~AUDIO_CODEC_ADCDAT_MASK);
        }
        const bool restored = audio_codec_feedback_set(&state, &fake, get_reg, set_reg, false);
        TEST_ASSERT_EQUAL(failure != 3, restored);
        if (!restored) {
            TEST_ASSERT_TRUE(state.saved.restore_pending);
            TEST_ASSERT_FALSE(audio_codec_feedback_set(&state, &fake, get_reg, set_reg, true));
            TEST_ASSERT_TRUE(audio_codec_feedback_set(&state, &fake, get_reg, set_reg, false));
        }
        TEST_ASSERT_EQUAL_HEX(TEST_REGISTER_OLD, fake.value);
        TEST_ASSERT_FALSE(state.saved.restore_pending);
        const unsigned writes = fake.writes;
        TEST_ASSERT_TRUE(audio_codec_feedback_set(&state, &fake, get_reg, set_reg, false));
        TEST_ASSERT_EQUAL_UINT(writes, fake.writes);
    }
}

static void
test_feedback_register_bits(void) {
    for (unsigned old = 0; old <= UINT8_MAX; old++) {
        const unsigned value = audio_codec_feedback_register(old);
        TEST_ASSERT_EQUAL_HEX(old & ~AUDIO_CODEC_ADCDAT_MASK, value & ~AUDIO_CODEC_ADCDAT_MASK);
        TEST_ASSERT_EQUAL_HEX(AUDIO_CODEC_ADCDAT_DAC_BOTH << AUDIO_CODEC_ADCDAT_SHIFT, value & AUDIO_CODEC_ADCDAT_MASK);
    }
    fake_codec_t fake = fixture();
    audio_codec_feedback_t state = {0};
    TEST_ASSERT_TRUE(audio_codec_feedback_set(&state, &fake, get_reg, set_reg, true));
    TEST_ASSERT_TRUE(audio_codec_feedback_set(&state, &fake, get_reg, set_reg, true));
    TEST_ASSERT_EQUAL_UINT(1, fake.reads);
    TEST_ASSERT_TRUE(audio_codec_feedback_set(&state, &fake, get_reg, set_reg, false));
    TEST_ASSERT_EQUAL_HEX(TEST_REGISTER_OLD, fake.value);
}

static void
test_amp_hold_requests(void) {
    audio_out_amp_t state = {0};
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, false, false));
    TEST_ASSERT_TRUE(audio_out_amp_update(&state, false, true));
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, true, true));
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, false, false));
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, false, true));
    TEST_ASSERT_TRUE(audio_out_amp_update(&state, true, false));
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, false, false));
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, true, true));
    TEST_ASSERT_FALSE(audio_out_amp_update(&state, true, false));
}

void
run_audio_feedback_suite(void) {
    RUN_TEST(test_feedback_register_bits);
    RUN_TEST(test_feedback_restore_errors);
    RUN_TEST(test_amp_hold_requests);
}
SUITE_REGISTER(run_audio_feedback_suite)

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
    TEST_ASSERT_FALSE(args.internal);
    TEST_ASSERT_TRUE(console_tone_parse("1000 100 internal", &args));
    TEST_ASSERT_TRUE(args.internal);
    TEST_ASSERT_TRUE(console_tone_parse("1000 100 10 internal ", &args));
    TEST_ASSERT_TRUE(args.internal);
    TEST_ASSERT_EQUAL_UINT8(10, args.volume_pct);
    TEST_ASSERT_TRUE(console_tone_parse("1000 100", &args));
    TEST_ASSERT_FALSE(args.internal);
    char limit[64];
    snprintf(limit, sizeof limit, "%u %u %u", AUDIO_OUT_NYQUIST_HZ - 1, AUDIO_OUT_MAX_MS, AUDIO_OUT_VOLUME_MAX_PCT);
    TEST_ASSERT_TRUE(console_tone_parse(limit, &args));
}

static void
test_tone_parse_invalid(void) {
    static const char* const invalid[] = {"",
                                          " \t ",
                                          "1000",
                                          "x 100",
                                          "1000 x",
                                          "1000 100 x",
                                          "-1 100",
                                          "1000 -1",
                                          "0 100",
                                          "1000 0",
                                          "1000 100 -1",
                                          "1000 100 1 extra",
                                          "1.5 100",
                                          "1000 100 1.0",
                                          "1000 10000000000000000000000000000000",
                                          "+1000 100",
                                          "1000 100 30 40 internal",
                                          "1000 internal 100",
                                          "1000 100 internal 10",
                                          "1000 100 internal junk",
                                          "1000 100 internal internal"};
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

#ifndef ESP_PLATFORM
#include "audio/audio_codec_feedback.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out_device.h"
#include "console/console_verbs.h"
#include "core/timing.h"
#include "sound/sound_engine.h"

#define TEST_TONE_REGISTER_OLD 0x58u
#define TEST_TONE_IO_ERROR     (-1)

typedef enum {
    TEST_TONE_OK,
    TEST_TONE_OPEN_FAIL,
    TEST_TONE_HOLD_FAIL,
    TEST_TONE_GET_FAIL,
    TEST_TONE_SET_FAIL,
    TEST_TONE_BASELINE_FAIL,
    TEST_TONE_START_FAIL,
    TEST_TONE_MEASURE_FAIL,
    TEST_TONE_CLOSE_FAIL
} tone_failure_t;

static struct {
    tone_failure_t failure;
    audio_codec_feedback_t feedback;
    audio_out_amp_t amp;
    int reg;
    unsigned measures;
    unsigned closes;
    bool playing;
    bool audible;
    bool internal;
    const char* reply;
} device;

static console_registry_t registry;

console_registry_t*
console_shared(void) {
    return &registry;
}

static int
tone_get_reg(void* context, int reg, int* value) {
    (void)context;
    TEST_ASSERT_EQUAL_INT(AUDIO_CODEC_FEEDBACK_REG, reg);
    *value = device.reg;
    return device.failure == TEST_TONE_GET_FAIL ? TEST_TONE_IO_ERROR : 0;
}

static int
tone_set_reg(void* context, int reg, int value) {
    (void)context;
    TEST_ASSERT_EQUAL_INT(AUDIO_CODEC_FEEDBACK_REG, reg);
    device.reg = value;
    return device.failure == TEST_TONE_SET_FAIL && value != TEST_TONE_REGISTER_OLD ? TEST_TONE_IO_ERROR : 0;
}

bool
audio_codec_dac_feedback(bool on) {
    return audio_codec_feedback_set(&device.feedback, NULL, tone_get_reg, tone_set_reg, on);
}

bool
audio_out_amp_hold(bool held) {
    (void)audio_out_amp_update(&device.amp, true, held);
    return !(held && device.failure == TEST_TONE_HOLD_FAIL);
}

bool
sound_capture_open(void) {
    return device.failure != TEST_TONE_OPEN_FAIL;
}

bool
sound_capture_close(void) {
    device.closes++;
    const bool restored = audio_codec_dac_feedback(false);
    (void)audio_out_amp_hold(false);
    return restored && device.failure != TEST_TONE_CLOSE_FAIL;
}

sound_voice_t
sound_play_tone(uint16_t hz, uint8_t gain_pct) {
    TEST_ASSERT_EQUAL_UINT16(TEST_TARGET_HZ, hz);
    TEST_ASSERT_EQUAL_UINT8(CONSOLE_TONE_DEFAULT_VOLUME_PCT, gain_pct);
    if (device.internal) {
        TEST_ASSERT_TRUE(device.amp.held);
        TEST_ASSERT_TRUE(device.feedback.enabled);
    }
    device.playing = device.failure != TEST_TONE_START_FAIL;
    return device.playing ? (sound_voice_t){.id = 1} : SOUND_VOICE_NULL;
}

bool
sound_is_playing(sound_voice_t voice) {
    return voice.id && device.playing;
}

bool
sound_voice_audible(sound_voice_t voice) {
    return sound_is_playing(voice) && device.audible;
}

void
timing_sleep_ms(uint32_t ms) {
    TEST_ASSERT_EQUAL_UINT(SOUND_CONTROL_POLL_MS, ms);
    if (device.internal) {
        TEST_ASSERT_TRUE(device.amp.held);
    }
    device.audible = true;
}

void
sound_stop(sound_voice_t voice) {
    (void)voice;
    device.playing = false;
}

bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, float* measured_hz) {
    TEST_ASSERT_EQUAL_UINT16(TEST_TARGET_HZ, hz);
    TEST_ASSERT_EQUAL_UINT16(TEST_DURATION_MS, ms);
    if (device.internal) {
        TEST_ASSERT_TRUE(device.amp.held);
        TEST_ASSERT_TRUE(device.feedback.enabled);
    }
    device.measures++;
    *db = device.measures == 1 ? -100.0F : -12.0F;
    if (measured_hz) {
        *measured_hz = hz;
    }
    return !((device.measures == 1 && device.failure == TEST_TONE_BASELINE_FAIL)
             || (device.measures == 2 && device.failure == TEST_TONE_MEASURE_FAIL));
}

static void
tone_collect(const char* reply) {
    device.reply = reply;
}

static void
test_tone_capture_restores_every_exit(void) {
    for (tone_failure_t failure = TEST_TONE_OK; failure <= TEST_TONE_CLOSE_FAIL; failure++) {
        memset(&device, 0, sizeof device);
        device.failure = failure;
        device.internal = true;
        device.reg = TEST_TONE_REGISTER_OLD;
        device.amp.requested = true;
        TEST_ASSERT_TRUE(console_registry_handle_line(&registry, "tone 1000 100 internal", tone_collect));
        TEST_ASSERT_EQUAL_UINT(1, device.closes);
        TEST_ASSERT_EQUAL_HEX(TEST_TONE_REGISTER_OLD, device.reg);
        TEST_ASSERT_FALSE(device.feedback.restore_pending);
        TEST_ASSERT_FALSE(device.amp.held);
        TEST_ASSERT_TRUE(device.amp.requested);
        TEST_ASSERT_FALSE(device.playing);
        if (failure == TEST_TONE_OK) {
            TEST_ASSERT_NULL(device.reply);
        } else {
            TEST_ASSERT_NOT_NULL(device.reply);
            TEST_ASSERT_NOT_NULL(strstr(device.reply, "TONE error:"));
        }
    }
}

static void
test_tone_acoustic_has_no_feedback(void) {
    memset(&device, 0, sizeof device);
    device.reg = TEST_TONE_REGISTER_OLD;
    TEST_ASSERT_TRUE(console_registry_handle_line(&registry, "tone 1000 100", tone_collect));
    TEST_ASSERT_EQUAL_UINT(2, device.measures);
    TEST_ASSERT_EQUAL_UINT(1, device.closes);
    TEST_ASSERT_EQUAL_HEX(TEST_TONE_REGISTER_OLD, device.reg);
    TEST_ASSERT_FALSE(device.feedback.restore_pending);
    TEST_ASSERT_NULL(device.reply);
}
#endif

void
run_console_tone_suite(void) {
#ifndef ESP_PLATFORM
    RUN_TEST(test_tone_capture_restores_every_exit);
    RUN_TEST(test_tone_acoustic_has_no_feedback);
#endif
    RUN_TEST(test_tone_parse_valid);
    RUN_TEST(test_tone_parse_invalid);
}
SUITE_REGISTER(run_console_tone_suite)

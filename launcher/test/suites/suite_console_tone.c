/* Tone command accepts complete decimal arguments within audio limits. */
#include <stdio.h>
#include "console/verbs/console_tone_parse.h"
#include "console/verbs/console_tone_reply.h"
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
    TEST_ASSERT_TRUE(console_tone_parse("1000\t100\tinternal\t \r\n", &args));
    TEST_ASSERT_TRUE(args.internal);
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
                                          "1000 100internal",
                                          "1000 100 internally",
                                          "internal",
                                          "1000 internal",
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
    TEST_TONE_CLOSE_FAIL,
    TEST_TONE_RESTORE_FAIL,
    TEST_TONE_RELEASE_FAIL,
    TEST_TONE_STOP_DROPPED,
    TEST_TONE_STOP_STUCK
} tone_failure_t;

static struct {
    tone_failure_t failure;
    audio_codec_feedback_t feedback;
    audio_out_amp_t amp;
    int reg;
    unsigned measures;
    unsigned closes;
    unsigned stops;
    unsigned drain_ms;
    int64_t stop_started;
    bool stop_pending;
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
    if (device.failure == TEST_TONE_RESTORE_FAIL && value == TEST_TONE_REGISTER_OLD) {
        return TEST_TONE_IO_ERROR;
    }
    device.reg = value;
    return device.failure == TEST_TONE_SET_FAIL && value != TEST_TONE_REGISTER_OLD ? TEST_TONE_IO_ERROR : 0;
}

bool
audio_codec_dac_feedback(bool on) {
    return audio_codec_feedback_set(&device.feedback, NULL, tone_get_reg, tone_set_reg, on);
}

bool
audio_out_amp_hold(bool held) {
    if (!held && device.internal && device.stops) {
        TEST_ASSERT_TRUE(!device.playing || device.failure == TEST_TONE_STOP_STUCK);
        TEST_ASSERT_EQUAL_UINT(AUDIO_OUT_DMA_DRAIN_MS, device.drain_ms);
        if (device.failure == TEST_TONE_STOP_STUCK) {
            TEST_ASSERT_GREATER_OR_EQUAL_INT64((int64_t)(SOUND_CAPTURE_TIMEOUT_MS - SOUND_CONTROL_POLL_MS)
                                                   * MATH_US_PER_MS,
                                               timing_now_us() - device.stop_started);
        }
    }
    (void)audio_out_amp_update(&device.amp, true, held);
    return !(held && device.failure == TEST_TONE_HOLD_FAIL) && !(!held && device.failure == TEST_TONE_RELEASE_FAIL);
}

bool
sound_capture_open(void) {
    return device.failure != TEST_TONE_OPEN_FAIL;
}

bool
sound_capture_close(void) {
    device.closes++;
    return device.failure != TEST_TONE_CLOSE_FAIL;
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
    if (ms != SOUND_CONTROL_POLL_MS) {
        TEST_ASSERT_FALSE(device.playing && device.failure != TEST_TONE_STOP_STUCK);
        device.drain_ms += ms;
    }
    if (device.internal) {
        TEST_ASSERT_TRUE(device.amp.held);
    }
    if (device.stop_pending) {
        device.playing = false;
        device.stop_pending = false;
    }
    device.audible = true;
}

void
sound_stop(sound_voice_t voice) {
    (void)voice;
    if (!device.stops) {
        device.stop_started = timing_now_us();
    }
    device.stops++;
    if (device.failure != TEST_TONE_STOP_STUCK && !(device.failure == TEST_TONE_STOP_DROPPED && device.stops == 1)) {
        device.stop_pending = true;
    }
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
    for (tone_failure_t failure = TEST_TONE_OK; failure <= TEST_TONE_STOP_STUCK; failure++) {
        memset(&device, 0, sizeof device);
        device.failure = failure;
        device.internal = true;
        device.reg = TEST_TONE_REGISTER_OLD;
        device.amp.requested = true;
        TEST_ASSERT_TRUE(console_registry_handle_line(&registry, "tone 1000 100 internal", tone_collect));
        TEST_ASSERT_EQUAL_UINT(1, device.closes);
        if (failure == TEST_TONE_RESTORE_FAIL) {
            TEST_ASSERT_TRUE(device.feedback.restore_pending);
            TEST_ASSERT_NOT_NULL(device.reply);
            TEST_ASSERT_NOT_NULL(strstr(device.reply, "TONE error:"));
            continue;
        }
        TEST_ASSERT_EQUAL_HEX(TEST_TONE_REGISTER_OLD, device.reg);
        TEST_ASSERT_FALSE(device.feedback.restore_pending);
        TEST_ASSERT_FALSE(device.amp.held);
        TEST_ASSERT_TRUE(device.amp.requested);
        TEST_ASSERT_EQUAL(failure == TEST_TONE_STOP_STUCK, device.playing);
        if (failure == TEST_TONE_STOP_DROPPED) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, device.stops);
        }
        if (failure == TEST_TONE_OK || failure == TEST_TONE_STOP_DROPPED) {
            TEST_ASSERT_NULL(device.reply);
        } else {
            TEST_ASSERT_NOT_NULL(device.reply);
            TEST_ASSERT_NOT_NULL(strstr(device.reply, "TONE error:"));
        }
    }
}

static void
test_tone_pending_restore_blocks_both_modes(void) {
    memset(&device, 0, sizeof device);
    device.internal = true;
    device.reg = TEST_TONE_REGISTER_OLD;
    device.failure = TEST_TONE_RESTORE_FAIL;
    TEST_ASSERT_TRUE(console_registry_handle_line(&registry, "tone 1000 100 internal", tone_collect));
    TEST_ASSERT_NOT_NULL(device.reply);
    TEST_ASSERT_NOT_NULL(strstr(device.reply, "TONE error:"));
    TEST_ASSERT_TRUE(device.feedback.restore_pending);
    const unsigned measures = device.measures;
    static const char* const commands[] = {"tone 1000 100", "tone 1000 100 internal"};
    device.internal = false;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        device.reply = NULL;
        TEST_ASSERT_TRUE(console_registry_handle_line(&registry, commands[i], tone_collect));
        TEST_ASSERT_NOT_NULL(device.reply);
        TEST_ASSERT_NOT_NULL(strstr(device.reply, "TONE error:"));
        TEST_ASSERT_EQUAL_UINT(measures, device.measures);
        TEST_ASSERT_TRUE(device.feedback.restore_pending);
    }
    device.failure = TEST_TONE_OK;
    device.reply = NULL;
    TEST_ASSERT_TRUE(console_registry_handle_line(&registry, commands[0], tone_collect));
    TEST_ASSERT_NULL(device.reply);
    TEST_ASSERT_FALSE(device.feedback.restore_pending);
    TEST_ASSERT_EQUAL_UINT(measures + 2, device.measures);
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

static void
test_tone_reply_frequency(void) {
    const console_tone_args_t args = fixture();
    static const float values[] = {TEST_TARGET_HZ, NAN, INFINITY, -INFINITY};
    char line[CONSOLE_TONE_REPLY_BYTES];
    char expected[CONSOLE_TONE_MEASURED_BYTES];
    snprintf(expected, sizeof expected, "measured %.2f Hz", (double)TEST_TARGET_HZ);
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        TEST_ASSERT_TRUE(console_tone_reply(line, sizeof line, &args, true, -12, -100, values[i], 0));
        TEST_ASSERT_NOT_NULL(strstr(line, i == 0 ? expected : "measured n/a"));
        puts(line);
    }
}

void
run_console_tone_suite(void) {
#ifndef ESP_PLATFORM
    RUN_TEST(test_tone_capture_restores_every_exit);
    RUN_TEST(test_tone_pending_restore_blocks_both_modes);
    RUN_TEST(test_tone_acoustic_has_no_feedback);
#endif
    RUN_TEST(test_tone_reply_frequency);
    RUN_TEST(test_tone_parse_valid);
    RUN_TEST(test_tone_parse_invalid);
}
SUITE_REGISTER(run_console_tone_suite)

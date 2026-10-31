/* Tone command accepts complete decimal arguments within audio limits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clock_fake.h"
#include "input/motion/imu.h"
#include "input/motion/imu_vibration.h"
#include "input_fake.h"
#include "math/scalar/math_db.h"
#include "sound/console/console_melody.h"
#include "sound/console/console_tone_parse.h"
#include "sound/console/console_tone_reply.h"
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
test_tone_reply_capacity(void) {
    console_tone_args_t args = fixture();
    args.hz = AUDIO_OUT_NYQUIST_HZ - 1;
    args.internal = false;
    const audio_loopback_result_t result = {
        .measured_hz = AUDIO_OUT_NYQUIST_HZ, .thd_db = MATH_DB_FLOOR, .peak_dbfs = MATH_DB_FLOOR};
    char line[CONSOLE_TONE_REPLY_BYTES];
    TEST_ASSERT_TRUE(
        console_tone_reply(line, sizeof line, &args, false, MATH_DB_FLOOR, MATH_DB_FLOOR, &result, SIZE_MAX));
    /* The farthest a sample can sit from its burst mean: full scale on all three axes. */
    const float widest_mg = sqrtf(3.0F) * (float)(INT16_MAX - INT16_MIN) * IMU_VIBRATION_MG_PER_G / IMU_COUNTS_PER_G;
    const imu_vibration_t widest = {.rms_mg = widest_mg, .peak_mg = widest_mg, .samples = IMU_FIFO_SAMPLES};
    TEST_ASSERT_TRUE(console_tone_append_shake(line, sizeof line, &widest, &widest));
    TEST_ASSERT_LESS_THAN_UINT(CONSOLE_TONE_REPLY_BYTES, strlen(line) + 1);
}

static void
test_tone_reply_shake(void) {
    char line[CONSOLE_TONE_REPLY_BYTES] = "TONE";
    const imu_vibration_t none = {0};
    const imu_vibration_t silence = {.rms_mg = 0.25F, .peak_mg = 1, .samples = IMU_FIFO_SAMPLES};
    const imu_vibration_t tone = {.rms_mg = 12.5F, .peak_mg = 30, .samples = IMU_FIFO_SAMPLES};
    TEST_ASSERT_TRUE(console_tone_append_shake(line, sizeof line, &silence, &tone));
    TEST_ASSERT_EQUAL_STRING("TONE, shake 12.50 mg rms 30.00 mg peak (silence 0.25 mg rms)", line);
    strcpy(line, "TONE");
    TEST_ASSERT_TRUE(console_tone_append_shake(line, sizeof line, &none, &tone));
    TEST_ASSERT_EQUAL_STRING("TONE, shake n/a", line);
    TEST_ASSERT_FALSE(console_tone_append_shake(line, strlen(line) + 1, &silence, &tone));
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
    TEST_ASSERT_TRUE(console_tone_parse_options(" ", &args));
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
#include "asset/asset_store.h"
#include "audio/audio_codec_feedback.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out_device.h"
#include "console/console_verbs.h"
#include "core/timing.h"
#include "sound/console/console_sound_views.h"
#include "sound/console/console_tone.h"
#include "sound/sound_engine.h"
#include "test_asset_dir.h"

#define TEST_CONSOLE_VOICE_FIRST      1u
#define TEST_CONSOLE_VOICE_SECOND     2u
#define TEST_CONSOLE_VOICE_SYSTEM     (TEST_CONSOLE_VOICE_SECOND + SOUND_VOICE_COUNT + 1u)

#define TEST_TONE_REGISTER_OLD        0x58u
#define TEST_TONE_IO_ERROR            (-1)
#define TEST_NS_PER_US                1000u
#define TEST_MELODY_FAIL_NOTE         2u
#define TEST_MELODY_MEASURES_PER_NOTE 2u
#define TEST_MELODY_START_MS          3u
#define TEST_MELODY_STOP_MS           2u
#define TEST_SHAKE_COUNTS             IMU_COUNTS_PER_G

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
    struct {
        bool enabled;
        bool system_playing;
        uint32_t playing;
        unsigned stop_drops;
        bool fence_timeout;
        int expected_stops;
        int phase;
        int mounted;
    } console;

    const sound_play_opts_t* play_opts;
    sound_voice_t play_voice;
    sound_voice_t stop_voice;
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
    bool replied;
    bool tone_error;
    unsigned odr_code;
    bool shake_short;

    struct {
        int64_t clock_us;
        int64_t boundary_us;
        size_t starts;
        size_t results;
        size_t fail_start;
        unsigned fail_measure;
        unsigned elapsed_ms;
        bool enabled;
        bool play;
        bool ended;
        bool error;
    } melody;
} device;

static console_registry_t registry;

#ifdef _WIN32
#define TEST_CLOCK_REAL __real__timespec64_get
#define TEST_CLOCK_WRAP __wrap__timespec64_get
#else
#define TEST_CLOCK_REAL __real_timespec_get
#define TEST_CLOCK_WRAP __wrap_timespec_get
#endif

int TEST_CLOCK_REAL(struct timespec* ts, int base);

int
TEST_CLOCK_WRAP(struct timespec* ts, int base) {
    if (clock_fake_read(ts)) {
        return base;
    }
    if (!device.melody.play) {
        return TEST_CLOCK_REAL(ts, base);
    }
    ts->tv_sec = device.melody.clock_us / MATH_US_PER_S;
    ts->tv_nsec = (device.melody.clock_us % MATH_US_PER_S) * TEST_NS_PER_US;
    return base;
}

static void
reset_device(void) {
    memset(&device, 0, sizeof device);
}

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
    if (device.melody.enabled) {
        device.drain_ms = 0;
    }
    return device.failure != TEST_TONE_OPEN_FAIL;
}

bool
sound_capture_close(void) {
    device.closes++;
    return device.failure != TEST_TONE_CLOSE_FAIL;
}

sound_voice_t
sound_play(const sound_t* sound, const sound_play_opts_t* opts) {
    device.play_opts = opts;
    device.playing = sound && sound->data && sound->frames && device.failure != TEST_TONE_START_FAIL;
    device.play_voice = device.playing ? (sound_voice_t){.id = TEST_CONSOLE_VOICE_FIRST} : SOUND_VOICE_NULL;
    return device.play_voice;
}

sound_voice_t
sound_play_tone(uint16_t hz, uint16_t ms, uint8_t gain_pct, sound_bus_t bus) {
    TEST_ASSERT_EQUAL_UINT16(SOUND_TONE_UNTIL_STOPPED, ms);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_SYSTEM.id, bus.id);
    TEST_ASSERT_EQUAL_UINT16(
        device.melody.enabled ? melody_note_hz(melody_notes[device.melody.starts++]) : TEST_TARGET_HZ, hz);
    TEST_ASSERT_EQUAL_UINT8(CONSOLE_TONE_DEFAULT_VOLUME_PCT, gain_pct);
    if (device.internal) {
        TEST_ASSERT_TRUE(device.amp.held);
        TEST_ASSERT_TRUE(device.feedback.enabled);
    }
    if (device.melody.play) {
        TEST_ASSERT_EQUAL_INT64(device.melody.boundary_us, device.melody.clock_us);
        device.melody.boundary_us +=
            (int64_t)melody_notes[device.melody.starts - 1].steps * MELODY_STEP_MS * MATH_US_PER_MS;
        device.melody.clock_us += TEST_MELODY_START_MS * MATH_US_PER_MS;
    }
    device.playing = device.failure != TEST_TONE_START_FAIL;
    if (device.melody.fail_start && device.melody.starts == device.melody.fail_start) {
        device.audible = false;
    }
    return device.playing ? (sound_voice_t){.id = 1} : SOUND_VOICE_NULL;
}

bool
sound_is_playing(sound_voice_t voice) {
    if (device.console.enabled) {
        return voice.id == TEST_CONSOLE_VOICE_SYSTEM ? device.console.system_playing
               : voice.id                            ? (device.console.playing & (1u << voice.id)) != 0
                                                     : false;
    }
    return voice.id && device.playing;
}

bool
sound_voice_audible(sound_voice_t voice) {
    return sound_is_playing(voice) && device.audible
           && !(device.melody.fail_start && device.melody.starts == device.melody.fail_start);
}

void
timing_sleep_ms(uint32_t ms) {
    if (device.melody.play) {
        device.melody.clock_us += (int64_t)ms * MATH_US_PER_MS;
    } else if (ms != SOUND_CONTROL_POLL_MS) {
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
    device.stop_voice = voice;
    if (device.console.enabled) {
        if (voice.id == TEST_CONSOLE_VOICE_SYSTEM) {
            device.console.system_playing = false;
        } else if (device.console.stop_drops) {
            device.console.stop_drops--;
            return;
        } else {
            device.console.playing &= ~(1u << voice.id);
        }
        device.console.phase++;
        return;
    }
    if (!device.stops) {
        device.stop_started = timing_now_us();
    }
    device.stops++;
    if (device.melody.play) {
        if (device.failure == TEST_TONE_OK && !device.melody.fail_start) {
            TEST_ASSERT_EQUAL_INT64(device.melody.boundary_us - (int64_t)MELODY_GAP_MS * MATH_US_PER_MS,
                                    device.melody.clock_us);
        }
        device.melody.clock_us += TEST_MELODY_STOP_MS * MATH_US_PER_MS;
    }
    if (device.failure != TEST_TONE_STOP_STUCK && !(device.failure == TEST_TONE_STOP_DROPPED && device.stops == 1)) {
        device.stop_pending = true;
    }
}

bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, audio_loopback_result_t* result) {
    TEST_ASSERT_EQUAL_UINT16(device.melody.enabled ? melody_note_hz(melody_notes[device.measures / 2]) : TEST_TARGET_HZ,
                             hz);
    TEST_ASSERT_EQUAL_UINT16(device.melody.enabled ? CONSOLE_MELODY_CHECK_MS : TEST_DURATION_MS, ms);
    if (device.internal) {
        TEST_ASSERT_TRUE(device.amp.held);
        TEST_ASSERT_TRUE(device.feedback.enabled);
    }
    device.measures++;
    *db = device.measures % 2 ? -100.0F : -12.0F;
    if (result) {
        result->measured_hz = hz;
        result->thd_db = -40.0F;
        result->peak_dbfs = -6.0F;
        result->noise_dbfs = -70.0F;
    }
    return device.measures != device.melody.fail_measure
           && !((device.measures == 1 && device.failure == TEST_TONE_BASELINE_FAIL)
                || (device.measures == 2 && device.failure == TEST_TONE_MEASURE_FAIL));
}

bool
imu_set_odr(unsigned odr_code) {
    device.odr_code = odr_code;
    return true;
}

/* A full queue of samples either side of zero on x, TEST_SHAKE_COUNTS wider per measured
 * window; device.shake_short leaves it one short, as another reader taking would. */
int
imu_take_fifo_at(unsigned odr_code, imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES],
                 bool* gap) {
    TEST_ASSERT_EQUAL_UINT(IMU_BUZZ_ODR_CODE, odr_code);
    TEST_ASSERT_EQUAL_UINT(IMU_BUZZ_ODR_CODE, device.odr_code);
    *gap = true;
    const int16_t counts = (int16_t)(device.measures * TEST_SHAKE_COUNTS);
    const int count = device.shake_short ? IMU_FIFO_SAMPLES - 1 : IMU_FIFO_SAMPLES;
    for (int i = 0; i < count; i++) {
        out[i] = (imu_sample_t){.ax = i % 2 ? (int16_t)-counts : counts};
        lost_before[i] = 0;
    }
    return count;
}

static void
tone_collect(const char* reply) {
    device.replied = true;
    device.tone_error = strstr(reply, "TONE error:") != NULL;
}

static void
melody_collect(const char* line) {
    if (strncmp(line, "MELODY note=", strlen("MELODY note=")) == 0) {
        TEST_ASSERT_LESS_THAN_UINT(MELODY_PHRASE_NOTES, device.melody.results);
        char name[MELODY_NAME_BYTES];
        melody_note_name(melody_notes[device.melody.results++], name);
        TEST_ASSERT_EQUAL_MEMORY(name, line + strlen("MELODY note="), strlen(name));
        TEST_ASSERT_NOT_NULL(strstr(line, " TONE heard "));
    } else if (strncmp(line, "MELODY_END", strlen("MELODY_END")) == 0) {
        size_t count = 0;
        TEST_ASSERT_EQUAL_INT(1, sscanf(line, "MELODY_END notes=%zu", &count));
        TEST_ASSERT_EQUAL_UINT(MELODY_PHRASE_NOTES, count);
        if (device.melody.play) {
            TEST_ASSERT_EQUAL_INT(2, sscanf(line, "MELODY_END notes=%zu ms=%u", &count, &device.melody.elapsed_ms));
        }
        device.melody.ended = true;
    } else {
        TEST_ASSERT_NOT_NULL(strstr(line, "MELODY_ERR "));
        device.melody.error = true;
    }
}

static void
test_melody_console_play_and_check(void) {
    suite_add_test_cleanup(reset_device);
    static const char* const commands[] = {"melody", "melody check", "melody check internal"};
    for (size_t mode = 0; mode < sizeof commands / sizeof commands[0]; mode++) {
        memset(&device, 0, sizeof device);
        device.melody.enabled = true;
        device.melody.play = mode == 0;
        device.internal = mode == 2;
        device.reg = TEST_TONE_REGISTER_OLD;
        TEST_ASSERT_TRUE(console_registry_dispatch(&registry, commands[mode], melody_collect) == CONSOLE_LINE_HANDLED);
        TEST_ASSERT_TRUE(device.melody.ended);
        TEST_ASSERT_FALSE(device.melody.error);
        TEST_ASSERT_EQUAL_UINT(MELODY_PHRASE_NOTES, device.melody.starts);
        TEST_ASSERT_FALSE(device.playing);
        TEST_ASSERT_FALSE(device.amp.held);
        TEST_ASSERT_FALSE(device.feedback.enabled);
        if (device.melody.play) {
            unsigned expected_ms = 0;
            for (size_t i = 0; i < MELODY_PHRASE_NOTES; i++) {
                expected_ms += melody_notes[i].steps * MELODY_STEP_MS;
            }
            TEST_ASSERT_EQUAL_UINT(expected_ms, device.melody.elapsed_ms);
        } else {
            TEST_ASSERT_EQUAL_UINT(MELODY_PHRASE_NOTES, device.melody.results);
            TEST_ASSERT_EQUAL_UINT(MELODY_PHRASE_NOTES, device.closes);
        }
    }
}

static void
test_melody_console_failure_and_usage(void) {
    static const char* const commands[] = {"melody internal", "melody check -1", "melody check check", "melody check"};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        memset(&device, 0, sizeof device);
        device.melody.enabled = true;
        device.failure = TEST_TONE_OPEN_FAIL;
        TEST_ASSERT_TRUE(console_registry_dispatch(&registry, commands[i], melody_collect) == CONSOLE_LINE_HANDLED);
        TEST_ASSERT_TRUE(device.melody.error);
        TEST_ASSERT_FALSE(device.melody.ended);
        TEST_ASSERT_EQUAL_UINT(0, device.melody.starts);
    }
}

static void
test_melody_console_later_failures(void) {
    suite_add_test_cleanup(reset_device);
    static const tone_failure_t failures[] = {TEST_TONE_START_FAIL, TEST_TONE_STOP_STUCK, TEST_TONE_MEASURE_FAIL};
    for (size_t i = 0; i < sizeof failures / sizeof failures[0]; i++) {
        const tone_failure_t failure = failures[i];
        reset_device();
        device.melody.enabled = true;
        device.melody.play = failure != TEST_TONE_MEASURE_FAIL;
        device.reg = TEST_TONE_REGISTER_OLD;
        if (failure == TEST_TONE_START_FAIL) {
            device.melody.fail_start = TEST_MELODY_FAIL_NOTE;
        } else if (failure == TEST_TONE_STOP_STUCK) {
            device.failure = failure;
        } else {
            device.melody.fail_measure = TEST_MELODY_FAIL_NOTE * TEST_MELODY_MEASURES_PER_NOTE;
        }
        TEST_ASSERT_TRUE(console_registry_dispatch(
                             &registry, failure == TEST_TONE_MEASURE_FAIL ? "melody check" : "melody", melody_collect)
                         == CONSOLE_LINE_HANDLED);
        TEST_ASSERT_TRUE(device.melody.error);
        TEST_ASSERT_FALSE(device.melody.ended);
        if (failure == TEST_TONE_STOP_STUCK) {
            TEST_ASSERT_GREATER_THAN_UINT(0, device.stops);
        } else {
            TEST_ASSERT_FALSE(device.playing);
            TEST_ASSERT_EQUAL_UINT(TEST_MELODY_FAIL_NOTE, device.melody.starts);
        }
        if (failure == TEST_TONE_MEASURE_FAIL) {
            TEST_ASSERT_EQUAL_UINT(1, device.melody.results);
            TEST_ASSERT_EQUAL_UINT(TEST_MELODY_FAIL_NOTE, device.closes);
        }
    }
}

static void
test_tone_capture_restores_every_exit(void) {
    for (tone_failure_t failure = TEST_TONE_OK; failure <= TEST_TONE_STOP_STUCK; failure++) {
        memset(&device, 0, sizeof device);
        device.failure = failure;
        device.internal = true;
        device.reg = TEST_TONE_REGISTER_OLD;
        device.amp.requested = true;
        TEST_ASSERT_TRUE(console_registry_dispatch(&registry, "tone 1000 100 internal", tone_collect)
                         == CONSOLE_LINE_HANDLED);
        TEST_ASSERT_EQUAL_UINT(1, device.closes);
        if (failure == TEST_TONE_RESTORE_FAIL) {
            TEST_ASSERT_TRUE(device.feedback.saved.restore_pending);
            TEST_ASSERT_TRUE(device.replied);
            TEST_ASSERT_TRUE(device.tone_error);
            continue;
        }
        TEST_ASSERT_EQUAL_HEX(TEST_TONE_REGISTER_OLD, device.reg);
        TEST_ASSERT_FALSE(device.feedback.saved.restore_pending);
        TEST_ASSERT_FALSE(device.amp.held);
        TEST_ASSERT_TRUE(device.amp.requested);
        TEST_ASSERT_EQUAL(failure == TEST_TONE_STOP_STUCK, device.playing);
        if (failure == TEST_TONE_STOP_DROPPED) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, device.stops);
        }
        if (failure == TEST_TONE_OK || failure == TEST_TONE_STOP_DROPPED) {
            TEST_ASSERT_FALSE(device.replied);
        } else {
            TEST_ASSERT_TRUE(device.replied);
            TEST_ASSERT_TRUE(device.tone_error);
        }
    }
}

static void
test_tone_pending_restore_blocks_both_modes(void) {
    memset(&device, 0, sizeof device);
    device.internal = true;
    device.reg = TEST_TONE_REGISTER_OLD;
    device.failure = TEST_TONE_RESTORE_FAIL;
    TEST_ASSERT_TRUE(console_registry_dispatch(&registry, "tone 1000 100 internal", tone_collect)
                     == CONSOLE_LINE_HANDLED);
    TEST_ASSERT_TRUE(device.replied);
    TEST_ASSERT_TRUE(device.tone_error);
    TEST_ASSERT_TRUE(device.feedback.saved.restore_pending);
    const unsigned measures = device.measures;
    static const char* const commands[] = {"tone 1000 100", "tone 1000 100 internal"};
    device.internal = false;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        device.replied = false;
        TEST_ASSERT_TRUE(console_registry_dispatch(&registry, commands[i], tone_collect) == CONSOLE_LINE_HANDLED);
        TEST_ASSERT_TRUE(device.replied);
        TEST_ASSERT_TRUE(device.tone_error);
        TEST_ASSERT_EQUAL_UINT(measures, device.measures);
        TEST_ASSERT_TRUE(device.feedback.saved.restore_pending);
    }
    device.failure = TEST_TONE_OK;
    device.replied = false;
    TEST_ASSERT_TRUE(console_registry_dispatch(&registry, commands[0], tone_collect) == CONSOLE_LINE_HANDLED);
    TEST_ASSERT_FALSE(device.replied);
    TEST_ASSERT_FALSE(device.feedback.saved.restore_pending);
    TEST_ASSERT_EQUAL_UINT(measures + 2, device.measures);
}

static void
test_tone_check_measures_shake_per_window(void) {
    suite_add_test_cleanup(input_fake_reset);
    memset(&device, 0, sizeof device);
    input_fake_motion_present(true);
    device.reg = TEST_TONE_REGISTER_OLD;
    const console_tone_args_t args = fixture();
    char line[CONSOLE_TONE_REPLY_BYTES];
    TEST_ASSERT_NULL(console_tone_check(&args, line));
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, device.odr_code);
    char expected[CONSOLE_TONE_REPLY_BYTES];
    const double mg = IMU_VIBRATION_MG_PER_G;
    snprintf(expected, sizeof expected, ", shake %.2f mg rms %.2f mg peak (silence %.2f mg rms)", 2 * mg, 2 * mg, mg);
    TEST_ASSERT_NOT_NULL(strstr(line, expected));
}

static void
test_tone_check_short_shake_window_is_unavailable(void) {
    suite_add_test_cleanup(input_fake_reset);
    memset(&device, 0, sizeof device);
    input_fake_motion_present(true);
    device.reg = TEST_TONE_REGISTER_OLD;
    device.shake_short = true;
    const console_tone_args_t args = fixture();
    char line[CONSOLE_TONE_REPLY_BYTES];
    TEST_ASSERT_NULL(console_tone_check(&args, line));
    TEST_ASSERT_NOT_NULL(strstr(line, ", shake n/a"));
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, device.odr_code);
}

static void
test_tone_acoustic_has_no_feedback(void) {
    memset(&device, 0, sizeof device);
    device.reg = TEST_TONE_REGISTER_OLD;
    TEST_ASSERT_TRUE(console_registry_dispatch(&registry, "tone 1000 100", tone_collect) == CONSOLE_LINE_HANDLED);
    TEST_ASSERT_EQUAL_UINT(2, device.measures);
    TEST_ASSERT_EQUAL_UINT(1, device.closes);
    TEST_ASSERT_EQUAL_HEX(TEST_TONE_REGISTER_OLD, device.reg);
    TEST_ASSERT_FALSE(device.feedback.saved.restore_pending);
    TEST_ASSERT_FALSE(device.replied);
}

bool
sound_fence(void) {
    TEST_ASSERT_EQUAL_INT(device.console.expected_stops, device.console.phase);
    TEST_ASSERT_EQUAL_UINT32(0, device.console.playing);
    TEST_ASSERT_EQUAL_INT(device.console.mounted + 1, asset_store_mounted());
    device.console.phase++;
    return !device.console.fence_timeout;
}

static console_sound_args_t
sound_fixture(void) {
    reset_device();
    device.console.enabled = true;
    device.console.expected_stops = 2;
    const char* probe = getenv("AUTANA_SOUND_PROBE");
    TEST_ASSERT_NOT_NULL(probe);
    char* folder = malloc(strlen(probe) + 1);
    TEST_ASSERT_NOT_NULL(folder);
    strcpy(folder, probe);
    char* slash = strrchr(folder, '/');
    TEST_ASSERT_NOT_NULL(slash);
    *slash = '\0';
    test_asset_dir_use(folder);
    free(folder);
    device.console.mounted = asset_store_mounted();
    console_sound_args_t args = {0};
    console_copy_text(args.pack, sizeof args.pack, "sound_probe");
    console_copy_text(args.name, sizeof args.name, "adpcm");
    return args;
}

static void
test_sound_transition_releases_all_views(void) {
    const console_sound_args_t args = sound_fixture();
    for (int transition = 0; transition < ASSET_STORE_MOUNTS_MAX; transition++) {
        int64_t open_us = 0;
        const sound_t* sound = console_sound_view(&args, &open_us);
        TEST_ASSERT_NOT_NULL(sound);
        device.console.playing = 1u << TEST_CONSOLE_VOICE_FIRST;
        console_sound_track_voice(sound, (sound_voice_t){.id = TEST_CONSOLE_VOICE_FIRST});
        for (uint32_t id = TEST_CONSOLE_VOICE_SECOND; id <= TEST_CONSOLE_VOICE_SECOND + SOUND_VOICE_COUNT; id++) {
            device.console.playing = (1u << TEST_CONSOLE_VOICE_FIRST) | (1u << id);
            console_sound_track_voice(sound, (sound_voice_t){.id = id});
        }
        device.console.stop_drops = 1;
        TEST_ASSERT_EQUAL_INT(device.console.mounted + 1, asset_store_mounted());
        device.console.phase = 0;
        device.console.system_playing = true;
        console_sound_release_views();
        TEST_ASSERT_TRUE(device.console.system_playing);
        TEST_ASSERT_EQUAL_INT(3, device.console.phase);
        TEST_ASSERT_EQUAL_INT(device.console.mounted, asset_store_mounted());
        TEST_ASSERT_NOT_NULL(console_sound_view(&args, &open_us));
        device.console.expected_stops = 0;
        device.console.phase = 0;
        console_sound_release_views();
        device.console.expected_stops = 2;
        TEST_ASSERT_EQUAL_INT(device.console.mounted, asset_store_mounted());
    }
}

static void
test_sound_transition_retains_views_until_a_fence_succeeds(void) {
    const console_sound_args_t args = sound_fixture();
    device.console.expected_stops = 0;
    device.console.fence_timeout = true;
    int64_t open_us = 0;
    TEST_ASSERT_NOT_NULL(console_sound_view(&args, &open_us));
    console_sound_release_views();
    TEST_ASSERT_EQUAL_INT(device.console.mounted + 1, asset_store_mounted());
    device.console.phase = 0;
    console_sound_release_views();
    TEST_ASSERT_EQUAL_INT(device.console.mounted + 1, asset_store_mounted());
    device.console.fence_timeout = false;
    device.console.phase = 0;
    console_sound_release_views();
    TEST_ASSERT_EQUAL_INT(device.console.mounted, asset_store_mounted());
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
        const audio_loopback_result_t result = {.measured_hz = values[i], .thd_db = values[i], .peak_dbfs = -6};
        TEST_ASSERT_TRUE(console_tone_reply(line, sizeof line, &args, true, -12, -100, &result, 0));
        TEST_ASSERT_NOT_NULL(strstr(line, i == 0 ? "thd 1000.0 dB" : "thd n/a dB"));
        TEST_ASSERT_NOT_NULL(strstr(line, "peak -6.0 dBFS"));
        puts(line);
        TEST_ASSERT_FALSE(console_tone_reply(line, strlen(line), &args, true, -12, -100, &result, 0));
        TEST_ASSERT_NOT_NULL(strstr(line, i == 0 ? expected : "measured n/a"));
    }
}

void
run_console_tone_suite(void) {
#ifndef ESP_PLATFORM
    RUN_TEST(test_sound_transition_retains_views_until_a_fence_succeeds);
    RUN_TEST(test_sound_transition_releases_all_views);
    RUN_TEST(test_melody_console_play_and_check);
    RUN_TEST(test_melody_console_failure_and_usage);
    RUN_TEST(test_melody_console_later_failures);
    RUN_TEST(test_tone_capture_restores_every_exit);
    RUN_TEST(test_tone_pending_restore_blocks_both_modes);
    RUN_TEST(test_tone_acoustic_has_no_feedback);
    RUN_TEST(test_tone_check_measures_shake_per_window);
    RUN_TEST(test_tone_check_short_shake_window_is_unavailable);
#endif
    RUN_TEST(test_tone_reply_frequency);
    RUN_TEST(test_tone_reply_capacity);
    RUN_TEST(test_tone_reply_shake);
    RUN_TEST(test_tone_parse_valid);
    RUN_TEST(test_tone_parse_invalid);
}
SUITE_REGISTER(run_console_tone_suite)

#ifndef ESP_PLATFORM
SUITE_READS(run_console_tone_suite, ASSET_ENGINE_PACK);
#endif

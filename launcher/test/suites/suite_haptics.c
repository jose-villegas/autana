#ifndef ESP_PLATFORM
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio/audio_out_device.h"
#include "console/console_verbs.h"
#include "core/timing.h"
#include "haptics/haptics.h"
#include "math/scalar/units.h"
#include "sound/sound_engine.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

static console_registry_t registry;

#define HAPTIC_TEST_FRAMES       16
#define HAPTIC_TEST_MUSIC_SAMPLE 1000

typedef struct {
    sound_control_t control;
    sound_mixer_t mixer;
    int16_t out[HAPTIC_TEST_FRAMES];
    int16_t scratch[HAPTIC_TEST_FRAMES];
    int32_t accumulator[HAPTIC_TEST_FRAMES];
    sound_command_t last;
    uint64_t now_ms;
    char reply[CONSOLE_LINE_MAX];
} haptic_fixture_t;

static haptic_fixture_t* haptic_rig;

static void
haptic_cleanup(void) {
    free(haptic_rig);
    haptic_rig = NULL;
}

static void
haptic_fixture(void) {
    haptic_rig = calloc(1, sizeof *haptic_rig);
    TEST_ASSERT_NOT_NULL(haptic_rig);
    suite_set_test_cleanup(haptic_cleanup);
    sound_control_init(&haptic_rig->control, true);
    sound_mixer_init(&haptic_rig->mixer);
}

static void
haptic_render(uint64_t now_ms) {
    haptic_rig->now_ms = now_ms;
    sound_mixer_sync(&haptic_rig->mixer, &haptic_rig->control);
    sound_mixer_render(&haptic_rig->mixer, haptic_rig->out, haptic_rig->accumulator, haptic_rig->scratch,
                       HAPTIC_TEST_FRAMES, now_ms, AUDIO_OUT_AMP_STARTUP_MS);
    sound_control_complete(&haptic_rig->control, &haptic_rig->mixer);
}

static void
haptic_assert_silence(void) {
    for (int i = 0; i < HAPTIC_TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, haptic_rig->out[i]);
    }
}

static void
haptic_collect(const char* line) {
    console_copy_text(haptic_rig->reply, sizeof haptic_rig->reply, line);
}

static sound_voice_t
haptic_test_play_tone(uint16_t hz, uint16_t ms, uint8_t gain_pct, sound_bus_t bus) {
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = gain_pct, .priority = SOUND_PRIORITY_MAX, .loop = true};
    const uint32_t generation = haptic_rig->control.app_generation;
    const sound_voice_t voice =
        sound_control_play(&haptic_rig->control, NULL, &opts, hz, ms, &haptic_rig->last, generation);
    if (voice.id) {
        sound_mixer_apply(&haptic_rig->mixer, &haptic_rig->last, generation);
    }
    return voice;
}

static void
haptic_test_stop(sound_voice_t voice) {
    sound_command_t command;
    if (sound_control_voice_command(&haptic_rig->control, voice, SOUND_COMMAND_STOP, 0, &command)) {
        sound_mixer_apply(&haptic_rig->mixer, &command, haptic_rig->control.app_generation);
    }
}

static bool
haptic_test_wait_audible(sound_voice_t voice, uint32_t timeout_ms) {
    if (timeout_ms && sound_control_is_playing(&haptic_rig->control, voice)
        && !sound_control_is_audible(&haptic_rig->control, voice)) {
        haptic_render(haptic_rig->now_ms + SOUND_CONTROL_POLL_MS);
    }
    return sound_control_is_audible(&haptic_rig->control, voice);
}

#define sound_play_tone haptic_test_play_tone
#define sound_stop      haptic_test_stop
#include "haptics/haptics.c"
#undef sound_play_tone
#undef sound_stop

#define console_shared()         (&registry)
#define sound_voice_wait_audible haptic_test_wait_audible
#define timing_now_us()          ((int64_t)haptic_rig->now_ms * MATH_US_PER_MS)
#include "haptics/console/console_haptic.c"
#undef console_shared
#undef sound_voice_wait_audible
#undef timing_now_us

static void
test_haptic_console_latency_and_errors(void) {
    haptic_fixture();
    haptic_render(AUDIO_OUT_AMP_STARTUP_MS);
    TEST_ASSERT_EQUAL(CONSOLE_LINE_HANDLED, console_registry_dispatch(&registry, "haptic light", haptic_collect));
    char expected[CONSOLE_LINE_MAX];
    (void)snprintf(expected, sizeof expected, "HAPTIC light audible_ms=%u", SOUND_CONTROL_POLL_MS);
    TEST_ASSERT_EQUAL_STRING(expected, haptic_rig->reply);
    TEST_ASSERT_EQUAL(CONSOLE_LINE_HANDLED, console_registry_dispatch(&registry, "haptic firm", haptic_collect));
    (void)snprintf(expected, sizeof expected, "HAPTIC firm audible_ms=%u", SOUND_CONTROL_POLL_MS);
    TEST_ASSERT_EQUAL_STRING(expected, haptic_rig->reply);
    TEST_ASSERT_EQUAL(CONSOLE_LINE_HANDLED, console_registry_dispatch(&registry, "haptic medium", haptic_collect));
    TEST_ASSERT_EQUAL_STRING("HAPTIC usage: haptic <light|firm>", haptic_rig->reply);
    haptic_rig->control.ready = false;
    TEST_ASSERT_EQUAL(CONSOLE_LINE_HANDLED, console_registry_dispatch(&registry, "haptic light", haptic_collect));
    TEST_ASSERT_EQUAL_STRING("HAPTIC error: pulse not audible", haptic_rig->reply);
}

static void
test_haptic_strengths_and_restart(void) {
    haptic_fixture();
    const sound_voice_t first = haptic_pulse(HAPTIC_LIGHT);
    TEST_ASSERT_EQUAL_UINT16(HAPTIC_HZ, haptic_rig->last.tone.hz);
    TEST_ASSERT_EQUAL_UINT16(HAPTIC_LIGHT_MS, haptic_rig->last.tone.ms);
    TEST_ASSERT_EQUAL_UINT8(HAPTIC_LIGHT_GAIN_PCT, haptic_rig->last.gain_pct);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_HAPTIC.id, haptic_rig->last.bus.id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_PRIORITY_MAX, haptic_rig->last.priority);
    haptic_render(0);
    haptic_assert_silence();
    haptic_render(AUDIO_OUT_AMP_STARTUP_MS - 1);
    haptic_assert_silence();
    TEST_ASSERT_FALSE(sound_control_is_audible(&haptic_rig->control, haptic_rig->last.voice));
    haptic_render(AUDIO_OUT_AMP_STARTUP_MS);
    TEST_ASSERT_TRUE(sound_control_is_audible(&haptic_rig->control, first));
    const sound_voice_t next = haptic_pulse(HAPTIC_FIRM);
    TEST_ASSERT_FALSE(sound_control_is_playing(&haptic_rig->control, first));
    TEST_ASSERT_EQUAL_UINT16(HAPTIC_FIRM_MS, haptic_rig->last.tone.ms);
    TEST_ASSERT_EQUAL_UINT8(HAPTIC_FIRM_GAIN_PCT, haptic_rig->last.gain_pct);
    haptic_render(AUDIO_OUT_AMP_STARTUP_MS);
    size_t left = audio_out_samples(HAPTIC_FIRM_MS) - HAPTIC_TEST_FRAMES;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        if (haptic_rig->mixer.voices[i].playing) {
            TEST_ASSERT_EQUAL_UINT32(next.id, haptic_rig->mixer.voices[i].handle.id);
            TEST_ASSERT_EQUAL_UINT32(left, haptic_rig->mixer.voices[i].source.tone.frames_left);
        }
    }
    while (left) {
        haptic_render(AUDIO_OUT_AMP_STARTUP_MS);
        left = left > HAPTIC_TEST_FRAMES ? left - HAPTIC_TEST_FRAMES : 0;
        TEST_ASSERT_EQUAL(left != 0, sound_control_is_playing(&haptic_rig->control, next));
    }
}

static void
test_haptic_bus_isolation_and_music(void) {
    haptic_fixture();
    const sound_bus_t muted[] = {SOUND_BUS_MASTER, SOUND_BUS_HAPTIC, SOUND_BUS_SYSTEM};
    for (size_t b = 0; b < sizeof muted / sizeof muted[0]; b++) {
        sound_mixer_init(&haptic_rig->mixer);
        sound_control_init(&haptic_rig->control, true);
        sound_command_t gain;
        TEST_ASSERT_TRUE(sound_control_bus_command(&haptic_rig->control, muted[b], SOUND_COMMAND_BUS_GAIN, 0, &gain));
        sound_mixer_apply(&haptic_rig->mixer, &gain, haptic_rig->control.app_generation);
        haptic_pulse(HAPTIC_LIGHT);
        haptic_render(0);
        haptic_render(AUDIO_OUT_AMP_STARTUP_MS);
        if (muted[b].id != SOUND_BUS_SYSTEM.id) {
            haptic_assert_silence();
        } else {
            TEST_ASSERT_NOT_EQUAL(0, haptic_rig->out[HAPTIC_TEST_FRAMES - 1]);
        }
    }
    uint8_t pcm[SOUND_PCM_BYTES] = {HAPTIC_TEST_MUSIC_SAMPLE & UINT8_MAX, HAPTIC_TEST_MUSIC_SAMPLE >> CHAR_BIT};
    const sound_t music = {.data = pcm, .frames = 1, .format = SOUND_PCM16};
    const sound_play_opts_t opts = {.bus = SOUND_BUS_MUSIC, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    sound_command_t command;
    const sound_voice_t voice =
        sound_control_play(&haptic_rig->control, &music, &opts, 0, 0, &command, haptic_rig->control.app_generation);
    sound_mixer_apply(&haptic_rig->mixer, &command, haptic_rig->control.app_generation);
    haptic_pulse(HAPTIC_LIGHT);
    haptic_render(AUDIO_OUT_AMP_STARTUP_MS);
    TEST_ASSERT_TRUE(sound_control_is_audible(&haptic_rig->control, voice));
    TEST_ASSERT_TRUE(sound_control_is_audible(&haptic_rig->control, haptic_rig->last.voice));
    TEST_ASSERT_EQUAL_INT16(HAPTIC_TEST_MUSIC_SAMPLE, haptic_rig->out[0]);
    TEST_ASSERT_NOT_EQUAL(HAPTIC_TEST_MUSIC_SAMPLE, haptic_rig->out[HAPTIC_TEST_FRAMES - 1]);
}

void
run_haptics_suite(void) {
    RUN_TEST(test_haptic_console_latency_and_errors);
    RUN_TEST(test_haptic_strengths_and_restart);
    RUN_TEST(test_haptic_bus_isolation_and_music);
}
SUITE_REGISTER(run_haptics_suite)
#endif
